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

int main(void) {
    TEST_BEGIN("via");
    test_port_direction_and_read();
    test_ier_set_clear_and_read();
    test_ifr_summary_and_clear();
    test_timer1_one_shot();
    test_timer1_free_run();
    test_timer2_one_shot();
    test_ca1_edge_interrupt();
    test_cb1_edge_interrupt();
    return TEST_SUMMARY("via");
}
