#include "TxCaptureBuffer.h"

#include <QIODevice>

namespace AetherSDR::TxCaptureBuffer {

QByteArray readBoundedInt16(QIODevice* device, int inputChannels)
{
    if (!device) {
        return {};
    }

    const qint64 channels = inputChannels <= 1 ? 1 : 2;
    const qint64 frameBytes = channels * static_cast<qint64>(sizeof(qint16));
    const qint64 alignedLimit = kMaxReadBytes - (kMaxReadBytes % frameBytes);
    return device->read(alignedLimit);
}

} // namespace AetherSDR::TxCaptureBuffer
