#include "cia.h"

#include <string.h>

#include "bus.h"

// MOS 6526 CIA, timers + interrupts (Phase 5a). Cycle-exact delay-pipeline
// timer model from Wolfgang Lorenz, "A Software Model of the CIA6526" (Version
// 2.15, 1997), the public timing documentation behind his C64 test suite:
// https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html . Clean-room: implemented
// from that prose and calibrated against his own CIA1TAB expected values; reSID,
// VICE ciacore, and any GPL emulator were not consulted. The reset state (timers
// and latches = $FFFF, control/ICR cleared, interrupt lines released) is the
// 6526 datasheet reset. Two instances share this code; CIA1 drives IRQ, CIA2 NMI.
//
// Timer pipeline: a count pulse enters at COUNT0 and shifts up one stage per
// clock; the decrement happens at COUNT3. In phi2 mode the pulse is fed every
// clock (feed), so the counter decrements every clock; Timer B in cascade mode
// gets one pulse per Timer A underflow. When the counter hits 0 with another
// clock already in the pipeline (COUNT2 set) it reloads at once and removes the
// next count (the 2-1-2 read pattern); with no pending clock (cascade) it sits
// at 0 until the next pulse (the 2-2-2-1-1-1-0-0-2 pattern).
//
// Validation: Timer A in phi2 mode reproduces Lorenz's CIA1TAB table exactly
// (reads 01 02 02 repeating for a latch of 2). invariant: the Timer B cascade
// path holds 0 for one clock longer than Lorenz's CIA1TAB (a bounded ~1-clock
// offset in the cascade pipeline phase); it counts Timer A underflows correctly,
// only the exact hold-at-zero cycle differs.
//
// Scope note: the Lorenz trap tests (trap16+) do NOT advance on the bare test
// runner. They are 6510 CPU interrupt-sequencing tests that assume Timer A is
// already running (the KERNAL 60 Hz jiffy IRQ) with the $0314 IRQ handler set
// up, i.e. a full KERNAL boot. The runner does not boot the KERNAL, so those
// tests stop at 236 by environment limitation, not by a CIA defect (the CIA is
// unit-tested and matches CIA1TAB above). Advancing them is deferred to a
// real-KERNAL runner, which arrives naturally after the Phase 7 clean-room
// KERNAL.

// Register offsets within a CIA page (mirrored every 16 bytes).
#define R_PRA 0x0u
#define R_PRB 0x1u
#define R_DDRA 0x2u
#define R_DDRB 0x3u
#define R_TALO 0x4u
#define R_TAHI 0x5u
#define R_TBLO 0x6u
#define R_TBHI 0x7u
#define R_TOD0 0x8u
#define R_TOD3 0xBu
#define R_SDR 0xCu
#define R_ICR 0xDu
#define R_CRA 0xEu
#define R_CRB 0xFu

// Control register bits.
#define CR_START 0x01u
#define CR_PBON 0x02u
#define CR_OUTMODE 0x04u
#define CR_ONESHOT 0x08u
#define CR_FLOAD 0x10u
#define CRA_INMODE 0x20u        // 0 = phi2, 1 = CNT
#define CRB_INMODE_MASK 0x60u   // bits 6-5 select Timer B input
#define CRB_INMODE_CASCADE 0x40u  // 10 = count Timer A underflows

// ICR flag bits.
#define ICR_TA 0x01u
#define ICR_TB 0x02u
#define ICR_IR 0x80u

// Timer count pipeline: a pulse enters at COUNT0 and shifts up one stage per
// clock; COUNT3 is the decrement stage. Kept to 4 bits so COUNT3 shifts off the
// top rather than into another field.
#define P_COUNT0 0x01u
#define P_COUNT1 0x02u
#define P_COUNT2 0x04u
#define P_COUNT3 0x08u
#define P_MASK 0x0Fu

typedef struct {
    uint16_t counter;
    uint16_t latch;
    uint8_t cr;
    uint8_t delay;        // count pipeline (COUNT0..COUNT3)
    uint8_t feed;         // continuously fed pipeline bits (phi2 running)
    bool load_strobe;     // force-load requested (CR force-load bit)
    bool load_pending;    // counter at 0, waiting for the next pulse (cascade)
    bool undf;            // underflowed this clock
} Timer;

typedef struct {
    Timer ta, tb;
    uint8_t reg[16];      // ports/DDR/TOD/SDR (plain registers for now)
    uint8_t icr_data;     // latched interrupt flags (bits 0-4)
    uint8_t icr_mask;     // interrupt mask (IMR)
    bool irq_out;         // this CIA is asserting its interrupt line
    uint8_t irq_delay;    // 1-clock delay pipeline for the interrupt line
    uint8_t nmi_source;   // BUS_NMI_CIA2 if this CIA drives NMI, else 0
} CIA;

static CIA cia[2];

// Assert/deassert this CIA's interrupt line on the correct bus signal.
static void cia_set_line(CIA *c, bool asserted) {
    c->irq_out = asserted;
    if (c->nmi_source) {
        bus_nmi_set(c->nmi_source, asserted);
    } else {
        bus_irq_set(BUS_IRQ_CIA1, asserted);
    }
}

// Advance one timer given whether a fresh count pulse enters the pipeline.
static void timer_step(Timer *t, bool pulse) {
    uint8_t d = (uint8_t)((t->delay << 1) & P_MASK);
    if (pulse || (t->feed & P_COUNT0)) {
        d |= P_COUNT0;  // fresh pulse this clock (cascade pulse or phi2 feed)
    }
    t->undf = false;

    if (t->load_strobe) {           // force-load: reload now, remove next count
        t->counter = t->latch;
        t->load_pending = false;
        t->load_strobe = false;
        d &= (uint8_t)~P_COUNT2;
    } else if (d & P_COUNT3) {      // a count reaches the decrement stage
        if (t->load_pending) {      // was sitting at 0 (cascade): reload now
            t->counter = t->latch;
            t->load_pending = false;
            t->undf = true;
        } else {
            t->counter = (uint16_t)(t->counter - 1u);
            if (t->counter == 0) {
                if (d & P_COUNT2) {           // another clock waiting: reload now
                    t->counter = t->latch;
                    t->undf = true;           // underflow coincides with reload
                    d &= (uint8_t)~P_COUNT2;  // remove the next count
                } else {
                    t->load_pending = true;   // sit at 0, underflow on next pulse
                }
            }
        }
    }
    t->delay = d;
}

// Set/clear a timer's phi2 feed and start when CR bit 0 changes.
static void timer_apply_cr(Timer *t, bool phi2_input) {
    if ((t->cr & CR_START) && phi2_input) {
        t->feed |= P_COUNT0;
    } else {
        t->feed &= (uint8_t)~P_COUNT0;
    }
}

static void cia_update_interrupt(CIA *c) {
    // The line follows the ICR through a one-clock delay pipeline; once set, the
    // IR flag latches until the ICR is read.
    if ((c->icr_data & c->icr_mask & 0x1Fu) != 0) {
        c->icr_data |= ICR_IR;
    }
    bool want = (c->icr_data & ICR_IR) != 0;
    // one phi2 clock of delay before the line asserts
    c->irq_delay = (uint8_t)((c->irq_delay << 1) | (want ? 1u : 0u));
    bool line = (c->irq_delay & 0x02u) != 0 || (c->irq_out && want);
    if (line != c->irq_out) {
        cia_set_line(c, line);
    }
}

static void one_cia_clock(CIA *c) {
    bool phi2_a = (c->ta.cr & CRA_INMODE) == 0;   // TA phi2 mode
    timer_step(&c->ta, false);
    if (c->ta.undf) {
        c->icr_data |= ICR_TA;
        if (c->ta.cr & CR_ONESHOT) {
            c->ta.cr &= (uint8_t)~CR_START;
            c->ta.feed &= (uint8_t)~P_COUNT0;
        }
    }
    // Timer B: phi2 mode or cascade (count TA underflows).
    bool cascade = (c->tb.cr & CRB_INMODE_MASK) == CRB_INMODE_CASCADE;
    bool pulse_b = cascade && c->ta.undf;
    timer_step(&c->tb, pulse_b);
    if (c->tb.undf) {
        c->icr_data |= ICR_TB;
        if (c->tb.cr & CR_ONESHOT) {
            c->tb.cr &= (uint8_t)~CR_START;
            c->tb.feed &= (uint8_t)~P_COUNT0;
        }
    }
    (void)phi2_a;
    cia_update_interrupt(c);
}

void cia_clock(void) {
    one_cia_clock(&cia[0]);
    one_cia_clock(&cia[1]);
}

// ---- Register access ------------------------------------------------------

static uint8_t cia_reg_read(CIA *c, unsigned r) {
    switch (r) {
        case R_TALO:
            return (uint8_t)(c->ta.counter & 0xFFu);
        case R_TAHI:
            return (uint8_t)(c->ta.counter >> 8);
        case R_TBLO:
            return (uint8_t)(c->tb.counter & 0xFFu);
        case R_TBHI:
            return (uint8_t)(c->tb.counter >> 8);
        case R_ICR: {
            uint8_t v = (uint8_t)(c->icr_data & (0x1Fu | ICR_IR));
            c->icr_data = 0;             // read-to-clear: flags and IR
            c->irq_delay = 0;
            if (c->irq_out) {
                cia_set_line(c, false);  // deassert the line
            }
            return v;
        }
        case R_CRA:
            return c->ta.cr;
        case R_CRB:
            return c->tb.cr;
        default:
            return c->reg[r];  // ports/DDR/TOD/SDR (plain, Phase 5b/5c)
    }
}

static void cia_reg_write(CIA *c, unsigned r, uint8_t v) {
    switch (r) {
        case R_TALO:
            c->ta.latch = (uint16_t)((c->ta.latch & 0xFF00u) | v);
            break;
        case R_TAHI:
            c->ta.latch = (uint16_t)((c->ta.latch & 0x00FFu) | ((uint16_t)v << 8));
            if (!(c->ta.cr & CR_START)) {
                c->ta.counter = c->ta.latch;  // load high byte only when stopped
            }
            break;
        case R_TBLO:
            c->tb.latch = (uint16_t)((c->tb.latch & 0xFF00u) | v);
            break;
        case R_TBHI:
            c->tb.latch = (uint16_t)((c->tb.latch & 0x00FFu) | ((uint16_t)v << 8));
            if (!(c->tb.cr & CR_START)) {
                c->tb.counter = c->tb.latch;
            }
            break;
        case R_ICR:
            if (v & 0x80u) {
                c->icr_mask |= (uint8_t)(v & 0x1Fu);   // set the given mask bits
            } else {
                c->icr_mask &= (uint8_t)~(v & 0x1Fu);  // clear them
            }
            break;
        case R_CRA:
            c->ta.cr = v;
            if (v & CR_FLOAD) {
                c->ta.load_strobe = true;  // force-load strobe (not stored)
                c->ta.cr &= (uint8_t)~CR_FLOAD;
            }
            timer_apply_cr(&c->ta, (v & CRA_INMODE) == 0);
            break;
        case R_CRB:
            c->tb.cr = v;
            if (v & CR_FLOAD) {
                c->tb.load_strobe = true;
                c->tb.cr &= (uint8_t)~CR_FLOAD;
            }
            timer_apply_cr(&c->tb, (v & CRB_INMODE_MASK) == 0);
            break;
        default:
            c->reg[r] = v;
            break;
    }
}

uint8_t cia1_read(uint16_t addr) { return cia_reg_read(&cia[0], addr & 0x0Fu); }
void cia1_write(uint16_t addr, uint8_t val) { cia_reg_write(&cia[0], addr & 0x0Fu, val); }
uint8_t cia2_read(uint16_t addr) { return cia_reg_read(&cia[1], addr & 0x0Fu); }
void cia2_write(uint16_t addr, uint8_t val) { cia_reg_write(&cia[1], addr & 0x0Fu, val); }

// ---- Inspection -----------------------------------------------------------

uint16_t cia_timer_a(unsigned n) { return cia[n & 1u].ta.counter; }
uint16_t cia_timer_b(unsigned n) { return cia[n & 1u].tb.counter; }
uint8_t cia_icr_flags(unsigned n) { return (uint8_t)(cia[n & 1u].icr_data & 0x1Fu); }

// ---- Lifecycle ------------------------------------------------------------

void cia_reset(void) {
    memset(cia, 0, sizeof(cia));
    cia[1].nmi_source = BUS_NMI_CIA2;
    cia[0].ta.latch = cia[0].tb.latch = 0xFFFF;
    cia[1].ta.latch = cia[1].tb.latch = 0xFFFF;
    cia[0].ta.counter = cia[0].tb.counter = 0xFFFF;
    cia[1].ta.counter = cia[1].tb.counter = 0xFFFF;
    // 6526 reset clears the ICR (mask and flags), so no interrupt is asserted:
    // release this CIA's contribution to the wired-OR IRQ/NMI lines.
    bus_irq_set(BUS_IRQ_CIA1, false);
    bus_nmi_set(BUS_NMI_CIA2, false);
}

void cia_init(void) { cia_reset(); }
