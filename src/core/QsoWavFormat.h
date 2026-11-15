#pragma once

#include <QString>
#include <QtTypes>

#include <optional>

class QIODevice;

namespace AetherSDR {

struct QsoWavFormat {
    int sampleRate = 0;
    int channelCount = 0;
    qint64 dataOffset = 0;
    qint64 dataBytes = 0;
    qint64 frameCount = 0;
};

// Parse a RIFF/WAVE header without reading PCM. Accepts PCM16 mono/stereo at
// 24000/44100/48000 Hz; skips fmt extensions and unknown (odd-padded) chunks;
// validates all chunks inside the RIFF size, ignores bytes beyond. Scans at
// most 4096 chunks with fixed-size reads. Source must be open, binary,
// seekable and stable; on success the cursor is at dataOffset and *error is
// cleared, on failure the cursor is unspecified.
[[nodiscard]] std::optional<QsoWavFormat> parseQsoWav(
    QIODevice& source, QString* error = nullptr);

} // namespace AetherSDR
