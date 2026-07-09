//! MOS 6581 SID: digital oscillator and waveform core (Phase 4a).
//!
//! Three voices, each a 24-bit phase-accumulating oscillator feeding the four
//! waveform generators (triangle, sawtooth, pulse, noise) with the 6581
//! combined-waveform behavior. No envelope (4b), filter (4c), or audio output
//! (4d) yet: this stage produces each voice's raw 12-bit waveform value from the
//! register file at $D400-$D41C, clocked at phi2 by sid_clock().
#ifndef SID_H
#define SID_H

#include <stdbool.h>
#include <stdint.h>

void sid_init(void);
void sid_reset(void);

// Advance every voice one phi2 clock (PAL ~985248 Hz). Produces no audio; it
// only updates oscillator and noise state. The machine loop drives this at phi2
// once the sample path lands in Phase 4d; in 4a the module is bus-routed and
// unit-clocked by tests.
void sid_clock(void);

// $D400-$D7FF decode (registers mirror every $20). $D400-$D418 are write-only
// control; $D419-$D41C are read-only. $D41B (OSC3) is implemented here because
// it reads the live oscillator; POTX/POTY ($D419/$D41A) and ENV3 ($D41C) read 0
// until their phases.
uint8_t sid_read(uint16_t addr);
void sid_write(uint16_t addr, uint8_t val);

// Inspection and cross-voice hooks. Hard sync (on accumulator overflow) and ring
// modulation become audible in Phase 4d; the oscillator exposes what they need
// now. v is taken modulo 3.
uint32_t sid_voice_accumulator(unsigned v);  // 24-bit phase
uint16_t sid_voice_output(unsigned v);        // current 12-bit waveform value
bool sid_voice_msb(unsigned v);               // accumulator bit 23 (ring source)
bool sid_voice_overflow(unsigned v);          // wrapped on the last clock (sync)
uint32_t sid_voice_noise(unsigned v);         // 23-bit noise LFSR (inspection)
uint8_t sid_voice_envelope(unsigned v);       // 8-bit ADSR envelope value
uint16_t sid_voice_rate_counter(unsigned v);  // 15-bit envelope rate counter (inspection)
void sid_voice_set_accumulator(unsigned v, uint32_t phase);  // test / sync reset

#endif // SID_H
