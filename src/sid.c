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

// Ring/sync source is the previous voice (voice 0 <- 2, 1 <- 0, 2 <- 1).
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

void sid_clock(void) {
    for (unsigned v = 0; v < 3; v++) {
        uint8_t ctrl = reg[REG_CTRL(v)];
        if (ctrl & CTRL_TEST) {
            voice[v].accumulator = 0;  // held in reset while TEST is set
            voice[v].overflow = false;
            voice[v].prev_bit19 = false;
            continue;
        }
        uint32_t freq =
            (uint32_t)reg[REG_FREQLO(v)] | ((uint32_t)reg[REG_FREQHI(v)] << 8);
        uint32_t sum = voice[v].accumulator + freq;
        voice[v].overflow = sum > ACC_MASK;
        voice[v].accumulator = sum & ACC_MASK;
        bool bit19 = (voice[v].accumulator >> 19) & 1u;
        if (bit19 && !voice[v].prev_bit19) {  // rising edge clocks the LFSR
            noise_clock(v);
        }
        voice[v].prev_bit19 = bit19;
    }
}

// ---- Bus interface --------------------------------------------------------

uint8_t sid_read(uint16_t addr) {
    unsigned r = addr & 0x1Fu;
    switch (r) {
        case 0x1B:  // OSC3: upper 8 bits of voice 3's waveform output
            return (uint8_t)(voice_output(2) >> 4);
        case 0x19:  // POTX (Phase 5+)
        case 0x1A:  // POTY (Phase 5+)
        case 0x1C:  // ENV3 (Phase 4b)
        default:    // write-only control registers read as 0
            return 0x00;
    }
}

void sid_write(uint16_t addr, uint8_t val) {
    unsigned r = addr & 0x1Fu;
    if (r > 0x18u) {
        return;  // $D419-$D41F are read-only / unused
    }
    reg[r] = val;
    // Writing a control register with TEST set resets the accumulator at once
    // (sid_clock keeps holding it while TEST stays set).
    if ((val & CTRL_TEST) && (r == REG_CTRL(0) || r == REG_CTRL(1) || r == REG_CTRL(2))) {
        unsigned v = (r == REG_CTRL(0)) ? 0u : (r == REG_CTRL(1)) ? 1u : 2u;
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
    memset(reg, 0, sizeof(reg));
}

void sid_init(void) { sid_reset(); }
