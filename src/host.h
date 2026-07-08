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
bool host_init(int width, int height, const char *title);

// Human-readable description of the last host failure (the SDL error string).
const char *host_error(void);

// Upload and draw one framebuffer (width x height ARGB8888 pixels).
void host_present(const uint32_t *framebuffer);

// Pump the event queue; returns true if the user requested quit (window close
// or Escape). No C64 keyboard emulation here; that is Phase 5 (CIA).
bool host_poll(void);

void host_shutdown(void);

#endif // HOST_H
