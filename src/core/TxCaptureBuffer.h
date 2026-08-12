#pragma once

#include <QByteArray>
#include <QtGlobal>

class QIODevice;

namespace AetherSDR::TxCaptureBuffer {

// QAudioSource normally delivers a fraction of its ~200 ms device buffer per
// readyRead. Keep a generous ceiling so a delayed audio-thread callback can
// catch up without allowing an unbounded backend residue to become one giant
// allocation. At 48 kHz stereo Int16 this is about 1.36 seconds of audio.
inline constexpr qint64 kMaxReadBytes = 256 * 1024;

QByteArray readBoundedInt16(QIODevice* device, int inputChannels);

} // namespace AetherSDR::TxCaptureBuffer
