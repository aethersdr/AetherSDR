#include "TxCaptureBuffer.h"

#include <QIODevice>

#include <algorithm>

namespace AetherSDR::TxCaptureBuffer {

namespace {

// Align to the device's real frame size, not the normalizer's 1-or-2 clamp: a
// 6-channel negotiated device has 12-byte frames, which 256 KiB does not divide
// evenly. Keeping a partial frame would leave every later block straddling a
// frame boundary.
qint64 frameBytesFor(int inputChannels, int bytesPerSample)
{
    return std::max<qint64>(1, inputChannels)
        * std::max<qint64>(1, bytesPerSample);
}

qint64 alignDown(qint64 bytes, qint64 frameBytes)
{
    return bytes - (bytes % frameBytes);
}

qint64 alignUp(qint64 bytes, qint64 frameBytes)
{
    const qint64 remainder = bytes % frameBytes;
    return remainder == 0 ? bytes : bytes + (frameBytes - remainder);
}

} // namespace

BoundedRead readLatestBounded(QIODevice* device, int inputChannels, int bytesPerSample)
{
    if (!device) {
        return {};
    }

    const qint64 frameBytes = frameBytesFor(inputChannels, bytesPerSample);
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

qint64 trimToLatestBounded(QByteArray& block, int inputChannels, int bytesPerSample)
{
    const qint64 frameBytes = frameBytesFor(inputChannels, bytesPerSample);
    const qint64 alignedLimit = alignDown(kMaxReadBytes, frameBytes);
    if (block.size() <= alignedLimit) {
        return 0;
    }

    // Cut a whole number of frames off the front. Rounding the discard UP keeps
    // the retained tail starting on a frame boundary even when the push buffer
    // itself ended mid-frame — trimming a bare byte count there would shift the
    // block by a sample and swap L/R for the rest of it.
    const qint64 discarded = alignUp(block.size() - alignedLimit, frameBytes);
    block = block.last(block.size() - discarded);
    return discarded;
}

} // namespace AetherSDR::TxCaptureBuffer
