// Smoke test for the SDL host adapter (src/host.c). host.c contains no emulation
// logic, so this does not assert behavior: it only drives host_init, host_audio_init,
// host_poll, host_present and the shutdown paths under SDL's dummy video and audio
// drivers (set at runtime via SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy), so a
// crash, use-after-free or leak in the adapter itself surfaces here when run under
// a sanitizer or Valgrind. If the dummy backend declines to initialise, that is
// acceptable and the test exits cleanly.
#include <stdint.h>
#include "host.h"
#include "cia.h"

static uint32_t fb[384u * 272u];

int main(void) {
    cia_init();  // host_poll routes keys through cia_key_set
    if (!host_init(384, 272, HOST_SCALE_DEFAULT, "smoke")) {
        return 0;  // no dummy video backend available: nothing to exercise
    }
    host_audio_init(44100);
    for (int i = 0; i < 3; i++) {
        host_poll();
        host_present(fb);
    }
    host_audio_shutdown();
    host_shutdown();
    return 0;
}
