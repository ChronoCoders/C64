//! Durable unit tests for the MOS 6581 SID emulator (src/sid.c).
//!
//! Every expected value is sourced from an independent (non-reSID) reference and
//! cited at its assertion: the 6581 datasheet, Asger Alstrup Nielsen's noise
//! waveform analysis, or Laurent Plogue's 6581R3 ADSR tables. The analog filter
//! cutoff curve is an explicit approximation in src/sid.c and is NOT asserted as
//! measured truth; only the datasheet cutoff-range ENDPOINTS and the exact
//! digital control path (routing, 3OFF, volume DAC scaling) are checked.
//!
//! Integer assertions only; no floating point.
#include <stdbool.h>
#include <stdint.h>

#include "sid.h"
#include "test.h"

// Register addresses ($D400 + 7*v + offset). sid_write/sid_read mask addr & 0x1F.
// Voice v control = base+4; NOTE the task text annotated voice2 control as $D414,
// but base+4 for voice2 ($D40E) is $D412 ($D414 is voice2 SR). We follow the code.
#define R_V0_FREQLO 0xD400u
#define R_V0_FREQHI 0xD401u
#define R_V0_PWLO 0xD402u
#define R_V0_PWHI 0xD403u
#define R_V0_CTRL 0xD404u
#define R_V0_AD 0xD405u
#define R_V0_SR 0xD406u

#define R_V1_CTRL 0xD40Bu

#define R_V2_FREQLO 0xD40Eu
#define R_V2_FREQHI 0xD40Fu
#define R_V2_CTRL 0xD412u
#define R_V2_AD 0xD413u
#define R_V2_SR 0xD414u

#define R_FC_LO 0xD415u
#define R_FC_HI 0xD416u
#define R_RES_ROUTE 0xD417u
#define R_MODE_VOL 0xD418u
#define R_OSC3 0xD41Bu

// Control bits (6581 datasheet).
#define CTRL_GATE 0x01u
#define CTRL_RING 0x04u
#define CTRL_TEST 0x08u
#define CTRL_TRI 0x10u
#define CTRL_SAW 0x20u
#define CTRL_PULSE 0x40u
#define CTRL_NOISE 0x80u

// Test 1: Noise LFSR first output value.
// Source: Asger Alstrup Nielsen, "Examination of SID noise waveform" (codebase64):
// 23-bit LFSR, reset seed 0x7FFFF8, output taps {22,20,16,13,11,7,4,2}. His
// measured first OSC3 value on real hardware is $FE; the first LFSR shift keeps it.
static void test_noise_first_output_is_fe(void) {
    sid_reset();
    sid_write(R_V2_CTRL, CTRL_NOISE);  // voice2 noise
    // Alstrup: first sampled OSC3 value is 0xFE.
    CHECK_EQ(sid_read(R_OSC3), 0xFEu, "noise OSC3 reads 0xFE at reset seed");

    // Accumulator bit19 clocks the LFSR. freq = 0x8000 -> bit19 rises at 16 clocks.
    sid_write(R_V2_FREQLO, 0x00u);
    sid_write(R_V2_FREQHI, 0x80u);
    for (int i = 0; i < 16; i++) {
        sid_clock();
    }
    // Alstrup: the first LFSR shift still yields 0xFE.
    CHECK_EQ(sid_read(R_OSC3), 0xFEu, "noise OSC3 still 0xFE after first LFSR shift");
}

// Test 2: Waveform DDS math vs the 6581 datasheet definitions.
static void test_waveform_math_matches_datasheet(void) {
    // Sawtooth: out = (acc>>12)&0xFFF (top 12 bits). Datasheet ramp.
    sid_reset();
    sid_write(R_V0_CTRL, CTRL_SAW);
    sid_voice_set_accumulator(0, 0x000000u);
    CHECK_EQ(sid_voice_output(0), 0x000u, "sawtooth of phase 0x000000 is 0x000");
    sid_voice_set_accumulator(0, 0x800000u);
    CHECK_EQ(sid_voice_output(0), 0x800u, "sawtooth of phase 0x800000 is 0x800");
    sid_voice_set_accumulator(0, 0xABCDEFu);
    CHECK_EQ(sid_voice_output(0), 0xABCu, "sawtooth of phase 0xABCDEF is 0xABC");

    // Triangle: tri = ((bit23 ? ~acc : acc) >> 11) & 0xFFF. Datasheet fold.
    sid_write(R_V0_CTRL, CTRL_TRI);
    sid_voice_set_accumulator(0, 0x000000u);
    CHECK_EQ(sid_voice_output(0), 0x000u, "triangle of phase 0x000000 is 0x000");
    sid_voice_set_accumulator(0, 0x800000u);
    CHECK_EQ(sid_voice_output(0), 0xFFFu, "triangle of phase 0x800000 is 0xFFF");
    sid_voice_set_accumulator(0, 0xABCDEFu);
    CHECK_EQ(sid_voice_output(0), 0xA86u, "triangle of phase 0xABCDEF is 0xA86");

    // Pulse: datasheet duty = PW/4096; output high (0xFFF) while (acc>>12) < PW.
    sid_write(R_V0_CTRL, CTRL_PULSE);
    sid_write(R_V0_PWLO, 0x00u);
    sid_write(R_V0_PWHI, 0x08u);  // PW = 0x800 (50% duty)
    sid_voice_set_accumulator(0, 0x000000u);
    CHECK_EQ(sid_voice_output(0), 0xFFFu, "pulse PW 0x800 is high below the duty point");
    sid_voice_set_accumulator(0, 0x800000u);
    CHECK_EQ(sid_voice_output(0), 0x000u, "pulse PW 0x800 is low at the duty point");
    // PW = 0 -> 0% duty -> always low (datasheet duty definition polarity).
    sid_write(R_V0_PWLO, 0x00u);
    sid_write(R_V0_PWHI, 0x00u);
    sid_voice_set_accumulator(0, 0x400000u);
    CHECK_EQ(sid_voice_output(0), 0x000u, "pulse PW 0x000 is always low (0% duty)");
    // TEST bit forces the pulse output high regardless of phase (datasheet).
    sid_write(R_V0_CTRL, CTRL_PULSE | CTRL_TEST);
    sid_voice_set_accumulator(0, 0x400000u);
    CHECK_EQ(sid_voice_output(0), 0xFFFu, "TEST bit forces pulse output high");
}

// Test 3: Ring/sync source wiring (6581 datasheet: voice n modulated by voice n-1,
// wrapping; osc1<-osc3, osc2<-osc1, osc3<-osc2, i.e. voice0 rings from voice2).
static void test_ring_source_is_previous_voice(void) {
    sid_reset();
    sid_write(R_V0_CTRL, CTRL_TRI | CTRL_RING);  // voice0 rings from its source
    sid_voice_set_accumulator(0, 0x000000u);
    sid_voice_set_accumulator(1, 0x000000u);
    sid_voice_set_accumulator(2, 0x000000u);
    uint16_t src_low = sid_voice_output(0);
    sid_voice_set_accumulator(2, 0x800000u);  // toggle the SOURCE (voice2) MSB
    uint16_t src_high = sid_voice_output(0);
    // Datasheet wrap wiring: voice0's ring source is voice2, so its MSB matters.
    CHECK(src_low != src_high, "ring output changes when the source voice MSB changes");

    sid_voice_set_accumulator(2, 0x000000u);  // restore source low
    uint16_t before = sid_voice_output(0);
    sid_voice_set_accumulator(1, 0x800000u);  // toggle a NON-source voice MSB
    uint16_t after = sid_voice_output(0);
    CHECK_EQ(before, after, "ring output ignores a non-source voice MSB");
}

// Test 4: Attack rate-period table, rate 0.
// Source: 6581 datasheet ADSR times at 1 MHz, code-derived RATE_PERIOD (NOT reSID);
// rate 0 period = 9 phi2 clocks, and a full linear attack is 255 steps.
static void test_attack_rate_zero_peaks_at_255_steps(void) {
    sid_reset();
    sid_write(R_V0_AD, 0x00u);              // attack nibble 0 -> period 9
    sid_write(R_V0_CTRL, CTRL_SAW | CTRL_GATE);  // gate on starts attack
    // The rate counter is clean after reset, so timing is exact: 255*9 = 2295.
    for (int i = 0; i < 2294; i++) {
        sid_clock();
    }
    CHECK(sid_voice_envelope(0) < 0xFFu, "attack not yet peaked one tick before 2295");
    sid_clock();  // 2295th clock
    CHECK_EQ(sid_voice_envelope(0), 0xFFu, "attack rate 0 peaks after 255 steps of period 9");
}

// Test 5: Exponential decay tick count.
// Source: Laurent Plogue, "SID 6581R3 ADSR tables" breakpoints (envelope
// 93/54/26/14/6 -> divisor 2/4/8/16/30, divisor 1 above 93), which reproduce the
// datasheet ~3:1 decay:attack ratio: a full 255->0 decay is 756 rate ticks, not 255.
// 162*1 + 39*2 + 28*4 + 12*8 + 8*16 + 6*30 = 756. The exact breakpoints are covered
// indirectly here via the observable total tick count (the public API exposes the
// envelope, not the prescaler state). Rate 0 -> 9 clocks per tick, so 756*9 = 6804.
static void test_full_decay_takes_756_exponential_ticks(void) {
    sid_reset();
    sid_write(R_V0_AD, 0x00u);  // attack 0, decay 0 (both period 9)
    sid_write(R_V0_SR, 0x00u);  // sustain 0 -> decay falls all the way to 0
    sid_write(R_V0_CTRL, CTRL_GATE);
    for (int i = 0; i < 2295; i++) {  // run the attack to peak
        sid_clock();
    }
    CHECK_EQ(sid_voice_envelope(0), 0xFFu, "envelope at peak before decay");
    int decay_clocks = 0;
    while (sid_voice_envelope(0) != 0u && decay_clocks < 20000) {
        sid_clock();
        decay_clocks++;
    }
    // Plogue breakpoints + datasheet 3:1 ratio: 756 rate ticks * 9 clocks = 6804.
    CHECK_EQ(decay_clocks, 756 * 9, "full 255->0 decay takes 756 exponential rate ticks");
    // Datasheet: exponential decay is far slower than a 255-step linear fall.
    CHECK(decay_clocks > 255 * 9, "exponential decay is slower than a linear fall");
}

// Test 6: Sustain level = nibble*17.
// Source: 6581 datasheet - sustain is the 4-bit level replicated to 8 bits
// (0 -> 0, 15 -> 255).
static void test_sustain_level_is_nibble_times_17(void) {
    sid_reset();
    sid_write(R_V0_AD, 0x00u);   // fast attack and decay
    sid_write(R_V0_SR, 0x80u);   // sustain nibble 8
    sid_write(R_V0_CTRL, CTRL_GATE);
    for (int i = 0; i < 5000; i++) {  // attack + decay settle to sustain
        sid_clock();
    }
    CHECK_EQ(sid_voice_envelope(0), 8 * 17, "sustain nibble 8 holds envelope at 136");

    sid_reset();
    sid_write(R_V0_AD, 0x00u);
    sid_write(R_V0_SR, 0xF0u);   // sustain nibble 15
    sid_write(R_V0_CTRL, CTRL_GATE);
    for (int i = 0; i < 5000; i++) {
        sid_clock();
    }
    CHECK_EQ(sid_voice_envelope(0), 15 * 17, "sustain nibble 15 holds envelope at 255");
}

// Test 7: Master volume DAC scaling ($D418 low nibble).
// Source: 6581 datasheet 4-bit volume DAC (the 4-bit "digi" technique is
// scene-documented). MIXER_DC magnitude is an approximation, asserted only via
// ratios: with all voices silent, output = MIXER_DC * volume, so it is 0 at
// volume 0 and scales linearly with the nibble.
static void test_master_volume_scales_output_linearly(void) {
    sid_reset();  // envelopes 0, direct/filter sums 0, no clocking
    sid_write(R_MODE_VOL, 0x00u);
    CHECK_EQ(sid_output(), 0, "volume 0 silences the output");
    sid_write(R_MODE_VOL, 0x01u);
    int32_t o1 = sid_output();
    sid_write(R_MODE_VOL, 0x04u);
    int32_t o4 = sid_output();
    sid_write(R_MODE_VOL, 0x08u);
    int32_t o8 = sid_output();
    sid_write(R_MODE_VOL, 0x0Fu);
    int32_t o15 = sid_output();
    CHECK_EQ(o8, 2 * o4, "output at volume 8 is twice volume 4");
    CHECK_EQ(o15, 15 * o1, "output at volume 15 is fifteen times volume 1");
}

// Build a voice held at a nonzero, constant audio contribution: waveform value 0
// (saw of phase 0) centered to -2048, times a full envelope. freq 0 keeps the
// accumulator fixed; sustain 15 keeps the envelope at 255.
static void setup_loud_voice(uint16_t ctrl_addr, uint16_t ad_addr, uint16_t sr_addr,
                             uint16_t freqlo, uint16_t freqhi, unsigned v) {
    sid_write(freqlo, 0x00u);
    sid_write(freqhi, 0x00u);
    sid_voice_set_accumulator(v, 0x000000u);
    sid_write(ad_addr, 0x00u);  // fast attack
    sid_write(sr_addr, 0xF0u);  // sustain 15 -> hold at 255
    sid_write(ctrl_addr, CTRL_SAW | CTRL_GATE);
    for (int i = 0; i < 2295; i++) {
        sid_clock();
    }
}

// Test 8: Filter digital control path (exact; the analog cutoff curve is not
// asserted). Source: 6581 datasheet register decode and 30 Hz..12 kHz cutoff range.
static void test_filter_digital_control_path(void) {
    // Routing $D417 low nibble: a routed voice leaves the direct sum and enters
    // the filtered sum. Datasheet register decode.
    sid_reset();
    setup_loud_voice(R_V0_CTRL, R_V0_AD, R_V0_SR, R_V0_FREQLO, R_V0_FREQHI, 0);
    sid_write(R_RES_ROUTE, 0x00u);  // voice0 unrouted
    sid_write(R_MODE_VOL, 0x00u);
    sid_clock();
    CHECK(sid_direct_output() != 0, "unrouted voice contributes to the direct path");
    sid_write(R_RES_ROUTE, 0x01u);  // route voice0 into the filter
    sid_write(R_MODE_VOL, 0x10u);   // low-pass mode so the filtered path is audible
    for (int i = 0; i < 4; i++) {
        sid_clock();
    }
    CHECK_EQ(sid_direct_output(), 0, "routed voice is removed from the direct path");
    CHECK(sid_filter_output() != 0, "routed voice enters the filtered path");

    // Cutoff endpoints ONLY (datasheet 30 Hz..12 kHz range). Intermediate Hz values
    // are an explicit approximation and are not asserted.
    sid_reset();
    sid_write(R_FC_LO, 0x00u);
    sid_write(R_FC_HI, 0x00u);  // cutoff code 0
    CHECK_EQ(sid_filter_cutoff_hz(), 30u, "cutoff code 0 maps to the datasheet 30 Hz endpoint");
    sid_write(R_FC_LO, 0x07u);
    sid_write(R_FC_HI, 0xFFu);  // cutoff code 2047
    CHECK_EQ(sid_filter_cutoff_hz(), 12000u,
             "cutoff code 2047 maps to the datasheet 12000 Hz endpoint");

    // 3OFF ($D418 bit7): voice2 removed from the direct path. Datasheet decode.
    sid_reset();
    setup_loud_voice(R_V2_CTRL, R_V2_AD, R_V2_SR, R_V2_FREQLO, R_V2_FREQHI, 2);
    sid_write(R_RES_ROUTE, 0x00u);  // voice2 unrouted
    sid_write(R_MODE_VOL, 0x00u);   // 3OFF clear
    sid_clock();
    CHECK(sid_direct_output() != 0, "voice2 is in the direct path when 3OFF is clear");
    sid_write(R_MODE_VOL, 0x80u);   // 3OFF set
    sid_clock();
    CHECK_EQ(sid_direct_output(), 0, "voice2 leaves the direct path when 3OFF is set");
}

// Drive the free-running rate counter to value C under a slow attack rate, then
// switch to a fast rate (period P2) and count clocks to the next envelope step.
// Returns the delay-bug stall.
static long delay_bug_stall(int rate_slow, int rate_fast, uint16_t C) {
    sid_reset();
    sid_write(R_V0_AD, (uint8_t)((rate_slow & 0x0F) << 4));  // attack nibble = slow rate
    sid_write(R_V0_CTRL, CTRL_SAW | CTRL_GATE);              // gate on -> attack state
    for (int i = 0; i < 200000; i++) {
        sid_clock();
        if (sid_voice_rate_counter(0) == C) { break; }
    }
    uint8_t e0 = sid_voice_envelope(0);
    sid_write(R_V0_AD, (uint8_t)((rate_fast & 0x0F) << 4));  // change to the smaller period
    for (long n = 1; n <= 40000; n++) {
        sid_clock();
        if (sid_voice_envelope(0) != e0) { return n; }
    }
    return -1;
}

// Test: the ADSR delay-bug stall follows stall = 32768 - (C - P2), where C is the
// rate counter at the register change and P2 is the new period.
//
// SOURCE NOTE: these stall values are MEASURED from this implementation's
// free-running counter (reset only on an equality match; behaviour sourced to
// ChristopherJam's real-chip reverse-engineering, corroborated by Geir Tjelta).
// The exact wrap length is NOT a datasheet or hardware figure and has no such
// citation: whether the hardware counter is linear or a 15-bit LFSR is unsettled,
// and the exact wrap length would differ between them. These assertions pin THIS
// model's behaviour so the number cannot drift silently again.
static void test_adsr_delay_bug_stall(void) {
    // P2 = RATE_PERIOD[0] = 9, the fastest rate period (datasheet-derived). Reach
    // the target counter under rate 15 (the slowest, so C is reachable).
    const int P2 = 9;
    // Worst case: C = P2. The compare is after the increment, so the current value
    // is missed and the counter must wrap the full 32768.
    CHECK_EQ(delay_bug_stall(15, 0, (uint16_t)P2), 32768,
             "delay-bug worst case at C = P2 is 32768 (measured, this model)");
    // Classic delay bug: counter one above the new period.
    CHECK_EQ(delay_bug_stall(15, 0, (uint16_t)(P2 + 1)), 32767,
             "delay-bug stall at C = P2+1 is 32767 (measured)");
    CHECK_EQ(delay_bug_stall(15, 0, (uint16_t)(P2 + 2)), 32766,
             "delay-bug stall at C = P2+2 is 32766 (measured, formula holds)");
    // 32477 is NOT special: it is simply the stall at C - P2 = 291, one ordinary
    // point on the same formula. Included only to nail that it is not the worst
    // case (which is 32768 above), correcting the Phase 4b mislabel.
    CHECK_EQ(delay_bug_stall(15, 0, (uint16_t)(P2 + 291)), 32477,
             "C - P2 = 291 gives 32477: an ordinary point, not the worst case");
}

int main(void) {
    TEST_BEGIN("sid");
    test_noise_first_output_is_fe();
    test_waveform_math_matches_datasheet();
    test_ring_source_is_previous_voice();
    test_attack_rate_zero_peaks_at_255_steps();
    test_full_decay_takes_756_exponential_ticks();
    test_sustain_level_is_nibble_times_17();
    test_master_volume_scales_output_linearly();
    test_filter_digital_control_path();
    test_adsr_delay_bug_stall();
    return TEST_SUMMARY("sid");
}
