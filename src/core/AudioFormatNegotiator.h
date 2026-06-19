#pragma once

// ─── Audio format / sample-rate negotiation policy ───────────────────────────
//
// One ladder, one set of per-OS rules — the single home for "what rate and
// sample format does this device want, and how do I get my 24 kHz canonical
// audio onto it" (issue #3306).
//
// Historically each audio sink/source re-implemented this with its own
// divergent fallback ladder and per-OS `#ifdef` branches, which is the root of
// a cluster of platform-specific audio bugs (44.1k-only devices silently
// failing on some sinks, WASAPI Float32-only devices rejecting Int16, macOS
// Bluetooth-HFP mics delivering silence, etc.).
//
// DESIGN CONSTRAINT (testability): this layer is a PURE function over an
// *injected* capability snapshot (`DeviceCaps`) with the target OS passed in as
// a PARAMETER, never an `#ifdef`. That lets a single headless test binary,
// built once on any CI runner, exercise every OS's ladder against every device
// shape — the reason the historical bugs escaped CI. The thin live wrapper
// (see AudioDeviceNegotiator, Qt-Multimedia) is the only platform-specific part.
//
// This header deliberately depends on nothing beyond Qt Core (QString/QList) so
// the policy can be unit-tested by an executable that links only Qt6::Core.
// QAudioFormat (Qt Multimedia) is intentionally NOT used here; the live wrapper
// converts SampleFmt <-> QAudioFormat::SampleFormat.

#include <QList>
#include <QString>

namespace AetherSDR {
namespace AudioFormatNegotiator {

// Canonical internal rate: the radio VITA-49 narrowband audio rate. Everything
// resamples to/from this single value (AudioEngine::DEFAULT_SAMPLE_RATE).
constexpr int kInternalRate = 24000;

// Target OS is data, not an #ifdef, so every runner tests every ladder.
enum class TargetOs { Windows, MacOS, Linux };

// Output = playback sink (RX speaker, sidetone, monitor, QSO playback, Quindar).
// Input  = capture source (PC mic / TX).
enum class Direction { Output, Input };

// Dependency-free mirror of QAudioFormat::SampleFormat (the live wrapper maps
// these to/from the Qt enum). Only the formats AetherSDR opens are modelled.
enum class SampleFmt { Int16, Float32 };

// Which resampler strategy converts between the device rate and kInternalRate.
//   None         — sink regenerates/consumes natively at the device rate
//                  (CW sidetone, Quindar tone), or rate already == kInternalRate.
//   PreservePan  — dual independent L/R r8brain instances; keeps VITA-49 per-
//                  channel pan intact. REQUIRED for RX speaker and QSO playback
//                  (collapsing to mono here regressed pan: #2403 / PR #2459).
//   MonoCollapse — Resampler::processStereoToStereo (downmix→resample→duplicate).
//                  Correct ONLY where the payload is inherently mono: TCI DAX TX,
//                  RADE modem. MUST NOT be used for RX/QSO.
// These two stereo strategies are deliberately distinct and must never be
// unified (the conflation that caused #2403).
enum class ResamplerKind { None, PreservePan, MonoCollapse };

// How a particular sink/source treats sample rate — drives ResamplerKind.
enum class ResamplerPolicy {
    RegenerateAtRate, // generator retunes to the device rate -> always None
    PreservePan,      // RX speaker / QSO playback -> PreservePan when rate != internal
    MonoCollapse,     // TCI DAX TX / RADE -> MonoCollapse when rate != internal
};

// Optional caller hint to lead the format order with a specific sample format.
//   Auto         — keep the per-direction default (Float-first output for
//                  float-native sinks like RX/sidetone; Int16-first input).
//   Int16First   — for Int16-native sinks (QSO/Pudu playback, recorded int16):
//                  prefer Int16 on normal devices so no conversion is needed,
//                  with Float as the fallback for Float-only endpoints.
//   Float32First — explicit float-first (== Auto for output today; here for
//                  symmetry/clarity).
enum class FormatPreference { Auto, Int16First, Float32First };

// Injected capability snapshot — everything the policy would otherwise read
// live from a QAudioDevice / PortAudio / the HAL. Pure inputs only.
struct DeviceCaps {
    // Rates the device genuinely supports (what isFormatSupported() *should*
    // report). Empty == nothing probeable (treat like an unreliable backend).
    QList<int>       supportedRates;
    // Sample formats the device's shared-mix / hardware accepts. On WASAPI a
    // Float32-only shared format rejects Int16 regardless of hardware (#3231).
    QList<SampleFmt> supportedFormats{SampleFmt::Float32, SampleFmt::Int16};
    int  channels = 2;

    // macOS Bluetooth hands-free/SCO capture route: caps out at 8/16/24k and
    // must be opened at its native low rate, NOT forced to 48k (#2615).
    bool isBluetoothHfp = false;

    // False => isFormatSupported() is not trustworthy for this backend, so the
    // caller must skip the probe and try-at-open instead. True for WASAPI on
    // Windows (#2120 / #2929 / Voicemeeter / FlexRadio DAX false-negatives).
    bool isFormatSupportedReliable = true;

    // Device's own preferred format (QAudioDevice::preferredFormat) — the final
    // ladder rung for awkward devices (#1090 / #3231). 0 == unknown.
    int       preferredRate = 0;
    SampleFmt preferredFormat = SampleFmt::Float32;
};

// One rung of the fallback ladder: a concrete format to attempt, in order.
struct FormatCandidate {
    int           rate = kInternalRate;
    SampleFmt     fmt = SampleFmt::Float32;
    int           channels = 2;
    ResamplerKind resampler = ResamplerKind::None;
    QString       reason;   // human-readable rung label for diagnostics/logs
};

// The negotiated outcome (the rung that won), plus context.
struct NegotiatedFormat {
    bool          ok = false;
    int           rate = kInternalRate;
    SampleFmt     fmt = SampleFmt::Float32;
    int           channels = 2;
    ResamplerKind resampler = ResamplerKind::None;
    bool          fellBack = false;       // true if not the first/preferred rung
    QString       reason;                 // why this rung was chosen
};

// Build the ordered fallback ladder for a direction. The first rung is the
// per-OS preferred format; later rungs are progressively more conservative,
// always ending with the universal rungs (44.1k + device preferredFormat) so
// no sink is left with "no fallback" the way Quindar historically was.
//
// `policy` selects PreservePan vs MonoCollapse vs RegenerateAtRate for the
// resampler kind attached to each non-internal-rate rung.
QList<FormatCandidate> buildLadder(TargetOs os,
                                   Direction dir,
                                   const DeviceCaps& caps,
                                   ResamplerPolicy policy,
                                   int internalRate = kInternalRate,
                                   FormatPreference pref = FormatPreference::Auto);

// Resolve the ladder against the device's actual capabilities — the PURE
// equivalent of walking the ladder and opening the device. When
// caps.isFormatSupportedReliable is true a rung "succeeds" iff its (rate,fmt,
// channels) is supported; when false (probe-at-open backends) a rung succeeds
// iff its rate is in supportedRates (format is decided by the device at open).
// Returns ok=false only if the device truly supports nothing in the ladder.
NegotiatedFormat negotiate(TargetOs os,
                           Direction dir,
                           const DeviceCaps& caps,
                           ResamplerPolicy policy,
                           int internalRate = kInternalRate,
                           FormatPreference pref = FormatPreference::Auto);

// The resampler kind for a concrete (deviceRate, policy) pair, independent of
// the ladder — used by sinks that already know their negotiated rate.
ResamplerKind resamplerKindFor(int deviceRate,
                               ResamplerPolicy policy,
                               int internalRate = kInternalRate);

// The host OS as a TargetOs (the ONE place the real #ifdef lives). The live
// wrapper passes this; tests pass an explicit value.
TargetOs hostTargetOs();

const char* toString(SampleFmt f);
const char* toString(ResamplerKind k);
const char* toString(TargetOs os);

} // namespace AudioFormatNegotiator
} // namespace AetherSDR
