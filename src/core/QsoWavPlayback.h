#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QString>

#include <optional>

class QIODevice;

namespace AetherSDR {

// Device-free prep for the recorder's in-memory playback: reads the PCM data
// chunk in bounded blocks, converting LE source to the sink's native format.
// The budget counts output FRAMES (checked before allocating), so duration
// doesn't depend on sink format (#3231). 67,108,864 stereo frames ≈ 46.6 min
// at 24 kHz / 23.3 min at 48 kHz, ≤ ~536 MiB (Float stereo, 8 B/frame).
// prefixOnly converts the longest prefix that fits; full playback refuses an
// over-budget recording. Device choice, sink lifetime and RX muting are the
// caller's.
inline constexpr qint64 kQsoPlaybackMaxFrames = 67'108'864;
[[nodiscard]] std::optional<QByteArray> prepareQsoWavPlayback(
    QIODevice& source, const QAudioFormat& sinkFormat, QString* error = nullptr,
    qint64 maxOutputFrames = kQsoPlaybackMaxFrames, bool prefixOnly = false);

} // namespace AetherSDR
