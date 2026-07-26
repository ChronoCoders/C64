# ROM images

This emulator needs the Commodore C64 ROMs to run. They are copyrighted, so they
are not included in this download. Supply your own, then run the emulator.

## Files

Put these in a `rom/` directory next to the `c64` binary:

| File | Size (bytes) | What it is |
|---|---|---|
| `rom/kernal.rom` | 8192 | C64 KERNAL |
| `rom/basic.rom` | 8192 | C64 BASIC |
| `rom/chargen.rom` | 4096 | character generator |
| `rom/1541.rom` | 16384 | 1541 drive DOS (optional; without it the C64 runs on its own) |

The sizes must match exactly. On start the binary checks for these files and, if any
are missing, prints the list and exits.

## How to obtain them lawfully

Either of these:

- **Dump them from a Commodore 64 you own.** The KERNAL, BASIC and CHARGEN live in the
  C64; the DOS lives in a 1541 drive.
- **Copy them from a VICE install you already have.** VICE ships the standard set.
  Copy and rename:

  | VICE file | rename to |
  |---|---|
  | `kernal-901227-03.bin` | `rom/kernal.rom` |
  | `basic-901226-01.bin` | `rom/basic.rom` |
  | `chargen-901225-01.bin` | `rom/chargen.rom` |
  | `dos1541` | `rom/1541.rom` |

  (In a VICE install these are under the `C64` and `DRIVES` data directories.)

Do not redistribute these ROM files.
