// Unit test for the demo-mode NoiseMixer (RFC #4288, Phase 2b — audio engine).
// Pins the contract: frame length, additive mixing + soft-clip bounds, notch
// (TNF/ANF) killing a tone in the audio AND carving it from the spectrum, ANF
// detection finding tonal channels but NOT broadband noise, and noise rendering
// that rises FROM the display floor (not a band floating above it).

#include "core/backends/sim/NoiseMixer.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using AetherSDR::NoiseMixer;
using Channel = NoiseMixer::Channel;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

// Goertzel-ish magnitude of buf at frequency f (for tone before/after checks).
double magAt(const QVector<float>& buf, double f)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < buf.size(); ++i) {
        static const double kPi = 3.14159265358979323846;
        const double w = 2.0 * kPi * f * i / NoiseMixer::kSampleRate;
        re += buf[i] * std::cos(w);
        im += buf[i] * std::sin(w);
    }
    return std::sqrt(re * re + im * im) / buf.size();
}

QVector<float> collect(NoiseMixer& mx, int frames)
{
    QVector<float> out;
    for (int f = 0; f < frames; ++f) out += mx.mixFrame();
    return out;
}

void testFrameLengthAndBounds()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::White, true);
    mx.setLevelDb(Channel::White, -6.0);
    const QVector<float> f = mx.mixFrame();
    const bool lenOk = f.size() == NoiseMixer::kFrameLen;
    const bool bounded = std::all_of(f.cbegin(), f.cend(),
                                     [](float v) { return v >= -1.0f && v <= 1.0f; });
    report("mixFrame length == kFrameLen", lenOk);
    report("mix soft-clipped into [-1,1]", bounded);
}

void testSilentWhenDisabled()
{
    NoiseMixer mx;
    const QVector<float> f = mx.mixFrame();
    report("silent with no channel enabled",
           std::all_of(f.cbegin(), f.cend(), [](float v) { return v == 0.0f; }));
}

void testAdditive()
{
    // Square in double, not float: `v * v` in float precision would round (and
    // in principle overflow) before the widening add into the accumulator.
    NoiseMixer a; a.setEnabled(Channel::White, true); a.setLevelDb(Channel::White, -20.0);
    double ra = 0; for (float v : collect(a, 8)) ra += static_cast<double>(v) * v;
    NoiseMixer b; b.setEnabled(Channel::White, true); b.setLevelDb(Channel::White, -20.0);
    b.setEnabled(Channel::Pink, true); b.setLevelDb(Channel::Pink, -20.0);
    double rb = 0; for (float v : collect(b, 8)) rb += static_cast<double>(v) * v;
    report("adding a channel raises total power", rb > ra);
}

void testAnfDetectionFindsTonesNotNoise()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::Birdie, true); mx.setKnob(Channel::Birdie, "hz", 1500.0);
    mx.setEnabled(Channel::Pink, true);   // broadband — must NOT be detected
    const auto tones = mx.autoNotchTones();
    const bool one = tones.size() == 1;
    const bool isBirdie = one && std::abs(tones[0].hz - 1500.0) < 1.0;
    report("ANF detects the birdie tone only (not pink noise)", one && isBirdie);
}

void testNotchKillsAudioTone()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::Birdie, true);
    mx.setLevelDb(Channel::Birdie, -6.0);
    mx.setKnob(Channel::Birdie, "hz", 1500.0);
    collect(mx, 20);                                  // warm up
    const double before = magAt(collect(mx, 32), 1500.0);
    mx.setNotches(mx.autoNotchTones());               // ANF engages
    collect(mx, 30);                                  // let biquad settle
    const double after = magAt(collect(mx, 32), 1500.0);
    report("notch attenuates the 1500 Hz tone in audio", after < before * 0.2);
}

void testSpectrumNotchCarvesLine()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::Birdie, true);
    mx.setLevelDb(Channel::Birdie, -14.0);
    mx.setKnob(Channel::Birdie, "hz", 1500.0);
    const int n = 1024, center = n / 2;
    const double floorDbm = -120.0, span = 8000.0;
    const int nb = center + static_cast<int>(1500.0 / (span / n));
    const float before = mx.spectrum(n, floorDbm, span, center)[nb];
    mx.setNotches(mx.autoNotchTones());
    const float after = mx.spectrum(n, floorDbm, span, center)[nb];
    report("spectrum notch carves the birdie line to the floor",
           before > floorDbm + 10 && after <= floorDbm + 1);
}

void testNoiseRisesFromFloor()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::White, true);
    mx.setLevelDb(Channel::White, -18.0);
    const int n = 2048, center = n / 2;
    const double floorDbm = -120.0;
    float lo = 1e9f, hi = -1e9f;
    for (int r = 0; r < 5; ++r)
        for (float v : mx.spectrum(n, floorDbm, 8000.0, center)) {
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    // bottoms out near the floor, rises well above it, and has a wide (grassy)
    // spread — not a tight ribbon floating high above the floor.
    const bool bottomsAtFloor = lo <= floorDbm + 6;
    const bool risesUp = hi > floorDbm + 25;
    report("noise bottoms out near the floor", bottomsAtFloor);
    report("noise rises up from the floor (grassy)", risesUp);
}

void testVoiceChannelSafeWithoutClip()
{
    // Voice plays a bundled resource (:/demo_voice.wav) — not available in this
    // standalone test exe (the app auto-inits the qrc). Contract here: the Voice
    // channel must be SAFE with no clip (produce silence, never crash), and it must
    // not disturb other channels. Audible-speech playback is verified live in the app.
    NoiseMixer mx;
    mx.setEnabled(Channel::Voice, true);
    mx.setLevelDb(Channel::Voice, -6);
    const QVector<float> f = mx.mixFrame();
    const bool bounded = std::all_of(f.cbegin(), f.cend(),
                                     [](float v) { return v >= -1.0f && v <= 1.0f; });
    report("voice channel is safe without a clip (bounded output)", bounded);
}

void testNoiseBlankerKnocksDownImpulses()
{
    auto peakOf = [](bool nb) {
        NoiseMixer mx;
        mx.setEnabled(Channel::Qrn, true);  mx.setLevelDb(Channel::Qrn, -12);
        mx.setEnabled(Channel::Pink, true); mx.setLevelDb(Channel::Pink, -24);
        mx.setNoiseBlank(nb);
        double peak = 0.0;
        for (int f = 0; f < 400; ++f)
            for (float v : mx.mixFrame()) peak = std::max(peak, (double)std::abs(v));
        return peak;
    };
    const double off = peakOf(false), on = peakOf(true);
    // The QRN impulses are the loudest thing; NB should cut the peak substantially.
    report("noise blanker knocks down impulse peaks", on < off * 0.6);
}

// The CW channel must key REAL Morse (the voice channel's analogue: genuine
// content a decoder can read). This test does NOT mirror genCw's schedule —
// it envelope-detects the audio, classifies mark/space runs by duration, and
// decodes them through its own independent Morse table. It fails on the old
// gap-less keying (one fused 9-dit tone) and passes only if the audio reads
// the intended id — repeating "L" with word gaps between repeats.
void testCwKeysDecodableMorse()
{
    NoiseMixer mx;
    mx.setEnabled(Channel::Cw, true);

    // ~20 s of audio (>2 full message cycles), envelope in 1 ms blocks.
    QVector<float> audio;
    for (int f = 0; f < 3750; ++f) audio += mx.mixFrame();
    const int blk = NoiseMixer::kSampleRate / 1000;
    QVector<double> env;
    for (int i = 0; i + blk <= audio.size(); i += blk) {
        double acc = 0.0;
        for (int j = 0; j < blk; ++j) acc += static_cast<double>(audio[i + j]) * audio[i + j];
        env.append(std::sqrt(acc / blk));
    }
    double emax = 0.0;
    for (double e : env) emax = std::max(emax, e);
    const double thresh = emax * 0.5;

    // Mark/space run lengths in ms.
    struct Run { bool on; int ms; };
    QVector<Run> runs;
    for (double e : env) {
        const bool on = e > thresh;
        if (!runs.isEmpty() && runs.last().on == on) ++runs.last().ms;
        else runs.append({on, 1});
    }
    if (runs.size() > 2) { runs.removeFirst(); runs.removeLast(); }  // partial edges

    // Independent decode: dit at 18 WPM = 66.7 ms; mark <2 dits = dit; space
    // >=5 dits = word gap, >=2 = char gap.
    const double dit = 1200.0 / 18.0;
    QString text, sym;
    auto flush = [&](bool word) {
        static const std::map<QString, QChar> kMorse = {
            {".-", 'A'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
            {"....", 'H'}, {".-..", 'L'}, {"--.-", 'Q'}, {".-.", 'R'},
            {"-", 'T'}};
        if (!sym.isEmpty()) {
            const auto it = kMorse.find(sym);
            text += (it != kMorse.end()) ? it->second : QChar('?');
            sym.clear();
        }
        if (word && !text.isEmpty() && !text.endsWith(' ')) text += ' ';
    };
    for (const Run& r : runs) {
        if (r.on) sym += (r.ms < 2.0 * dit) ? '.' : '-';
        else if (r.ms >= 5.0 * dit) flush(true);
        else if (r.ms >= 2.0 * dit) flush(false);
    }
    flush(false);
    report("cw channel decodes as repeating \"L\" id",
           text.contains(QStringLiteral("L L L")));
}

// #4618: the CW tone must be a phase ACCUMULATOR, not sin(2π·hz·t_absolute).
// With absolute-time synthesis, a live pitch change (setKnob "hz" reaches the
// mixer via SimBackend's passthrough, and the Demo applet ships that slider)
// teleports the phase by 2π·Δhz·t — a hard click mid-element that the 5 ms
// raised-cosine key edges cannot mask.
//
// BOTH halves below are load-bearing. Continuity alone is also satisfied by a
// genCw that ignores c.hz outright — a constant dphi is perfectly smooth — so
// the frequency half is what pins the knob to the oscillator. Dropping either
// one lets a broken generator through green.
void testCwPitchChangeSlidesWithoutClick()
{
    // ---- half 1: no phase jump across repeated pitch flips ------------------
    NoiseMixer mx;
    mx.setEnabled(Channel::Cw, true);

    // ~5 s, flipping the pitch every 10 frames (64 ms). The "L" id schedule
    // keys roughly half the time, so dozens of flips land mid-element.
    QVector<float> audio;
    for (int f = 0; f < 940; ++f) {
        if (f % 10 == 0)
            mx.setKnob(Channel::Cw, QStringLiteral("hz"), (f % 20 == 0) ? 550.0 : 650.0);
        audio += mx.mixFrame();
    }

    float amp = 0.0f;
    for (float v : audio) amp = std::max(amp, std::abs(v));

    // Max slope of a keyed sine at 650 Hz / 24 kHz is amp·2π·650/24000 ≈
    // 0.17·amp; the raised-cosine key edges are far slower. A phase teleport
    // produces steps up to 2·amp. Threshold at 0.6·amp cleanly separates them:
    // measured 0.17·amp with the accumulator, 1.81·amp with absolute time.
    float worst = 0.0f;
    for (int i = 1; i < audio.size(); ++i)
        worst = std::max(worst, std::abs(audio[i] - audio[i - 1]));

    report("cw pitch change keeps the tone phase continuous",
           amp > 0.0f && worst < 0.6f * amp);

    // ---- half 2: the tone lands ON the commanded pitch ----------------------
    // Hold each pitch for ~2.1 s and Goertzel BOTH candidates: the commanded
    // one has to dominate. Measured ≈600x for the real generator and ≈1x for
    // one that ignores the knob, so 20x sits far from either. The window is
    // deliberately long — at ~2 s the two candidates are ~200 bins apart, so
    // keying sidebands can't leak one into the other.
    auto holdPitch = [](double hz) {
        NoiseMixer m;
        m.setEnabled(Channel::Cw, true);
        m.setKnob(Channel::Cw, QStringLiteral("hz"), hz);
        return collect(m, 400);
    };
    const QVector<float> at550 = holdPitch(550.0);
    const QVector<float> at650 = holdPitch(650.0);
    report("cw tone frequency follows the hz knob",
           magAt(at550, 550.0) > 20.0 * magAt(at550, 650.0) &&
           magAt(at650, 650.0) > 20.0 * magAt(at650, 550.0));
}

void testLevelScalesNoiseHeight()
{
    auto top = [](double lvl) {
        NoiseMixer mx; mx.setEnabled(Channel::White, true); mx.setLevelDb(Channel::White, lvl);
        float hi = -1e9f;
        for (float v : mx.spectrum(1024, -120.0, 8000.0, 512)) hi = std::max(hi, v);
        return hi;
    };
    report("louder noise => taller grass", top(-10.0) > top(-40.0));
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testFrameLengthAndBounds();
    testSilentWhenDisabled();
    testAdditive();
    testAnfDetectionFindsTonesNotNoise();
    testNotchKillsAudioTone();
    testSpectrumNotchCarvesLine();
    testNoiseRisesFromFloor();
    testLevelScalesNoiseHeight();
    testNoiseBlankerKnocksDownImpulses();
    testVoiceChannelSafeWithoutClip();
    testCwKeysDecodableMorse();
    testCwPitchChangeSlidesWithoutClick();
    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES ABOVE");
    return g_failed == 0 ? 0 : 1;
}
