// Standalone test harness for ClientGate DSP.
// Build: produced by CMake as `client_gate_test` target.
// Run:   ./build/client_gate_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/ClientGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using AetherSDR::ClientGate;

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

std::vector<float> makeSilence(int frames)
{
    return std::vector<float>(frames * 2, 0.0f);
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

void processBlocks(ClientGate& gate, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        gate.process(p, n, 2);
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

// Disabled gate = pure pass-through on any input.
void testBypass()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(false);

    const int frames = 1024;
    auto ref = makeTone(1000.0, frames, 0.001f);  // very quiet: -60 dBFS
    auto out = ref;
    processBlocks(gate, out.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("bypass: disabled passes through unchanged",
           maxDiff < 1e-6f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Signal well above threshold → unity gain (no attenuation).
void testAboveThresholdPassthrough()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);
    gate.setRatio(10.0f);
    gate.setAttackMs(0.5f);
    gate.setReleaseMs(50.0f);
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);

    // Prime envelope with a long burst at -6 dBFS.
    const float ampIn = dbToLin(-6.0f);
    auto buf = makeTone(1000.0, 4800, ampIn);  // 200 ms
    processBlocks(gate, buf.data(), 4800);

    // Now measure the trailing block — envelope is fully settled.
    const int tailFrames = 512;
    auto tail = makeTone(1000.0, tailFrames, ampIn);
    processBlocks(gate, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / ampIn);

    report("above-threshold: unity gain (no reduction)",
           std::fabs(grDb) < 0.5f,
           "GR=" + std::to_string(grDb) + " dB");
}

// Signal below threshold, soft expander (ratio 2:1, floor -15 dB):
// expected reduction = min(floor, (T - env) * (ratio - 1))
// with env = -50 dBFS, T = -40 dB, r = 2 → shortfall 10, slope 1 → -10 dB
void testExpanderBelowThreshold()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);
    gate.setRatio(2.0f);
    gate.setAttackMs(0.5f);
    gate.setReleaseMs(10.0f);       // fast release so envelope settles quickly
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);        // deep enough not to clamp the -10 dB expected

    const float ampIn = dbToLin(-50.0f);
    // Long burst for envelope + gain to settle.
    auto prime = makeTone(1000.0, 24000, ampIn);  // 1 s
    processBlocks(gate, prime.data(), 24000);

    const int tailFrames = 512;
    auto tail = makeTone(1000.0, tailFrames, ampIn);
    processBlocks(gate, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / ampIn);

    // Expected ≈ -10 dB; allow ±1.5 dB for envelope ripple at low frequency.
    report("expander: -50 dBFS → ~-10 dB reduction (2:1)",
           std::fabs(grDb - (-10.0f)) < 1.5f,
           "GR=" + std::to_string(grDb) + " dB (expected ~-10)");
}

// Hard gate (ratio 10:1): signal far below threshold gets clamped to `floor`.
void testGateClampsToRange()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRatio(10.0f);
    gate.setAttackMs(0.5f);
    gate.setReleaseMs(10.0f);
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);

    // With env 60 dB below threshold and slope 9, raw curve demands -540 dB;
    // the floor clamp must cap it at -40 dB.
    const float ampIn = dbToLin(-90.0f);
    auto prime = makeTone(1000.0, 24000, ampIn);
    processBlocks(gate, prime.data(), 24000);

    const int tailFrames = 512;
    auto tail = makeTone(1000.0, tailFrames, ampIn);
    processBlocks(gate, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / ampIn);

    report("gate: far-below-threshold clamps to floor",
           std::fabs(grDb - (-40.0f)) < 1.5f,
           "GR=" + std::to_string(grDb) + " dB (floor=-40)");
}

// Hold: after signal drops below threshold, gain should stay at its open
// value for `holdMs` before starting to close.
void testHoldFreezesGain()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);
    gate.setRatio(10.0f);
    gate.setAttackMs(0.1f);
    gate.setReleaseMs(500.0f);   // slow release so hold effect is visible
    gate.setHoldMs(50.0f);       // 50 ms hold = 1200 samples @ 24 kHz
    gate.setFloorDb(-40.0f);

    // Open the gate with a -6 dBFS burst for 100 ms.
    const float ampLoud = dbToLin(-6.0f);
    auto open = makeTone(1000.0, 2400, ampLoud);
    processBlocks(gate, open.data(), 2400);

    // Now feed silence.  The envelope drops instantly (well, over the
    // attack coefficient one sample at a time, but attack is fast).
    // During the first 50 ms of silence the gain should stay at ~0 dB
    // (gate still held open) — we test at t=25 ms into the hold.
    auto sil = makeSilence(600);  // 25 ms
    processBlocks(gate, sil.data(), 600);
    const float grDuringHold = gate.gainReductionDb();

    // Then continue into the release region (past hold).  Another 25 ms
    // brings us to 50 ms which is right at the boundary; skip ahead to
    // sample 2000+ to be safely past it with a slow 500 ms release
    // already chewing on the gain.
    sil = makeSilence(2000);
    processBlocks(gate, sil.data(), 2000);
    const float grAfterHold = gate.gainReductionDb();

    // During hold: no reduction (gate still open).
    // After hold: some reduction (release underway) but nowhere near -40
    // yet thanks to the slow release.
    const bool holdOk    = std::fabs(grDuringHold) < 0.5f;
    const bool releaseOk = grAfterHold < -0.5f && grAfterHold > -20.0f;

    report("hold: gain frozen during hold window",
           holdOk && releaseOk,
           "grHold=" + std::to_string(grDuringHold)
           + " grAfter=" + std::to_string(grAfterHold));
}

// Mode toggle: Expander sets ratio 2 / floor -15; Gate sets ratio 10 / floor -40.
// Other params (threshold/attack/release/hold) are preserved.
void testModeToggle()
{
    ClientGate gate;
    gate.prepare(kSampleRate);

    gate.setThresholdDb(-35.0f);
    gate.setAttackMs(2.0f);
    gate.setReleaseMs(150.0f);
    gate.setHoldMs(30.0f);

    gate.setMode(ClientGate::Mode::Expander);
    const bool expOk =
        std::fabs(gate.ratio() - 2.0f) < 0.01f &&
        std::fabs(gate.floorDb() - (-15.0f)) < 0.01f &&
        std::fabs(gate.thresholdDb() - (-35.0f)) < 0.01f &&
        std::fabs(gate.attackMs() - 2.0f) < 0.01f &&
        std::fabs(gate.releaseMs() - 150.0f) < 0.01f &&
        std::fabs(gate.holdMs() - 30.0f) < 0.01f;

    gate.setMode(ClientGate::Mode::Gate);
    const bool gateOk =
        std::fabs(gate.ratio() - 10.0f) < 0.01f &&
        std::fabs(gate.floorDb() - (-40.0f)) < 0.01f &&
        std::fabs(gate.thresholdDb() - (-35.0f)) < 0.01f &&
        std::fabs(gate.attackMs() - 2.0f) < 0.01f &&
        std::fabs(gate.releaseMs() - 150.0f) < 0.01f &&
        std::fabs(gate.holdMs() - 30.0f) < 0.01f;

    report("mode: Expander/Gate snap ratio+floor, preserve other knobs",
           expOk && gateOk);
}

// Stereo linking: different L/R amplitudes → same gain applied to both.
void testStereoLinking()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRatio(4.0f);
    gate.setAttackMs(0.5f);
    gate.setReleaseMs(10.0f);
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);

    const int frames = 24000;  // 1 s
    std::vector<float> buf(frames * 2);
    const float ampL = dbToLin(-50.0f);
    const float ampR = dbToLin(-45.0f);  // 5 dB louder — drives the envelope
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float s = static_cast<float>(
            std::sin(twoPi * 1000.0 * i / kSampleRate));
        buf[i * 2]     = ampL * s;
        buf[i * 2 + 1] = ampR * s;
    }
    processBlocks(gate, buf.data(), frames);

    // Measure ratio of output peak L vs R in the tail.  Should equal
    // ampL / ampR (same gain applied, L remains 5 dB quieter).
    const int tailStart = frames - 512;
    float pkL = 0.0f, pkR = 0.0f;
    for (int i = tailStart; i < frames; ++i) {
        pkL = std::max(pkL, std::fabs(buf[i * 2]));
        pkR = std::max(pkR, std::fabs(buf[i * 2 + 1]));
    }
    const float ratioDb = linToDb(pkL / pkR);
    const float expected = linToDb(ampL / ampR);  // -5 dB

    report("stereo: identical gain applied to L and R",
           std::fabs(ratioDb - expected) < 0.3f,
           "L/R=" + std::to_string(ratioDb) + " expected=-5");
}

// Attack time: starting from fully closed (silence), a loud step should
// reach near-unity within a handful of attack time-constants.
void testAttackTiming()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRatio(10.0f);
    gate.setAttackMs(5.0f);
    gate.setReleaseMs(200.0f);
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);

    // Start with a full second of silence so the gate is fully closed
    // at -floor (-40 dB reduction).
    auto sil = makeSilence(24000);
    processBlocks(gate, sil.data(), 24000);
    const float grClosed = gate.gainReductionDb();
    const bool closedOk = std::fabs(grClosed - (-40.0f)) < 3.0f;

    // Hit it with a loud tone.  After 10× attack (50 ms = 1200 samples)
    // the gate should be open (GR within 1 dB of 0).
    const float amp = dbToLin(-6.0f);
    auto burst = makeTone(1000.0, 1200, amp);
    processBlocks(gate, burst.data(), 1200);
    const float grOpen = gate.gainReductionDb();
    const bool openedOk = std::fabs(grOpen) < 1.0f;

    report("attack: opens within ~10× τ after loud onset",
           closedOk && openedOk,
           "closedGR=" + std::to_string(grClosed)
           + " openedGR=" + std::to_string(grOpen));
}

// Reset clears envelope + hold state.
void testReset()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRatio(10.0f);
    gate.setAttackMs(0.5f);
    gate.setReleaseMs(50.0f);
    gate.setHoldMs(100.0f);
    gate.setFloorDb(-40.0f);

    // Open gate, then drop to silence mid-hold.
    const float amp = dbToLin(-6.0f);
    auto burst = makeTone(1000.0, 2400, amp);
    processBlocks(gate, burst.data(), 2400);

    gate.reset();

    // After reset with silence, envelope should be zero and gain
    // should track toward floor from that fresh state.  Just check
    // no NaN/Inf and that processing still works.
    auto sil = makeSilence(1024);
    processBlocks(gate, sil.data(), 1024);

    report("reset: no NaN/Inf after flush + fresh block",
           !anyNaNorInf(sil.data(), 1024));
}

// Sanity: AM tone crossing threshold repeatedly produces finite output
// with no NaNs / Infs and output peak ≤ input peak.
void testTransientSanity()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRatio(6.0f);
    gate.setAttackMs(1.0f);
    gate.setReleaseMs(100.0f);
    gate.setHoldMs(20.0f);
    gate.setFloorDb(-30.0f);

    const int frames = 24000;
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float env = 0.3f + 0.3f
            * static_cast<float>(std::sin(twoPi * 3.0 * i / kSampleRate));
        const float s = env * static_cast<float>(
            std::sin(twoPi * 1000.0 * i / kSampleRate));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    const float inPeak = peakAbsStereo(buf.data(), frames);
    processBlocks(gate, buf.data(), frames);
    const float outPeak = peakAbsStereo(buf.data(), frames);

    report("transient: AM tone finite + output ≤ input peak",
           !anyNaNorInf(buf.data(), frames) && outPeak <= inPeak + 1e-5f,
           "inPeak=" + std::to_string(inPeak)
           + " outPeak=" + std::to_string(outPeak));
}

// Return (hysteresis): once the gate is open, it should stay open until
// the envelope drops below threshold - returnDb.  Test by priming open
// at a loud level, then feeding a level between close-threshold and
// threshold — gate should remain open despite being below the opening
// threshold.
void testReturnHysteresis()
{
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setReturnDb(6.0f);        // close-threshold = -36 dB
    gate.setRatio(10.0f);
    gate.setAttackMs(0.1f);
    gate.setReleaseMs(50.0f);
    gate.setHoldMs(0.0f);
    gate.setFloorDb(-40.0f);

    // Open the gate with a loud burst.
    const float ampLoud = dbToLin(-6.0f);
    auto open = makeTone(1000.0, 2400, ampLoud);   // 100 ms
    processBlocks(gate, open.data(), 2400);

    // Drop to -33 dBFS (below threshold -30, above close-threshold -36).
    // Gate should REMAIN open despite being below the opening threshold.
    const float ampMid = dbToLin(-33.0f);
    auto mid = makeTone(1000.0, 12000, ampMid);    // 500 ms
    processBlocks(gate, mid.data(), 12000);
    const float grInBand = gate.gainReductionDb();

    // Now drop below the close-threshold → gate should close and
    // curve attenuation should kick in.
    const float ampQuiet = dbToLin(-45.0f);
    auto quiet = makeTone(1000.0, 24000, ampQuiet); // 1 s
    processBlocks(gate, quiet.data(), 24000);
    const float grClosed = gate.gainReductionDb();

    report("return: gate stays open in hysteresis band",
           std::fabs(grInBand) < 0.5f && grClosed < -5.0f,
           "grInBand=" + std::to_string(grInBand)
           + " grClosed=" + std::to_string(grClosed));
}

// Lookahead: with N samples of lookahead the first N samples of a
// loud tone should pass through at the previous (attenuated) gain,
// because those samples were already in the delay line BEFORE the
// detector saw the loud signal.  The gate should start opening
// "N samples early" relative to when the user hears it in the output.
// We verify the delay line is in use by comparing a lookahead=0 run
// vs lookahead=1 ms: the lookahead version should delay the output
// by exactly 24 samples (1 ms @ 24 kHz).
void testLookaheadDelay()
{
    const int frames = 4800;
    const int la = 24;  // 1 ms @ 24 kHz

    // Build an impulse train: quiet for the first 1000 samples, then
    // a single full-scale spike, then quiet.
    auto makeImpulse = [frames](int spikeAt) {
        std::vector<float> buf(frames * 2, 0.0f);
        buf[spikeAt * 2]     = 1.0f;
        buf[spikeAt * 2 + 1] = 1.0f;
        return buf;
    };

    // Configure: gate bypassed (enabled=false), so the delay line is
    // the ONLY effect — we can measure delay in isolation.
    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(false);
    gate.setLookaheadMs(1.0f);

    auto buf = makeImpulse(1000);
    processBlocks(gate, buf.data(), frames);

    // Find the position of the first non-zero sample.
    int firstNonZero = -1;
    for (int i = 0; i < frames; ++i) {
        if (std::fabs(buf[i * 2]) > 0.1f) { firstNonZero = i; break; }
    }

    report("lookahead: signal delayed by exactly N samples",
           firstNonZero == 1000 + la,
           "spikeAt=1000 lookahead=" + std::to_string(la)
           + " foundAt=" + std::to_string(firstNonZero));
}

} // namespace

int main()
{
    std::printf("ClientGate test harness @ %.0f Hz, %d-frame blocks\n\n",
                kSampleRate, kBlockSize);

    testBypass();
    testAboveThresholdPassthrough();
    testExpanderBelowThreshold();
    testGateClampsToRange();
    testHoldFreezesGain();
    testModeToggle();
    testStereoLinking();
    testAttackTiming();
    testReset();
    testTransientSanity();
    testReturnHysteresis();
    testLookaheadDelay();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
