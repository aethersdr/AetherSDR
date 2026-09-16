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
// This caps converted output, including equal-rate PCM: stereo lasts at most
// about 46.60 min at 24k Int16, 23.30 min at 48k Int16, or 11.65 min at 48k Float.
// Source bytes remain little-endian; sink bytes use the native sample format.
// Device selection, negotiation, sink lifetime and RX muting belong to caller.
inline constexpr qint64 kQsoPlaybackByteLimit = 256 * 1024 * 1024;
[[nodiscard]] std::optional<QByteArray> prepareQsoWavPlayback(
    QIODevice& source, const QAudioFormat& sinkFormat, QString* error = nullptr,
    qint64 maxOutputBytes = kQsoPlaybackByteLimit);

} // namespace AetherSDR
