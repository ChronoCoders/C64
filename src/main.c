#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "host.h"
#include "cia.h"
#include "disk.h"
#include "drive.h"
#include "iec.h"
#include "mem.h"
#include "sid.h"
#include "vic.h"

#define AUDIO_RATE 44100
#define AUDIO_TARGET_SAMPLES 3528  // ~4 PAL frames of buffered audio (paces to realtime)
#define WARP_FRAMES 20  // emulated frames per presented frame while warp (F10) is on

// C64 emulator entry point. By default it opens an SDL2 window, boots the real
// ROMs, and runs frames continuously, presenting each rendered frame. With
// --headless it runs the same machine without a display and reports where the
// CPU settles (useful where no display is available). The full machine (CPU,
// VIC, SID, both CIAs, and the 1541 drive when its ROM is present) runs; the
// KERNAL boots to the READY prompt and idles in the keyboard-input loop.

#define KERNAL_PATH "rom/kernal.rom"
#define BASIC_PATH "rom/basic.rom"
#define CHAR_PATH "rom/chargen.rom"
#define DRIVE_ROM_PATH "rom/1541.rom"
#define WINDOW_TITLE "Commodore 64"

#define HEADLESS_FRAMES 200u

static bool load_roms(void) {
    bool k = mem_load_rom(ROM_KERNAL, KERNAL_PATH);
    bool b = mem_load_rom(ROM_BASIC, BASIC_PATH);
    bool c = mem_load_rom(ROM_CHAR, CHAR_PATH);
    if (k && b && c) {
        return true;
    }
    printf("C64: ROM images not found. Copy them from a VICE install into rom/, "
           "renamed to *.rom (they are copyrighted and never committed):\n");
    if (!k) {
        printf("  rom/kernal.rom   8192 bytes  (VICE kernal-901227-03.bin)\n");
    }
    if (!b) {
        printf("  rom/basic.rom    8192 bytes  (VICE basic-901226-01.bin)\n");
    }
    if (!c) {
        printf("  rom/chargen.rom  4096 bytes  (VICE chargen-901225-01.bin)\n");
    }
    return false;
}

// Output window scale, --scale N. Presentation only: the framebuffer is always
// rendered at the VIC's own resolution, so this cannot affect emulation.
static int window_scale = HOST_SCALE_DEFAULT;

// --autorun: once the cold boot reaches READY, type LOAD"*",8,1 then RUN into the
// KERNAL keyboard buffer so a mounted disk loads and starts hands-free. One key is
// queued per frame only when the buffer is empty, which self-paces around the load
// (the buffer holds no new keys while the DOS load runs). Only when a disk is mounted.
#define KB_BUFFER 0x0277u          // KERNAL keyboard buffer front
#define KB_NDX 0x00C6u             // number of keys queued in it
#define AUTORUN_BOOT_FRAMES 150u   // wait for the cold boot to reach READY
static const char AUTORUN_SEQ[] = "LOAD\"*\",8,1\rRUN\r";
static bool autorun_enabled;

static int run_visible(void) {
    if (!host_init(vic_fb_width(), vic_fb_height(), window_scale, WINDOW_TITLE)) {
        printf("C64: could not open a display window (%s). Is a display "
               "available? Try --headless.\n", host_error());
        return 1;
    }
    bool audio = host_audio_init(AUDIO_RATE);
    sid_set_audio(audio);  // when enabled, the machine loop clocks the SID at phi2
    if (!audio) {
        printf("C64: audio device unavailable (%s); running without sound.\n",
               host_error());
    }
    // Audio pacing cushion. The default suits capable machines; a jittery host (for
    // example WSL2 audio) can widen it with C64_AUDIO_TARGET to trade latency for a
    // deeper buffer against underrun.
    unsigned audio_target = AUDIO_TARGET_SAMPLES;
    const char *target_env = getenv("C64_AUDIO_TARGET");
    if (target_env) {
        long v = strtol(target_env, NULL, 10);
        if (v > 0 && v < 100000) { audio_target = (unsigned)v; }
    }
    unsigned autorun_frame = 0;
    size_t autorun_idx = 0;
    while (!host_poll()) {
        if (autorun_enabled && AUTORUN_SEQ[autorun_idx] != '\0') {
            if (autorun_frame < AUTORUN_BOOT_FRAMES) {
                autorun_frame++;                    // let the boot reach READY first
            } else if (mem_read(KB_NDX) == 0u) {    // feed one key once the buffer drains
                mem_write(KB_BUFFER, (uint8_t)AUTORUN_SEQ[autorun_idx]);
                mem_write(KB_NDX, 1u);
                autorun_idx++;
            }
        }
        bool warp = host_warp();  // F10: run unthrottled, many frames per presented one
        int frames = warp ? WARP_FRAMES : 1;
        for (int i = 0; i < frames; i++) {
            iec_step_frame();  // C64 and drive interleaved per cycle over the serial bus
            if (audio) {
                int16_t abuf[2048];
                unsigned n;
                while ((n = sid_audio_read(abuf, 2048)) > 0) {
                    if (!warp) { host_audio_push(abuf, (int)n); }  // drain but mute in warp
                }
            }
        }
        host_present(vic_framebuffer());
        if (audio && !warp) {
            host_audio_pace(audio_target);  // pace to audio realtime (skipped in warp)
        }
    }
    host_audio_shutdown();
    host_shutdown();
    if (disk_writeback()) {  // persist a SAVE back to the .d64 on a clean exit
        printf("1541: wrote the disk image back to its file.\n");
    }
    return 0;
}

static int run_headless(void) {
    uint16_t lo = cpu.pc;
    uint16_t hi = cpu.pc;
    for (unsigned f = 0; f < HEADLESS_FRAMES; f++) {
        iec_step_frame();  // C64 and drive interleaved per cycle over the serial bus
        if (cpu.pc < lo) {
            lo = cpu.pc;
        }
        if (cpu.pc > hi) {
            hi = cpu.pc;
        }
    }
    printf("C64 headless bring-up:\n");
    printf("  frames run      %u\n", HEADLESS_FRAMES);
    printf("  PC per frame    $%04X-$%04X, final $%04X\n", lo, hi, cpu.pc);
    printf("  border/bg       $D020=%u $D021=%u\n", vic_read(0xD020) & 0x0F,
           vic_read(0xD021) & 0x0F);
    printf("  note            KERNAL booted to READY and idles in the keyboard "
           "scan; the full machine (both CIAs, the 1541 drive) is present.\n");
    if (disk_writeback()) {
        printf("1541: wrote the disk image back to its file.\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    mem_init();
    if (!load_roms()) {
        return 1;
    }
    vic_init();
    cia_init();
    cpu_init();
    cpu_reset();

    drive_init();
    iec_reset();
    if (drive_load_rom(DRIVE_ROM_PATH)) {
        drive_reset();
        printf("1541: DOS ROM loaded; drive attached (1.0 MHz, own bus).\n");
    } else {
        printf("1541: no DOS ROM at %s; drive not attached, C64 runs normally.\n",
               DRIVE_ROM_PATH);
    }

    // Optional disk: --disk <path.d64>. Writes land on the in-memory surface and
    // reach the file only through disk_writeback() on a clean exit. A rejected or
    // absent image just leaves the drive empty; the machine runs normally.
    bool headless = false;
    bool autorun = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--autorun") == 0) {
            autorun = true;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            char *end = NULL;
            long v = strtol(arg, &end, 10);
            if (end == arg || *end != '\0' || v < HOST_SCALE_MIN || v > HOST_SCALE_MAX) {
                printf("C64: --scale must be an integer %d..%d (got \"%s\").\n",
                       HOST_SCALE_MIN, HOST_SCALE_MAX, arg);
                return 1;
            }
            window_scale = (int)v;
        } else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            if (disk_mount(path)) {
                printf("1541: mounted %s (read/write; a SAVE reaches the file "
                       "only on a clean exit).\n", path);
            } else {
                printf("1541: could not mount %s (missing or not a 35-track .d64).\n", path);
            }
        }
    }

    // --autorun only makes sense with a mounted disk to LOAD"*" from.
    autorun_enabled = autorun && disk_present();
    if (autorun && !disk_present()) {
        printf("C64: --autorun ignored (no disk mounted; pass --disk <path.d64>).\n");
    }

    debug_init_from_env();
    int rc = headless ? run_headless() : run_visible();
    debug_shutdown();
    return rc;
}
