// ClientFinalLimiter: the look-ahead brickwall at the end of the TX chain.
// No sample leaves above the ceiling at any look-ahead, and the look-ahead
// path never needs the safety clip to get there. The delay is exactly the
// look-ahead, and so is the reported latency (none while disabled). The gain
// ramps down over the look-ahead ahead of a peak instead of stepping, and a
// look-ahead or enable change mid-signal fades the delay tap instead of
// jumping it, also when a second change arrives mid-fade in short blocks. A
// ceiling change while limiting still never lets a sample past the new
// ceiling. Ranges clamp, and the defaults are the 2026-11-15 ruling's.
// Standalone: no Qt event loop, no settings, no audio device.

#include "core/ClientFinalLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using AetherSDR::ClientFinalLimiter;

namespace {

constexpr double kRate  = 48000.0;
constexpr int    kBlock = 480;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-66s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

std::string num(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

float dbToLin(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// A limiter with DC block and trim out of the way, so the output is the
// delayed input times the limiter gain and nothing else.
void configure(ClientFinalLimiter& lim, float lookaheadMs, float ceilingDb)
{
    lim.setEnabled(true);
    lim.setDcBlockEnabled(false);
    lim.setOutputTrimDb(0.0f);
    lim.setCeilingDb(ceilingDb);
    lim.setLookaheadMs(lookaheadMs);
    lim.prepare(kRate);
}

// Interleaved stereo, processed in kBlock-frame blocks.
void run(ClientFinalLimiter& lim, std::vector<float>& stereo)
{
    const int frames = static_cast<int>(stereo.size() / 2);
    for (int at = 0; at < frames; at += kBlock) {
        lim.process(stereo.data() + at * 2, std::min(kBlock, frames - at), 2);
    }
}

std::vector<float> stereoOf(const std::vector<float>& mono)
{
    std::vector<float> out(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        out[i * 2]     = mono[i];
        out[i * 2 + 1] = mono[i];
    }
    return out;
}

// Speech-like material that drives the limiter hard: noise bursts with a
// syllable envelope, isolated full-scale spikes, a step, and a sine burst
// far over the ceiling. Deterministic.
std::vector<float> torture(int frames)
{
    std::vector<float> x(static_cast<std::size_t>(frames));
    uint32_t seed = 12345u;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
    };
    for (int n = 0; n < frames; ++n) {
        const double t = n / kRate;
        const float syllable = static_cast<float>(
            0.5 + 0.5 * std::sin(2.0 * 3.14159265358979 * 4.0 * t));
        float v = 1.6f * syllable * rnd();
        if (n % 4801 == 0) v = (n / 4801) % 2 ? 4.0f : -4.0f;      // spikes
        if (n > frames / 2 && n < frames / 2 + 2400) v = 1.5f;     // step
        if (n > frames * 3 / 4 && n < frames * 3 / 4 + 4800) {     // loud sine
            v = 3.0f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
        }
        x[static_cast<std::size_t>(n)] = v;
    }
    return x;
}

float peakOf(const std::vector<float>& stereo)
{
    float p = 0.0f;
    for (float v : stereo) p = std::max(p, std::fabs(v));
    return p;
}

void testDefaults()
{
    ClientFinalLimiter lim;
    lim.prepare(kRate);
    report("defaults: on, ceiling -0.5 dB, look-ahead 1.5 ms",
           lim.isEnabled() && lim.ceilingDb() == -0.5f && lim.lookaheadMs() == 1.5f,
           "ceiling=" + num(lim.ceilingDb()) + " lookahead=" + num(lim.lookaheadMs()));
    report("latency is the look-ahead: 72 frames at 1.5 ms / 48 kHz",
           lim.latencyFrames() == 72, "frames=" + std::to_string(lim.latencyFrames()));
    lim.setEnabled(false);
    report("disabled, it adds no latency", lim.latencyFrames() == 0,
           "frames=" + std::to_string(lim.latencyFrames()));
}

void testRangesClamp()
{
    ClientFinalLimiter lim;
    lim.setLookaheadMs(9.0f);
    const bool high = lim.lookaheadMs() == ClientFinalLimiter::kMaxLookaheadMs;
    lim.setLookaheadMs(-2.0f);
    const bool low = lim.lookaheadMs() == 0.0f;
    lim.setLookaheadMs(std::numeric_limits<float>::quiet_NaN());
    const bool nan = lim.lookaheadMs() == ClientFinalLimiter::kDefaultLookaheadMs;
    lim.setCeilingDb(std::numeric_limits<float>::quiet_NaN());
    const bool nanCeiling = lim.ceilingDb() == ClientFinalLimiter::kDefaultCeilingDb;
    report("look-ahead clamps to 0..5 ms; NaN takes the default",
           high && low && nan && nanCeiling);

    lim.setLookaheadMs(5.0f);
    lim.prepare(96000.0);  // 480 frames wanted, capacity is 240
    report("a higher rate caps the look-ahead at the ring's capacity",
           lim.latencyFrames() == ClientFinalLimiter::kMaxLookaheadFrames,
           "frames=" + std::to_string(lim.latencyFrames()));
}

// The core promise, at every look-ahead an operator can pick.
void testNothingPassesTheCeiling()
{
    for (const float ms : {0.0f, 0.5f, 1.5f, 3.0f, 5.0f}) {
        for (const float ceilingDb : {-0.5f, -3.0f, -12.0f}) {
            ClientFinalLimiter lim;
            configure(lim, ms, ceilingDb);
            auto buf = stereoOf(torture(48000 * 3));
            run(lim, buf);
            const float peak = peakOf(buf);
            const float ceiling = dbToLin(ceilingDb);
            const std::string name = "no sample above the ceiling, "
                + num(ms) + " ms, " + num(ceilingDb) + " dB";
            report(name.c_str(), peak <= ceiling * (1.0f + 1e-6f),
                   "peak=" + num(peak) + " ceiling=" + num(ceiling));
            const std::string sname = "and the look-ahead never needed the safety clip, "
                + num(ms) + " ms, " + num(ceilingDb) + " dB";
            report(sname.c_str(), lim.safetyClipCount() == 0,
                   "clips=" + std::to_string(lim.safetyClipCount()));
        }
    }
}

// Under the ceiling the limiter is a pure delay of exactly the look-ahead.
void testDelayIsTheLookahead()
{
    for (const float ms : {0.0f, 1.5f, 5.0f}) {
        ClientFinalLimiter lim;
        configure(lim, ms, -0.5f);
        std::vector<float> mono(4800, 0.0f);
        mono[100] = 0.5f;
        auto buf = stereoOf(mono);
        run(lim, buf);
        const int expected = 100 + lim.latencyFrames();
        int at = -1;
        for (int i = 0; i < 4800; ++i) {
            if (buf[static_cast<std::size_t>(i) * 2] != 0.0f) { at = i; break; }
        }
        const bool exact = at == expected && buf[static_cast<std::size_t>(at) * 2] == 0.5f;
        const std::string name = "under the ceiling it is a pure delay of " + num(ms) + " ms";
        report(name.c_str(), exact,
               "impulse at " + std::to_string(at) + ", expected " + std::to_string(expected));
    }
}

// One spike in a steady tone: with look-ahead the gain ramps down over L+1
// samples before the spike arrives, so the neighbours' gain changes in small
// steps; at 0 ms the gain steps straight to what the spike needs.
void testGainRampsInsteadOfStepping()
{
    const auto largestGainStep = [](float ms) {
        ClientFinalLimiter lim;
        configure(lim, ms, -0.5f);
        std::vector<float> mono(9600, 0.5f);
        mono[4800] = 2.0f;
        auto buf = stereoOf(mono);
        run(lim, buf);
        // The output is the delayed input times the gain, and the input is
        // known, so the gain is readable at every sample, the spike's too.
        const int delay = lim.latencyFrames();
        const auto gainAt = [&](int i) {
            return buf[static_cast<std::size_t>(i) * 2]
                / mono[static_cast<std::size_t>(i - delay)];
        };
        float step = 0.0f;
        for (int i = 4000; i < 5600; ++i) {
            step = std::max(step, std::fabs(gainAt(i) - gainAt(i - 1)));
        }
        return step;
    };
    const float need = dbToLin(-0.5f) / 2.0f;          // the spike's gain
    const float ramped = largestGainStep(1.5f);
    const float instant = largestGainStep(0.0f);
    const float bound = (1.0f - need) / 73.0f * 1.01f;  // a 73-sample ramp
    report("with 1.5 ms look-ahead the gain ramps in steps of (1 - g)/(L+1)",
           ramped <= bound, "step=" + num(ramped) + " bound=" + num(bound));
    report("at 0 ms the gain steps at once (the case look-ahead fixes)",
           instant > 0.5f, "step=" + num(instant));
}

// A look-ahead change or an enable toggle mid-tone: the tap fades, so no
// sample-to-sample jump beyond what the tone itself does, and the output
// still never passes the ceiling. The tone is off the block grid (1234.5 Hz),
// so a change never lands on a zero crossing that would hide a jump.
void testChangesFadeTheTap()
{
    constexpr double kToneHz = 1234.5;
    const auto run1k = [](ClientFinalLimiter& lim, int frames, int startFrame,
                          std::vector<float>& out) {
        std::vector<float> mono(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            mono[static_cast<std::size_t>(i)] = 0.3f * static_cast<float>(
                std::sin(2.0 * 3.14159265358979 * kToneHz * (startFrame + i) / kRate));
        }
        auto buf = stereoOf(mono);
        run(lim, buf);
        for (int i = 0; i < frames; ++i) out.push_back(buf[static_cast<std::size_t>(i) * 2]);
    };
    const float toneSlope = 0.3f * 2.0f * 3.14159265f * static_cast<float>(kToneHz) / 48000.0f;
    const auto largestJump = [](const std::vector<float>& y) {
        float j = 0.0f;
        for (std::size_t i = 1; i < y.size(); ++i) j = std::max(j, std::fabs(y[i] - y[i - 1]));
        return j;
    };

    {
        ClientFinalLimiter lim;
        configure(lim, 0.0f, -0.5f);
        std::vector<float> y;
        run1k(lim, 4800, 0, y);
        lim.setLookaheadMs(5.0f);
        run1k(lim, 4800, 4800, y);
        lim.setLookaheadMs(1.5f);
        run1k(lim, 4800, 9600, y);
        const float jump = largestJump(y);
        report("a look-ahead change mid-tone fades the tap, no jump",
               jump < toneSlope * 1.2f, "jump=" + num(jump) + " tone=" + num(toneSlope));
    }
    {
        ClientFinalLimiter lim;
        configure(lim, 1.5f, -0.5f);
        std::vector<float> y;
        run1k(lim, 4800, 0, y);
        lim.setEnabled(false);
        run1k(lim, 4800, 4800, y);
        lim.setEnabled(true);
        run1k(lim, 4800, 9600, y);
        const float jump = largestJump(y);
        report("switching LIM off and on mid-tone fades the tap, no jump",
               jump < toneSlope * 1.2f, "jump=" + num(jump) + " tone=" + num(toneSlope));
    }
    {
        // Loud material across a change still never passes the ceiling.
        ClientFinalLimiter lim;
        configure(lim, 0.0f, -0.5f);
        auto a = stereoOf(torture(24000));
        run(lim, a);
        lim.setLookaheadMs(5.0f);
        auto b = stereoOf(torture(24000));
        run(lim, b);
        const float peak = std::max(peakOf(a), peakOf(b));
        report("loud material across a look-ahead change stays under the ceiling",
               peak <= dbToLin(-0.5f) * (1.0f + 1e-6f), "peak=" + num(peak));
    }
}

// A second look-ahead change while the first fade is still running, in
// 128-frame blocks so the fade cannot finish between the changes (the review
// probe on #7355): the output must not jump.
void testChangeDuringAFadeDoesNotJump()
{
    constexpr double kToneHz = 1234.5;
    ClientFinalLimiter lim;
    configure(lim, 0.0f, -0.5f);
    const float toneSlope = 0.3f * 2.0f * 3.14159265f * static_cast<float>(kToneHz) / 48000.0f;
    std::vector<float> y;
    int n = 0;
    const auto feed = [&](int frames) {
        while (frames > 0) {
            const int block = std::min(128, frames);
            std::vector<float> buf(static_cast<std::size_t>(block) * 2);
            for (int i = 0; i < block; ++i) {
                const float v = 0.3f * static_cast<float>(
                    std::sin(2.0 * 3.14159265358979 * kToneHz * (n + i) / kRate));
                buf[static_cast<std::size_t>(i) * 2]     = v;
                buf[static_cast<std::size_t>(i) * 2 + 1] = v;
            }
            lim.process(buf.data(), block, 2);
            for (int i = 0; i < block; ++i) y.push_back(buf[static_cast<std::size_t>(i) * 2]);
            n += block;
            frames -= block;
        }
    };
    feed(4191);
    lim.setLookaheadMs(5.0f);
    feed(128);
    lim.setLookaheadMs(1.5f);      // the first fade has 112 frames to go
    feed(1);
    lim.setLookaheadMs(0.0f);
    feed(64);
    lim.setEnabled(false);
    feed(64);
    lim.setEnabled(true);
    feed(2048);
    float jump = 0.0f;
    for (std::size_t i = 1; i < y.size(); ++i) jump = std::max(jump, std::fabs(y[i] - y[i - 1]));
    report("changes arriving mid-fade in 128-frame blocks do not jump",
           jump <= toneSlope * 1.05f, "jump=" + num(jump) + " tone=" + num(toneSlope));
    // The deferred changes all land once the fades finish: the last one set
    // 0 ms, so an impulse now comes straight through, undelayed.
    lim.setLookaheadMs(5.0f);
    feed(1);                       // starts a fade toward 240 frames
    lim.setLookaheadMs(1.5f);      // waits for it
    std::vector<float> quiet(960 * 2, 0.0f);
    lim.process(quiet.data(), 960, 2);  // both fades finish
    std::vector<float> imp(480 * 2, 0.0f);
    imp[100 * 2] = imp[100 * 2 + 1] = 0.5f;
    lim.process(imp.data(), 480, 2);
    int at = -1;
    for (int i = 0; i < 480; ++i) {
        if (imp[static_cast<std::size_t>(i) * 2] != 0.0f) { at = i; break; }
    }
    report("a change deferred by a fade still lands once it finishes",
           at == 100 + 72, "impulse at " + std::to_string(at) + ", expected 172");
}

// Lowering the ceiling while limiting: gains computed for the old ceiling
// are still in the window, so the safety clip acts, but nothing passes the
// new ceiling; at a steady ceiling it never acts.
void testCeilingChangeWhileLimiting()
{
    ClientFinalLimiter lim;
    configure(lim, 1.5f, 0.0f);
    std::vector<float> mono(9600);
    for (int i = 0; i < 9600; ++i) {
        mono[static_cast<std::size_t>(i)] = 0.8f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
    }
    auto a = stereoOf(mono);
    run(lim, a);
    const quint64 before = lim.safetyClipCount();
    lim.setCeilingDb(-12.0f);
    auto b = stereoOf(mono);
    run(lim, b);
    const float peak = peakOf(b);
    report("a ceiling change while limiting never passes the new ceiling",
           peak <= dbToLin(-12.0f) * (1.0f + 1e-6f), "peak=" + num(peak));
    report("the steady ceiling before it needed no safety clip", before == 0,
           "clips before=" + std::to_string(before) + " after="
               + std::to_string(lim.safetyClipCount()));
}

void testDisabledPassesThrough()
{
    ClientFinalLimiter lim;
    configure(lim, 1.5f, -0.5f);
    lim.setEnabled(false);
    lim.prepare(kRate);
    std::vector<float> mono(960);
    for (int i = 0; i < 960; ++i) mono[static_cast<std::size_t>(i)] = 1.5f * std::sin(i * 0.05f);
    auto buf = stereoOf(mono);
    const auto in = buf;
    run(lim, buf);
    report("disabled, the signal passes untouched and undelayed", buf == in);
}

}  // namespace

int main()
{
    testDefaults();
    testRangesClamp();
    testNothingPassesTheCeiling();
    testDelayIsTheLookahead();
    testGainRampsInsteadOfStepping();
    testChangesFadeTheTap();
    testChangeDuringAFadeDoesNotJump();
    testCeilingChangeWhileLimiting();
    testDisabledPassesThrough();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
