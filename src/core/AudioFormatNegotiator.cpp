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
        // Linux: native 24k first, then the common rates.
        case TargetOs::Linux:   return {internalRate, 48000, 44100};
        }
    }
    Q_UNREACHABLE(); // every TargetOs value is covered in both branches above
}

// Format attempt order per direction. Output is Float-first (RX/sidetone/Quindar
// produce float internally; Int16 is the WASAPI Int16-only fallback — #2669).
// Input is Int16-first (mic native is Int16; Float is the virtual-driver /
// Float-only-capture fallback — #1090).
QList<SampleFmt> formatOrder(Direction dir)
{
    return dir == Direction::Output
        ? QList<SampleFmt>{SampleFmt::Float32, SampleFmt::Int16}
        : QList<SampleFmt>{SampleFmt::Int16, SampleFmt::Float32};
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

QList<FormatCandidate> buildLadder(TargetOs os,
                                   Direction dir,
                                   const DeviceCaps& caps,
                                   ResamplerPolicy policy,
                                   int internalRate)
{
    QList<FormatCandidate> ladder;
    const QList<SampleFmt> fmts = formatOrder(dir);

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
    // #2615). preferredFormat is honoured here too.
    const bool preferredFirst =
        (dir == Direction::Input && os == TargetOs::MacOS && caps.preferredRate > 0);
    if (preferredFirst) {
        add(caps.preferredRate, caps.preferredFormat,
            caps.isBluetoothHfp ? QStringLiteral("macOS Bluetooth-HFP native rate (#2615)")
                                : QStringLiteral("macOS mic preferred rate first (#2930)"));
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
                           int internalRate)
{
    const QList<FormatCandidate> ladder = buildLadder(os, dir, caps, policy, internalRate);

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
