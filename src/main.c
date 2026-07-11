#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "host.h"
#include "cia.h"
#include "drive.h"
#include "mem.h"
#include "sid.h"
#include "vic.h"

#define AUDIO_RATE 44100
#define AUDIO_TARGET_SAMPLES 3528  // ~4 PAL frames of buffered audio (paces to realtime)

// C64 emulator entry point. By default it opens an SDL2 window, boots the real
// ROMs, and runs frames continuously, presenting each rendered frame. With
// --headless it runs the same machine without a display and reports where the
// CPU settles (useful where no display is available). VIC pixels render in
// standard text mode; the CIA (keyboard, Phase 5) is not present, so the machine
// reaches the READY prompt and idles in the keyboard-input loop.

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

static int run_visible(void) {
    if (!host_init(vic_fb_width(), vic_fb_height(), WINDOW_TITLE)) {
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
    while (!host_poll()) {
        vic_run_frame();  // per-cycle rendering fills the framebuffer as it runs
        drive_run_phi2(vic_cycles_per_frame());  // step the drive in its own domain
        if (audio) {
            int16_t abuf[2048];
            unsigned n;
            while ((n = sid_audio_read(abuf, 2048)) > 0) {
                host_audio_push(abuf, (int)n);
            }
        }
        host_present(vic_framebuffer());
        if (audio) {
            host_audio_pace(AUDIO_TARGET_SAMPLES);  // pace emulation to audio realtime
        }
    }
    host_audio_shutdown();
    host_shutdown();
    return 0;
}

static int run_headless(void) {
    uint16_t lo = cpu.pc;
    uint16_t hi = cpu.pc;
    for (unsigned f = 0; f < HEADLESS_FRAMES; f++) {
        vic_run_frame();
        drive_run_phi2(vic_cycles_per_frame());  // step the drive in its own domain
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
    printf("  note            reached the keyboard-input loop; keyboard needs "
           "the CIA (Phase 5).\n");
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
    if (drive_load_rom(DRIVE_ROM_PATH)) {
        drive_reset();
        printf("1541: DOS ROM loaded; drive attached (1.0 MHz, own bus).\n");
    } else {
        printf("1541: no DOS ROM at %s; drive not attached, C64 runs normally.\n",
               DRIVE_ROM_PATH);
    }

    if (argc > 1 && strcmp(argv[1], "--headless") == 0) {
        return run_headless();
    }
    return run_visible();
}
