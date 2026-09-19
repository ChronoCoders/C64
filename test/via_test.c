// MOS 6522 VIA behaviour, sourced from the 6522 datasheet. This is the chip in
// isolation; test/drive_test.c covers what the real 1541 DOS does with it. Every
// expected value is a datasheet rule, not a reading of via.c.
#include <stdint.h>
#include "test.h"
#include "via.h"

// Register offsets (6522 datasheet, table 1).
#define R_ORB 0x0u
#define R_ORA 0x1u
#define R_DDRB 0x2u
#define R_DDRA 0x3u
#define R_T1CL 0x4u
#define R_T1CH 0x5u
#define R_T1LL 0x6u
#define R_T2CL 0x8u
#define R_T2CH 0x9u
#define R_ACR 0xBu
#define R_PCR 0xCu
#define R_IFR 0xDu
#define R_IER 0xEu

#define ACR_T2_PULSE 0x20u  // ACR bit 5 = 1: T2 counts PB6 pulses
#define PB6 0x40u           // pb_in bit 6 is the PB6 pin

// Port read composition: output bits (DDR=1) read the output register, input bits
// (DDR=0) read the pin level. Datasheet, port A/B description.
static void test_port_direction_and_read(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_DDRB, 0x0Fu);   // low nibble output, high nibble input
    via_write(&v, R_ORB, 0xA5u);    // drive 0x5 on the low nibble
    v.pb_in = 0x3Cu;                // pins: high nibble 0x3, low nibble ignored (output)
    CHECK_EQ(via_read(&v, R_ORB), (0xA5u & 0x0Fu) | (0x3Cu & 0xF0u),
             "port B read: output bits from ORB, input bits from the pins");
    CHECK_EQ(via_read(&v, R_DDRB), 0x0Fu, "DDRB reads back the direction");
}

// IER: writing with bit 7 set turns ON the given enable bits, bit 7 clear turns
// them OFF; reading returns the enables with bit 7 forced to 1. Datasheet, IER.
static void test_ier_set_clear_and_read(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_IER, 0x82u);  // bit7=1: enable CA1 (bit1)
    CHECK_EQ(via_read(&v, R_IER), 0x82u, "IER read: CA1 enabled, bit7 reads 1");
    via_write(&v, R_IER, 0xC0u);  // bit7=1: also enable T1 (bit6)
    CHECK_EQ(via_read(&v, R_IER), 0xC2u, "IER accumulates enables, bit7 = 1");
    via_write(&v, R_IER, 0x02u);  // bit7=0: clear CA1
    CHECK_EQ(via_read(&v, R_IER), 0xC0u, "IER bit7=0 write clears the named bits");
}

// IFR: bit 7 is a summary, set only when an enabled flag is set; writing a 1 to a
// flag bit clears it. Datasheet, IFR.
static void test_ifr_summary_and_clear(void) {
    VIA6522 v;
    via_reset(&v);
    via_set_ca1(&v, false);  // falling edge (PCR default) sets the CA1 flag
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, VIA_IRQ_CA1, "CA1 edge sets the CA1 flag");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_ANY, 0, "IFR bit7 clear while the flag is not enabled");
    CHECK_EQ(via_irq(&v) ? 1 : 0, 0, "no IRQ while the flag is disabled");
    via_write(&v, R_IER, 0x82u);  // enable CA1
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_ANY, VIA_IRQ_ANY, "IFR bit7 set once the flag is enabled");
    CHECK_EQ(via_irq(&v) ? 1 : 0, 1, "IRQ asserted when an enabled flag is set");
    via_write(&v, R_IFR, VIA_IRQ_CA1);  // write 1 to clear
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, 0, "writing 1 to a flag bit clears it");
    CHECK_EQ(via_irq(&v) ? 1 : 0, 0, "IRQ released after the flag is cleared");
}

// Timer 1 one-shot (ACR bits 6-7 = 0): loading T1C-H starts a countdown; the T1
// flag is clear until the counter underflows, then set once; reading T1C-L clears
// it and it does not re-arm. Datasheet, Timer 1 one-shot.
static void test_timer1_one_shot(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);   // T1 one-shot
    via_write(&v, R_T1LL, 0x05u);  // latch low = 5
    via_write(&v, R_T1CH, 0x00u);  // latch high = 0, load counter = 5, start
    CHECK_EQ(via_read(&v, R_T1CH), 0x00u, "T1 counter high loaded");
    CHECK_EQ(via_read(&v, R_T1CL), 0x05u, "T1 counter low loaded (= 5)");
    for (int i = 0; i < 3; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "T1 flag clear before underflow");
    for (int i = 0; i < 6; i++) { via_step(&v); }  // past the underflow
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, VIA_IRQ_T1, "T1 flag set at underflow");
    via_read(&v, R_T1CL);  // reading T1C-L clears the flag
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "reading T1C-L clears the T1 flag");
    for (int i = 0; i < 200; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "one-shot does not re-arm the T1 flag");
}

// Timer 1 free-run (ACR bit 6 = 1): the counter reloads from the latch at each
// underflow and the flag sets repeatedly. Datasheet, Timer 1 free-run.
static void test_timer1_free_run(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_ACR, 0x40u);   // T1 free-run
    via_write(&v, R_T1LL, 0x04u);
    via_write(&v, R_T1CH, 0x00u);  // counter = 4, free-run
    for (int i = 0; i < 8; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, VIA_IRQ_T1, "T1 flag set after the first underflow");
    via_write(&v, R_IFR, VIA_IRQ_T1);  // clear it
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "flag cleared");
    for (int i = 0; i < 8; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, VIA_IRQ_T1, "free-run re-sets the T1 flag (reloaded)");
}

// Timer 2 timed one-shot (ACR bit 5 = 0): counts down, sets the T2 flag once at
// underflow, reading T2C-L clears it. Datasheet, Timer 2.
static void test_timer2_one_shot(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);
    via_write(&v, R_T2CL, 0x06u);  // latch low = 6
    via_write(&v, R_T2CH, 0x00u);  // counter = 6, start
    for (int i = 0; i < 4; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "T2 flag clear before underflow");
    for (int i = 0; i < 6; i++) { via_step(&v); }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, VIA_IRQ_T2, "T2 flag set at underflow");
    via_read(&v, R_T2CL);
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "reading T2C-L clears the T2 flag");
}

// CA1 handshake interrupt: the PCR selects the active edge; the active edge sets
// the CA1 flag, and reading port A ($1) clears it. Datasheet, PCR and handshake.
static void test_ca1_edge_interrupt(void) {
    VIA6522 v;
    via_reset(&v);  // CA1 idles high
    via_write(&v, R_PCR, 0x00u);   // CA1 negative (falling) edge
    via_set_ca1(&v, true);         // no edge (stays high)
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, 0, "no CA1 flag without an edge");
    via_set_ca1(&v, false);        // falling edge
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, VIA_IRQ_CA1, "falling CA1 edge sets the flag");
    via_read(&v, R_ORA);           // reading port A clears CA1/CA2
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, 0, "reading port A clears the CA1 flag");

    via_reset(&v);
    via_write(&v, R_PCR, 0x01u);   // CA1 positive (rising) edge
    via_set_ca1(&v, false);
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, 0, "no flag on the wrong edge");
    via_set_ca1(&v, true);         // rising edge
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CA1, VIA_IRQ_CA1, "rising CA1 edge sets the flag");
}

// CB1 handshake interrupt, symmetric to CA1 with its edge selected by PCR bit 4;
// reading or writing port B ($0) clears the CB1 flag. Datasheet, PCR and port B.
static void test_cb1_edge_interrupt(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_PCR, 0x00u);   // CB1 negative (falling) edge
    via_set_cb1(&v, false);        // falling edge
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CB1, VIA_IRQ_CB1, "falling CB1 edge sets the flag");
    via_read(&v, R_ORB);           // reading port B clears CB1
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CB1, 0, "reading port B clears the CB1 flag");

    via_reset(&v);
    via_write(&v, R_PCR, 0x10u);   // CB1 positive (rising) edge (PCR bit 4)
    via_set_cb1(&v, false);
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CB1, 0, "no CB1 flag on the wrong edge");
    via_set_cb1(&v, true);
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_CB1, VIA_IRQ_CB1, "rising CB1 edge sets the flag");
}

// Timer 2 PB6 pulse counter (ACR bit 5 = 1): T2 counts high-to-low transitions on
// PB6 instead of phi2, underflows once like the one-shot, and does not re-arm until
// T2C-H is rewritten. Datasheet, Timer 2 pulse counting. pb_in bit 6 is PB6, high
// after reset; the fixture drives it explicitly so the first falling edge is clear.
static void test_timer2_pulse_count(void) {
    VIA6522 v;

    // Positive control: in timed mode the fixture drives the chip and T2 counts phi2.
    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);
    via_write(&v, R_T2CL, 0x10u);
    via_write(&v, R_T2CH, 0x00u);
    uint8_t pc0 = via_read(&v, R_T2CL);
    via_step(&v);
    uint8_t pc1 = via_read(&v, R_T2CL);
    CHECK_EQ(pc1, (uint8_t)(pc0 - 1u), "positive control: timed T2 decrements on via_step");

    // 1. Switching to pulse mode stops phi2 counting.
    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);
    via_write(&v, R_T2CL, 0x20u);
    via_write(&v, R_T2CH, 0x00u);
    v.pb_in |= PB6;
    uint8_t a0 = via_read(&v, R_T2CL);
    via_step(&v);
    uint8_t a1 = via_read(&v, R_T2CL);
    CHECK_EQ(a1, (uint8_t)(a0 - 1u), "1a: T2 counts phi2 in timed mode before the switch");
    via_write(&v, R_ACR, ACR_T2_PULSE);
    uint8_t b0 = via_read(&v, R_T2CL);
    for (int i = 0; i < 4; i++) { via_step(&v); }  // PB6 held high
    uint8_t b1 = via_read(&v, R_T2CL);
    CHECK_EQ(b1, b0, "1b: switching to pulse mode stops phi2 counting");

    // 2-5. Edge behaviour on one continuous counter.
    via_reset(&v);
    via_write(&v, R_ACR, ACR_T2_PULSE);
    via_write(&v, R_T2CL, 0x20u);
    via_write(&v, R_T2CH, 0x00u);
    v.pb_in |= PB6;
    via_step(&v);  // establish PB6 high, no edge
    uint8_t c_before = via_read(&v, R_T2CL);
    v.pb_in &= (uint8_t)~PB6;
    via_step(&v);
    uint8_t c_fall = via_read(&v, R_T2CL);
    CHECK_EQ(c_fall, (uint8_t)(c_before - 1u), "2: one PB6 falling edge decrements T2 by one");
    via_step(&v);  // PB6 still low
    uint8_t c_hold = via_read(&v, R_T2CL);
    CHECK_EQ(c_hold, c_fall, "3: holding PB6 low does not decrement again");
    v.pb_in |= PB6;
    via_step(&v);
    uint8_t c_rise = via_read(&v, R_T2CL);
    CHECK_EQ(c_rise, c_hold, "4: a rising PB6 edge does not decrement");
    v.pb_in &= (uint8_t)~PB6;
    via_step(&v);
    uint8_t c_fall2 = via_read(&v, R_T2CL);
    CHECK_EQ(c_fall2, (uint8_t)(c_rise - 1u), "5: the edge detector rearms; the next falling edge decrements");

    // 6. Underflow sets the T2 flag and raises IRQ when enabled.
    via_reset(&v);
    via_write(&v, R_ACR, ACR_T2_PULSE);
    via_write(&v, R_IER, (uint8_t)(0x80u | VIA_IRQ_T2));
    via_write(&v, R_T2CL, 0x02u);
    via_write(&v, R_T2CH, 0x00u);
    v.pb_in |= PB6;
    via_step(&v);
    for (int i = 0; i < 3; i++) {  // 2 -> 1 -> 0 -> underflow
        v.pb_in |= PB6; via_step(&v);
        v.pb_in &= (uint8_t)~PB6; via_step(&v);
    }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, VIA_IRQ_T2, "6: a PB6 underflow sets the T2 flag");
    CHECK(via_irq(&v), "6: a PB6 underflow raises IRQ when T2 is enabled in IER");

    // 7. One-shot in pulse mode: the timer does not re-arm itself. Clear the flag, then
    // drive a full second wrap (65536 falling edges) without rewriting T2C-H: the flag
    // must stay clear. Then writing T2C-H re-arms it and the next underflow sets it again.
    via_write(&v, R_IFR, VIA_IRQ_T2);
    for (int i = 0; i < 65536; i++) {
        v.pb_in |= PB6; via_step(&v);
        v.pb_in &= (uint8_t)~PB6; via_step(&v);
    }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "7: no re-arm; the second wrap does not re-set the T2 flag");
    CHECK(!via_irq(&v), "7: no re-arm; the second wrap does not raise IRQ");
    via_write(&v, R_T2CL, 0x02u);
    via_write(&v, R_T2CH, 0x00u);  // rewriting T2C-H re-arms the one-shot
    for (int i = 0; i < 3; i++) {
        v.pb_in |= PB6; via_step(&v);
        v.pb_in &= (uint8_t)~PB6; via_step(&v);
    }
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, VIA_IRQ_T2, "7: T2C-H re-arms; the next underflow sets the flag");
    CHECK(via_irq(&v), "7: T2C-H re-arms; the next underflow raises IRQ");
}

// T2 must count the effective Port B pin level, not the raw external input. With PB6
// configured as an output (DDRB bit 6 = 1), the pin follows ORB6 and the external
// input is masked out. pb_in bit 6 is held at the opposite of ORB6 throughout, so an
// implementation that read pb_in directly would see no transitions at all.
static void test_timer2_pulse_output_pin(void) {
    VIA6522 v;
    via_reset(&v);
    via_write(&v, R_ACR, ACR_T2_PULSE);
    via_write(&v, R_T2CL, 0x20u);
    via_write(&v, R_T2CH, 0x00u);
    via_write(&v, R_DDRB, PB6);   // PB6 output, so the pin follows ORB6
    via_write(&v, R_ORB, PB6);    // ORB6 high -> effective PB6 high
    v.pb_in &= (uint8_t)~PB6;     // external input PB6 low: opposite of ORB6, held constant

    uint8_t d0 = via_read(&v, R_T2CL);
    for (int i = 0; i < 3; i++) { via_step(&v); }  // ORB6 high, no edge
    uint8_t d1 = via_read(&v, R_T2CL);
    CHECK_EQ(d1, d0, "out-1: ORB6 high, no edge, T2 does not decrement");

    via_write(&v, R_ORB, 0x00u);  // ORB6 high -> low: effective PB6 falling edge
    via_step(&v);
    uint8_t d2 = via_read(&v, R_T2CL);
    CHECK_EQ(d2, (uint8_t)(d1 - 1u), "out-2: ORB6 high-to-low decrements T2 by one");

    via_step(&v);                 // ORB6 still low
    uint8_t d3 = via_read(&v, R_T2CL);
    CHECK_EQ(d3, d2, "out-3: ORB6 held low does not decrement again");

    via_write(&v, R_ORB, PB6);    // ORB6 low -> high
    via_step(&v);
    uint8_t d4 = via_read(&v, R_T2CL);
    CHECK_EQ(d4, d3, "out-4: ORB6 low-to-high does not decrement");

    via_write(&v, R_ORB, 0x00u);  // ORB6 high -> low again
    via_step(&v);
    uint8_t d5 = via_read(&v, R_T2CL);
    CHECK_EQ(d5, (uint8_t)(d4 - 1u), "out-5: the next ORB6 high-to-low decrements again");
}

// Underflow boundary contract: the timer interrupt flag is raised on the 0x0000 ->
// 0xFFFF underflow, one step after the counter reaches zero, not on 0x0001 -> 0x0000.
// The flag-clear-at-zero assertion is the pin; the counter values guard against a
// shift in either direction. The counter is read from the struct field; the flag is
// read only via the IFR register (R_IFR has no clearing side effect, unlike a T1C-L
// or T2C-L read). Timed and pulse T2 share one terminal-count rule, checked by arms
// 2 and 3 landing on the same sequence.
static void test_timer_underflow_boundary(void) {
    VIA6522 v;

    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);
    via_write(&v, R_T1LL, 0x02u);
    via_write(&v, R_T1CH, 0x00u);
    via_step(&v);
    CHECK_EQ(v.t1c, 1u, "T1 boundary: counter 2 -> 1");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "T1 boundary: flag clear at 1");
    via_step(&v);
    CHECK_EQ(v.t1c, 0u, "T1 boundary: counter 1 -> 0");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, 0, "T1 boundary: flag still clear at 0");
    via_step(&v);
    CHECK_EQ(v.t1c, 0xFFFFu, "T1 boundary: counter 0 -> FFFF");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T1, VIA_IRQ_T1, "T1 boundary: flag set at FFFF underflow");

    via_reset(&v);
    via_write(&v, R_ACR, 0x00u);
    via_write(&v, R_T2CL, 0x02u);
    via_write(&v, R_T2CH, 0x00u);
    via_step(&v);
    CHECK_EQ(v.t2c, 1u, "T2 timed boundary: counter 2 -> 1");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "T2 timed boundary: flag clear at 1");
    via_step(&v);
    CHECK_EQ(v.t2c, 0u, "T2 timed boundary: counter 1 -> 0");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "T2 timed boundary: flag still clear at 0");
    via_step(&v);
    CHECK_EQ(v.t2c, 0xFFFFu, "T2 timed boundary: counter 0 -> FFFF");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, VIA_IRQ_T2, "T2 timed boundary: flag set at FFFF underflow");

    via_reset(&v);
    via_write(&v, R_ACR, ACR_T2_PULSE);
    via_write(&v, R_T2CL, 0x02u);
    via_write(&v, R_T2CH, 0x00u);
    v.pb_in |= PB6;
    via_step(&v);  // establish PB6 high, no edge
    v.pb_in &= (uint8_t)~PB6; via_step(&v);
    CHECK_EQ(v.t2c, 1u, "T2 pulse boundary: counter 2 -> 1 on PB6 falling edge");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "T2 pulse boundary: flag clear at 1");
    v.pb_in |= PB6; via_step(&v);
    v.pb_in &= (uint8_t)~PB6; via_step(&v);
    CHECK_EQ(v.t2c, 0u, "T2 pulse boundary: counter 1 -> 0 on PB6 falling edge");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, 0, "T2 pulse boundary: flag still clear at 0");
    v.pb_in |= PB6; via_step(&v);
    v.pb_in &= (uint8_t)~PB6; via_step(&v);
    CHECK_EQ(v.t2c, 0xFFFFu, "T2 pulse boundary: counter 0 -> FFFF on PB6 falling edge");
    CHECK_EQ(via_read(&v, R_IFR) & VIA_IRQ_T2, VIA_IRQ_T2, "T2 pulse boundary: flag set at FFFF underflow");
}

int main(void) {
    TEST_BEGIN("via");
    test_port_direction_and_read();
    test_ier_set_clear_and_read();
    test_ifr_summary_and_clear();
    test_timer1_one_shot();
    test_timer1_free_run();
    test_timer2_one_shot();
    test_timer2_pulse_count();
    test_timer2_pulse_output_pin();
    test_timer_underflow_boundary();
    test_ca1_edge_interrupt();
    test_cb1_edge_interrupt();
    return TEST_SUMMARY("via");
}
