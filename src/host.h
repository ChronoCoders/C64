//! SDL2 host layer: presents the VIC framebuffer in a window and pumps the
//! event queue. This is the only file that touches SDL; the emulator core has no
//! display dependency, so a kmsdrm/kiosk backend can replace this file without
//! changing the core. The SDL2 video driver is chosen by SDL at runtime (wayland
//! or x11 under WSLg during development, kmsdrm on the kiosk later).
#ifndef HOST_H
#define HOST_H

#include <stdbool.h>
#include <stdint.h>

// Open a window/renderer/texture sized for a width x height ARGB8888
// framebuffer (scaled up for display). Returns false on failure.
// Window scale: the framebuffer is always rendered at VIC_FB_WIDTH x VIC_FB_HEIGHT;
// scale only multiplies the output window. Present cost grows with the scaled
// pixel count, so a constrained compositor may need a smaller window.
#define HOST_SCALE_MIN 1
#define HOST_SCALE_MAX 4
#define HOST_SCALE_DEFAULT 3

/// Opens the window at width*scale by height*scale. Returns false if scale is
/// outside HOST_SCALE_MIN..HOST_SCALE_MAX or the display cannot be opened.
bool host_init(int width, int height, int scale, const char *title);

// Human-readable description of the last host failure (the SDL error string).
const char *host_error(void);

// Upload and draw one framebuffer (width x height ARGB8888 pixels).
void host_present(const uint32_t *framebuffer);

// Pump the event queue; returns true if the user requested quit (window close
// or F12). Also maps the host keyboard and joystick onto the CIA1 matrix.
bool host_poll(void);

// True while warp/turbo (F10) is on: the caller should run the machine unthrottled
// (many emulated frames per presented frame, audio pacing skipped).
bool host_warp(void);

void host_shutdown(void);

// SDL audio output (Phase 4d): mono signed-16-bit at the given rate, via SDL's
// queue API (fed from the same thread as the main loop, so no locking). Returns
// false on failure (the caller may continue without sound).
bool host_audio_init(int rate);
void host_audio_push(const int16_t *samples, int count);
// Block until the queued audio drains to target_samples, pacing the emulation to
// audio realtime.
void host_audio_pace(unsigned target_samples);
void host_audio_shutdown(void);

#endif // HOST_H
