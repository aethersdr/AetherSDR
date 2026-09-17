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
// The budget counts converted FRAMES, so the playable duration depends on the
// recording's rate and not on the negotiated sample format.
// Source bytes remain little-endian; sink bytes use the native sample format.
// Device selection, negotiation, sink lifetime and RX muting belong to caller.
// Bounded in FRAMES, not bytes, so the ceiling is a property of the recording
// rather than of a sink format the operator never chose: a Float-only mixer
// would otherwise replay half as long as an Int16 one (#3231's WASAPI path).
// 67,108,864 stereo frames is ~46.6 min at 24 kHz and ~23.3 min at 48 kHz, and
// caps the allocation at ~536 MiB in the worst case (Float stereo, 8 B/frame).
inline constexpr qint64 kQsoPlaybackMaxFrames = 67'108'864;
[[nodiscard]] std::optional<QByteArray> prepareQsoWavPlayback(
    QIODevice& source, const QAudioFormat& sinkFormat, QString* error = nullptr,
    qint64 maxOutputFrames = kQsoPlaybackMaxFrames);

} // namespace AetherSDR
