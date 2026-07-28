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
    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES ABOVE");
    return g_failed == 0 ? 0 : 1;
}
