#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

// Turns a user's WAV file into the audio a Flex DVK slot plays correctly.
//
// The radio does not resample an uploaded slot: it plays the samples at its
// 24 kHz codec rate (measured on a FLEX-8600, fw 4.2.20 — a 2 s 48 kHz file
// previews for 4 s) and keeps only the first channel. So every import is
// decoded, all channels averaged into one, resampled to 24 kHz and written as
// 16-bit mono PCM.
namespace DvkWavConverter {

inline constexpr int kRadioSampleRate = 24'000;
// The radio stops a recording at 10 s; a longer import is refused, not cut.
inline constexpr int kMaxDurationMs = 10'000;

// Accepts PCM 8/16/24/32-bit and 32-bit float (plain or WAVE_FORMAT_EXTENSIBLE),
// 1 to 8 channels, 8 to 384 kHz, any chunk order. Returns a 24 kHz mono 16-bit PCM WAV, or
// an empty array with a user-facing reason in `error`.
QByteArray convertForRadio(const QByteArray& wav, QString& error);

}  // namespace DvkWavConverter

}  // namespace AetherSDR
