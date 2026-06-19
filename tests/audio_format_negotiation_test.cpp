// Golden matrix for the consolidated audio format/rate negotiation policy
// (issue #3306). The whole point of consolidation is that the negotiation
// POLICY becomes testable in one headless place, so the entire class of
// "44.1k device silently fails on sink X / OS Y" bugs is caught on CI rather
// than in the field.
//
// Because TargetOs is DATA (not an #ifdef), every cell below runs on every CI
// runner: a Linux runner proves the Windows and macOS ladders too.
//
// Run: ./build/audio_format_negotiation_test

#include "core/AudioFormatNegotiator.h"

#include <cstdio>
#include <string>

using namespace AetherSDR::AudioFormatNegotiator;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    ++g_total;
    std::printf("%s %-70s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    if (!ok) ++g_failed;
}

std::string fmtOf(const NegotiatedFormat& n)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ok=%d rate=%d fmt=%s ch=%d resampler=%s fellBack=%d",
                  n.ok ? 1 : 0, n.rate, toString(n.fmt), n.channels,
                  toString(n.resampler), n.fellBack ? 1 : 0);
    return buf;
}

// One golden row: negotiate(os,dir,caps,policy) must equal the expected fields.
struct Row {
    std::string name;
    TargetOs    os;
    Direction   dir;
    ResamplerPolicy policy;
    DeviceCaps  caps;
    // expected
    bool          ok;
    int           rate;
    SampleFmt     fmt;
    ResamplerKind resampler;
    int           channels;
    bool          fellBack;
    FormatPreference pref = FormatPreference::Auto;   // optional caller hint
};

void runRow(const Row& r)
{
    const NegotiatedFormat n = negotiate(r.os, r.dir, r.caps, r.policy, kInternalRate, r.pref);
    const bool ok = n.ok == r.ok
                 && (!r.ok || (n.rate == r.rate && n.fmt == r.fmt
                               && n.resampler == r.resampler && n.channels == r.channels
                               && n.fellBack == r.fellBack));
    std::string detail = "got [" + fmtOf(n) + "]";
    report(r.name, ok, ok ? std::string() : detail);
}

// Helpers to build common device shapes.
DeviceCaps dev(QList<int> rates,
               QList<SampleFmt> fmts = {SampleFmt::Float32, SampleFmt::Int16},
               int channels = 2)
{
    DeviceCaps c;
    c.supportedRates = std::move(rates);
    c.supportedFormats = std::move(fmts);
    c.channels = channels;
    if (!c.supportedRates.isEmpty()) {
        c.preferredRate = c.supportedRates.first();
        c.preferredFormat = c.supportedFormats.contains(SampleFmt::Float32)
                                ? SampleFmt::Float32 : SampleFmt::Int16;
    }
    return c;
}

} // namespace

int main()
{
    const auto F = SampleFmt::Float32;
    const auto I = SampleFmt::Int16;
    const auto Pan = ResamplerPolicy::PreservePan;
    const auto Mono = ResamplerPolicy::MonoCollapse;
    const auto Regen = ResamplerPolicy::RegenerateAtRate;
    const auto Out = Direction::Output;
    const auto In = Direction::Input;
    const auto Win = TargetOs::Windows;
    const auto Mac = TargetOs::MacOS;
    const auto Lin = TargetOs::Linux;

    // ── Standard 48k-only DAC, RX speaker (PreservePan) ───────────────────────
    runRow({"std 48k DAC / Win / RX",  Win, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"std 48k DAC / Mac / RX",  Mac, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    // Linux prefers 24k first; 48k-only device -> falls back to 48k (fellBack).
    runRow({"std 48k DAC / Linux / RX", Lin, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, true});

    // ── 24k-capable device: documents the INTENDED per-OS divergence ──────────
    runRow({"24k+48k dev / Win / RX -> forces 48k (#2120)", Win, Out, Pan, dev({24000, 48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"24k+48k dev / Mac / RX -> 48k (A2DP #1705)",   Mac, Out, Pan, dev({24000, 48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"24k+48k dev / Linux / RX -> native 24k (no resample)", Lin, Out, Pan, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── 44.1k-ONLY device: the regression guard. Today RX/Quindar FAIL this;
    //    the unified ladder must succeed on every OS (#3385 / #3306). ──────────
    runRow({"44.1k-only / Win / RX",   Win, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});
    runRow({"44.1k-only / Mac / RX",   Mac, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});
    runRow({"44.1k-only / Linux / RX", Lin, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});

    // ── WASAPI Int16-only device: must skip the Float rungs (#2669). The
    //    skipped 48k Float rung counts as a fall-back (fellBack=true). ────────
    runRow({"Int16-only WASAPI / Win / RX", Win, Out, Pan, dev({48000}, {I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, true});

    // ── WASAPI false-negative isFormatSupported: nothing probeable, probe at
    //    open -> take the preferred 48k rung (#2120 / #3231) ──────────────────
    {
        DeviceCaps c;                       // supportedRates empty
        c.isFormatSupportedReliable = false;
        runRow({"WASAPI false-negative / Win / RX -> force 48k probe-at-open",
                Win, Out, Pan, c,
                true, 48000, F, ResamplerKind::PreservePan, 2, false});
    }

    // ── CW sidetone (RegenerateAtRate): no resampler, generator retunes ───────
    runRow({"sidetone 48k dev / Mac -> regenerate, no resampler", Mac, Out, Regen, dev({48000}),
            true, 48000, F, ResamplerKind::None, 2, false});
    runRow({"sidetone 24k-cap / Linux -> 24k, no resampler", Lin, Out, Regen, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── Int16-first output (Pudu/QSO playback are Int16-native, #3306 6b). On a
    //    normal device they get Int16 — no conversion — at the per-OS rate;
    //    Win/Mac prefer 48k (dodges the WASAPI 24k artifacts #2120, same as RX),
    //    Linux stays native 24k. On a Float-only device they fall back to Float
    //    (then the sink's Int16->Float guard #3231 converts). ───────────────────
    runRow({"Int16-native / Mac / playback -> 48k Int16", Mac, Out, Pan, dev({24000, 48000}),
            true, 48000, I, ResamplerKind::PreservePan, 2, false, FormatPreference::Int16First});
    runRow({"Int16-native / Linux / playback -> native 24k Int16", Lin, Out, Pan, dev({24000, 48000}),
            true, 24000, I, ResamplerKind::None, 2, false, FormatPreference::Int16First});
    runRow({"Int16-native / Mac / Float-only dev -> Float fallback (#3231 guard)",
            Mac, Out, Pan, dev({48000}, {F}),
            true, 48000, F, ResamplerKind::PreservePan, 2, true, FormatPreference::Int16First});

    // ── macOS Bluetooth-HFP mic: native low rate first, NOT forced to 48k
    //    (#2615). preferred-first puts 16k ahead of the 48k ladder. ───────────
    {
        DeviceCaps c = dev({8000, 16000, 24000}, {I});
        c.isBluetoothHfp = true;
        c.preferredRate = 16000;
        c.preferredFormat = I;
        // preferred-first means the native rate is the PRIMARY choice, not a
        // fallback -> fellBack=false.
        runRow({"BT-HFP mic / Mac / TX -> native 16k (#2615)", Mac, In, Pan, c,
                true, 16000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── macOS 16k-native mic that lies about 48k support (#2930): preferred
    //    rate first avoids the silent-48k-open trap. ──────────────────────────
    {
        DeviceCaps c = dev({16000, 48000}, {I});
        c.preferredRate = 16000;
        c.preferredFormat = I;
        runRow({"16k-native mic / Mac / TX -> preferred 16k first (#2930)", Mac, In, Pan, c,
                true, 16000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── Linux mic standard: native 24k, no resample ──────────────────────────
    runRow({"std mic / Linux / TX -> 24k native", Lin, In, Pan, dev({24000, 48000}, {I}),
            true, 24000, I, ResamplerKind::None, 2, false});

    // ── Windows mic: probe-at-open, 48k first (#2929) ─────────────────────────
    {
        DeviceCaps c;
        c.isFormatSupportedReliable = false;
        runRow({"std mic / Win / TX -> force 48k probe-at-open", Win, In, Pan, c,
                true, 48000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── Mono-only output device: open at device channel count, downmix ────────
    runRow({"mono-only 48k / Win / RX -> ch=1 downmix", Win, Out, Pan, dev({48000}, {F, I}, 1),
            true, 48000, F, ResamplerKind::PreservePan, 1, false});

    // ── TCI DAX TX / RADE use MonoCollapse, never PreservePan ─────────────────
    runRow({"TCI-TX 48k client / Linux -> MonoCollapse to 24k", Lin, Out, Mono, dev({48000}),
            true, 48000, F, ResamplerKind::MonoCollapse, 2, true});
    // TCI client negotiates 24k then transmits: rate already internal -> None
    // (NOT a hardcoded 48000->24000 mis-resample; the TciServer.cpp:1296 bug).
    runRow({"TCI-TX 24k client / Linux -> no resample", Lin, Out, Mono, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── Total failure: device supports nothing usable ────────────────────────
    runRow({"empty reliable device -> negotiation fails", Lin, Out, Pan, dev({}, {F, I}),
            false, 0, F, ResamplerKind::None, 0, false});

    // ── resamplerKindFor unit checks (the two stereo strategies stay distinct).
    report("resamplerKindFor 24k Pan == None",
           resamplerKindFor(24000, Pan) == ResamplerKind::None);
    report("resamplerKindFor 48k Pan == PreservePan",
           resamplerKindFor(48000, Pan) == ResamplerKind::PreservePan);
    report("resamplerKindFor 48k Mono == MonoCollapse",
           resamplerKindFor(48000, Mono) == ResamplerKind::MonoCollapse);
    report("resamplerKindFor 8k Regen == None",
           resamplerKindFor(8000, Regen) == ResamplerKind::None);

    std::printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
