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
//
// The spin is this file's convention, not an oversight: every timing test here
// advances the same real clock the generator reads, and the element durations
// they assert on are only as tight as the wait that produces them.  Swapping in
// sleep_for() would be a file-wide change with those assertions downstream of
// it, so it wants to be one deliberate decision rather than a local edit.
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

    // ── 12. Push-pump race: an element keyed while the render head is ahead
    // of wall clock keeps its exact duration (#4890).  A push-model sink
    // keeps the device buffer full, so process() renders ahead of real time
    // and an edge's timestamp-mapped position can already be rendered by the
    // time the edge is consumed.  Clamping just that edge to the block start
    // quantized BOTH edges of the element to render-head positions — on the
    // bench this collapsed rhythm into whole-block steps (Linux: emission
    // SD 0.2 ms rendered as 6–8 ms, in 5.3 ms quanta).  The anchor must
    // shift forward instead, preserving the element's length.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);

        // Element A anchors the burst and flushes normally.  Deliberately
        // twice element B's length: if B collapses at the render head, the
        // last loud run the assertions measure is A, and A's length must
        // not fit B's expected window.
        gen.setKeyDown(true);
        auto blk = runFrames(gen, 128);
        std::vector<float> cat(blk.begin(), blk.end());
        const auto a1 = clock::now();
        while (clock::now() - a1 < std::chrono::milliseconds(20)) { /* spin */ }
        gen.setKeyDown(false);
        while (cat.size() < 2000) {
            blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }

        // Inter-element gap, during which the "pump" tops the buffer up —
        // the render head moves ~80 ms ahead while the anchor is retained
        // (gap and lead both well under the idle re-anchor threshold).
        const auto g0 = clock::now();
        while (clock::now() - g0 < std::chrono::milliseconds(40)) { /* spin */ }
        while (cat.size() < 6000) {
            blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }

        // Element B: keyed in real time, consumed with the head far ahead.
        gen.setKeyDown(true);
        const auto b1 = clock::now();
        while (clock::now() - b1 < std::chrono::milliseconds(10)) { /* spin */ }
        const auto c0 = clock::now();
        gen.setKeyDown(false);
        const auto c1 = clock::now();
        for (int b = 0; b < 40; ++b) {
            blk = runFrames(gen, 128);
            cat.insert(cat.end(), blk.begin(), blk.end());
        }

        // Locate element B — the last loud run — and measure its length.
        int lastStart = -1, lastEnd = -1;
        bool loudRun = false;
        for (int i = 0; i + 8 <= static_cast<int>(cat.size()); i += 8) {
            const float peak = maxAbs(std::vector<float>(
                cat.begin() + i, cat.begin() + i + 8));
            const bool loud = peak > 0.1f;
            if (loud && !loudRun) lastStart = i;
            if (loud) lastEnd = i + 8;
            loudRun = loud;
        }
        ok &= expect(toneBursts(cat) == 2,
                     "push-pump race: late element still renders as its own"
                     " burst");
        // The assertions below only exercise the late-edge branch if the
        // render head actually got ahead of wall clock above; on a runner
        // where runFrames() is slower than real time nothing is ever late and
        // they would pass against the pre-fix code too.  Require the branch.
        ok &= expect(gen.shiftCount() > 0,
                     "push-pump race: the late-edge branch actually fired");
        const auto loNs = std::chrono::duration_cast<std::chrono::nanoseconds>(c0 - b1).count();
        const auto hiNs = std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - b1).count();
        const int lo = static_cast<int>(loNs * 48000 / 1'000'000'000LL) - 16;
        const int hi = static_cast<int>(hiNs * 48000 / 1'000'000'000LL) + 144;
        const int lenB = (lastStart >= 0 && lastEnd > lastStart)
                             ? lastEnd - lastStart : -1;
        ok &= expect(lenB >= lo && lenB <= hi,
                     "push-pump race: element duration is preserved, not"
                     " clamped to the render head");
    }

    // ── 13. Sustained racing does not walk the mapping into the staleness
    // guard (#4890).  Review asked whether the per-edge forward shift, being
    // uncapped, could accumulate until the guard fires mid-burst and re-quantizes
    // an onset at an unpredictable moment.  It cannot: a shift moves the mapping
    // ONTO the render head rather than past it, so the guard's quantity after a
    // shift is the wall time since that edge, not a running total.  This pins
    // that — a long burst rendered faster than real time, so every element
    // shifts, must produce no staleness re-anchor at all.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        const auto spin = [](int ms) {
            const auto t = clock::now();
            while (clock::now() - t < std::chrono::milliseconds(ms)) { /* spin */ }
        };

        // 60 elements keyed on the real clock but rendered ~3x faster, so the
        // head gains on wall clock every single element and each edge is late.
        for (int n = 0; n < 60; ++n) {
            gen.setKeyDown(true);
            spin(5);
            for (int b = 0; b < 6; ++b) runFrames(gen, 128);   // 16 ms audio / 5 ms real
            gen.setKeyDown(false);
            spin(5);
            for (int b = 0; b < 6; ++b) runFrames(gen, 128);
        }
        ok &= expect(gen.shiftCount() > 20,
                     "sustained racing: the late-edge branch fired throughout");
        ok &= expect(gen.staleReanchorCount() == 0,
                     "sustained racing: shifts never trip the staleness guard");
    }

    // ── 14. Learned slack decays (#4890).  It must be able to shrink: this
    // generator exists to beat the radio's 30–100 ms round trip, so a slack
    // that only ratcheted upward would let one transient stall park the tone
    // inside that range for the rest of the session.  A burst that never runs
    // late is evidence the sink has headroom, and gives some back.
    {
        using clock = std::chrono::steady_clock;
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);

        const auto spin = [](int ms) {
            const auto t = clock::now();
            while (clock::now() - t < std::chrono::milliseconds(ms)) { /* spin */ }
        };

        // Stall the pump so an edge arrives far behind the render head; that
        // teaches the mapping the maximum slack.
        gen.setKeyDown(true);
        for (int b = 0; b < 300; ++b) runFrames(gen, 128);   // race ~800 ms ahead
        gen.setKeyDown(false);
        gen.setKeyDown(true);
        gen.setKeyDown(false);
        for (int b = 0; b < 8; ++b) runFrames(gen, 128);
        ok &= expect(gen.shiftCount() > 0, "slack decay: the stall produced late edges");

        // Clean bursts: both edges are queued with no rendering between them,
        // so neither can land behind the head however far ahead the head is —
        // the anchor is taken relative to the current block. Each burst is
        // followed by enough idle to release the anchor, which is where a
        // burst that never ran late gives slack back.
        const auto cleanBurst = [&](int idleBlocks, std::vector<float>* out) {
            gen.setKeyDown(true);
            spin(5);
            gen.setKeyDown(false);
            for (int b = 0; b < idleBlocks; ++b) {
                auto blk = runFrames(gen, 128);
                if (out) out->insert(out->end(), blk.begin(), blk.end());
            }
        };

        // Release the stall's anchor (it ran late, so releasing it does not
        // halve the slack) so the next burst re-anchors fresh: the onset that
        // burst renders IS the carried slack, measured end to end.
        for (int b = 0; b < 100; ++b) runFrames(gen, 128);  // > kReanchorIdleMs

        // seg is mono — runFrames() returns one entry per frame.
        const auto onsetOf = [](const std::vector<float>& seg) -> int64_t {
            for (int i = 0; i + 8 <= static_cast<int>(seg.size()); i += 8) {
                float peak = 0.0f;
                for (int j = i; j < i + 8; ++j)
                    peak = std::max(peak, std::abs(seg[j]));
                if (peak > 0.1f) return i;
            }
            return -1;
        };

        // Both ends of the decay are pinned.  Near end: the first fresh
        // anchor after the stall spends the full taught slack, so the onset
        // sits at the cap (1920 samples = 40 ms at 48 kHz).  If the idle
        // counter survived the anchor take, the still-saturated counter
        // would release each new anchor once per empty run-up block and
        // halve the slack every time — collapsing the onset to under five
        // blocks (measured: 632 samples) and silently capping carried slack
        // at one audio block instead of kAnchorSlackCapMs.
        std::vector<float> first;
        cleanBurst(130, &first);           // 130×128 ≈ 347 ms > kReanchorIdleMs
        const int64_t onset0 = onsetOf(first);
        ok &= expect(onset0 >= 1920 - 256 && onset0 <= 1920 + 256,
                     "slack decay: first fresh anchor spends the full taught slack");

        for (int burst = 0; burst < 8; ++burst)
            cleanBurst(130, nullptr);

        // Far end: after repeated clean bursts the slack must have decayed
        // back to (about) the block start instead of staying latched at what
        // the stall taught it.
        std::vector<float> seg;
        cleanBurst(40, &seg);
        const int64_t onset = onsetOf(seg);
        ok &= expect(onset >= 0 && onset <= 256,
                     "slack decay: onset returns to the block start after clean bursts");
    }

    return ok ? 0 : 1;
}
