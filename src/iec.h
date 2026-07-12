//! The IEC serial bus between the C64 (CIA2) and the 1541 drive (VIA1): three
//! open-collector wired-AND lines (ATN, CLK, DATA). Each side reports which lines
//! it pulls low and reads the union of both. This models the wires only; the
//! KERNAL and the DOS perform the LISTEN/TALK protocol themselves against them.
//! The two machines run at independent clocks (985248 Hz and 1.0 MHz), so
//! iec_step_frame interleaves them per cycle with the bus updated between.
#ifndef IEC_H
#define IEC_H

void iec_reset(void);
void iec_update(void);      // propagate both sides' pulls across the wired-AND
void iec_step_frame(void);  // run one C64 frame, the drive interleaved per cycle

#endif // IEC_H
