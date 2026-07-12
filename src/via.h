//! MOS 6522 VIA (Versatile Interface Adapter), clean-room from the 6522
//! datasheet. Two instances serve the 1541 drive (VIA1 at $1800, the serial bus;
//! VIA2 at $1C00, the mechanism); the drive wires the port pins and composes the
//! open-collector lines. This is NOT the 6526 CIA: no TOD, Timer 1 has a free-run
//! mode with PB7 toggling, Timer 2 is a one-shot or a PB6 pulse counter, the CA1
//! /CA2/CB1/CB2 handshake lines and the PCR/IFR/IER semantics are the VIA's own,
//! and the register layout differs. The core holds all register/timer/interrupt
//! state; the owner drives pa_in/pb_in and reads via_irq.
#ifndef VIA_H
#define VIA_H

#include <stdbool.h>
#include <stdint.h>

// IFR/IER bit positions (6522 datasheet). Bit 7 of the IFR is the IRQ summary
// (set when any enabled flag is set); it is not independently settable.
#define VIA_IRQ_CA2 0x01u
#define VIA_IRQ_CA1 0x02u
#define VIA_IRQ_SR 0x04u
#define VIA_IRQ_CB2 0x08u
#define VIA_IRQ_CB1 0x10u
#define VIA_IRQ_T2 0x20u
#define VIA_IRQ_T1 0x40u
#define VIA_IRQ_ANY 0x80u

typedef struct {
    uint8_t orb, ora;    // output registers (port B, port A)
    uint8_t ddrb, ddra;  // data direction: 1 = output
    uint8_t pa_in, pb_in;  // external input pin levels, driven by the owner

    uint16_t t1c, t1l;   // Timer 1 counter and 16-bit latch
    uint16_t t2c;        // Timer 2 counter
    uint8_t t2l_lo;      // Timer 2 has only a low-order latch
    bool t1_undf_pending;  // one-shot: flag already raised once this run
    bool t2_undf_pending;
    bool pb7;            // Timer 1 PB7 output (toggle in free-run, level in one-shot)

    uint8_t sr;          // shift register
    uint8_t acr;         // auxiliary control register
    uint8_t pcr;         // peripheral control register
    uint8_t ifr;         // interrupt flags (bit 7 = summary)
    uint8_t ier;         // interrupt enable (bit 7 reads 1)

    bool ca1, cb1;       // last sampled handshake input levels, for edge detection
} VIA6522;

void via_reset(VIA6522 *v);

// Register access, addr masked to 4 bits by the caller. Reads have side effects
// (they clear interrupt flags), so read is not const.
uint8_t via_read(VIA6522 *v, uint8_t reg);
void via_write(VIA6522 *v, uint8_t reg, uint8_t val);

// Advance the VIA one phi2 cycle: the timers count and raise their flags.
void via_step(VIA6522 *v);

// Drive the CA1 / CB1 handshake input to the given level. On the edge selected by
// the PCR the corresponding interrupt flag is set. The 1541 wires ATN to VIA1 CA1
// and BYTE READY to VIA2 CA1.
void via_set_ca1(VIA6522 *v, bool level);
void via_set_cb1(VIA6522 *v, bool level);

// True when the VIA is pulling the IRQ line low (any enabled flag set).
bool via_irq(const VIA6522 *v);

#endif // VIA_H
