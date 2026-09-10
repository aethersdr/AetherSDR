#pragma once

// ─── Audio format / sample-rate negotiation policy ───────────────────────────
//
// One ladder, one set of per-OS rules — the single home for "what rate and
// sample format does this device want, and how do I bridge between that rate
// and the caller's canonical device-boundary rate" (issue #3306).
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
#include <functional>
#include <QString>

namespace AetherSDR {
namespace AudioFormatNegotiator {

// Default device-boundary rate: the radio VITA-49 narrowband audio rate and
// the canonical rate for RX and several digital/legacy routes
// (AudioEngine::DEFAULT_SAMPLE_RATE). This is not a universal DSP rate: normal
// PC-mic voice is normalized to TxVoiceProcessor's fixed 48 kHz float domain,
// then returns to 24 kHz at its current transport/backend seam.
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
    // must be opened at its native low rate (#2615). After capture, normal
    // voice normalizes to 48k; the separate RADE route converts to 24k.
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

// ─── WASAPI silent-open recovery ladder (#2929) ──────────────────────────────
//
// A separate failure mode from the ladders above, and the reason it needs its
// own policy: WASAPI can return a NON-NULL QIODevice that then delivers zero
// bytes for an open the endpoint cannot actually honour. Nothing fails, so the
// null-open fallback ladder never sees it; a watchdog notices the silence after
// ~1.5 s and reopens.
//
// That recovery used to walk ONE dimension — channel count — because a
// mono-only USB PnP mic accepting a stereo open was the only known shape. Once
// capture leads with Float32, the SAMPLE FORMAT becomes a second way to open
// successfully and receive nothing: an Int16-capable endpoint that accepts a
// Float open and returns silence would never reach the format it does support
// (review of PR #5017).
//
// The ladder is ordered so the historical behaviour is preserved exactly: mono
// is still tried before the format changes, because a mono-only mic is the
// common case and Int16 is the rarer one.
struct TxOpenAttempt {
    int       rate = 48000;
    SampleFmt fmt = SampleFmt::Float32;
    int       channels = 2;   // 1 == forced mono

    bool operator==(const TxOpenAttempt& other) const
    {
        return rate == other.rate && fmt == other.fmt && channels == other.channels;
    }
};

// The FULL ordered TX capture attempt sequence for an initial open of
// `initialChannels`. Index 0 is the initial open; everything after it is
// recovery, whichever failure produced it.
//
// This is one ladder because there is one device. It previously WAS two —
// a (format, channels) silent-open ladder at 48 kHz and, in AudioEngine, a
// separate rate x channels x format loop entered only on a null open — and
// they could interleave into a permanently silent mic: at the last stage of
// the silent ladder a null open dropped into the other loop, which restarted
// at 48 kHz Float stereo, a tuple already OBSERVED silent, and accepted it
// with no watchdog budget left to catch it a second time (round-3 review of
// PR #5017).
//
// Order:
//   rate outermost (48000, 44100, 24000, 16000)
//     format next   (Float32, then Int16)
//       channels innermost (the clamped count, then forced mono)
// which keeps the historical 48 kHz prefix exactly: Float32 clamped, Float32
// mono (the #2929 one-reopen recovery), Int16 clamped, Int16 mono. Duplicate
// rungs are collapsed, so a device already clamped to mono gets a shorter
// ladder rather than reopening an identical format.
QList<TxOpenAttempt> txOpenLadder(int initialChannels);

// What a single open attempt did.
//
// Null and SilentNonNull are DELIBERATELY not distinguished by the cursor.
// Both mean "this tuple does not work", and treating them differently is
// precisely what let the two old ladders disagree about where they were.
enum class TxOpenOutcome { Null, SilentNonNull, Delivers };

// The cursor AudioEngine drives, and the one a test drives with simulated
// outcomes — so a test cannot pass against a state machine that is not the
// shipped one.
//
// It moves forward only. That is what makes "never re-accept a tuple already
// observed silent" structural rather than bookkeeping: every such tuple is
// behind the cursor.
class TxOpenCursor {
public:
    explicit TxOpenCursor(int initialChannels, int stage = 0);

    int  stage() const { return m_stage; }
    int  size() const { return static_cast<int>(m_ladder.size()); }
    const TxOpenAttempt& attempt() const { return m_ladder.at(m_stage); }
    const QList<TxOpenAttempt>& ladder() const { return m_ladder; }

    // Is there anywhere left to go? The watchdog arms iff this is true, and
    // it is the same question advance() answers — one rule, not two.
    bool hasNext() const { return m_stage + 1 < size(); }

    // Step to the next attempt. False means the ladder is exhausted, which is
    // a hard failure for a null open and a permanently silent mic for a
    // no-data one.
    bool advance();

private:
    QList<TxOpenAttempt> m_ladder;
    int                  m_stage = 0;
};

// Drive a cursor to completion against a simulated device, returning the stage
// that ends up carrying audio, or -1 if the mic never delivers. Uses the
// cursor's own transitions, so it walks the shipped state machine.
int walkTxOpen(TxOpenCursor& cursor,
               const std::function<TxOpenOutcome(const TxOpenAttempt&)>& probe);

// The host OS as a TargetOs (the ONE place the real #ifdef lives). The live
// wrapper passes this; tests pass an explicit value.
TargetOs hostTargetOs();

const char* toString(SampleFmt f);
const char* toString(ResamplerKind k);
const char* toString(TargetOs os);

} // namespace AudioFormatNegotiator
} // namespace AetherSDR
