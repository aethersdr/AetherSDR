// Standalone test harness for ClientPudu DSP.
// CMake target `client_pudu_test`.  Exit 0 = pass.

#include "core/ClientPudu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::ClientPudu;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n",
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
    for (int i = 0; i < samples; ++i) peak = std::max(peak, std::fabs(data[i]));
    return peak;
}

float rmsStereo(const float* data, int frames)
{
    double sumSq = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) sumSq += data[i] * data[i];
    return static_cast<float>(std::sqrt(sumSq / samples));
}

// Crude even-harmonic detector: DC (mean) of rectified signal rises
// when the waveform is asymmetric.  A pure symmetric tanh of a sine
// has zero mean (odd-only); one-sided clipping has non-zero mean.
// We measure mean(x) over several cycles as a proxy for asymmetry.
float meanDC(const float* data, int frames)
{
    double sum = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) sum += data[i];
    return static_cast<float>(sum / samples);
}

void processBlocks(ClientPudu& p, float* buf, int frames)
{
    int remaining = frames;
    float* ptr = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        p.process(ptr, n, 2);
        ptr += n * 2;
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

// ── Tests ──────────────────────────────────────────────────────

void testBypass()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(false);
    p.setPooMix(1.0f);
    p.setDooMix(1.0f);
    p.setPooDriveDb(24.0f);
    p.setDooHarmonicsDb(24.0f);

    const int frames = 1024;
    auto ref = makeTone(1000.0, frames, dbToLin(-6.0f));
    auto out = ref;
    processBlocks(p, out.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("bypass: disabled passes through unchanged",
           maxDiff < 1e-6f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Zero mix: both bands at 0% wet → output = dry (enabled is true,
// but the wet-dry sum adds nothing).
void testZeroMix()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(0.0f);
    p.setDooMix(0.0f);
    p.setPooDriveDb(24.0f);
    p.setDooHarmonicsDb(24.0f);

    const int frames = 1024;
    auto ref = makeTone(1000.0, frames, dbToLin(-6.0f));
    auto out = ref;
    processBlocks(p, out.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("zero mix: both bands at 0% return dry",
           maxDiff < 1e-4f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Aphex HF mode: asymmetric clipping on a 5 kHz tone in the Doo
// band should produce measurable DC offset (pre-DC-block).  We
// can't measure the pre-DC-block signal externally, but we CAN
// verify that the output's symmetry metric differs from the
// Behringer-mode output for the same input.
void testAphexDooAddsAsymmetry()
{
    auto runMode = [](ClientPudu::Mode m) {
        ClientPudu p;
        p.prepare(kSampleRate);
        p.setEnabled(true);
        p.setMode(m);
        p.setPooMix(0.0f);             // Poo band off
        p.setDooMix(1.0f);
        p.setDooTuneHz(2000.0f);       // well below the 5 kHz tone
        p.setDooHarmonicsDb(18.0f);    // heavy drive
        p.setPooTuneHz(100.0f);

        auto buf = makeTone(5000.0, 2400, dbToLin(-6.0f));
        processBlocks(p, buf.data(), 2400);
        return rmsStereo(buf.data(), 2400);
    };

    const float rmsA = runMode(ClientPudu::Mode::Aphex);
    const float rmsB = runMode(ClientPudu::Mode::Behringer);

    // Both modes produce non-zero RMS (they're adding harmonics);
    // Aphex typically yields more output energy due to one-sided
    // clipping pushing half the waveform harder.
    report("mode toggle: A and B produce distinct HF outputs",
           rmsA != rmsB && rmsA > 0.01f && rmsB > 0.01f,
           "rmsA=" + std::to_string(rmsA)
           + " rmsB=" + std::to_string(rmsB));
}

// Above-band tone (10 kHz) with Doo tuned low (2 kHz) should still
// excite Doo; below-band tone (200 Hz) should pass through with no
// HF contribution.  Tests the HPF is doing its job.
void testDooHPFRejectsLows()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(0.0f);
    p.setDooMix(1.0f);
    p.setDooTuneHz(5000.0f);
    p.setDooHarmonicsDb(12.0f);

    // 200 Hz tone — well below the HPF corner; Doo path should be
    // quiet, output ≈ dry.
    const float amp = dbToLin(-6.0f);
    auto buf = makeTone(200.0, 2400, amp);
    const float inRms = rmsStereo(buf.data(), 2400);
    processBlocks(p, buf.data(), 2400);
    const float outRms = rmsStereo(buf.data(), 2400);

    // Output should be very close to dry input RMS since the HPF
    // shoves most of the 200 Hz content aside.
    const float ratioDb = linToDb(outRms / inRms);
    report("Doo HPF: 200 Hz tone barely excites the HF band",
           std::fabs(ratioDb) < 1.0f,
           "outRms/inRms=" + std::to_string(ratioDb) + " dB");
}

// Poo band: a 100 Hz tone with drive + mix should produce a
// detectable output difference vs dry.  Test in Aphex mode where
// the LF saturation adds harmonics.
void testPooLFAphex()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(1.0f);
    p.setDooMix(0.0f);
    p.setPooTuneHz(120.0f);
    p.setPooDriveDb(18.0f);

    const float amp = dbToLin(-12.0f);
    auto buf = makeTone(80.0, 4800, amp);
    const float inRms = rmsStereo(buf.data(), 4800);
    processBlocks(p, buf.data(), 4800);
    const float outRms = rmsStereo(buf.data(), 4800);

    // Expect output RMS noticeably higher than input — dry + wet
    // adds bass content.
    report("Poo band (Aphex): saturation adds LF content",
           outRms > inRms * 1.1f,
           "inRms=" + std::to_string(inRms)
           + " outRms=" + std::to_string(outRms));
}

// Behringer Poo: a 100 Hz tone with the bass processor should
// produce output different from dry, but without generating
// excessive harmonic content (phase rotator + compressor, not a
// waveshaper).
void testPooLFBehringer()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Behringer);
    p.setPooMix(1.0f);
    p.setDooMix(0.0f);
    p.setPooTuneHz(120.0f);
    p.setPooDriveDb(18.0f);

    const float amp = dbToLin(-12.0f);
    auto dry = makeTone(80.0, 4800, amp);
    auto out = dry;
    processBlocks(p, out.data(), 4800);

    // Confirm output differs from dry (the compressor + all-pass
    // alters the signal) and stays finite.
    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - dry[i]));

    report("Poo band (Behringer): compressor + all-pass alters LF",
           maxDiff > 1e-3f && !anyNaNorInf(out.data(), 4800),
           "maxDiff=" + std::to_string(maxDiff));
}

// Wet RMS meter: with both bands active on a busy signal, the
// wetRmsDb() snapshot should be well above -120 dB (silence).
void testWetRmsMeter()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(1.0f);
    p.setDooMix(1.0f);
    p.setPooDriveDb(12.0f);
    p.setDooHarmonicsDb(12.0f);

    auto buf = makeTone(3000.0, 2400, dbToLin(-6.0f));
    processBlocks(p, buf.data(), 2400);

    const float wetDb = p.wetRmsDb();
    report("wet RMS meter: active exciter publishes non-zero wet RMS",
           wetDb > -60.0f,
           "wetRmsDb=" + std::to_string(wetDb));
}

// Reset: after processing, reset() + silence → finite, quiet output.
void testReset()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(1.0f);
    p.setDooMix(1.0f);
    p.setPooDriveDb(18.0f);
    p.setDooHarmonicsDb(18.0f);

    auto buf = makeTone(1000.0, 4800, dbToLin(-6.0f));
    processBlocks(p, buf.data(), 4800);

    p.reset();

    std::vector<float> sil(1024, 0.0f);
    p.process(sil.data(), 512, 2);
    report("reset: no NaN/Inf after flush + silence",
           !anyNaNorInf(sil.data(), 512));
}

// Sanity: complex signal with max drive + both bands stays finite.
void testTransientSanity()
{
    ClientPudu p;
    p.prepare(kSampleRate);
    p.setEnabled(true);
    p.setMode(ClientPudu::Mode::Aphex);
    p.setPooMix(1.0f);
    p.setDooMix(1.0f);
    p.setPooDriveDb(24.0f);
    p.setDooHarmonicsDb(24.0f);
    p.setPooTuneHz(100.0f);
    p.setDooTuneHz(3000.0f);

    const int frames = 12000;
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float env = 0.3f + 0.3f
            * static_cast<float>(std::sin(twoPi * 3.0 * i / kSampleRate));
        const float s = env * (
            0.5f * static_cast<float>(std::sin(twoPi * 80.0   * i / kSampleRate))
          + 0.5f * static_cast<float>(std::sin(twoPi * 4500.0 * i / kSampleRate)));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    processBlocks(p, buf.data(), frames);

    report("transient: max-drive + both bands stays finite",
           !anyNaNorInf(buf.data(), frames)
             && peakAbsStereo(buf.data(), frames) < 5.0f,
           "peak=" + std::to_string(peakAbsStereo(buf.data(), frames)));
}

} // namespace

int main()
{
    std::printf("ClientPudu test harness @ %.0f Hz, %d-frame blocks\n\n",
                kSampleRate, kBlockSize);

    testBypass();
    testZeroMix();
    testAphexDooAddsAsymmetry();
    testDooHPFRejectsLows();
    testPooLFAphex();
    testPooLFBehringer();
    testWetRmsMeter();
    testReset();
    testTransientSanity();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
