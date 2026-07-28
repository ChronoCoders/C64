# Debugging with the emulator

Three facilities help find and reproduce problems: headless mode for scripted runs,
snapshots for capturing exact machine state, and a bus watchpoint and snapshot trigger
compiled in only when you ask for it.

## Headless mode

`--headless` runs the full machine with no window. It boots the ROMs, runs a fixed
number of frames, and prints where the CPU ended up: the frame count, the range of
program-counter values seen, the final PC, and the border and background colours. It
is the fastest way to check that a build boots and to script a run without a display.
`--disk <path.d64>` mounts an image; `--scale N` sets the window size for the normal
windowed mode. See `src/main.c`.

Because a headless run is deterministic, it is also useful as a smoke check in a
script: same input, same reported state every time.

## Snapshots

`src/snapshot.c` serialises the entire machine state, both CPUs and all their chips,
into a single image tagged `C64SNAP2`. Save and restore round-trip exactly, so a
snapshot is a way to freeze a hard-to-reach state and return to it. The test suite
uses this to pin behaviour: capture a state once, then restore and continue from it
deterministically.

The block order in the format is fixed. Changing a block, or its order, requires a
version bump so that older images fail cleanly at the version check rather than
loading as garbage.

## The debug facility

`src/debug.c` adds bus watchpoints and a triggered snapshot. It is compiled in only
when the build defines `DEBUG_TOOLS`, so it costs nothing in a release binary. Build
it with `make DEBUG_TOOLS=1`, or with `-DC64_DEBUG_TOOLS=ON` under CMake. If the
environment variables below are set but the binary was built without the flag, it
prints a notice and does nothing.

It is driven by environment variables:

- `C64_WATCH="LO-HI[:r|w|rw]"` logs accesses to an address range. The optional suffix
  restricts it to reads, writes, or both.
- `C64_WATCH_PC="LO-HI"` restricts the watch to accesses made while the program
  counter is in that range.
- `C64_WATCH_OUT=<file>` sends the watch log to a file instead of standard error.
- `C64_SNAP="PC@ADDR"`, both hex, dumps a snapshot when the program counter reaches
  `PC` and the address `ADDR` is touched.
- `C64_SNAP_OUT=<file>` names the snapshot file (default `c64-snap.bin`).

The usual workflow for a timing or loader bug: run under `C64_WATCH` to see who reads
or writes the address you care about and from what code, narrow it with
`C64_WATCH_PC`, then set `C64_SNAP` at the moment things go wrong so you can restore
that exact state and study it. This is the built-in alternative to editing the source
to add print statements, which the project's rules forbid.
