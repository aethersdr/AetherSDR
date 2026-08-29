#include "core/AudioFormatNegotiator.h"

// PURE policy implementation — no Qt Multimedia, no live device I/O. Everything
// here is a function of (TargetOs, Direction, DeviceCaps, ResamplerPolicy) so it
// is exhaustively unit-testable on any CI runner (see
// tests/audio_format_negotiation_test.cpp). Keep it that way: any call that
// touches a real QAudioDevice belongs in the live wrapper, not here.

namespace AetherSDR {
namespace AudioFormatNegotiator {

namespace {

// Per-OS preferred rate order. The first entry is what that OS wants by
// default; later entries are progressively more conservative. The universal
// 44.1k rung and the device-preferred rung are appended by buildLadder() so
// every sink gets the SAME complete fallback set (no more "Quindar has no
// fallback / RX never tries 44.1k" divergence — #3306, #3385).
QList<int> primaryRateOrder(TargetOs os, Direction dir, int internalRate)
{
    if (dir == Direction::Output) {
        switch (os) {
        // Windows: force 48k — WASAPI's shared-mode resampler adds artifacts at
        // 24k that become audible once radio-side NR removes the noise floor;
        // r8brain does the clean 24k->48k conversion instead (#2120 / PR #2123).
        case TargetOs::Windows: return {48000, internalRate};
        // macOS: prefer 48k so A2DP-capable Bluetooth devices stay on the normal
        // output profile rather than being routed onto HFP/telephony (#1705).
        case TargetOs::MacOS:   return {48000, internalRate};
        // Linux: native 24k is fine (no WASAPI resampler in the path) — avoid an
        // unnecessary upsample. Deliberate, documented divergence from Win/Mac.
        // RX is still canonically 24 kHz, so this stays 24k-first even though
        // the Linux *input* ladder now leads with 48k for the TX voice strip.
        case TargetOs::Linux:   return {internalRate, 48000};
        }
    } else { // Input (mic / TX capture)
        switch (os) {
        // Windows: WASAPI shared mode converts transparently; try 48k first.
        case TargetOs::Windows: return {48000, 44100, internalRate, 16000};
        // macOS: preferred-rate-first is enforced in buildLadder (CoreAudio lies
        // about isFormatSupported(48000) for 16k-native / BT-HFP mics — #2930 /
        // #2615); the remaining order is the conservative ladder.
        case TargetOs::MacOS:   return {48000, 44100, internalRate, 16000};
        // Linux: 48k first. Normal voice TX is normalized to TxVoiceProcessor's
        // fixed 48 kHz DSP domain, so opening the mic at 48k is what removes the
        // ingress SRC. This inverts the pre-48k-strip rule, where native 24k
        // capture was preferred precisely because it meant no conversion at all.
        // A mic that cannot open 48k still lands on 24k at the next rung.
        case TargetOs::Linux:   return {48000, internalRate, 44100};
        }
    }
    Q_UNREACHABLE(); // every TargetOs value is covered in both branches above
}

// Format attempt order. A caller hint (FormatPreference) overrides the default;
// otherwise output is Float-first (RX/sidetone/Quindar produce float internally;
// Int16 is the WASAPI Int16-only fallback — #2669) and input is Int16-first (mic
// native is Int16; Float is the virtual-driver / Float-only fallback — #1090).
// Int16-native playback sinks (QSO/Pudu) pass Int16First so they avoid a
// conversion on normal devices while still falling back to Float.
QList<SampleFmt> formatOrder(TargetOs os, Direction dir, FormatPreference pref)
{
    if (pref == FormatPreference::Int16First)
        return {SampleFmt::Int16, SampleFmt::Float32};
    if (pref == FormatPreference::Float32First)
        return {SampleFmt::Float32, SampleFmt::Int16};
    if (dir == Direction::Output)
        return {SampleFmt::Float32, SampleFmt::Int16};

    // Input. Same inversion the 48 kHz strip forced on the RATE ladder, applied
    // to the FORMAT ladder. Int16-first was right while the DSP island was
    // Int16: the mic's own samples are integer, so asking for Int16 meant no
    // conversion anywhere. TxVoiceProcessor is float now, and every host engine
    // we open through already mixes in float — WASAPI's shared mix is float32 by
    // construction, and converts to integer only at the app boundary
    // (learn.microsoft.com/windows/win32/coreaudio/device-formats); PipeWire and
    // PulseAudio likewise. So Int16 capture buys a quantization on the way in
    // and processCapturedFloat32() would widen it straight back — a round trip
    // at the ENTRANCE of a chain whose stated design is "become float once,
    // quantize once at the 24 kHz transport boundary".
    //
    // Int16 stays the next rung, so an Int16-only endpoint negotiates exactly as
    // it did before.
    switch (os) {
    case TargetOs::Windows:
    case TargetOs::Linux:
        return {SampleFmt::Float32, SampleFmt::Int16};
    // macOS deliberately keeps Int16-first. The argument above applies to
    // CoreAudio too, but this change only measures Windows and Linux, and the
    // mac input ladder additionally leads with the device's preferredFormat for
    // BT-HFP / 16k-native mics (#2615 / #2930). Left for a follow-up with real
    // hardware behind it rather than changed blind.
    case TargetOs::MacOS:
        return {SampleFmt::Int16, SampleFmt::Float32};
    }
    Q_UNREACHABLE();
}

bool ladderHas(const QList<FormatCandidate>& ladder, int rate, SampleFmt fmt)
{
    for (const auto& c : ladder) {
        if (c.rate == rate && c.fmt == fmt) return true;
    }
    return false;
}

} // namespace

ResamplerKind resamplerKindFor(int deviceRate, ResamplerPolicy policy, int internalRate)
{
    if (deviceRate == internalRate) return ResamplerKind::None;
    switch (policy) {
    case ResamplerPolicy::RegenerateAtRate: return ResamplerKind::None;
    case ResamplerPolicy::PreservePan:      return ResamplerKind::PreservePan;
    case ResamplerPolicy::MonoCollapse:     return ResamplerKind::MonoCollapse;
    }
    return ResamplerKind::None;
}

QList<TxOpenAttempt> txOpenLadder(int initialChannels)
{
    // Stage 0 must reproduce exactly what AudioEngine opens first: 48 kHz
    // Float32 at whatever channel count survived the maximumChannelCount()
    // clamp. Everything after it is recovery.
    const int initial = (initialChannels <= 1) ? 1 : 2;

    QList<TxOpenAttempt> ladder;
    const auto add = [&](int rate, SampleFmt fmt, int channels) {
        const TxOpenAttempt candidate{rate, fmt, channels};
        for (const auto& existing : ladder) {
            if (existing == candidate) return;
        }
        ladder.append(candidate);
    };

    // Rate outermost, then format, then channels. Within 48 kHz that reads
    // Float32/clamped, Float32/mono, Int16/clamped, Int16/mono — the exact
    // order the #2929 recovery had, so a merely mono-only mic still recovers
    // in ONE reopen and the format dimension only costs extra reopens on
    // devices that actually need it.
    //
    // The lower rates are the rungs the null-open path used to own as a
    // separate loop. They are here rather than there because a rung reached by
    // a null open and a rung reached by a silent open are the same rung, and
    // the bug that produced this restructure was two sequences disagreeing
    // about which one they were on.
    for (int rate : {48000, 44100, 24000, 16000}) {
        for (SampleFmt fmt : {SampleFmt::Float32, SampleFmt::Int16}) {
            add(rate, fmt, initial);
            add(rate, fmt, 1);
        }
    }
    return ladder;
}

TxOpenCursor::TxOpenCursor(int initialChannels, int stage)
    : m_ladder(txOpenLadder(initialChannels))
{
    // A persisted stage from a previous pass is clamped rather than trusted:
    // the ladder's LENGTH depends on the channel clamp, so a stage carried
    // across a device change could otherwise index past the end.
    m_stage = qBound(0, stage, static_cast<int>(m_ladder.size()) - 1);
}

bool TxOpenCursor::advance()
{
    if (!hasNext())
        return false;
    ++m_stage;
    return true;
}

int walkTxOpen(TxOpenCursor& cursor,
               const std::function<TxOpenOutcome(const TxOpenAttempt&)>& probe)
{
    for (;;) {
        const TxOpenOutcome outcome = probe(cursor.attempt());
        if (outcome == TxOpenOutcome::Delivers)
            return cursor.stage();
        // Null and SilentNonNull take the same branch on purpose — see the
        // enum's comment. If the ladder is exhausted the mic never opens
        // (null) or never speaks (silent); either way there is nothing left.
        if (!cursor.advance())
            return -1;
    }
}

QList<FormatCandidate> buildLadder(TargetOs os,
                                   Direction dir,
                                   const DeviceCaps& caps,
                                   ResamplerPolicy policy,
                                   int internalRate,
                                   FormatPreference pref)
{
    QList<FormatCandidate> ladder;
    const QList<SampleFmt> fmts = formatOrder(os, dir, pref);

    const auto add = [&](int rate, SampleFmt fmt, const QString& reason) {
        if (rate <= 0) return;
        if (ladderHas(ladder, rate, fmt)) return;
        FormatCandidate c;
        c.rate = rate;
        c.fmt = fmt;
        c.channels = 2;
        c.resampler = resamplerKindFor(rate, policy, internalRate);
        c.reason = reason;
        ladder.append(c);
    };

    // macOS / preferred-first inputs: the device's own preferred rate leads the
    // ladder so we never force a 16k-native or BT-HFP mic up to 48k (#2930 /
    // #2615).
    //
    // This rung exists for the RATE. Taking caps.preferredFormat with it would
    // quietly hand macOS a different FORMAT policy than formatOrder() states,
    // because CoreAudio inputs report Float as their preferred format — so
    // every Mac mic advertising a preferred rate would capture Float32 while
    // formatOrder() below still says macOS leads with Int16. That contradiction
    // was invisible while AudioEngine discarded every non-Int16 rung itself;
    // once the engine honours the rung, it decides macOS behaviour (review of
    // PR #5017). Lead with the per-OS format order instead, and let the
    // preferredFormat catch-all rung at the bottom keep Float32-only endpoints
    // working — strictly more capable than the engine-side filter it replaces.
    const bool preferredFirst =
        (dir == Direction::Input && os == TargetOs::MacOS && caps.preferredRate > 0);
    if (preferredFirst) {
        const QString reason =
            caps.isBluetoothHfp ? QStringLiteral("macOS Bluetooth-HFP native rate (#2615)")
                                : QStringLiteral("macOS mic preferred rate first (#2930)");
        for (SampleFmt fmt : fmts) {
            add(caps.preferredRate, fmt, reason);
        }
    }

    // Main per-OS rate order × format order.
    const QList<int> rates = primaryRateOrder(os, dir, internalRate);
    for (int rate : rates) {
        for (SampleFmt fmt : fmts) {
            QString reason = (rate == rates.first())
                ? QStringLiteral("%1 preferred %2 Hz").arg(toString(os)).arg(rate)
                : QStringLiteral("fallback %1 Hz").arg(rate);
            add(rate, fmt, reason);
        }
    }

    // Universal 44.1k rung — historically present only on the mic/CW paths, so a
    // 44.1k-only device silently failed on RX/Quindar/QSO. Now every sink has it
    // (#3385 / #3306 regression guard).
    for (SampleFmt fmt : fmts) {
        add(44100, fmt, QStringLiteral("universal 44.1 kHz fallback (#3385)"));
    }

    // Final rung: the device's own preferred format. For awkward backends
    // (CommonRadioAudio / BlackHole Float32-only, WASAPI Float32-only) this is
    // the catch-all that always opens (#1090 / #3231).
    if (caps.preferredRate > 0) {
        add(caps.preferredRate, caps.preferredFormat,
            QStringLiteral("device preferredFormat catch-all (#3231)"));
    }

    return ladder;
}

NegotiatedFormat negotiate(TargetOs os,
                           Direction dir,
                           const DeviceCaps& caps,
                           ResamplerPolicy policy,
                           int internalRate,
                           FormatPreference pref)
{
    const QList<FormatCandidate> ladder = buildLadder(os, dir, caps, policy, internalRate, pref);

    const bool reliable = caps.isFormatSupportedReliable;
    const bool nothingProbeable = caps.supportedRates.isEmpty();

    const auto rungSupported = [&](const FormatCandidate& c, int index) -> bool {
        if (!reliable) {
            // Probe-at-open backends (WASAPI): we cannot trust isFormatSupported.
            // If we know nothing about the device, optimistically take the first
            // (preferred) rung and let the device decide the format at open.
            if (nothingProbeable) return index == 0;
            return caps.supportedRates.contains(c.rate);
        }
        if (!caps.supportedRates.contains(c.rate)) return false;
        if (!caps.supportedFormats.contains(c.fmt)) return false;
        // A mono-only device still satisfies a stereo rung — we open at the
        // device's channel count and downmix (documented mono behaviour).
        return caps.channels >= 1;
    };

    for (int i = 0; i < ladder.size(); ++i) {
        const FormatCandidate& c = ladder.at(i);
        if (!rungSupported(c, i)) continue;
        NegotiatedFormat out;
        out.ok = true;
        out.rate = c.rate;
        out.fmt = c.fmt;
        // Open at the device's channel count when it has fewer than requested
        // (mono device -> downmix). Probe-at-open backends keep the requested
        // count since we don't have a trustworthy channel report.
        out.channels = (reliable && caps.channels < c.channels) ? caps.channels : c.channels;
        out.resampler = c.resampler;
        out.fellBack = (i != 0);
        out.reason = c.reason;
        return out;
    }

    NegotiatedFormat out;
    out.ok = false;
    out.reason = QStringLiteral("device supports no rung in the negotiation ladder");
    return out;
}

TargetOs hostTargetOs()
{
#if defined(Q_OS_WIN)
    return TargetOs::Windows;
#elif defined(Q_OS_MAC)
    return TargetOs::MacOS;
#else
    return TargetOs::Linux;
#endif
}

const char* toString(SampleFmt f)
{
    switch (f) {
    case SampleFmt::Int16:   return "Int16";
    case SampleFmt::Float32: return "Float32";
    }
    return "?";
}

const char* toString(ResamplerKind k)
{
    switch (k) {
    case ResamplerKind::None:         return "None";
    case ResamplerKind::PreservePan:  return "PreservePan";
    case ResamplerKind::MonoCollapse: return "MonoCollapse";
    }
    return "?";
}

const char* toString(TargetOs os)
{
    switch (os) {
    case TargetOs::Windows: return "Windows";
    case TargetOs::MacOS:   return "macOS";
    case TargetOs::Linux:   return "Linux";
    }
    return "?";
}

} // namespace AudioFormatNegotiator
} // namespace AetherSDR
