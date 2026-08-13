#include "TxCaptureBuffer.h"

#include <QIODevice>

#include <algorithm>

namespace AetherSDR::TxCaptureBuffer {

BoundedRead readLatestBoundedInt16(QIODevice* device, int inputChannels)
{
    if (!device) {
        return {};
    }

    // Align to the device's real frame size, not the normalizer's 1-or-2 clamp:
    // a 6-channel negotiated device has 12-byte frames, which 256 KiB does not
    // divide evenly. Reading a partial frame would leave every later read
    // straddling a frame boundary.
    const qint64 channels = std::max<qint64>(1, inputChannels);
    const qint64 frameBytes = channels * static_cast<qint64>(sizeof(qint16));
    const qint64 alignedLimit = kMaxReadBytes - (kMaxReadBytes % frameBytes);

    BoundedRead out;

    // Drop to latest before reading. skip() consumes in bounded internal chunks,
    // so a multi-gigabyte residue is discarded without ever being allocated.
    const qint64 available = device->bytesAvailable();
    if (available > kCatchUpThresholdBytes) {
        const qint64 staleBytes = available - alignedLimit;
        const qint64 alignedStale = staleBytes - (staleBytes % frameBytes);
        if (alignedStale > 0) {
            // skip() returns -1 on a device error; treat that as "nothing was
            // discarded" so the health counter never goes backwards.
            out.discardedBytes = std::max<qint64>(0, device->skip(alignedStale));
        }
    }

    out.block = device->read(alignedLimit);
    return out;
}

} // namespace AetherSDR::TxCaptureBuffer
