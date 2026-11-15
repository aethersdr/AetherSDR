// Standalone test harness for ClientTube DSP.
// Build: CMake target `client_tube_test`.  Exit 0 = pass.

#include "core/ClientTube.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::ClientTube;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-54s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

std::vector<float> makeTone(double freq, int frames, float amplitude)
{
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float s = amplitude * static_cast<float>(
            std::sin(twoPi * freq * i / kSampleRate));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    return buf;
}

float peakAbsStereo(const float* data, int frames)
{
    float peak = 0.0f;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        peak = std::max(peak, std::fabs(data[i]));
    }
    return peak;
}

float rmsStereo(const float* data, int frames)
{
    double sumSq = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) sumSq += data[i] * data[i];
    return static_cast<float>(std::sqrt(sumSq / samples));
}

void processBlocks(ClientTube& t, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        t.process(p, n, 2);
        p += n * 2;
        remaining -= n;
    }
}

bool anyNaNorInf(const float* data, int frames)
{
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        if (!std::isfinite(data[i])) return true;
    }
    return false;
}

float linToDb(float lin)
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

float dbToLin(float db)
{
    return std::pow(10.0f, db * 0.05f);
}

// ── Tests ──────────────────────────────────────────────────────────────

void testBypass()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(false);

    const int frames = 1024;
    auto ref = makeTone(1000.0, frames, dbToLin(-6.0f));
    auto out = ref;
    processBlocks(t, out.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("bypass: disabled passes through unchanged",
           maxDiff < 1e-6f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Drive = 0 dB, dryWet = 100% wet: signal is shaped but at unity
// drive the waveshaper barely alters a -20 dBFS tone.
void testLowDrivePreservesShape()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(true);
    t.setModel(ClientTube::Model::A);
    t.setDriveDb(0.0f);
    t.setBiasAmount(0.0f);
    t.setTone(0.0f);
    t.setOutputGainDb(0.0f);
    t.setDryWet(1.0f);
    t.setEnvelopeAmount(0.0f);

    const float amp = dbToLin(-20.0f);
    auto buf = makeTone(1000.0, 4800, amp);
    processBlocks(t, buf.data(), 4800);

    const int tailFrames = 512;
    auto tail = makeTone(1000.0, tailFrames, amp);
    processBlocks(t, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    // At -20 dBFS with unity drive, tanh(x) ≈ x.  Expect < 0.5 dB delta.
    const float delta = linToDb(outPeak / amp);
    report("low-drive: -20 dBFS stays ~unchanged at drive=0 dB",
           std::fabs(delta) < 0.5f,
           "delta=" + std::to_string(delta) + " dB");
}

// Heavy drive (+18 dB) on a -3 dBFS tone should clip the tops and
// bottoms near ±1 — output peak caps at or below ±1.0.
void testHeavyDriveClips()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(true);
    t.setModel(ClientTube::Model::A);
    t.setDriveDb(18.0f);
    t.setBiasAmount(0.0f);
    t.setDryWet(1.0f);
    t.setOutputGainDb(0.0f);
    t.setEnvelopeAmount(0.0f);

    const float amp = dbToLin(-3.0f);
    auto buf = makeTone(1000.0, 4800, amp);
    processBlocks(t, buf.data(), 4800);

    const float outPeak = peakAbsStereo(buf.data(), 4800);
    report("heavy-drive: output stays bounded (tanh saturation)",
           outPeak <= 1.01f && outPeak > 0.9f,
           "outPeak=" + std::to_string(outPeak));
}

// Bias adds DC / even-harmonic content — RMS should rise slightly
// while a pure tanh at symmetric bias=0 would produce ~zero DC.
void testBiasAddsAsymmetry()
{
    auto runOnce = [](float bias) {
        ClientTube t;
        t.prepare(kSampleRate);
        t.setEnabled(true);
        t.setModel(ClientTube::Model::A);
        t.setDriveDb(12.0f);
        t.setBiasAmount(bias);
        t.setDryWet(1.0f);
        t.setOutputGainDb(0.0f);
        t.setEnvelopeAmount(0.0f);

        auto buf = makeTone(1000.0, 4800, dbToLin(-6.0f));
        processBlocks(t, buf.data(), 4800);

        double dc = 0.0;
        const int samples = 4800 * 2;
        for (int i = 0; i < samples; ++i) dc += buf[i];
        return static_cast<float>(dc / samples);
    };

    const float dcSym  = std::fabs(runOnce(0.0f));
    const float dcAsym = std::fabs(runOnce(1.0f));

    // Symmetric bias=0 should have near-zero DC; bias=1 should show
    // measurable DC offset from the asymmetric term.
    report("bias: asymmetric shaper introduces DC offset",
           dcSym < 1e-3f && dcAsym > 5e-3f,
           "dc_sym=" + std::to_string(dcSym)
           + " dc_asym=" + std::to_string(dcAsym));
}

// Dry/Wet blend: at 0% the output is the dry signal unchanged.
void testDryWetZero()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(true);
    t.setDriveDb(24.0f);         // heavy drive — would distort if wet
    t.setDryWet(0.0f);           // fully dry
    t.setOutputGainDb(0.0f);
    t.setEnvelopeAmount(0.0f);

    const float amp = dbToLin(-6.0f);
    auto ref = makeTone(1000.0, 1024, amp);
    auto out = ref;
    processBlocks(t, out.data(), 1024);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("dry/wet: 0% wet returns dry signal",
           maxDiff < 1e-5f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Envelope positive: quiet input → low drive; loud input → high drive.
// Compare heavy drive + envAmount=+1 on two tones at different levels —
// loud tone should be more distorted (higher THD proxy: RMS/peak ratio
// diverges from pure sine's 1/sqrt(2)).
void testEnvelopePositive()
{
    auto driveRMS = [](float inputDb) {
        ClientTube t;
        t.prepare(kSampleRate);
        t.setEnabled(true);
        t.setModel(ClientTube::Model::A);
        t.setDriveDb(12.0f);
        t.setBiasAmount(0.0f);
        t.setTone(0.0f);
        t.setDryWet(1.0f);
        t.setOutputGainDb(0.0f);
        t.setEnvelopeAmount(1.0f);       // loud → more drive
        t.setAttackMs(0.5f);
        t.setReleaseMs(20.0f);

        auto buf = makeTone(1000.0, 12000, dbToLin(inputDb));
        processBlocks(t, buf.data(), 12000);
        // Measure the trailing block after envelope settles.
        return peakAbsStereo(buf.data() + (12000 - 512) * 2, 512);
    };

    const float quietPeak = driveRMS(-40.0f);
    const float loudPeak  = driveRMS(-3.0f);

    // Loud signal should clamp near 1.0 (envelope-boosted drive
    // tips into tanh clipping).  Quiet signal stays well below the
    // tanh knee — baseDrive still amplifies it but envelope barely
    // modulates (envLin ≈ 0 at -40 dBFS), so output stays linear.
    const bool loudClips   = loudPeak > 0.85f;
    const bool quietLinear = quietPeak < 0.3f;   // well below tanh knee

    report("envelope +1: loud saturates, quiet stays linear",
           loudClips && quietLinear,
           "quiet=" + std::to_string(quietPeak)
           + " loud=" + std::to_string(loudPeak));
}

// Envelope negative: same loud input should NOT push into clipping as
// hard because high input level pulls drive DOWN.  Peak should stay
// below the positive-envelope loud case.
void testEnvelopeNegative()
{
    auto loudPeakAtEnv = [](float env) {
        ClientTube t;
        t.prepare(kSampleRate);
        t.setEnabled(true);
        t.setModel(ClientTube::Model::A);
        t.setDriveDb(12.0f);
        t.setBiasAmount(0.0f);
        t.setTone(0.0f);
        t.setDryWet(1.0f);
        t.setOutputGainDb(0.0f);
        t.setEnvelopeAmount(env);
        t.setAttackMs(0.5f);
        t.setReleaseMs(20.0f);

        auto buf = makeTone(1000.0, 12000, dbToLin(-3.0f));
        processBlocks(t, buf.data(), 12000);
        return peakAbsStereo(buf.data() + (12000 - 512) * 2, 512);
    };

    const float posPeak = loudPeakAtEnv(+1.0f);
    const float negPeak = loudPeakAtEnv(-1.0f);

    // Negative envelope should produce a lower peak on the same loud
    // input because drive is scaled DOWN when input is loud.
    report("envelope -1: reverse modulation reduces saturation on loud input",
           negPeak < posPeak - 0.05f,
           "pos=" + std::to_string(posPeak)
           + " neg=" + std::to_string(negPeak));
}

// Three models produce audibly distinct outputs on the same input.
// Quick sanity: running A/B/C with identical settings should yield
// different RMS values (they're different curves).
void testModelsDiffer()
{
    auto modelRms = [](ClientTube::Model m) {
        ClientTube t;
        t.prepare(kSampleRate);
        t.setEnabled(true);
        t.setModel(m);
        t.setDriveDb(12.0f);
        t.setBiasAmount(0.5f);
        t.setTone(0.0f);
        t.setDryWet(1.0f);
        t.setOutputGainDb(0.0f);
        t.setEnvelopeAmount(0.0f);

        auto buf = makeTone(1000.0, 4800, dbToLin(-6.0f));
        processBlocks(t, buf.data(), 4800);
        return rmsStereo(buf.data(), 4800);
    };

    const float a = modelRms(ClientTube::Model::A);
    const float b = modelRms(ClientTube::Model::B);
    const float c = modelRms(ClientTube::Model::C);

    const bool aDiffersB = std::fabs(a - b) > 0.005f;
    const bool bDiffersC = std::fabs(b - c) > 0.005f;
    const bool aDiffersC = std::fabs(a - c) > 0.005f;

    report("models: A / B / C produce distinct outputs",
           aDiffersB && bDiffersC && aDiffersC,
           "A=" + std::to_string(a)
           + " B=" + std::to_string(b)
           + " C=" + std::to_string(c));
}

// Sanity: complex signal stays finite + bounded at max drive.
void testTransientSanity()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(true);
    t.setModel(ClientTube::Model::B);
    t.setDriveDb(24.0f);
    t.setBiasAmount(0.8f);
    t.setTone(0.5f);
    t.setDryWet(1.0f);
    t.setOutputGainDb(6.0f);
    t.setEnvelopeAmount(-0.5f);

    const int frames = 24000;
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float env = 0.3f + 0.3f
            * static_cast<float>(std::sin(twoPi * 3.0 * i / kSampleRate));
        const float s = env * static_cast<float>(
            std::sin(twoPi * 800.0 * i / kSampleRate));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    processBlocks(t, buf.data(), frames);

    report("transient: max-drive AM tone stays finite + bounded",
           !anyNaNorInf(buf.data(), frames)
             && peakAbsStereo(buf.data(), frames) < 5.0f,
           "peak=" + std::to_string(peakAbsStereo(buf.data(), frames)));
}

void testReset()
{
    ClientTube t;
    t.prepare(kSampleRate);
    t.setEnabled(true);
    t.setDriveDb(12.0f);
    t.setDryWet(1.0f);

    auto buf = makeTone(1000.0, 4800, dbToLin(-6.0f));
    processBlocks(t, buf.data(), 4800);

    t.reset();
    std::vector<float> sil(1024, 0.0f);
    t.process(sil.data(), 512, 2);
    report("reset: no NaN/Inf after flush + silence",
           !anyNaNorInf(sil.data(), 512));
}

} // namespace

int main()
{
    std::printf("ClientTube test harness @ %.0f Hz, %d-frame blocks\n\n",
                kSampleRate, kBlockSize);

    testBypass();
    testLowDrivePreservesShape();
    testHeavyDriveClips();
    testBiasAddsAsymmetry();
    testDryWetZero();
    testEnvelopePositive();
    testEnvelopeNegative();
    testModelsDiffer();
    testTransientSanity();
    testReset();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
