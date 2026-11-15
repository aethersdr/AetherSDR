// Standalone test harness for ClientQuindarTone DSP (#2262).
// Build: produced by CMake as `client_quindar_test` target.
// Run:   ./build/client_quindar_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/ClientQuindarTone.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using AetherSDR::ClientQuindarTone;
using Phase = ClientQuindarTone::Phase;
using Style = ClientQuindarTone::Style;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// Process the stage in `kBlockSize`-frame chunks until either the
// stage returns to Idle or `maxFrames` have been processed.  Returns
// the total interleaved samples produced (frames × 2).
std::vector<float> runUntilIdle(ClientQuindarTone& q, int maxFrames)
{
    std::vector<float> out;
    out.reserve(static_cast<size_t>(maxFrames) * 2);
    std::vector<float> block(kBlockSize * 2, 0.0f);
    int produced = 0;
    while (produced < maxFrames) {
        // Reset the block so we can tell whether the stage wrote to
        // it (Idle is no-op, leaves the block as zeros).
        std::fill(block.begin(), block.end(), 0.0f);
        q.process(block.data(), kBlockSize, 2);
        out.insert(out.end(), block.begin(), block.end());
        produced += kBlockSize;
        if (q.phase() == Phase::Idle) break;
        if (q.phase() == Phase::Live) break;
    }
    return out;
}

float peakAbs(const std::vector<float>& buf)
{
    float p = 0.0f;
    for (float s : buf) p = std::max(p, std::fabs(s));
    return p;
}

float rms(const std::vector<float>& buf)
{
    if (buf.empty()) return 0.0f;
    double sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(sumSq / buf.size()));
}

// Goertzel algorithm — single-bin energy estimate at `freq` (Hz).
// Used to verify the dominant frequency of a generated tone block.
double goertzelEnergy(const float* mono, int frames, double freq)
{
    const double w = 2.0 * 3.14159265358979323846 * freq / kSampleRate;
    const double cw = std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < frames; ++i) {
        s0 = mono[i] + 2.0 * cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2;
}

double monoBand(const std::vector<float>& interleaved, double freq)
{
    const int frames = static_cast<int>(interleaved.size() / 2);
    std::vector<float> mono(frames);
    for (int i = 0; i < frames; ++i)
        mono[i] = interleaved[i * 2];
    return goertzelEnergy(mono.data(), frames, freq);
}

} // namespace

// ── Tests ────────────────────────────────────────────────────────────

void test_disabledIsBypass()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(false);
    q.startIntro();

    std::vector<float> block(kBlockSize * 2, 0.123f);
    q.process(block.data(), kBlockSize, 2);

    bool unchanged = true;
    for (float s : block) {
        if (std::fabs(s - 0.123f) > 1e-6f) { unchanged = false; break; }
    }
    report("disabled stage is bypass (samples untouched)", unchanged);
    report("disabled stage forces phase Idle",
           q.phase() == Phase::Idle);
}

void test_phaseTransitionsCorrectly()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setDurationMs(250);

    report("starts in Idle",
           q.phase() == Phase::Idle);

    q.startIntro();
    // Run a single block — should observe Engaging.
    std::vector<float> block(kBlockSize * 2, 0.0f);
    q.process(block.data(), kBlockSize, 2);
    report("Idle → Engaging on startIntro",
           q.phase() == Phase::Engaging);

    // 250 ms at 24 kHz = 6000 frames.  Run enough frames for the
    // intro to finish — phase should transition to Live.
    runUntilIdle(q, 8000);
    report("Engaging → Live after intro duration",
           q.phase() == Phase::Live);

    q.startOutro();
    q.process(block.data(), kBlockSize, 2);
    report("Live → Disengaging on startOutro",
           q.phase() == Phase::Disengaging);

    runUntilIdle(q, 8000);
    report("Disengaging → Idle after outro duration",
           q.phase() == Phase::Idle);
}

void test_introToneFrequency()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setIntroFreqHz(2525.0f);
    q.setOutroFreqHz(2475.0f);
    q.setLevelDb(0.0f);
    q.setDurationMs(250);
    q.startIntro();

    auto out = runUntilIdle(q, 8000);

    const double e2525 = monoBand(out, 2525.0);
    const double e2475 = monoBand(out, 2475.0);
    const double e1000 = monoBand(out, 1000.0);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
        "e2525=%.1e e2475=%.1e e1000=%.1e",
        e2525, e2475, e1000);
    report("intro tone peaks at 2525 Hz, not 2475 / 1000",
        e2525 > e2475 * 5.0 && e2525 > e1000 * 5.0, detail);
}

void test_outroToneFrequency()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setIntroFreqHz(2525.0f);
    q.setOutroFreqHz(2475.0f);
    q.setLevelDb(0.0f);
    q.setDurationMs(250);

    // Skip intro to Live, then outro.
    q.startIntro();
    runUntilIdle(q, 8000);
    q.startOutro();
    auto out = runUntilIdle(q, 8000);

    const double e2475 = monoBand(out, 2475.0);
    const double e2525 = monoBand(out, 2525.0);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "e2475=%.1e e2525=%.1e",
        e2475, e2525);
    report("outro tone peaks at 2475 Hz, not 2525",
        e2475 > e2525 * 5.0, detail);
}

void test_levelMatchesConfiguredDb()
{
    // Verify the steady-state amplitude (post-ramp) tracks the
    // configured level within ~1 dB.  Pick a level that's well
    // inside the ramp's settled region.
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setLevelDb(-6.0f);
    q.setDurationMs(250);
    q.startIntro();

    auto out = runUntilIdle(q, 8000);

    // Expected steady-state peak: 10^(-6/20) ≈ 0.501.
    const float peak = peakAbs(out);
    const float expected = std::pow(10.0f, -6.0f / 20.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
        "peak=%.3f expected≈%.3f", peak, expected);
    report("peak amplitude matches level (-6 dB)",
        std::fabs(peak - expected) < 0.05f, detail);
}

void test_envelopeRampPreventsClicks()
{
    // The first sample of an intro tone should be (very close to) 0
    // because of the cos² fade-in.  Without the ramp, sin(0) starts
    // mid-amplitude * level, producing a step that listeners hear as
    // a click.
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setLevelDb(0.0f);
    q.setDurationMs(250);
    q.startIntro();

    std::vector<float> block(kBlockSize * 2, 0.0f);
    q.process(block.data(), kBlockSize, 2);
    char detail[80];
    std::snprintf(detail, sizeof(detail), "first-sample=%.4f", block[0]);
    report("first sample of intro is ~0 (cos² fade-in)",
        std::fabs(block[0]) < 0.01f, detail);
}

void test_morseKEncodesCorrectElements()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Morse);
    q.setMorseWpm(45);
    q.setMorsePitchHz(750.0f);
    q.setLevelDb(0.0f);
    q.startIntro();

    // K = 9 dot-units at 1200/45 ≈ 26.67 ms each → ~240 ms.
    auto out = runUntilIdle(q, 8000);

    // K should contain non-trivial energy at 750 Hz and very little
    // at 600/900 Hz (basic sanity that the carrier is the configured
    // pitch, not at some other harmonic).
    const double e750 = monoBand(out, 750.0);
    const double e500 = monoBand(out, 500.0);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "e750=%.1e e500=%.1e",
        e750, e500);
    report("Morse K carrier energy at configured pitch",
        e750 > e500 * 4.0, detail);

    // Total duration should fall within a tolerance of the spec.
    // 9 × 1200/45 ms = 240 ms = 5760 frames.  Allow ±10 % slop.
    const int produced = static_cast<int>(out.size() / 2);
    char detail2[160];
    std::snprintf(detail2, sizeof(detail2), "frames=%d", produced);
    report("Morse K duration ~240 ms (5760 frames ±15 %)",
        produced > 4900 && produced < 6700, detail2);
}

void test_morseBKEncodesCorrectElements()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Morse);
    q.setMorseWpm(45);
    q.setLevelDb(0.0f);

    // Skip to Live then start outro.
    q.startIntro();
    runUntilIdle(q, 8000);
    q.startOutro();
    auto out = runUntilIdle(q, 16000);

    // BK = 21 dot-units at 1200/45 ≈ 26.67 ms each → ~560 ms.
    // = 13440 frames at 24 kHz.  Allow ±15 % slop.
    const int produced = static_cast<int>(out.size() / 2);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "frames=%d", produced);
    report("Morse BK duration ~560 ms (13440 frames ±15 %)",
        produced > 11400 && produced < 15500, detail);
}

void test_outroDurationMs()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);

    // Tone style: outro = durationMs.
    q.setStyle(Style::Tone);
    q.setDurationMs(250);
    char d1[80];
    const int outroTone = q.currentOutroDurationMs();
    std::snprintf(d1, sizeof(d1), "got=%d", outroTone);
    report("Tone outro duration matches durationMs (250)",
        outroTone == 250, d1);

    // Morse style: 21 × 1200/WPM ms.  At 45 WPM = 560 ms.
    q.setStyle(Style::Morse);
    q.setMorseWpm(45);
    char d2[80];
    const int outroMorse = q.currentOutroDurationMs();
    std::snprintf(d2, sizeof(d2), "got=%d", outroMorse);
    report("Morse outro duration ≈ 560 ms at 45 WPM",
        outroMorse >= 555 && outroMorse <= 565, d2);
}

void test_coalesceReEngage()
{
    ClientQuindarTone q;
    q.prepare(kSampleRate);
    q.setEnabled(true);
    q.setStyle(Style::Tone);
    q.setDurationMs(250);

    q.startIntro();
    runUntilIdle(q, 8000);   // → Live
    q.startOutro();          // → Disengaging
    report("phase is Disengaging after startOutro",
        q.phase() == Phase::Disengaging);

    const bool coalesced = q.coalesceReEngage();
    report("coalesceReEngage returns true mid-outro", coalesced);
    report("coalesce flips phase back to Live",
        q.phase() == Phase::Live);

    // Subsequent coalesce while in Live should be a no-op.
    const bool noop = q.coalesceReEngage();
    report("coalesceReEngage is no-op when not Disengaging", !noop);
}

int main()
{
    std::printf("Running ClientQuindarTone test harness…\n\n");

    test_disabledIsBypass();
    test_phaseTransitionsCorrectly();
    test_introToneFrequency();
    test_outroToneFrequency();
    test_levelMatchesConfiguredDb();
    test_envelopeRampPreventsClicks();
    test_morseKEncodesCorrectElements();
    test_morseBKEncodesCorrectElements();
    test_outroDurationMs();
    test_coalesceReEngage();

    std::printf("\n%s — %d failure(s)\n",
        g_failed == 0 ? "ALL PASS" : "FAIL",
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
