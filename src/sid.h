//! MOS 6581 SID: three-voice synthesizer, clean-room (not reSID).
//!
//! Three voices, each a 24-bit phase-accumulating oscillator feeding the four
//! waveform generators (triangle, sawtooth, pulse, noise) with the 6581
//! combined-waveform behavior, an ADSR envelope, a state-variable filter, and
//! the mix/volume path resampled to host audio. Registers at $D400-$D41C;
//! sid_clock() advances every voice one phi2 cycle.
#ifndef SID_H
#define SID_H

#include <stdbool.h>
#include <stdint.h>

void sid_init(void);
void sid_reset(void);

// Advance every voice one phi2 clock (PAL ~985248 Hz): oscillators, noise,
// envelopes, and the filter, and (when audio is enabled) resample into the host
// buffer. The machine loop drives this at phi2; tests unit-clock it directly.
void sid_clock(void);

// $D400-$D7FF decode (registers mirror every $20). $D400-$D418 are write-only
// control; $D419-$D41C are read-only. OSC3 ($D41B) reads the live oscillator and
// ENV3 ($D41C) the live envelope; the paddle registers POTX/POTY ($D419/$D41A)
// are not modelled and read 0.
uint8_t sid_read(uint16_t addr);
void sid_write(uint16_t addr, uint8_t val);

// Inspection and cross-voice hooks. Hard sync (on accumulator overflow) and ring
// modulation are applied in the mix; these expose the oscillator state they use.
// v is taken modulo 3.
uint32_t sid_voice_accumulator(unsigned v);  // 24-bit phase
uint16_t sid_voice_output(unsigned v);        // current 12-bit waveform value
bool sid_voice_msb(unsigned v);               // accumulator bit 23 (ring source)
bool sid_voice_overflow(unsigned v);          // wrapped on the last clock (sync)
uint32_t sid_voice_noise(unsigned v);         // 23-bit noise LFSR (inspection)
uint8_t sid_voice_envelope(unsigned v);       // 8-bit ADSR envelope value
uint16_t sid_voice_rate_counter(unsigned v);  // 15-bit envelope rate counter (inspection)

// Internal audio path. Signal-unit integers, not host format; sid_output() is
// resampled and delivered to the host (see the audio output section below).
int32_t sid_filter_output(void);  // filtered path (routed voices through the SVF)
int32_t sid_direct_output(void);  // sum of the unrouted voices
int32_t sid_output(void);         // final mix: (direct + filtered + DC) * volume
uint32_t sid_filter_cutoff_hz(void);  // mapped cutoff Hz (representative curve)

// Audio output (Phase 4d). The machine loop clocks the SID at phi2 when audio is
// enabled; sid_clock() resamples to the host rate and buffers 16-bit mono
// samples. The host drains them with sid_audio_read().
void sid_set_audio(bool on);
bool sid_audio_enabled(void);
unsigned sid_audio_read(int16_t *dst, unsigned max);  // returns samples copied
unsigned sid_audio_available(void);
void sid_voice_set_accumulator(unsigned v, uint32_t phase);  // test / sync reset

#endif // SID_H
