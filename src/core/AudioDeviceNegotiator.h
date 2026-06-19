#pragma once

// ─── Live Qt-Multimedia wrapper around the pure AudioFormatNegotiator ─────────
//
// This is the ONLY platform-specific part of the audio format/rate negotiation
// stack. It builds a DeviceCaps snapshot from a real QAudioDevice and runs the
// pure policy (AudioFormatNegotiator) against it, returning a ready-to-open
// QAudioFormat plus the resampler strategy. Sinks/sources call this instead of
// hand-rolling their own per-OS ladder (issue #3306).
//
// Keep all live device I/O here; the policy itself stays pure and headless-
// testable in AudioFormatNegotiator. See docs/audio-sink-factory.md.

#include "core/AudioFormatNegotiator.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QList>

namespace AetherSDR {
namespace AudioDeviceNegotiator {

// Probe a real device into the pure policy's injected capability snapshot.
// `bluetoothHfp` is supplied by the caller (the CoreAudio-HAL detection that
// already lives in AudioEngine), since it can't be derived from QAudioDevice.
// `preferredRateOverride` (>0) replaces the device's reported preferred rate —
// used on macOS to put a Bluetooth-HFP mic's HAL-native rate first (#2615),
// which QAudioDevice::preferredFormat() does not expose.
AudioFormatNegotiator::DeviceCaps probe(
    const QAudioDevice& dev,
    AudioFormatNegotiator::Direction dir,
    AudioFormatNegotiator::TargetOs os = AudioFormatNegotiator::hostTargetOs(),
    bool bluetoothHfp = false,
    int preferredRateOverride = 0);

struct Result {
    bool                                 ok = false;
    QAudioFormat                         format;   // hand straight to QAudioSink/Source
    AudioFormatNegotiator::ResamplerKind resampler = AudioFormatNegotiator::ResamplerKind::None;
    bool                                 fellBack = false;
    QString                              reason;
};

// Negotiate against a real device (reliable backends resolve fully here; for
// probe-at-open backends the returned format is the preferred first rung — use
// formatLadder() to walk the fallbacks at open).
Result negotiate(
    const QAudioDevice& dev,
    AudioFormatNegotiator::Direction dir,
    AudioFormatNegotiator::ResamplerPolicy policy,
    AudioFormatNegotiator::TargetOs os = AudioFormatNegotiator::hostTargetOs(),
    int internalRate = AudioFormatNegotiator::kInternalRate,
    bool bluetoothHfp = false,
    AudioFormatNegotiator::FormatPreference pref = AudioFormatNegotiator::FormatPreference::Auto);

// The full ordered Qt-format ladder, for backends where isFormatSupported()
// is unreliable and the caller must try-at-open (Windows WASAPI), or for sinks
// that walk-and-probe themselves (Quindar/Pudu/QSO). `pref` leads the format
// order — Int16-native playback sinks pass Int16First.
QList<QAudioFormat> formatLadder(
    const QAudioDevice& dev,
    AudioFormatNegotiator::Direction dir,
    AudioFormatNegotiator::ResamplerPolicy policy,
    AudioFormatNegotiator::TargetOs os = AudioFormatNegotiator::hostTargetOs(),
    int internalRate = AudioFormatNegotiator::kInternalRate,
    bool bluetoothHfp = false,
    int preferredRateOverride = 0,
    AudioFormatNegotiator::FormatPreference pref = AudioFormatNegotiator::FormatPreference::Auto);

QAudioFormat::SampleFormat       toQt(AudioFormatNegotiator::SampleFmt f);
AudioFormatNegotiator::SampleFmt fromQt(QAudioFormat::SampleFormat f);

// Build a concrete QAudioFormat from a negotiator candidate/result.
QAudioFormat makeFormat(int rate, AudioFormatNegotiator::SampleFmt fmt, int channels);

} // namespace AudioDeviceNegotiator
} // namespace AetherSDR
