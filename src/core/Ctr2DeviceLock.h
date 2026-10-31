#pragma once

#include <QString>

#include <memory>

namespace AetherSDR {

// One process per CTR2 HID path, in a per-user namespace independent of
// temporary-directory and settings overrides. Acquisitions within a process
// share ownership until their last holder releases; dead owners are recovered.
class Ctr2DeviceLock {
public:
    // The device at path, taken for this process until the last holder is
    // released; nullptr when another process has it, with that process's ID
    // in *otherPid (0 if not known).
    using Holder = std::shared_ptr<void>;
    static Holder acquire(const QString& path, qint64* otherPid);

    // Tests: lock files go here instead of the production folder.
    static void setDirectoryForTesting(const QString& directory);
};

} // namespace AetherSDR
