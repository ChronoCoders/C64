# Building

Two build paths:

- **Makefile** (Linux): the canonical dev build and full gate (`make`, `make test`,
  `test-slow`, `test-cpu`, `test-asan`, `coverage`, `valgrind`, `fuzz`). Unchanged.
- **CMake** (Linux, Windows): the portable build plus the three test tiers
  (`make test` / `test-slow` / `test-cpu` equivalents) via CTest. The sanitizer,
  coverage, valgrind and fuzz targets stay in the Makefile; they are Linux-only.
  It targets gcc and MinGW-w64 gcc only; the MSVC and macOS paths were removed, so
  the warning flags and toolchain assumptions match the two verified platforms.

Supported platforms: **Linux x86-64** and **Windows x86-64 (MSYS2 MinGW64)**, both
verified. The exact toolchains are listed under each section.

## Prerequisites

- A C11 compiler: gcc on Linux, MinGW-w64 gcc on Windows.
- CMake 3.16 or newer.
- SDL2 (development package). Needed only for the `c64` GUI binary; the tests and the
  Lorenz runner build without it.

## ROMs (not shipped)

The KERNAL, BASIC, CHARGEN and 1541 DOS ROMs are copyrighted and are not in the repo.
Place your own legally obtained copies before running the emulator or the drive/iec
suites:

```
rom/kernal.rom    8192 bytes
rom/basic.rom     8192 bytes
rom/chargen.rom   4096 bytes
rom/1541.rom     16384 bytes
```

Run `./c64` with no ROMs present for the exact filenames and sizes it expects.

## Linux (verified)

```sh
sudo apt install libsdl2-dev cmake        # or the distro equivalent
cmake -B build
cmake --build build -j
ctest --test-dir build                    # unit suites (fast group)
ctest --test-dir build -L slow            # DOS/serial integration group
./build/c64 --disk games/some.d64         # run
```

## Windows, MSYS2 / MinGW64 (verified)

From an MSYS2 MinGW64 shell:

```sh
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The build copies `SDL2.dll` next to `c64.exe` so it runs from the build tree.

Verified on this toolchain: configure and build produced **zero warnings with
`-DC64_WERROR=ON`**, `ctest` passed **13/13** (mem, cpu, cia, sid, vic, drive, via, iec,
gcr, debug, snapshot, plus the drive/iec slow groups) in ~57s, and `c64.exe` opened a
window and mounted a `.d64`.

- gcc 16.1.0 (Rev5, MSYS2 project)
- cmake 4.4.0
- ninja 1.13.2
- SDL2 2.32.10

## CMake options

- `-DC64_WERROR=ON|OFF` (default ON): treat warnings as errors (`-Werror`).
- `-DC64_BUILD_TESTS=ON|OFF` (default ON): build the unit suites and Lorenz runner.
- `-DC64_DEBUG_TOOLS=ON|OFF` (default OFF): compile the bus-watchpoint / snapshot hooks.

## Lorenz CPU-conformance suite

Built as `lorenz-runner` but not a default CTest (the suite files under `test/lorenz/`
are gitignored and usually absent; the runner exits non-zero without them). See
`test/lorenz/PROVENANCE.md`. Run it directly once the suite is in place:

```sh
./build/lorenz-runner
```

## Notes

- Snapshots (save-states) are within-platform only: raw struct images in host byte
  order and this compiler's layout. They resume in the same build, not across a
  different endianness or compiler ABI. See the header of `src/snapshot.c`.
