#include "sid.h"

#include <string.h>

// MOS 6581 oscillator + waveform core (Phase 4a). Integer/fixed-point only.
//
// Hardware facts below are taken from independent (non-reSID) analysis:
//   Noise LFSR: Asger Alstrup Nielsen, "Examination of SID noise waveform"
//   (codebase64), reverse-engineered by sampling $D41B on a real 6581 via REU.
//   That work establishes: a 23-bit left-shifting LFSR; feedback bit0 = bit22
//   EOR bit17; a measured 2^23-1 period; the 8 output bits taken from LFSR bits
//   22,20,16,13,11,7,4,2; and (from its timing table: one new value every 32
//   cycles at frequency $8000 = a 2^20 accumulator period) the LFSR is clocked
//   by accumulator bit 19. The reset seed 0x7FFFF8 is Asger's measured value.
//   The oxyron.de register table lists the output taps offset by -2; since it
//   agrees on the feedback taps (same bit-numbering origin), that offset is a
//   transcription discrepancy, not a convention difference, and is not used.
//   Waveform formulas (triangle fold, sawtooth, pulse compare) follow the 6581
//   datasheet duty-cycle relationship and standard DDS description.
// invariant: combined waveforms use the documented wired-AND model (the shared
//   DAC output lines pull each other, approximated as a bitwise AND of the
//   selected generators). The exact 6581 analog bit-bleed (measured per-chip)
//   is not reproduced here and can be validated against real hardware later.

#define CTRL_GATE 0x01u
#define CTRL_SYNC 0x02u
#define CTRL_RING 0x04u
#define CTRL_TEST 0x08u
#define CTRL_TRI 0x10u
#define CTRL_SAW 0x20u
#define CTRL_PULSE 0x40u
#define CTRL_NOISE 0x80u

#define REG_FREQLO(v) (7u * (v) + 0u)
#define REG_FREQHI(v) (7u * (v) + 1u)
#define REG_PWLO(v) (7u * (v) + 2u)
#define REG_PWHI(v) (7u * (v) + 3u)
#define REG_CTRL(v) (7u * (v) + 4u)

// Ring/sync source is the previous voice, per the 6581 datasheet (osc1<-osc3,
// osc2<-osc1, osc3<-osc2, i.e. voice 0 <- 2, 1 <- 0, 2 <- 1). Independent of
// reSID.
#define SYNC_SRC(v) (((v) + 2u) % 3u)

#define ACC_MASK 0x00FFFFFFu
#define ACC_MSB 0x00800000u
#define LFSR_MASK 0x007FFFFFu
#define LFSR_SEED 0x007FFFF8u

// Noise output bit j (j=0..7) is LFSR bit NOISE_LFSR_TAP[j], placed at waveform
// bit 4+j (the 8 noise bits occupy the top 8 of the 12-bit waveform value).
static const uint8_t NOISE_LFSR_TAP[8] = {2u, 4u, 7u, 11u, 13u, 16u, 20u, 22u};

typedef struct {
    uint32_t accumulator;  // 24-bit phase
    uint32_t noise_lfsr;   // 23-bit LFSR
    bool prev_bit19;       // previous accumulator bit 19, for the noise clock edge
    bool overflow;         // accumulator wrapped on the last clock (hard-sync trigger)
} Voice;

static Voice voice[3];
static uint8_t reg[0x20];  // $D400-$D41F register file (writes land in 0x00-0x18)

// ---- ADSR envelope (Phase 4b) ---------------------------------------------
//
// Sources (independent of reSID):
//   Rate periods derive from the MOS 6581 datasheet attack/decay/release times
//   (stated on a 1.0 MHz phi2 basis; a full 255-step attack at period P takes
//   255*P clocks). The rate counter is free-running: it is reset only on an
//   equality match with the current period, never on a gate change or a
//   register write (ChristopherJam's real-chip reverse-engineering on CSDb,
//   corroborated by Geir Tjelta). Both the gate phase carry-over and the ADSR
//   delay bug follow from that one rule: a change to a smaller period leaves the
//   counter above the new value, so it wraps before matching again. The stall is
//   32768 - (C - P2) for counter C and new period P2, worst case 32768 at C = P2
//   (the compare is after the increment, so the current value is missed). That
//   figure is measured from this implementation (see the delay-bug test), not a
//   datasheet or hardware value. Whether the hardware counter is a linear counter
//   or a 15-bit LFSR is not settled by the independent sources: the behavior
//   class is identical and only the exact wrap length would differ (a 15-bit LFSR
//   has period 32767), so this is a bounded invariant, not a proven fact about
//   the chip. Every rate period fits 15 bits; the datasheet's slowest attack
//   (8 s) gives ~31250, inside that. Exponential decay/release breakpoints (envelope 93/54/26/14/6
//   -> divider 2/4/8/16/30, and 1 above 93) are the measured 6581R3 values
//   reported by Laurent Plogue, "SID 6581R3 ADSR tables, up close"; they also
//   reproduce the datasheet's 3:1 decay-to-attack ratio (a full 255->0 decay is
//   756 rate ticks versus 255 for attack). reSID was not consulted.
// invariant: the datasheet high-rate times are its own nominal figures; some
//   emulators use longer measured high-rate periods. The values here reproduce
//   the datasheet and stay within the 15-bit rate counter modelled here.

#define REG_AD(v) (7u * (v) + 5u)  // attack (high nibble) / decay (low nibble)
#define REG_SR(v) (7u * (v) + 6u)  // sustain (high nibble) / release (low nibble)
#define ENV_RATE_MASK 0x7FFFu      // 15-bit rate counter

// Rate counter period per 4-bit rate value, in envelope-clock (phi2) units.
static const uint16_t RATE_PERIOD[16] = {
    9,   32,   63,   95,   149,  220,   267,   313,
    392, 977,  1954, 3137, 3922, 11765, 19531, 31250,
};

typedef enum { ENV_ATTACK = 0, ENV_DECAY = 1, ENV_RELEASE = 2 } EnvState;

typedef struct {
    uint8_t envelope;       // 8-bit output level
    uint16_t rate_counter;  // 15-bit rate counter (exact-equality compare)
    uint8_t exp_counter;    // exponential prescaler counter
    uint8_t exp_period;     // current exponential divider (1,2,4,8,16,30)
    EnvState state;
} Env;

static Env env[3];

// ---- Filter (Phase 4c) ----------------------------------------------------
//
// Multimode state-variable filter; the 6581 is a two-integrator-loop design, so
// one shared SVF fed by the sum of routed voices produces low/band/high-pass
// outputs, and $D418 selects which are summed (modes combine, e.g. LP+HP=notch).
// The digital control path (routing $D417, mode/volume $D418, cutoff/resonance
// decode, which modes sum) is exact. The analog response is an approximate
// fixed-point model, not bit-exact. Datasheet-sourced facts (independent of
// reSID): the cutoff range 30 Hz..~12 kHz (2200 pF caps), the register layout,
// and the resonance range (linear 0..15) are from the MOS 6581 datasheet.
// invariant: the cutoff register->Hz map is a PLAUSIBLE-SHAPE APPROXIMATION, not
//   a measured curve. The datasheet idealizes FC as linear; the real 6581 is
//   strongly non-linear (nearly closed at low FC, then a steep rise) and varies
//   widely chip to chip. The cubic here reproduces that KNOWN GENERAL SHAPE over
//   the datasheet endpoints. It is NOT fitted to any measured per-chip data and
//   is NOT derived from reSID/reSIDfp; no independent numeric cutoff-vs-register
//   dataset was available clean-room. Treat cutoff frequency as approximate.
// invariant: the resonance nibble maps to a modest Q range (~0.7 at res 0 to
//   ~2.0 at res 15). The datasheet gives only "linear 0..15"; the Q values are a
//   plausible choice for the 6581's mild resonance, not measured.
// invariant: the 6581 analog distortion / component non-linearity ("grit") is
//   approximated by the linear SVF, not modeled at the transistor level.
// invariant: the per-voice DC offset and mixer non-linearity are not modelled;
//   voices are centered on 0x800 here.
//
// $D417 low nibble routes voices/external into the filter (bit v -> voice v,
// bit 3 -> external in). $D418 bits 4/5/6 select LP/BP/HP, bit 7 cuts voice 3
// from the direct path, low nibble is master volume.
#define MODE_LP 0x10u
#define MODE_BP 0x20u
#define MODE_HP 0x40u
#define MODE_3OFF 0x80u

#define FILT_COEF_SHIFT 24      // cutoff/resonance coefficients are Q24
#define FILT_STATE_SHIFT 6      // extra fractional bits on the integrator states
#define FILT_CUTOFF_MIN 30      // Hz at FC=0 (datasheet)
#define FILT_CUTOFF_SPAN 11970  // Hz added by FC=2047 -> ~12 kHz (datasheet)
#define FILT_FC_MAX 2047u
// f coefficient (Q24) per Hz: 2*pi*2^24 / PAL phi2 (985248); sin(x)~x for fc<<fs.
#define FILT_HZ_TO_COEF 107
// Resonance damping 1/Q (Q24): ~1.4 (res 0) down to ~0.5 (res 15), a modest peak.
#define FILT_Q_BASE 23488102
#define FILT_Q_STEP 1006633

static struct {
    int32_t lp;      // low-pass integrator state (scaled by FILT_STATE_SHIFT)
    int32_t bp;      // band-pass integrator state
    int32_t coef_f;  // cutoff coefficient (Q24)
    int32_t coef_q;  // resonance damping = 1/Q (Q24)
} filt;
static int32_t filter_out;  // filtered path output (signal units)
static int32_t direct_out;  // unrouted voices sum (signal units)

// ---- Audio output / resampling (Phase 4d) ---------------------------------
//
// The SID is mono. sid_clock() (driven at phi2 by the machine loop) box-averages
// the phi2-rate mix down to the host rate by Bresenham decimation, DC-blocks it
// (AC-coupling), and pushes 16-bit samples into a ring the host drains.
// Producer (sid_clock) and consumer (sid_audio_read) are single each; the host
// uses SDL's queue API from the same thread as the loop, so no locking is
// needed. Resampling and the mix are fixed-point; no floating point.
// invariant: box-average decimation is only first-order anti-aliasing; strong
//   SID high harmonics still alias somewhat (a polyphase FIR would improve it).
#define SID_PHI2_HZ 985248u   // PAL phi2, the resampling reference rate
#define AUDIO_RATE_HZ 44100u  // host output rate
#define AUDIO_OUT_SHIFT 10    // mix -> 16-bit output scale
#define AUDIO_DC_SHIFT 11     // one-pole DC-blocker time constant (~3 Hz)
#define AUDIO_RING_SIZE 8192u
#define AUDIO_RING_MASK (AUDIO_RING_SIZE - 1u)
// 6581 mixer DC reference, scaled by master volume so volume-register writes
// step the output. The volume register driving the output DAC is described in
// the 6581 datasheet; the 4-bit $D418 sample ("digi") technique built on it is
// widely documented in the C64 scene. Independent of reSID.
// invariant: the magnitude is a plausible approximation, not a measured 6581
//   offset. The DC blocker AC-couples the output (keeping digi transients,
//   removing steady DC); per-voice waveform DAC offset and mixer non-linearity
//   are not modelled. A full 4-bit digi stream written to $D418 does play (each
//   write sets a proportional output level, resampled and AC-coupled).
#define MIXER_DC 524288

static bool audio_on;
static int16_t audio_ring[AUDIO_RING_SIZE];
static unsigned audio_head;  // producer index
static unsigned audio_tail;  // consumer index
static int64_t rs_sum;       // box-average accumulator
static int32_t rs_count;
static uint32_t rs_phase;    // Bresenham resample phase
static int64_t dc_lp;        // DC-blocker lowpass estimate

// ---- Waveform generators (12-bit) -----------------------------------------

static uint16_t wave_triangle(unsigned v) {
    uint32_t acc = voice[v].accumulator;
    uint32_t msb = acc;
    if (reg[REG_CTRL(v)] & CTRL_RING) {
        msb = acc ^ voice[SYNC_SRC(v)].accumulator;  // ring mod substitutes the MSB
    }
    uint32_t bits = (msb & ACC_MSB) ? ~acc : acc;  // fold the ramp into a triangle
    return (uint16_t)((bits >> 11) & 0x0FFFu);
}

static uint16_t wave_sawtooth(unsigned v) {
    return (uint16_t)((voice[v].accumulator >> 12) & 0x0FFFu);
}

static uint16_t wave_pulse(unsigned v) {
    if (reg[REG_CTRL(v)] & CTRL_TEST) {
        return 0x0FFFu;  // TEST forces the pulse output high
    }
    uint16_t pw =
        (uint16_t)(((uint16_t)(reg[REG_PWHI(v)] & 0x0Fu) << 8) | reg[REG_PWLO(v)]);
    uint16_t top12 = (uint16_t)((voice[v].accumulator >> 12) & 0x0FFFu);
    // 6581 datasheet: high fraction (duty) = PW / 4096, so high while top12 < PW.
    return (top12 < pw) ? 0x0FFFu : 0x0000u;
}

static uint16_t wave_noise(unsigned v) {
    uint32_t l = voice[v].noise_lfsr;
    uint16_t o = 0;
    for (unsigned j = 0; j < 8; j++) {
        o |= (uint16_t)(((l >> NOISE_LFSR_TAP[j]) & 1u) << (4u + j));
    }
    return o;
}

// AND of the enabled non-noise generators (0x0FFF if none are enabled).
static uint16_t wave_non_noise(unsigned v) {
    uint8_t ctrl = reg[REG_CTRL(v)];
    uint16_t out = 0x0FFFu;
    if (ctrl & CTRL_TRI) {
        out &= wave_triangle(v);
    }
    if (ctrl & CTRL_SAW) {
        out &= wave_sawtooth(v);
    }
    if (ctrl & CTRL_PULSE) {
        out &= wave_pulse(v);
    }
    return out;
}

// Full 12-bit waveform value for a voice, combining all selected waveforms with
// the wired-AND model. No waveform selected reads as 0.
static uint16_t voice_output(unsigned v) {
    uint8_t ctrl = reg[REG_CTRL(v)];
    if (!(ctrl & (CTRL_TRI | CTRL_SAW | CTRL_PULSE | CTRL_NOISE))) {
        return 0;
    }
    uint16_t out = wave_non_noise(v);
    if (ctrl & CTRL_NOISE) {
        out &= wave_noise(v);
    }
    return out;
}

// ---- Oscillator + noise clocking ------------------------------------------

static void noise_clock(unsigned v) {
    uint32_t l = voice[v].noise_lfsr;
    uint32_t bit0 = ((l >> 22) ^ (l >> 17)) & 1u;
    l = ((l << 1) | bit0) & LFSR_MASK;
    // 6581 combined-waveform noise clearing: when noise is selected alongside
    // another waveform, the shared output lines pull the tapped LFSR bits toward
    // 0, eventually silencing the noise until TEST/reset. Model it by clearing
    // each tapped bit whose combined non-noise output bit is 0.
    uint8_t ctrl = reg[REG_CTRL(v)];
    if (ctrl & (CTRL_TRI | CTRL_SAW | CTRL_PULSE)) {
        uint16_t other = wave_non_noise(v);
        for (unsigned j = 0; j < 8; j++) {
            if (!((other >> (4u + j)) & 1u)) {
                l &= ~(1u << NOISE_LFSR_TAP[j]);
            }
        }
    }
    voice[v].noise_lfsr = l;
}

// ---- ADSR envelope generator ----------------------------------------------

static uint8_t env_rate_value(unsigned v) {
    switch (env[v].state) {
        case ENV_ATTACK:
            return (uint8_t)(reg[REG_AD(v)] >> 4);
        case ENV_DECAY:
            return (uint8_t)(reg[REG_AD(v)] & 0x0Fu);
        default:
            return (uint8_t)(reg[REG_SR(v)] & 0x0Fu);
    }
}

static uint8_t env_sustain_level(unsigned v) {
    uint8_t s = (uint8_t)(reg[REG_SR(v)] >> 4);
    return (uint8_t)(s * 17u);  // nibble*17: 0->0, 15->255
}

static void envelope_clock(unsigned v) {
    Env *e = &env[v];
    // Free-running 15-bit rate counter, reset only on an exact-equality match
    // with the period (not on gate or register writes): the source of the ADSR
    // delay bug. After a change to a smaller period the stall is 32768 - (C - P2),
    // worst case 32768 at C = P2 (compare is after the increment). Measured from
    // this model (see the delay-bug test); linear vs 15-bit LFSR is unsettled, so
    // the exact wrap length is a bounded invariant, not a proven fact.
    e->rate_counter = (uint16_t)((e->rate_counter + 1u) & ENV_RATE_MASK);
    if (e->rate_counter != RATE_PERIOD[env_rate_value(v)]) {
        return;
    }
    e->rate_counter = 0;

    if (e->state == ENV_ATTACK) {
        if (e->envelope < 0xFFu) {
            e->envelope++;  // linear rise
        }
        if (e->envelope == 0xFFu) {
            e->state = ENV_DECAY;
            e->exp_counter = 0;
            e->exp_period = 1;
        }
        return;
    }

    // Decay and release: exponential via the prescaler counter.
    e->exp_counter++;
    if (e->exp_counter != e->exp_period) {
        return;
    }
    e->exp_counter = 0;
    uint8_t target = (e->state == ENV_DECAY) ? env_sustain_level(v) : 0u;
    // Exact-equality hold (decay stops at sustain, release at 0); this also gives
    // the documented comparator behavior when sustain is changed mid-decay.
    if (e->envelope != target && e->envelope != 0u) {
        e->envelope--;
        switch (e->envelope) {  // documented 6581 exponential breakpoints
            case 93: e->exp_period = 2; break;
            case 54: e->exp_period = 4; break;
            case 26: e->exp_period = 8; break;
            case 14: e->exp_period = 16; break;
            case 6:  e->exp_period = 30; break;
            default: break;
        }
    }
}

// ---- Filter (state-variable, fixed-point) ---------------------------------

static int32_t voice_audio(unsigned v) {
    int32_t wf = (int32_t)voice_output(v) - 2048;  // center the 12-bit waveform
    return wf * (int32_t)env[v].envelope;           // apply the 8-bit envelope
}

// Cutoff curve: a cubic in the 11-bit register over the datasheet 30 Hz..12 kHz
// range, shaped to the 6581's known general behavior (a low plateau at small FC,
// then a steep rise near the top). See the invariant in the filter section: this
// is a plausible-shape approximation, not fitted to measured data.
static uint32_t filter_fc_to_hz(uint32_t fc) {
    return (uint32_t)FILT_CUTOFF_MIN +
           (uint32_t)(((uint64_t)FILT_CUTOFF_SPAN * fc * fc * fc) /
                      ((uint64_t)FILT_FC_MAX * FILT_FC_MAX * FILT_FC_MAX));
}

static void filter_update_cutoff(void) {
    uint32_t fc = ((uint32_t)reg[0x16] << 3) | (reg[0x15] & 0x07u);  // 0..2047
    filt.coef_f = (int32_t)(filter_fc_to_hz(fc) * (uint32_t)FILT_HZ_TO_COEF);
}

static void filter_update_res(void) {
    uint32_t res = (uint32_t)(reg[0x17] >> 4);  // 0..15
    filt.coef_q = FILT_Q_BASE - (int32_t)(res * (uint32_t)FILT_Q_STEP);
}

// One SVF step (Chamberlin form) over the sum of routed voices.
static void filter_clock(void) {
    uint8_t routing = (uint8_t)(reg[0x17] & 0x0Fu);
    uint8_t mode = reg[0x18];
    int32_t routed = 0;
    int32_t direct = 0;
    for (unsigned v = 0; v < 3; v++) {
        int32_t s = voice_audio(v);
        if (routing & (1u << v)) {
            routed += s;
        } else if (!(v == 2 && (mode & MODE_3OFF))) {
            direct += s;  // voice 3 can be cut from the direct path (3OFF)
        }
    }
    int32_t in = routed << FILT_STATE_SHIFT;
    int32_t lp = filt.lp;
    int32_t bp = filt.bp;
    lp += (int32_t)(((int64_t)filt.coef_f * bp) >> FILT_COEF_SHIFT);
    int32_t hp = in - lp - (int32_t)(((int64_t)filt.coef_q * bp) >> FILT_COEF_SHIFT);
    bp += (int32_t)(((int64_t)filt.coef_f * hp) >> FILT_COEF_SHIFT);
    filt.lp = lp;
    filt.bp = bp;
    int32_t fout = 0;
    if (mode & MODE_LP) {
        fout += lp;
    }
    if (mode & MODE_BP) {
        fout += bp;
    }
    if (mode & MODE_HP) {
        fout += hp;
    }
    filter_out = fout >> FILT_STATE_SHIFT;
    direct_out = direct;
}

void sid_clock(void) {
    bool msb_rose[3];
    // Pass 1: advance oscillators; detect each MSB 0->1 (the hard-sync trigger,
    // per the 6581 datasheet: sync resets a voice when the source MSB increases).
    for (unsigned v = 0; v < 3; v++) {
        bool old_msb = (voice[v].accumulator & ACC_MSB) != 0;
        if (reg[REG_CTRL(v)] & CTRL_TEST) {
            voice[v].accumulator = 0;  // held in reset while TEST is set
            voice[v].overflow = false;
        } else {
            uint32_t freq =
                (uint32_t)reg[REG_FREQLO(v)] | ((uint32_t)reg[REG_FREQHI(v)] << 8);
            uint32_t sum = voice[v].accumulator + freq;
            voice[v].overflow = sum > ACC_MASK;
            voice[v].accumulator = sum & ACC_MASK;
        }
        bool new_msb = (voice[v].accumulator & ACC_MSB) != 0;
        msb_rose[v] = new_msb && !old_msb;
    }
    // Pass 2: hard sync resets voice v when its source (voice n-1) MSB rose.
    for (unsigned v = 0; v < 3; v++) {
        if ((reg[REG_CTRL(v)] & CTRL_SYNC) && msb_rose[SYNC_SRC(v)]) {
            voice[v].accumulator = 0;
        }
    }
    // Pass 3: noise LFSR (post-sync accumulator) and envelope.
    for (unsigned v = 0; v < 3; v++) {
        if (reg[REG_CTRL(v)] & CTRL_TEST) {
            voice[v].prev_bit19 = false;
        } else {
            bool bit19 = (voice[v].accumulator >> 19) & 1u;
            if (bit19 && !voice[v].prev_bit19) {  // rising edge clocks the LFSR
                noise_clock(v);
            }
            voice[v].prev_bit19 = bit19;
        }
        envelope_clock(v);  // envelope runs independently of TEST
    }
    filter_clock();

    // Pass 4: resample the phi2-rate mix to the host rate (box average +
    // Bresenham decimation), DC-block, and push a 16-bit sample.
    if (audio_on) {
        rs_sum += sid_output();
        rs_count++;
        rs_phase += AUDIO_RATE_HZ;
        if (rs_phase >= SID_PHI2_HZ) {
            rs_phase -= SID_PHI2_HZ;
            int32_t avg = (int32_t)(rs_sum / rs_count);
            rs_sum = 0;
            rs_count = 0;
            dc_lp += avg - (int32_t)(dc_lp >> AUDIO_DC_SHIFT);
            int32_t hp = avg - (int32_t)(dc_lp >> AUDIO_DC_SHIFT);
            int32_t o = hp >> AUDIO_OUT_SHIFT;
            if (o > 32767) {
                o = 32767;
            } else if (o < -32768) {
                o = -32768;
            }
            unsigned next = (audio_head + 1u) & AUDIO_RING_MASK;
            if (next != audio_tail) {
                audio_ring[audio_head] = (int16_t)o;
                audio_head = next;
            }
        }
    }
}

// ---- Bus interface --------------------------------------------------------

uint8_t sid_read(uint16_t addr) {
    unsigned r = addr & 0x1Fu;
    switch (r) {
        case 0x1B:  // OSC3: upper 8 bits of voice 3's waveform output
            return (uint8_t)(voice_output(2) >> 4);
        case 0x1C:  // ENV3: voice 3 envelope output
            return env[2].envelope;
        case 0x19:  // POTX (paddle X, not modelled)
        case 0x1A:  // POTY (paddle Y, not modelled)
        default:    // write-only control registers read as 0
            return 0x00;
    }
}

void sid_write(uint16_t addr, uint8_t val) {
    unsigned r = addr & 0x1Fu;
    if (r > 0x18u) {
        return;  // $D419-$D41F are read-only / unused
    }
    uint8_t old = reg[r];
    reg[r] = val;
    if (r == 0x15 || r == 0x16) {  // cutoff registers
        filter_update_cutoff();
        return;
    }
    if (r == 0x17) {  // resonance (high nibble); routing (low nibble) read live
        filter_update_res();
        return;
    }
    if (r != REG_CTRL(0) && r != REG_CTRL(1) && r != REG_CTRL(2)) {
        return;
    }
    unsigned v = (r == REG_CTRL(0)) ? 0u : (r == REG_CTRL(1)) ? 1u : 2u;
    // Gate edges drive the envelope. The rate counter is intentionally not reset
    // here, so a subsequent rate-register change feeds the delay bug.
    if (!(old & CTRL_GATE) && (val & CTRL_GATE)) {
        env[v].state = ENV_ATTACK;
    } else if ((old & CTRL_GATE) && !(val & CTRL_GATE)) {
        env[v].state = ENV_RELEASE;
    }
    if (val & CTRL_TEST) {  // TEST resets the accumulator at once
        voice[v].accumulator = 0;
        voice[v].prev_bit19 = false;
    }
}

// ---- Inspection / cross-voice hooks ---------------------------------------

uint32_t sid_voice_accumulator(unsigned v) { return voice[v % 3u].accumulator; }
uint16_t sid_voice_output(unsigned v) { return voice_output(v % 3u); }
bool sid_voice_msb(unsigned v) { return (voice[v % 3u].accumulator & ACC_MSB) != 0; }
bool sid_voice_overflow(unsigned v) { return voice[v % 3u].overflow; }
uint32_t sid_voice_noise(unsigned v) { return voice[v % 3u].noise_lfsr; }
uint8_t sid_voice_envelope(unsigned v) { return env[v % 3u].envelope; }
uint16_t sid_voice_rate_counter(unsigned v) { return env[v % 3u].rate_counter; }

int32_t sid_filter_output(void) { return filter_out; }
int32_t sid_direct_output(void) { return direct_out; }
uint32_t sid_filter_cutoff_hz(void) {
    return filter_fc_to_hz(((uint32_t)reg[0x16] << 3) | (reg[0x15] & 0x07u));
}
int32_t sid_output(void) {
    uint8_t vol = (uint8_t)(reg[0x18] & 0x0Fu);
    return (direct_out + filter_out + MIXER_DC) * (int32_t)vol;
}

void sid_set_audio(bool on) { audio_on = on; }
bool sid_audio_enabled(void) { return audio_on; }

unsigned sid_audio_read(int16_t *dst, unsigned max) {
    unsigned n = 0;
    while (n < max && audio_tail != audio_head) {
        dst[n++] = audio_ring[audio_tail];
        audio_tail = (audio_tail + 1u) & AUDIO_RING_MASK;
    }
    return n;
}

unsigned sid_audio_available(void) {
    return (audio_head - audio_tail) & AUDIO_RING_MASK;
}

void sid_voice_set_accumulator(unsigned v, uint32_t phase) {
    voice[v % 3u].accumulator = phase & ACC_MASK;
    voice[v % 3u].prev_bit19 = (voice[v % 3u].accumulator >> 19) & 1u;
}

// ---- Lifecycle ------------------------------------------------------------

void sid_reset(void) {
    memset(voice, 0, sizeof(voice));
    for (unsigned v = 0; v < 3; v++) {
        voice[v].noise_lfsr = LFSR_SEED;
    }
    memset(env, 0, sizeof(env));
    for (unsigned v = 0; v < 3; v++) {
        env[v].state = ENV_RELEASE;  // gate low, envelope held at 0
        env[v].exp_period = 1;
    }
    memset(reg, 0, sizeof(reg));
    filt.lp = 0;
    filt.bp = 0;
    filter_out = 0;
    direct_out = 0;
    filter_update_cutoff();
    filter_update_res();
    audio_on = false;
    audio_head = 0;
    audio_tail = 0;
    rs_sum = 0;
    rs_count = 0;
    rs_phase = 0;
    dc_lp = 0;
}

void sid_init(void) { sid_reset(); }
