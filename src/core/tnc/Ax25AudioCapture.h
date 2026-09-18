#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

enum class Ax25AudioCaptureStage {
    Rx,
    TxGenerated,
    TxIcomPostResample,
};

// One identifier ties the RX recording and every TX-stage recording made by a
// single AetherModem Capture 3m run together.
QString makeAx25AudioCaptureId();

// Returns an empty path when captureId or packetSequence is invalid. RX has no
// packet sequence; both TX stages require a positive sequence number.
QString ax25AudioCapturePath(Ax25AudioCaptureStage stage,
                             const QString& captureId,
                             int packetSequence = 0);

// Writes interleaved IEEE-float PCM as a standard RIFF/WAVE file. QSaveFile
// keeps an interrupted diagnostic write from replacing a previously completed
// capture with a partial file.
bool writeAx25Float32Wav(const QString& path,
                         const QByteArray& pcm,
                         int sampleRate,
                         int channels,
                         QString* error = nullptr);

} // namespace AetherSDR
