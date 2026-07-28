# The 1541 and the disk surface

The drive is a full second computer: a 6502 at 1.0 MHz, two 6522 VIAs, and a
rotating magnetic surface. The CPU and VIAs are in `src/drive.c`; the surface is
built and read in `src/disk.c`. This page covers the surface model, because that is
where the subtle behaviour lives.

## GCR

Data is not stored as raw bytes. The 1541 uses group-coded recording: every four
data bits become five bits on disk, chosen so no long run of zeros occurs, which the
read electronics need to stay in sync. A sector on disk is a SYNC mark (a run of
one-bits), a header block (which track and sector this is, with a checksum), a gap, a
second SYNC, and the data block (the 256 bytes plus a checksum), all GCR-encoded.
`src/disk.c` builds this ring for each track from a `.d64` image.

## Density zones

The disk spins at a constant 300 RPM, but the outer tracks are physically longer, so
they hold more data. The drive switches bit rate across four zones. `ZONE_SECTORS` and
`ZONE_BYTE_CYCLES` in `src/disk.c` give the layout: 21 sectors at 26 cycles per byte
on the outer tracks, down to 17 sectors at 32 cycles per byte on the inner tracks. The
faster the bit rate, the more bytes fit in one revolution.

## Rotation

One revolution is 200000 drive cycles in every zone (300 RPM, a 200000 microsecond
period at 1 MHz). The head position is a bit index into the current track's ring that
advances only while the motor is on, at that zone's bit rate. It wraps once per
revolution.

The number of bytes on a track is the revolution time divided by the byte period,
which is not a whole number, so the model advances the head so that a full revolution
takes exactly 200000 cycles regardless of zone. Without that, the head angle drifts a
little each revolution, and code that reads by angle rather than by sector number
would slowly desync.

## Why interleave-1 is slow

Sector interleave is the gap, in sectors, between logically consecutive blocks of a
file as they sit on the track. A file written at interleave 10 puts its next block ten
sectors further round, so by the time the drive has read one block and is ready for
the next, that next block is just arriving under the head.

Interleave 1 puts the next block physically adjacent. By the time the drive finishes
one block, checks its checksum, and asks for the next, the adjacent block has already
passed under the head, so the drive must wait almost a full revolution for it to come
round again. Each block then costs close to one revolution. The standard KERNAL loader
on an interleave-1 disk is close to the worst case, and its load time is dominated by
rotational latency rather than transfer speed.

## What a raw-track loader depends on

A sector-addressed loader waits for the SYNC and header of the exact sector it wants,
so it does not care where the head is when it starts; it just waits longer or shorter.
The rotation timing changes when a block arrives, not which block.

A raw-track loader reads by rotational position instead. It waits for a SYNC and then
streams whatever bytes pass under the head, decoding them itself. It depends on the
surface being a continuous ring read at the true bit rate, and on the head entering at
the angle it expects. This is why the exact-200000 revolution above matters, and why a
raw-track loader is the strictest test of the disk model. It is also where the model's
edges show: the entry angle after a seek is a real physical quantity that depends on
the specific drive, so a raw loader that a particular disk relies on can be sensitive
in ways a sector loader never is.
