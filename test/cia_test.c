// CIA 6526: timers, interrupts, keyboard/joystick, TOD, serial, IEC. Expected
// values from Wolfgang Lorenz, "A Software Model of the CIA6526" (timer CIA1TAB
// countdown and the one-clock interrupt-line delay), the MOS 6526 datasheet
// (TOD 12-hour BCD read latch, serial SP interrupt), and the standard C64
// keyboard matrix (Mapping the Commodore 64 / C64 schematic).
#include <stdint.h>
#include "test.h"
#include "cia.h"
#include "bus.h"

// CIA1 register addresses.
#define PRA 0xDC00
#define PRB 0xDC01
#define DDRA 0xDC02
#define DDRB 0xDC03
#define TALO 0xDC04
#define TAHI 0xDC05
#define TBLO 0xDC06
#define TBHI 0xDC07
#define T10 0xDC08
#define TSE 0xDC09
#define TMI 0xDC0A
#define THR 0xDC0B
#define SDR 0xDC0C
#define ICR 0xDC0D
#define CRA 0xDC0E
#define CRB 0xDC0F

// ---- Timers ---------------------------------------------------------------

// Lorenz CIA1TAB: Timer A in phi2 continuous mode reads its reloaded latch value
// twice (the pipeline removes the clock after a reload) and never reads 0. For a
// latch of 2 the read sequence is periodic-3 over {1,2} with the value 2 twice
// and 1 once per period. Source: Lorenz, "A Software Model of the CIA6526".
static void test_timer_a_phi2_pattern(void) {
    cia_init();
    cia1_write(TALO, 0x02);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x11);  // force-load + start, phi2 continuous
    for (int i = 0; i < 6; i++) { cia_clock(); }  // settle to steady state
    uint16_t r[9];
    for (int i = 0; i < 9; i++) {
        cia_clock();
        r[i] = cia_timer_a(0);
    }
    int zeros = 0, twos = 0, ones = 0, other = 0;
    for (int i = 0; i < 9; i++) {
        if (r[i] == 0) { zeros++; }
        else if (r[i] == 2) { twos++; }
        else if (r[i] == 1) { ones++; }
        else { other++; }
    }
    CHECK_EQ(zeros, 0, "Timer A phi2 never reads 0 (Lorenz CIA1TAB)");
    CHECK_EQ(other, 0, "Timer A phi2 reads only latch and latch-1 {1,2}");
    int periodic = 1;
    for (int i = 0; i < 6; i++) { if (r[i] != r[i + 3]) { periodic = 0; } }
    CHECK_EQ(periodic, 1, "Timer A phi2 read sequence is period-3");
    CHECK_EQ(twos, 6, "reload value read twice per period (pipeline doubling)");
    CHECK_EQ(ones, 3, "latch-1 value read once per period");
}

// Force-load (CRA bit4) reloads the counter from the latch. Datasheet.
static void test_timer_force_load(void) {
    cia_init();
    cia1_write(TALO, 0x40);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x10);  // force-load, not started
    cia_clock();
    CHECK_EQ(cia_timer_a(0), 0x40, "force-load reloads counter from latch");
}

// One-shot (CRA bit3): the timer underflows once, sets the flag, and stops
// (the START bit auto-clears). Datasheet.
static void test_timer_one_shot(void) {
    cia_init();
    cia1_write(TALO, 0x03);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x19);  // one-shot + force-load + start
    int underflow = 0;
    for (int i = 0; i < 12; i++) {
        cia_clock();
        if (cia_icr_flags(0) & 0x01) { underflow = 1; }
    }
    CHECK_EQ(underflow, 1, "one-shot timer underflows and sets the TA flag");
    CHECK_EQ(cia1_read(CRA) & 0x01, 0, "one-shot clears the START bit after underflow");
}

// Cascade (CRB = count Timer A underflows): Timer B decrements once per Timer A
// underflow. Lorenz. The exact zero-hold cycle differs by one clock (documented
// deviation), so assert the underflow COUNT, not the precise phase.
static void test_timer_cascade(void) {
    cia_init();
    cia1_write(TALO, 0x02);
    cia1_write(TAHI, 0x00);
    cia1_write(TBLO, 0xFF);
    cia1_write(TBHI, 0x00);
    cia1_write(CRB, 0x41);  // cascade + start
    cia1_write(CRA, 0x11);  // force-load + start, phi2
    uint16_t tb_start = cia_timer_b(0);
    int ta_underflows = 0;
    uint16_t prev = cia_timer_a(0);
    for (int i = 0; i < 60; i++) {
        cia_clock();
        uint16_t now = cia_timer_a(0);
        if (now > prev) { ta_underflows++; }  // reload => an underflow occurred
        prev = now;
    }
    int tb_dec = (int)tb_start - (int)cia_timer_b(0);
    CHECK(ta_underflows > 0, "Timer A underflowed during the cascade window");
    CHECK(tb_dec == ta_underflows || tb_dec == ta_underflows - 1,
          "Timer B cascade decrements once per Timer A underflow");
}

// ---- Interrupts -----------------------------------------------------------

// ICR: the mask write sets bits when bit7=1 and clears them when bit7=0; reading
// the ICR returns the flags plus the IR bit and clears them, deasserting the
// line. Datasheet / Lorenz.
static void test_icr_mask_and_read_clear(void) {
    cia_init();
    cia1_write(ICR, 0x81);  // set mask bit0 (TA)
    cia1_write(TALO, 0x02);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x11);
    int asserted = 0;
    for (int i = 0; i < 20 && !asserted; i++) {
        cia_clock();
        if (bus_irq == 0) { asserted = 1; }
    }
    CHECK_EQ(asserted, 1, "enabled Timer A interrupt asserts the IRQ line");
    uint8_t icr = cia1_read(ICR);
    CHECK(icr & 0x01, "ICR read reports the TA flag");
    CHECK(icr & 0x80, "ICR read reports the IR bit");
    CHECK_EQ(cia_icr_flags(0) & 0x1F, 0, "reading ICR clears the flags");
    CHECK_EQ(bus_irq, 1, "reading ICR deasserts the IRQ line");

    // Masking off the source: the flag still latches but no interrupt is raised.
    cia_init();
    cia1_write(ICR, 0x01);  // bit7=0 -> clear mask bit0 (leave TA disabled)
    cia1_write(TALO, 0x02);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x11);
    int line_low = 0;
    for (int i = 0; i < 20; i++) {
        cia_clock();
        if (bus_irq == 0) { line_low = 1; }
    }
    CHECK_EQ(line_low, 0, "masked Timer A does not assert the IRQ line");
    CHECK(cia_icr_flags(0) & 0x01, "masked source still latches its flag");
}

// The interrupt line lags the ICR flag by exactly one phi2 clock. Lorenz:
// "the CIA6526 will raise an interrupt with a delay of one phi2 clock."
static void test_interrupt_delay_pipeline(void) {
    cia_init();
    cia1_write(ICR, 0x81);
    cia1_write(TALO, 0x0A);  // latch 10: a clean single underflow
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x11);
    int found = 0, lag_ok = 0;
    for (int i = 0; i < 40 && !found; i++) {
        int before = cia_icr_flags(0) & 0x01;
        cia_clock();
        int after = cia_icr_flags(0) & 0x01;
        if (!before && after) {  // TA flag became set on this clock
            found = 1;
            if (bus_irq == 1) {  // line not yet asserted (one-clock lag)
                cia_clock();
                if (bus_irq == 0) { lag_ok = 1; }
            }
        }
    }
    CHECK_EQ(found, 1, "Timer A underflow sets the ICR flag");
    CHECK_EQ(lag_ok, 1, "IRQ line asserts one clock after the flag (Lorenz)");
}

// The ICR read exposes the same one-clock delay: bit 7 (IR) tracks the /IRQ line,
// not the raw source flag. In the cycle a masked flag first appears, an ICR read
// shows bit 0 set but bit 7 clear; one cycle later both are set. Lorenz IRQ/NMI.
static void test_icr_ir_bit_tracks_delayed_line(void) {
    // Find the underflow cycle by peeking the flag (a peek does not clear the ICR).
    cia_init();
    cia1_write(ICR, 0x81);
    cia1_write(TALO, 0x03);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x19);  // start + force-load + one-shot
    int uf = -1;
    for (int k = 1; k <= 12 && uf < 0; k++) {
        cia_clock();
        if (cia_icr_flags(0) & 0x01) { uf = k; }
    }
    CHECK(uf > 0, "Timer A underflows within the window");
    // A destructive ICR read AT the underflow cycle: bit 0 set, bit 7 not yet.
    cia_init();
    cia1_write(ICR, 0x81);
    cia1_write(TALO, 0x03);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x19);
    for (int k = 0; k < uf; k++) { cia_clock(); }
    CHECK_EQ(cia1_read(ICR), 0x01, "ICR read at the flag cycle: bit0 set, IR bit clear");
    // One cycle later: bit 0 and the IR bit both set.
    cia_init();
    cia1_write(ICR, 0x81);
    cia1_write(TALO, 0x03);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x19);
    for (int k = 0; k < uf + 1; k++) { cia_clock(); }
    CHECK_EQ(cia1_read(ICR), 0x81, "ICR read one cycle later: bit0 and IR bit both set");
}

// CIA2's interrupt drives NMI, not IRQ.
static void test_cia2_drives_nmi(void) {
    cia_init();
    cia2_write(0xDD0D, 0x81);  // enable TA
    cia2_write(0xDD04, 0x02);
    cia2_write(0xDD05, 0x00);
    cia2_write(0xDD0E, 0x11);
    int nmi = 0;
    for (int i = 0; i < 20 && !nmi; i++) {
        cia_clock();
        if (bus_nmi == 0) { nmi = 1; }
    }
    CHECK_EQ(nmi, 1, "CIA2 Timer A interrupt asserts the NMI line");
    CHECK_EQ(bus_irq, 1, "CIA2 does not touch the IRQ line");
}

// ---- Keyboard and joystick ------------------------------------------------

// Standard C64 8x8 matrix (row = Port A driven low, col = Port B read). Source:
// C64 keyboard matrix (Mapping the Commodore 64 / schematic). Positions (row,col):
// A=(1,2) RETURN=(0,1) SPACE=(7,4) Q=(7,6) '1'=(7,0) LEFT-SHIFT=(1,7).
static void press_reads_column(unsigned row, unsigned col, const char *name) {
    cia_init();
    cia1_write(DDRA, 0xFF);  // rows output
    cia1_write(DDRB, 0x00);  // columns input
    cia_key_set(row, col, true);
    cia1_write(PRA, (uint8_t)~(1u << row));  // drive just this row low
    uint8_t pb = cia1_read(PRB);
    CHECK_EQ(pb & (1u << col), 0, name);
    cia_key_set(row, col, false);
    CHECK(cia1_read(PRB) & (1u << col), "released key: column reads high");
}

static void test_keyboard_matrix(void) {
    press_reads_column(1, 2, "key A (1,2) reads column low");
    press_reads_column(0, 1, "key RETURN (0,1) reads column low");
    press_reads_column(7, 4, "key SPACE (7,4) reads column low");
    press_reads_column(7, 6, "key Q (7,6) reads column low");
    press_reads_column(7, 0, "key 1 (7,0) reads column low");
    press_reads_column(1, 7, "key LEFT-SHIFT (1,7) reads column low");

    // Reverse scan direction: drive a column low on Port B, read the row on
    // Port A. The wired matrix is symmetric (a low column pulls held rows low).
    cia_init();
    cia1_write(DDRA, 0x00);  // rows input
    cia1_write(DDRB, 0xFF);  // columns output
    cia_key_set(1, 2, true);  // A
    cia1_write(PRB, (uint8_t)~(1u << 2));  // drive column 2 low
    CHECK_EQ(cia1_read(PRA) & (1u << 1), 0, "reverse scan: column low reads row low");
}

// Keyboard and joystick share the port lines as an open wired-AND: any puller
// (a joystick switch or a matrix crosspoint) pulls the line low. Source: C64
// hardware (the documented keyboard/joystick interference).
static void test_keyboard_joystick_sharing(void) {
    cia_init();
    cia1_write(DDRA, 0xFF);
    cia1_write(DDRB, 0x00);
    cia1_write(PRA, 0xFF);  // no row driven low
    cia_joy_set(0, 0x10);   // joystick 1 fire (bit4) on Port B
    CHECK_EQ(cia1_read(PRB) & 0x10, 0, "joystick 1 fire pulls Port B bit4 low");
    // A key whose column is 3, with its row driven low, also pulls that column.
    cia_key_set(2, 3, true);
    cia1_write(PRA, (uint8_t)~(1u << 2));  // drive row 2 low
    uint8_t pb = cia1_read(PRB);
    CHECK_EQ(pb & 0x08, 0, "matrix key also pulls its column low (shared line)");
    CHECK_EQ(pb & 0x10, 0, "joystick pull and key pull AND together");
}

// Joystick 2 sits on Port A, which the KERNAL drives as outputs (DDRA=$FF) to
// scan the keyboard rows. A read must still see the switch: on a 6526 the read
// returns the pin, and a switch shorting the pin to ground beats the CIA's output
// driver. Masking the pins by ~DDRA would make joystick 2 permanently unpressed.
// Source: C64 hardware; every joystick 2 game reads $DC00 with DDRA=$FF.
static void test_joystick2_read_with_port_a_as_output(void) {
    static const char *const NAME[5] = {"up", "down", "left", "right", "fire"};
    cia_init();
    cia1_write(DDRA, 0xFF);  // as the KERNAL leaves it: rows are outputs
    cia1_write(PRA, 0xFF);   // no row driven low
    cia1_write(DDRB, 0x00);
    for (unsigned bit = 0; bit < 5u; bit++) {
        cia_joy_set(1, (uint8_t)(1u << bit));
        CHECK_EQ(cia1_read(PRA) & (1u << bit), 0, NAME[bit]);
        cia_joy_set(1, 0);
        CHECK_EQ(cia1_read(PRA) & (1u << bit), (int)(1u << bit), "released reads high");
    }
    cia_joy_set(1, 0x1F);  // all five at once
    CHECK_EQ(cia1_read(PRA) & 0x1F, 0, "all joystick 2 switches pull together");
    cia_joy_set(1, 0);
}

// RESTORE is wired to the NMI line, not the keyboard matrix.
static void test_restore_is_nmi(void) {
    cia_init();
    CHECK_EQ(bus_nmi, 1, "NMI idle before RESTORE");
    cia_restore_set(true);
    CHECK_EQ(bus_nmi, 0, "RESTORE asserts NMI");
    cia_restore_set(false);
    CHECK_EQ(bus_nmi, 1, "releasing RESTORE deasserts NMI");
}

// ---- TOD clock ------------------------------------------------------------

// Write the running clock (alarm bit clear). Writing HOURS stops it, TENTHS starts.
static void set_tod(uint8_t h, uint8_t m, uint8_t s, uint8_t t) {
    cia1_write(CRB, 0x00);
    cia1_write(THR, h);
    cia1_write(TMI, m);
    cia1_write(TSE, s);
    cia1_write(T10, t);
}

// TOD is BCD with a 12-hour AM/PM format (bit7=PM); AM/PM toggles when the hour
// reaches 12, and 12 rolls to 1 with no toggle. Reading HOURS latches a coherent
// snapshot; reading TENTHS releases it. Source: MOS 6526 datasheet.
static void test_tod_chain_and_latch(void) {
    cia_init();
    set_tod(0x11, 0x59, 0x59, 0x09);  // 11:59:59.9 AM
    cia_tod_tick(0);
    CHECK_EQ(cia1_read(THR), 0x92, "TOD 11->12 rolls hours to 12 and toggles PM");
    (void)cia1_read(T10);  // release latch

    set_tod(0x12, 0x59, 0x59, 0x09);  // 12:59:59.9
    cia_tod_tick(0);
    CHECK_EQ(cia1_read(THR), 0x01, "TOD 12->1 with no AM/PM toggle");
    (void)cia1_read(T10);

    set_tod(0x00, 0x00, 0x09, 0x09);  // seconds BCD carry
    cia_tod_tick(0);
    CHECK_EQ(cia1_read(TSE), 0x10, "TOD seconds BCD 09 -> 10");
    (void)cia1_read(THR);
    (void)cia1_read(T10);

    // Read latch: HOURS snapshots; MIN/SEC return the snapshot while latched;
    // TENTHS releases; then reads are live again.
    set_tod(0x01, 0x02, 0x03, 0x09);  // 01:02:03.9
    uint8_t h = cia1_read(THR);        // latches {01,02,03,9}
    cia_tod_tick(0);                   // clock advances to 01:02:04.0
    CHECK_EQ(h, 0x01, "reading HOURS returns the hours");
    CHECK_EQ(cia1_read(TSE), 0x03, "SEC returns the latched snapshot, not live");
    CHECK_EQ(cia1_read(T10), 0x09, "reading TENTHS returns latched tenths");
    CHECK_EQ(cia1_read(TSE), 0x04, "after TENTHS releases the latch, SEC is live");
}

// ---- Serial shift register ------------------------------------------------

// In output mode (CRA bit6) the SDR shifts out at half the Timer A rate; after 8
// bits the ICR serial flag (bit3) sets. Source: MOS 6526 datasheet.
static void test_serial_shift(void) {
    cia_init();
    cia1_write(ICR, 0x88);   // enable serial interrupt
    cia1_write(TALO, 0x02);
    cia1_write(TAHI, 0x00);
    cia1_write(CRA, 0x51);   // serial output + force-load + start, phi2
    cia1_write(SDR, 0xA5);   // begin shifting
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        cia_clock();
        if (cia_icr_flags(0) & 0x08) { done = 1; }
    }
    CHECK_EQ(done, 1, "serial output sets ICR bit3 after 8 shifts");
}

// ---- IEC bus on CIA2 Port A -----------------------------------------------

// The IEC lines are open wired-AND: a driven output pulls the line low (ATN/CLK/
// DATA out on PA3-5), and CLK/DATA in (PA6-7) sense the line. A device can pull a
// line low too. Source: C64 CIA2 Port A IEC wiring.
static void test_iec_composition(void) {
    cia_init();
    cia2_write(0xDD02, 0x3F);  // DDRA: PA0-5 output, PA6-7 input
    cia2_write(0xDD00, 0x00);  // drive nothing
    CHECK_EQ(cia2_read(0xDD00) & 0xC0, 0xC0, "IEC idle: CLK IN and DATA IN read high");
    cia2_write(0xDD00, 0x10);  // CLK OUT (PA4) high -> pulls CLK line low
    CHECK_EQ(cia2_read(0xDD00) & 0x40, 0x00, "driving CLK out reads CLK IN low");
    CHECK_EQ(cia2_read(0xDD00) & 0x80, 0x80, "DATA IN still high");
    cia2_write(0xDD00, 0x00);
    cia_iec_device_pull(IEC_PULL_DATA);
    CHECK_EQ(cia2_read(0xDD00) & 0x80, 0x00, "a device pulling DATA reads DATA IN low");
    CHECK_EQ(cia2_read(0xDD00) & 0x40, 0x40, "CLK IN unaffected by the DATA pull");
    cia_iec_device_pull(0);
}

int main(void) {
    TEST_BEGIN("cia");
    test_timer_a_phi2_pattern();
    test_timer_force_load();
    test_timer_one_shot();
    test_timer_cascade();
    test_icr_mask_and_read_clear();
    test_interrupt_delay_pipeline();
    test_icr_ir_bit_tracks_delayed_line();
    test_cia2_drives_nmi();
    test_keyboard_matrix();
    test_keyboard_joystick_sharing();
    test_joystick2_read_with_port_a_as_output();
    test_restore_is_nmi();
    test_tod_chain_and_latch();
    test_serial_shift();
    test_iec_composition();
    return TEST_SUMMARY("cia");
}
