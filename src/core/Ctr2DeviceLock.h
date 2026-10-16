#pragma once

#include <QString>

#include <memory>

namespace AetherSDR {

// One AetherSDR at a time on a CTR2's USB HID interface. The operating
// system lets several programs open the same HID device (Linux hidraw,
// Windows, macOS), and two AetherSDR instances relaying one CTR2 break each
// other's link. A lock file per device, in the user's runtime folder (the
// same for every instance, whatever its TMPDIR or settings), marks the
// device as taken: another instance is refused, and a lock left by one that
// crashed is taken over. Within one AetherSDR the relay, the version probe
// and the firmware updater hand the device to each other and may overlap
// while a port closes, so a process holds the lock for as long as any of
// its holders does.
class Ctr2DeviceLock {
public:
    // The device at path, taken for this process until the last holder is
    // released; nullptr when another process has it, with that process's ID
    // in *otherPid (0 if not known).
    using Holder = std::shared_ptr<void>;
    static Holder acquire(const QString& path, qint64* otherPid);

    // Tests: lock files go here instead of the runtime folder.
    static void setDirectoryForTesting(const QString& directory);
};

} // namespace AetherSDR
