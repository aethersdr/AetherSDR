#include "core/CwSidetoneGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace AetherSDR;

namespace {

bool expect(bool cond, const char* label)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", label);
    return cond;
}

double rms(const std::vector<float>& buf)
{
    if (buf.empty()) return 0.0;
    double s = 0.0;
    for (float v : buf) s += static_cast<double>(v) * v;
    return std::sqrt(s / buf.size());
}

float maxAbs(const std::vector<float>& buf)
{
    float m = 0.0f;
    for (float v : buf) m = std::max(m, std::abs(v));
    return m;
}

// Run the generator for `frames` samples, returning the L channel only.
std::vector<float> runFrames(CwSidetoneGenerator& gen, int frames)
{
    std::vector<float> stereo(frames * 2, 0.0f);
    gen.process(stereo.data(), frames);
    std::vector<float> mono(frames);
    for (int i = 0; i < frames; ++i) mono[i] = stereo[2 * i];
    return mono;
}

// 48 kHz samples spanned by a measured wall-clock interval, plus margin.
// The placement cases size their render window from the interval they
// actually measured rather than a fixed constant: a scheduler stall inside
// one of their spin loops then lengthens the window instead of pushing the
// edge past the end of it and failing.
int samplesFor(std::chrono::steady_clock::duration d, int marginFrames = 960)
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    return static_cast<int>(ns * 48000 / 1'000'000'000LL) + marginFrames;
}

// Count contiguous runs of audible 8-sample windows.
int toneBursts(const std::vector<float>& buf)
{
    int bursts = 0;
    bool loudRun = false;
    for (int i = 0; i + 8 <= static_cast<int>(buf.size()); i += 8) {
        const float peak = maxAbs(std::vector<float>(
            buf.begin() + i, buf.begin() + i + 8));
        const bool loud = peak > 0.1f;
        if (loud && !loudRun) ++bursts;
        loudRun = loud;
    }
    return bursts;
}

// Key one element of `ms` on, `ms` off, spinning on the real clock.
void keyElement(CwSidetoneGenerator& gen, int ms)
{
    using clock = std::chrono::steady_clock;
    gen.setKeyDown(true);
    auto t = clock::now();
    while (clock::now() - t < std::chrono::milliseconds(ms)) { /* spin */ }
    gen.setKeyDown(false);
    t = clock::now();
    while (clock::now() - t < std::chrono::milliseconds(ms)) { /* spin */ }
}

} // namespace

int main()
{
    bool ok = true;

    // ── 1. Disabled generator emits nothing ─────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(false);
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);  // 10 ms
        ok &= expect(maxAbs(mono) == 0.0f, "disabled generator emits silence");
    }

    // ── 2. Enabled, key down → produces a tone ──────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);  // hard keying for this test
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);
        ok &= expect(rms(mono) > 0.3, "key-down at full volume produces tone");
        ok &= expect(maxAbs(mono) <= 1.0f, "tone never clips");
    }

    // ── 3. Pitch is approximately correct ───────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        gen.setPitchHz(600.0f);
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 4800);  // 100 ms

        // Count zero crossings — for a 600 Hz tone over 100 ms, expect 120
        // crossings (60 cycles × 2).  Allow ±2 for boundary effects.
        int crossings = 0;
        for (size_t i = 1; i < mono.size(); ++i) {
            if ((mono[i - 1] < 0.0f && mono[i] >= 0.0f) ||
                (mono[i - 1] >= 0.0f && mono[i] < 0.0f))
                ++crossings;
        }
        ok &= expect(std::abs(crossings - 120) <= 2,
                     "600 Hz pitch yields ~120 zero-crossings in 100 ms");
    }

    // ── 4. Volume scaling ──────────────────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setShapingMs(0.0f);
        gen.setKeyDown(true);

        gen.setVolume(1.0f);
        const double rmsFull = rms(runFrames(gen, 480));

        gen.setVolume(0.5f);
        const double rmsHalf = rms(runFrames(gen, 480));

        // Half volume → roughly half RMS (within 5% tolerance for phase
        // continuity across the runs).
        const double ratio = rmsHalf / rmsFull;
        ok &= expect(ratio > 0.45 && ratio < 0.55,
                     "volume 0.5 produces ~half RMS of volume 1.0");
    }

    // ── 5. Raised-cosine envelope: no instant jump from 0 to peak ──────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setPan(0.0f);        // test reads L channel only
        gen.setShapingMs(5.0f);  // 5 ms ramp
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);  // 10 ms total — first 5 ms is ramp

        // Check peak over a small window — single-sample checks land
        // arbitrarily on the sine wave so use windowed maxAbs instead.
        const auto peakIn = [&](int begin, int end) {
            float m = 0.0f;
            for (int i = begin; i < end && i < (int)mono.size(); ++i)
                m = std::max(m, std::abs(mono[i]));
            return m;
        };
        const float startPeak = peakIn(0, 24);   // first 0.5 ms of ramp
        const float endPeak   = peakIn(240, 480); // post-ramp sustain
        ok &= expect(startPeak < 0.05f,
                     "raised-cosine ramp starts near zero");
        ok &= expect(endPeak > 0.9f,
                     "envelope reaches full amplitude after 5 ms ramp");
    }

    // ── 6. Key-up ramps down to silence ────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(5.0f);
        gen.setKeyDown(true);
        runFrames(gen, 4800);  // sustain for 100 ms

        gen.setKeyDown(false);
        auto rampDown = runFrames(gen, 480);  // 10 ms

        // First samples loud (mid-tone), last samples near zero.
        const float startAmp = maxAbs(std::vector<float>(
            rampDown.begin(), rampDown.begin() + 24));  // first 0.5 ms
        const float endAmp = maxAbs(std::vector<float>(
            rampDown.end() - 24, rampDown.end()));     // last 0.5 ms
        ok &= expect(startAmp > 0.5f, "ramp-down starts at full amplitude");
        ok &= expect(endAmp < 0.05f, "ramp-down ends near silence");
    }

    // ── 7. Reset returns to idle state ─────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        gen.setKeyDown(true);
        runFrames(gen, 480);
        gen.reset();
        gen.setKeyDown(false);
        auto silent = runFrames(gen, 480);
        ok &= expect(maxAbs(silent) == 0.0f, "reset + key-up produces silence");
    }

    // ── 8. Timestamped edges: a sub-block down/up pair is rendered at the
    // exact sample spacing, not lost or snapped to a block boundary (#4809).
    // On the pre-#4809 block-polling gate this exact sequence produced
    // total silence: both transitions landed before the first process()
    // call, so the single bool already read `false` again.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);  // 1-sample ramp — near-hard keying

        const auto a0 = clock::now();
        gen.setKeyDown(true);
        const auto a1 = clock::now();
        while (clock::now() - a1 < std::chrono::milliseconds(10)) { /* spin */ }
        const auto b0 = clock::now();
        gen.setKeyDown(false);
        const auto b1 = clock::now();

        // One block covering both edges, sized from the interval measured
        // above so a stall in the spin loop can't push the key-up past it.
        auto mono = runFrames(gen, std::max(1440, samplesFor(b1 - a0)));

        // Envelope edge: last 8-sample window whose peak clears threshold
        // (single samples of a sine pass through zero mid-tone).
        int lastLoud = -1;
        for (int i = 0; i + 8 <= static_cast<int>(mono.size()); i += 8) {
            const float peak = maxAbs(std::vector<float>(
                mono.begin() + i, mono.begin() + i + 8));
            if (peak > 0.1f) lastLoud = i + 8;
        }
        const auto loNs = std::chrono::duration_cast<std::chrono::nanoseconds>(b0 - a1).count();
        const auto hiNs = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - a0).count();
        const int lo = static_cast<int>(loNs * 48000 / 1'000'000'000LL) - 16;
        const int hi = static_cast<int>(hiNs * 48000 / 1'000'000'000LL) + 16;
        ok &= expect(lastLoud > 0, "sub-block down/up pair produces a tone at all");
        ok &= expect(lastLoud >= lo && lastLoud <= hi,
                     "key-up lands at the timestamp-mapped sample, not a block edge");

        // Same pair pumped through 128-frame blocks must place the edge
        // mid-block (~480), not quantize it to a 128-sample boundary.
        CwSidetoneGenerator gen2(48000);
        gen2.setEnabled(true);
        gen2.setVolume(1.0f);
        gen2.setShapingMs(0.0f);
        const auto c0 = clock::now();
        gen2.setKeyDown(true);
        const auto c1 = clock::now();
        while (clock::now() - c1 < std::chrono::milliseconds(10)) { /* spin */ }
        const auto d0 = clock::now();
        gen2.setKeyDown(false);
        const auto d1 = clock::now();
        std::vector<float> cat;
        const int blocks2 = std::max(12, (samplesFor(d1 - c0) + 127) / 128);
        for (int b = 0; b < blocks2; ++b) {
            auto blk = runFrames(gen2, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }
        int lastLoud2 = -1;
        for (int i = 0; i + 8 <= static_cast<int>(cat.size()); i += 8) {
            const float peak = maxAbs(std::vector<float>(
                cat.begin() + i, cat.begin() + i + 8));
            if (peak > 0.1f) lastLoud2 = i + 8;
        }
        const auto lo2Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d0 - c1).count();
        const auto hi2Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d1 - c0).count();
        const int lo2 = static_cast<int>(lo2Ns * 48000 / 1'000'000'000LL) - 16;
        const int hi2 = static_cast<int>(hi2Ns * 48000 / 1'000'000'000LL) + 16;
        ok &= expect(lastLoud2 >= lo2 && lastLoud2 <= hi2,
                     "128-frame blocks: edge placed mid-block by timestamp");
    }

    // ── 8b. The anchor survives an inter-element gap ───────────────────────
    // A second element after a real gap must onset at its timestamp-mapped
    // sample.  With a per-gap re-anchor the queued second key-down snaps to
    // the next block's start instead — element lengths stay exact but the
    // rhythm (onset-to-onset spacing) stays block-quantized, which is the
    // irregularity #4809 is about.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);

        const auto a0 = clock::now();
        gen.setKeyDown(true);
        const auto a1 = clock::now();
        while (clock::now() - a1 < std::chrono::milliseconds(10)) { /* spin */ }
        gen.setKeyDown(false);
        const auto b1 = clock::now();
        // Inter-element gap: long enough for the ramp to finish and many
        // Idle blocks to elapse, far below the idle re-anchor timeout.
        while (clock::now() - b1 < std::chrono::milliseconds(40)) { /* spin */ }
        const auto c0 = clock::now();
        gen.setKeyDown(true);
        const auto c1 = clock::now();
        while (clock::now() - c1 < std::chrono::milliseconds(10)) { /* spin */ }
        gen.setKeyDown(false);

        std::vector<float> cat;
        const int blocks = std::max(30, (samplesFor(c1 - a0) + 127) / 128);
        for (int b = 0; b < blocks; ++b) {
            auto blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }
        // Second element's onset: first loud window after the quiet gap
        // that follows the first loud run.
        int onset2 = -1;
        bool sawLoud = false, sawGap = false;
        for (int i = 0; i + 8 <= static_cast<int>(cat.size()); i += 8) {
            const float peak = maxAbs(std::vector<float>(
                cat.begin() + i, cat.begin() + i + 8));
            const bool loud = peak > 0.1f;
            if (loud && !sawLoud && sawGap) { onset2 = i; break; }
            if (!loud && sawLoud) sawGap = true;
            sawLoud = loud;
        }
        const auto loNs = std::chrono::duration_cast<std::chrono::nanoseconds>(c0 - a1).count();
        const auto hiNs = std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - a0).count();
        const int lo = static_cast<int>(loNs * 48000 / 1'000'000'000LL) - 16;
        const int hi = static_cast<int>(hiNs * 48000 / 1'000'000'000LL) + 16;
        ok &= expect(onset2 > 0, "cross-element: second element sounds at all");
        ok &= expect(onset2 >= lo && onset2 <= hi,
                     "cross-element: onset lands at the timestamp-mapped sample,"
                     " anchor survives the gap");
    }

    // ── 9. Queue-overflow fallback: the true final state still lands ───────
    // 201 toggles overflow the 64-slot ring; the last edge the ring caught
    // is a key-DOWN, but the true final state is key-UP — only the
    // m_keyDown fallback can deliver it.  The gate must not stick down.
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        for (int i = 0; i <= 200; ++i)          // i=200 (even) ends key-UP
            gen.setKeyDown((i % 2) == 1);
        auto first = runFrames(gen, 480);        // replays the 64 queued edges
        ok &= expect(maxAbs(first) > 0.0f,
                     "overflow replay leaves the stale key-down sounding");
        runFrames(gen, 480);                     // fallback fires at this block
        auto third = runFrames(gen, 480);
        ok &= expect(maxAbs(third) == 0.0f,
                     "true final key-up survives queue overflow via fallback");
    }

    // ── 10. Edges queued while process() is not being called must not replay
    // into the next burst.  Not every consumer is pumped continuously: the
    // recorder's sidetone generator renders only while the radio is
    // transmitting a CW over (AudioEngine::onCwRecordPump), while
    // setKeyDown() keeps queueing from every paddle edge regardless.  Without
    // the staleness guard the next over anchors on keying from seconds ago
    // and plays it back ahead of the real elements.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);

        // Three elements keyed with the consumer stopped — nothing rendered.
        for (int k = 0; k < 3; ++k) keyElement(gen, 10);
        // Age them past the re-anchor threshold, still unpumped.
        const auto q = clock::now();
        while (clock::now() - q < std::chrono::milliseconds(300)) { /* spin */ }

        // The consumer starts again, and one real element is keyed.
        const auto e0 = clock::now();
        keyElement(gen, 10);

        std::vector<float> cat;
        const int blocks = std::max(40, (samplesFor(clock::now() - e0) + 127) / 128);
        for (int b = 0; b < blocks; ++b) {
            auto blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }
        ok &= expect(toneBursts(cat) == 1,
                     "stale queued edges are dropped, not replayed into the"
                     " next burst");
    }

    // ── 11. A consumer that pauses long enough for the anchor's wall clock to
    // outrun its stream position re-anchors instead of deferring every edge
    // past the end of the block forever (the same pause, seen from the other
    // side: here the queue is empty across the gap, so only the mapping is
    // stale).
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);

        keyElement(gen, 10);                       // establishes the anchor
        for (int b = 0; b < 20; ++b) runFrames(gen, 128);

        const auto q = clock::now();               // consumer stops rendering
        while (clock::now() - q < std::chrono::milliseconds(400)) { /* spin */ }

        const auto e0 = clock::now();
        keyElement(gen, 10);
        std::vector<float> cat;
        const int blocks = std::max(40, (samplesFor(clock::now() - e0) + 127) / 128);
        for (int b = 0; b < blocks; ++b) {
            auto blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }
        ok &= expect(toneBursts(cat) == 1,
                     "element after a consumer pause still sounds, once");
    }

    return ok ? 0 : 1;
}
