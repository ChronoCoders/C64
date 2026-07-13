// libFuzzer harness over the .d64 mount path, the only place the emulator ingests
// a file from an untrusted source. A corrupt or hostile image must be rejected
// cleanly: never crash, never read or write out of bounds (AddressSanitizer), never
// invoke undefined behaviour (UBSan), never loop forever. Build and run: `make fuzz`.
#include <stddef.h>
#include <stdint.h>

#include "disk.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (disk_mount_image(data, size)) {
        // A mount that validated: exercise the GCR track readout the drive uses and
        // the ring parser the write-back path uses (disk_read_sector), so any out-of-
        // bounds in the track builder or the sector decoder shows up under ASan.
        for (unsigned t = 1; t <= DISK_TRACKS; t++) {
            unsigned nbits = 0;
            const uint8_t *ring = disk_track_gcr(t, &nbits);
            if (ring != NULL && nbits > 0u) {
                volatile uint8_t sink = ring[(nbits - 1u) >> 3];  // touch the last byte
                (void)sink;
            }
            uint8_t sector[256];
            disk_read_sector(t, 0u, sector);  // decode sector 0 off the (fuzzed) ring
        }
    }
    disk_unmount();
    return 0;
}
