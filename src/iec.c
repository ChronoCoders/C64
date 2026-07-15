#include "iec.h"

#include "cia.h"
#include "drive.h"
#include "vic.h"

bool iec_dirty = true;  // recompute once out of reset

void iec_reset(void) {
    // Both device-pull hooks cleared: the open-collector lines idle high until a
    // side pulls one low.
    cia_iec_device_pull(0);
    drive_set_iec_ext(0);
    iec_dirty = true;
}

// One wired-AND per line: each side sees the union of both sides' pulls. We feed
// the drive the lines the C64 pulls and the C64 the lines the drive pulls; each
// side ORs in its own drive internally, so both reconstruct the same line state.
// The drive's ATN acknowledge depends on the C64's ATN, so set the drive's view
// before reading its output, giving the auto-acknowledge the current ATN.
// The whole result is a pure function of four bytes: CIA2 PRA/DDRA (cia2_iec_out)
// and the drive's VIA1 ORB/DDRB (drive_iec_out); it writes only iec_ext and
// iec_dev_pull. Those four change only on a register write, and the owners set
// iec_dirty there, so while it is clear the outputs already hold the answer and
// recomputing is a no-op. VIA PB7 cannot forge a change: via_step toggles a
// separate pb7 field, and drive_iec_out reads PB1/PB3/PB4 only.
void iec_update(void) {
    if (!drive_present()) {
        return;  // no drive attached: the C64 runs alone
    }
    if (!iec_dirty) {
        return;
    }
    iec_dirty = false;
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
