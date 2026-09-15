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

// Inspect a RIFF/WAVE file from byte zero, without reading its PCM payload.
// Accepts PCM16 mono/stereo at 24000, 44100 or 48000 Hz. PCM fmt extensions
// and unknown chunks are skipped, including odd-byte padding. All chunks in
// the declared RIFF container are validated; bytes beyond it are ignored.
// At most 4096 chunks are scanned, with fixed-size header reads (<=16 bytes)
// and no allocation proportional to chunk/file size. This is a metadata
// bound, not a recording-duration limit; classic RIFF's 32-bit size applies.
// The source must be open, readable, binary and seekable, with a stable size
// and contents throughout the call. Success leaves its cursor at dataOffset;
// failure leaves the cursor unspecified. Error is cleared on success.
[[nodiscard]] std::optional<QsoWavFormat> parseQsoWav(
    QIODevice& source, QString* error = nullptr);

} // namespace AetherSDR
