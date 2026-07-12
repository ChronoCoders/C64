#include "iec.h"

#include "cia.h"
#include "drive.h"
#include "vic.h"

void iec_reset(void) {
    // Both device-pull hooks cleared: the open-collector lines idle high until a
    // side pulls one low.
    cia_iec_device_pull(0);
    drive_set_iec_ext(0);
}

// One wired-AND per line: each side sees the union of both sides' pulls. We feed
// the drive the lines the C64 pulls and the C64 the lines the drive pulls; each
// side ORs in its own drive internally, so both reconstruct the same line state.
// The drive's ATN acknowledge depends on the C64's ATN, so set the drive's view
// before reading its output, giving the auto-acknowledge the current ATN.
void iec_update(void) {
    if (!drive_present()) {
        return;  // no drive attached: the C64 runs alone
    }
    uint8_t c64_pulls = cia2_iec_out();
    drive_set_iec_ext(c64_pulls);
    uint8_t drive_pulls = drive_iec_out();
    cia_iec_device_pull(drive_pulls);
}

// Advance both machines through one C64 frame, one C64 cycle at a time, with the
// bus propagated on both sides of each step so a line change is seen within a
// cycle. drive_run_phi2 scales one C64 cycle into the drive's own cycles.
void iec_step_frame(void) {
    uint16_t cycles = vic_cycles_per_frame();
    for (uint16_t i = 0; i < cycles; i++) {
        iec_update();
        vic_step();
        iec_update();
        drive_run_phi2(1u);
    }
}
