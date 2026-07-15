//! The IEC serial bus between the C64 (CIA2) and the 1541 drive (VIA1): three
//! open-collector wired-AND lines (ATN, CLK, DATA). Each side reports which lines
//! it pulls low and reads the union of both. This models the wires only; the
//! KERNAL and the DOS perform the LISTEN/TALK protocol themselves against them.
//! The two machines run at independent clocks (985248 Hz and 1.0 MHz), so
//! iec_step_frame interleaves them per cycle with the bus updated between.
#ifndef IEC_H
#define IEC_H

#include <stdbool.h>

// Set by the only four bytes iec_update reads: CIA2 $DD00/$DD02 (PRA/DDRA) and
// the drive's VIA1 ORB/DDRB. iec_update's output is a pure function of those, so
// it is skipped while this is clear. Owners set it on write; see iec_update.
extern bool iec_dirty;

void iec_reset(void);
void iec_update(void);      // propagate both sides' pulls across the wired-AND
void iec_step_frame(void);  // run one C64 frame, the drive interleaved per cycle

#endif // IEC_H
