#include "TxCaptureBuffer.h"

#include <QIODevice>

#include <algorithm>

namespace AetherSDR::TxCaptureBuffer {

namespace {

// Align to the device's real frame size, not the normalizer's 1-or-2 clamp: a
// 6-channel negotiated device has 12-byte frames, which 256 KiB does not divide
// evenly. Keeping a partial frame would leave every later block straddling a
// frame boundary.
qint64 frameBytesFor(int inputChannels)
{
    return std::max<qint64>(1, inputChannels) * static_cast<qint64>(sizeof(qint16));
}

qint64 alignDown(qint64 bytes, qint64 frameBytes)
{
    return bytes - (bytes % frameBytes);
}

} // namespace

BoundedRead readLatestBoundedInt16(QIODevice* device, int inputChannels)
{
    if (!device) {
        return {};
    }

    const qint64 frameBytes = frameBytesFor(inputChannels);
    const qint64 alignedLimit = alignDown(kMaxReadBytes, frameBytes);

    BoundedRead out;

    // Drop to latest before reading. skip() consumes in bounded internal chunks,
    // so a multi-gigabyte residue is discarded without ever being allocated.
    const qint64 available = device->bytesAvailable();
    if (available > kCatchUpThresholdBytes) {
        const qint64 alignedStale = alignDown(available - alignedLimit, frameBytes);
        if (alignedStale > 0) {
            // skip() returns -1 on a device error; treat that as "nothing was
            // discarded" so the health counter never goes backwards.
            out.discardedBytes = std::max<qint64>(0, device->skip(alignedStale));
        }
    }

    // Ask only for what is there. QIODevice::read(qint64) sizes its result to
    // maxSize before reading and only shrinks afterwards, so passing the full
    // ceiling would allocate 256 KiB on every readyRead to carry a few KB. A
    // device that reports nothing available still gets the full ceiling, since
    // then the count cannot be trusted to mean "no data".
    const qint64 want = available > 0
        ? std::min(alignedLimit, alignDown(available, frameBytes))
        : alignedLimit;
    if (want > 0) {
        out.block = device->read(want);
    }
    return out;
}

qint64 trimToLatestBoundedInt16(QByteArray& block, int inputChannels)
{
    const qint64 frameBytes = frameBytesFor(inputChannels);
    const qint64 alignedLimit = alignDown(kMaxReadBytes, frameBytes);
    if (block.size() <= alignedLimit) {
        return 0;
    }

    const qint64 discarded = block.size() - alignedLimit;
    block = block.last(alignedLimit);
    return discarded;
}

} // namespace AetherSDR::TxCaptureBuffer
