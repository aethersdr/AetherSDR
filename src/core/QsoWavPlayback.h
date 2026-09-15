#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QString>

#include <optional>

class QIODevice;

namespace AetherSDR {

// Device-free preparation for the recorder's existing in-memory playback.
// Reads only the parsed PCM data chunk in bounded blocks. Output must fit the
// explicit budget, checked before reading samples or allocating the payload.
// Source bytes remain little-endian; sink bytes use the native sample format.
// Device selection, negotiation, sink lifetime and RX muting belong to caller.
inline constexpr qint64 kQsoPlaybackByteLimit = 256 * 1024 * 1024;
[[nodiscard]] std::optional<QByteArray> prepareQsoWavPlayback(
    QIODevice& source, const QAudioFormat& sinkFormat, QString* error = nullptr,
    qint64 maxOutputBytes = kQsoPlaybackByteLimit);

} // namespace AetherSDR
