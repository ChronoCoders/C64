#include "cia.h"

#include <string.h>

#include "bus.h"
#include "iec.h"

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
#define R_TODTEN 0x8u   // tenths of a second (BCD)
#define R_TODSEC 0x9u
#define R_TODMIN 0xAu
#define R_TODHR 0xBu    // hours, bit 7 = PM
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
#define CRA_SPMODE 0x40u        // serial port: 0 = input, 1 = output (Timer A)
#define CRA_TODIN 0x80u         // TOD input: 0 = 60 Hz, 1 = 50 Hz
#define CRB_INMODE_MASK 0x60u   // bits 6-5 select Timer B input
#define CRB_INMODE_CASCADE 0x40u  // 10 = count Timer A underflows
#define CRB_ALARM 0x80u         // TOD register writes target the alarm

// IEC serial bus lines on CIA2 Port A (C64 side). Out bits drive through
// inverters (a 1 pulls the line low); in bits sense the wired-AND line.
#define IEC_ATN_OUT 0x08u   // PA3
#define IEC_CLK_OUT 0x10u   // PA4
#define IEC_DATA_OUT 0x20u  // PA5
#define IEC_CLK_IN 0x40u    // PA6
#define IEC_DATA_IN 0x80u   // PA7
// Device pulls on the wired-AND bus (IEC_PULL_* in cia.h); 0 with no device.
static uint8_t iec_dev_pull;

// ICR serial and TOD-alarm flags.
#define ICR_TOD 0x04u
#define ICR_SP 0x08u

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

// Time of Day clock (BCD): tenths/seconds/minutes/hours, hours bit 7 = PM.
typedef struct {
    uint8_t tenths, seconds, minutes, hours;      // running clock
    uint8_t a_tenths, a_seconds, a_minutes, a_hours;  // alarm
    uint8_t l_tenths, l_seconds, l_minutes, l_hours;  // read-latch snapshot
    bool latched;    // read latch held: set on HOURS read, released on TENTHS read
    bool stopped;    // clock halted between a HOURS write and the TENTHS write
    uint32_t acc;    // phi2 -> 10 Hz tenth-tick accumulator
} Tod;

typedef struct {
    Timer ta, tb;
    uint8_t reg[16];      // ports/DDR
    uint8_t icr_data;     // latched interrupt flags (bits 0-4)
    uint8_t icr_mask;     // interrupt mask (IMR)
    bool irq_out;         // this CIA is asserting its interrupt line
    uint8_t irq_delay;    // 1-clock delay pipeline for the interrupt line
    uint8_t nmi_source;   // BUS_NMI_CIA2 if this CIA drives NMI, else 0
    Tod tod;
    uint8_t sdr;          // serial data register
    uint8_t sr_shift;     // serial shift register (output)
    uint8_t sr_bits;      // bits left to shift out
    bool sr_active;       // a byte is shifting out
    bool sr_cnt;          // internal CNT toggle for output serial
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

// ---- TOD clock and serial shift register (Phase 5c) -----------------------

#define CIA_PHI2_HZ 985248u   // PAL phi2; the TOD tenth ticks at 10 Hz off it

// Increment a two-digit BCD value in the 00..59 range; wraps to 0 with carry.
static uint8_t bcd_inc60(uint8_t v, bool *carry) {
    v = (uint8_t)(((v & 0x0Fu) == 9) ? (v & 0xF0u) + 0x10u : (unsigned)v + 1u);
    if (v >= 0x60u) {
        v = 0;
        *carry = true;
    }
    return v;
}

// Advance a running TOD by one tenth: propagate the BCD carry chain through
// seconds/minutes and the 12-hour hours with the AM/PM toggle, then test the
// alarm. The real TOD input is 50/60 Hz divided to a 10 Hz tenth rate; the
// tenth rate is identical either way, so CRA bit 7 selects the pin frequency
// but does not change what is observable here.
static void tod_advance(CIA *c) {
    Tod *t = &c->tod;
    if (t->stopped) {
        return;
    }
    bool carry = false;
    if (t->tenths >= 0x09u) {
        t->tenths = 0;
        carry = true;
    } else {
        t->tenths++;
    }
    if (carry) {
        carry = false;
        t->seconds = bcd_inc60(t->seconds, &carry);
        if (carry) {
            carry = false;
            t->minutes = bcd_inc60(t->minutes, &carry);
            if (carry) {
                uint8_t h = (uint8_t)(t->hours & 0x1Fu);  // BCD 1..12
                uint8_t pm = (uint8_t)(t->hours & 0x80u);
                if (h == 0x12u) {
                    h = 0x01u;  // 12 -> 1, no toggle
                } else {
                    h = (uint8_t)(((h & 0x0Fu) == 9) ? (h & 0xF0u) + 0x10u : (unsigned)h + 1u);
                    if (h == 0x12u) {
                        pm ^= 0x80u;  // AM/PM toggles when the hour reaches 12
                    }
                }
                t->hours = (uint8_t)(h | pm);
            }
        }
    }
    if (t->tenths == t->a_tenths && t->seconds == t->a_seconds &&
        t->minutes == t->a_minutes && t->hours == t->a_hours) {
        c->icr_data |= ICR_TOD;  // alarm match sets ICR bit 2
    }
}

// Serial output: with CRA bit 6 set, a byte written to the SDR shifts out at
// half the Timer A underflow rate (CNT toggles each underflow, one bit per
// falling edge). After 8 bits, ICR bit 3 sets.
static void serial_ta_underflow(CIA *c) {
    if (!(c->ta.cr & CRA_SPMODE) || !c->sr_active) {
        return;
    }
    c->sr_cnt = !c->sr_cnt;
    if (!c->sr_cnt) {  // falling CNT edge shifts one bit out
        c->sr_shift = (uint8_t)(c->sr_shift << 1);
        c->sr_bits--;
        if (c->sr_bits == 0) {
            c->sr_active = false;
            c->icr_data |= ICR_SP;
        }
    }
}

static void one_cia_clock(CIA *c) {
    bool phi2_a = (c->ta.cr & CRA_INMODE) == 0;   // TA phi2 mode
    timer_step(&c->ta, false);
    if (c->ta.undf) {
        c->icr_data |= ICR_TA;
        serial_ta_underflow(c);  // serial output is clocked by Timer A
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
    // TOD: phi2 -> 10 Hz tenth tick via a fractional accumulator.
    c->tod.acc += 10u;
    if (c->tod.acc >= CIA_PHI2_HZ) {
        c->tod.acc -= CIA_PHI2_HZ;
        tod_advance(c);
    }
    cia_update_interrupt(c);
}

void cia_clock(void) {
    one_cia_clock(&cia[0]);
    one_cia_clock(&cia[1]);
}

// ---- Keyboard matrix and joysticks (Phase 5b, CIA1 only) ------------------
//
// The C64 keyboard is an 8x8 matrix wired between CIA1 Port A (rows, driven low
// one at a time by the KERNAL scan) and Port B (columns, read). kb_matrix[r] has
// bit c set when the key at row r, column c is held. The joysticks share the
// same lines: joystick 2 grounds Port A bits, joystick 1 grounds Port B bits
// (bits 0-4 = up/down/left/right/fire). A port read is the wired composition of
// the driven register bits, the joystick pulls, and the matrix crosspoints
// (rows driven low pull their columns low, and columns driven low pull their
// rows low), which is why joystick and keyboard input interfere.
// invariant: multi-hop keyboard ghosting (phantom keys from 3+ keys bridging
// shared rows and columns) is not modelled; single-hop matrix reads, multi-key
// in a row, both scan directions, and joystick sharing are exact.
static uint8_t kb_matrix[8];   // kb_matrix[row] bit col = key held
static uint8_t joy_pull[2];    // [0] = joystick 1 (Port B), [1] = joystick 2 (Port A)

// Compute the Port A and Port B pin values for CIA1 (matrix + joystick + driven).
static void cia1_pins(uint8_t *out_pa, uint8_t *out_pb) {
    CIA *c = &cia[0];
    uint8_t ddra = c->reg[R_DDRA], ddrb = c->reg[R_DDRB];
    // Output bits drive the data register; input bits float high (pull-up).
    uint8_t pa = (uint8_t)((c->reg[R_PRA] & ddra) | (uint8_t)~ddra);
    uint8_t pb = (uint8_t)((c->reg[R_PRB] & ddrb) | (uint8_t)~ddrb);
    pa &= (uint8_t)~joy_pull[1];  // joystick 2 grounds Port A lines
    pb &= (uint8_t)~joy_pull[0];  // joystick 1 grounds Port B lines
    uint8_t row_low = (uint8_t)~pa;  // rows currently low
    uint8_t col_low = (uint8_t)~pb;  // columns currently low
    for (unsigned r = 0; r < 8; r++) {
        if (row_low & (1u << r)) {
            pb &= (uint8_t)~kb_matrix[r];  // a low row pulls its held columns low
        }
        for (unsigned col = 0; col < 8; col++) {
            if ((col_low & (1u << col)) && (kb_matrix[r] & (1u << col))) {
                pa &= (uint8_t)~(1u << r);  // a low column pulls its held rows low
            }
        }
    }
    *out_pa = pa;
    *out_pb = pb;
}

// The read of a CIA1 port returns the pin state for every bit, input or output:
// an external puller (a joystick switch, a matrix crosspoint) wins over the CIA's
// output driver. cia1_pins already folds the driven value in, so the pins are the
// answer. This is what makes joystick 2 readable at all: the KERNAL leaves
// DDRA=$FF to drive the keyboard rows, so masking the pins by ~DDRA would discard
// every joystick 2 pull.
static uint8_t cia1_read_port(unsigned r) {
    uint8_t pa, pb;
    cia1_pins(&pa, &pb);
    return (r == R_PRA) ? pa : pb;
}

// ---- IEC serial bus (Phase 5c, CIA2 Port A, C64 side) ---------------------
//
// The IEC lines are open-collector (wired-AND): a line is low if any driver
// pulls it low. The C64 drives ATN/CLK/DATA out (a 1 on the OUT bit pulls the
// line low through an inverter) and senses CLK/DATA in on bits 6-7. With no
// device attached (Phase 5c) a line is low only when the C64 itself pulls it;
// iec_dev_pull lets a test, and later the Phase 6 drive, act as a second puller
// on the same wired-AND, exactly like the keyboard/joystick sharing on CIA1.
static uint8_t cia2_read_pa(void) {
    CIA *c = &cia[1];
    uint8_t ddra = c->reg[R_DDRA];
    uint8_t v = (uint8_t)((c->reg[R_PRA] & ddra) | (uint8_t)~ddra);
    uint8_t out = (uint8_t)((c->reg[R_PRA] & ddra) | (uint8_t)~ddra);  // input pin floats high; the 7406 inverter pulls its line low
    bool clk_low = (out & IEC_CLK_OUT) || (iec_dev_pull & IEC_PULL_CLK);
    bool data_low = (out & IEC_DATA_OUT) || (iec_dev_pull & IEC_PULL_DATA);
    if (clk_low) {
        v &= (uint8_t)~IEC_CLK_IN;
    } else {
        v |= IEC_CLK_IN;
    }
    if (data_low) {
        v &= (uint8_t)~IEC_DATA_IN;
    } else {
        v |= IEC_DATA_IN;
    }
    return v;
}

// The IEC lines the C64 is currently pulling low (IEC_PULL_* convention), for the
// shared drive bus (Phase 6c): a CIA2 Port A pin pulls its line low when the pin
// state is high, i.e. driven high OR floating (input) into the 7406 inverter.
// This is the read side of the connection; cia_iec_device_pull is the write side.
uint8_t cia2_iec_out(void) {
    uint8_t ddra = cia[1].reg[R_DDRA];
    uint8_t out = (uint8_t)((cia[1].reg[R_PRA] & ddra) | (uint8_t)~ddra);  // input pin floats high; the 7406 inverter pulls its line low
    uint8_t m = 0;
    if (out & IEC_ATN_OUT) { m |= IEC_PULL_ATN; }
    if (out & IEC_CLK_OUT) { m |= IEC_PULL_CLK; }
    if (out & IEC_DATA_OUT) { m |= IEC_PULL_DATA; }
    return m;
}

// VIC video bank (0-3) from CIA2 Port A bits 0-1. VA14/VA15 are the inverted
// driven port bits, so the reset/boot default (DDR=0, pulled up) selects bank 0.
uint8_t cia2_vic_bank(void) {
    CIA *c = &cia[1];
    uint8_t pa = (uint8_t)((c->reg[R_PRA] & c->reg[R_DDRA]) | (uint8_t)~c->reg[R_DDRA]);
    return (uint8_t)((~pa) & 0x03u);
}

// ---- Register access ------------------------------------------------------

static uint8_t cia_reg_read(CIA *c, unsigned r) {
    switch (r) {
        case R_PRA:
        case R_PRB:
            if (c == &cia[0]) {
                return cia1_read_port(r);  // CIA1 ports carry the keyboard/joystick
            }
            if (r == R_PRA) {
                return cia2_read_pa();  // CIA2 Port A carries the IEC bus lines
            }
            return c->reg[r];
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
        case R_TODTEN: {
            uint8_t v = c->tod.latched ? c->tod.l_tenths : c->tod.tenths;
            c->tod.latched = false;  // reading tenths releases the read latch
            return v;
        }
        case R_TODSEC:
            return c->tod.latched ? c->tod.l_seconds : c->tod.seconds;
        case R_TODMIN:
            return c->tod.latched ? c->tod.l_minutes : c->tod.minutes;
        case R_TODHR:
            if (!c->tod.latched) {  // reading hours latches a coherent snapshot
                c->tod.l_tenths = c->tod.tenths;
                c->tod.l_seconds = c->tod.seconds;
                c->tod.l_minutes = c->tod.minutes;
                c->tod.l_hours = c->tod.hours;
                c->tod.latched = true;
            }
            return c->tod.l_hours;
        case R_SDR:
            return c->sdr;
        default:
            return c->reg[r];  // ports/DDR (plain)
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
        case R_TODTEN:
            if (c->tb.cr & CRB_ALARM) {
                c->tod.a_tenths = v;
            } else {
                c->tod.tenths = v;
                c->tod.stopped = false;  // writing tenths starts the clock
            }
            break;
        case R_TODSEC:
            if (c->tb.cr & CRB_ALARM) {
                c->tod.a_seconds = v;
            } else {
                c->tod.seconds = v;
            }
            break;
        case R_TODMIN:
            if (c->tb.cr & CRB_ALARM) {
                c->tod.a_minutes = v;
            } else {
                c->tod.minutes = v;
            }
            break;
        case R_TODHR:
            if (c->tb.cr & CRB_ALARM) {
                c->tod.a_hours = v;
            } else {
                c->tod.hours = v;
                c->tod.stopped = true;  // writing hours stops the clock
            }
            break;
        case R_SDR:
            c->sdr = v;
            if ((c->ta.cr & CRA_SPMODE) && !c->sr_active) {
                c->sr_shift = v;      // output mode: begin shifting the byte out
                c->sr_bits = 8;
                c->sr_active = true;
                c->sr_cnt = false;
            }
            break;
        default:
            c->reg[r] = v;
            break;
    }
}

uint8_t cia1_read(uint16_t addr) { return cia_reg_read(&cia[0], addr & 0x0Fu); }
void cia1_write(uint16_t addr, uint8_t val) { cia_reg_write(&cia[0], addr & 0x0Fu, val); }
uint8_t cia2_read(uint16_t addr) { return cia_reg_read(&cia[1], addr & 0x0Fu); }
void cia2_write(uint16_t addr, uint8_t val) {
    unsigned r = addr & 0x0Fu;
    cia_reg_write(&cia[1], r, val);
    if (r == R_PRA || r == R_DDRA) {
        iec_dirty = true;  // the only CIA2 bytes cia2_iec_out reads
    }
}

// ---- Inspection -----------------------------------------------------------

uint16_t cia_timer_a(unsigned n) { return cia[n & 1u].ta.counter; }
uint16_t cia_timer_b(unsigned n) { return cia[n & 1u].tb.counter; }
uint8_t cia_icr_flags(unsigned n) { return (uint8_t)(cia[n & 1u].icr_data & 0x1Fu); }

// ---- Keyboard / joystick input (host and tests) ---------------------------

void cia_key_set(unsigned row, unsigned col, bool pressed) {
    if (row >= 8 || col >= 8) {
        return;
    }
    if (pressed) {
        kb_matrix[row] |= (uint8_t)(1u << col);
    } else {
        kb_matrix[row] &= (uint8_t)~(1u << col);
    }
}

void cia_key_reset(void) { memset(kb_matrix, 0, sizeof(kb_matrix)); }

void cia_joy_set(unsigned port, uint8_t low_mask) {
    joy_pull[port & 1u] = (uint8_t)(low_mask & 0x1Fu);
}

void cia_restore_set(bool pressed) {
    bus_nmi_set(BUS_NMI_RESTORE, pressed);  // RESTORE is wired to NMI, not the matrix
}

// Advance a CIA's TOD by one tenth directly (the 10 Hz internal tick), for
// tests that need to reach BCD carries and the 12-hour wrap without running
// the phi2 divider for real time.
void cia_tod_tick(unsigned n) {
    tod_advance(&cia[n & 1u]);
}

// Simulate an IEC device pulling bus lines low (Phase 6 attaches here). The
// mask uses IEC_PULL_CLK/DATA/ATN; the C64 sees the wired-AND on CIA2 Port A.
void cia_iec_device_pull(uint8_t mask) {
    iec_dev_pull = (uint8_t)(mask & (IEC_PULL_CLK | IEC_PULL_DATA | IEC_PULL_ATN));
}

// ---- Lifecycle ------------------------------------------------------------

void cia_reset(void) {
    memset(cia, 0, sizeof(cia));
    memset(kb_matrix, 0, sizeof(kb_matrix));
    joy_pull[0] = 0;
    joy_pull[1] = 0;
    iec_dev_pull = 0;
    cia[1].nmi_source = BUS_NMI_CIA2;
    cia[0].ta.latch = cia[0].tb.latch = 0xFFFF;
    cia[1].ta.latch = cia[1].tb.latch = 0xFFFF;
    cia[0].ta.counter = cia[0].tb.counter = 0xFFFF;
    cia[1].ta.counter = cia[1].tb.counter = 0xFFFF;
    // 6526 reset clears the ICR (mask and flags), so no interrupt is asserted:
    // release this CIA's contribution to the wired-OR IRQ/NMI lines.
    bus_irq_set(BUS_IRQ_CIA1, false);
    bus_nmi_set(BUS_NMI_CIA2, false);
    bus_nmi_set(BUS_NMI_RESTORE, false);
}

void cia_init(void) { cia_reset(); }
