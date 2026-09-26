// WDSP patch 13 (#5954): a minimum-phase FIR core holds its design workspace
// only while designing. create_minphase() builds seven buffers of nc * pfactor
// elements and four FFTW plans -- 13.6 MB at the 8192-tap, pfactor-16 geometry
// Hl2RxDsp runs -- and mp_imp_exec() is its only reader. Upstream kept it for
// the life of the core, which after #5954 made every non-CW HL2 receiver hold
// ~49 MB more than a linear-phase one. calc_fircore() now frees it after each
// design, so:
//
//   1. an 8192-tap RX channel opened at MINIMUM phase holds exactly as many
//      live WDSP allocations as the same channel at LINEAR phase;
//   2. that stays true after a passband change, which re-designs every
//      minimum-phase core (and so rebuilds and frees the workspace again);
//   3. repeated designs do not accumulate: allocations after twenty filter
//      changes equal allocations after one.
//
// Counted with WdspChannel::outstandingAllocationsForTest(), the WDSP port's
// own live-allocation counter, so the test sees exactly WDSP's heap and nothing
// else in the process. Socket-free; no audio is processed.

#include "core/dsp/WdspChannel.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    std::fprintf(stderr, "  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

WdspChannel::Config hl2Shaped(bool minimumPhase)
{
    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Receive;
    c.inputBlockSize = 1024;
    c.dspBlockSize = 1024;
    c.inputSampleRate = 48000;
    c.dspSampleRate = 48000;
    c.outputSampleRate = 24000;
    c.mode = WdspChannel::Mode::Usb;
    c.filterLowHz = 150.0;
    c.filterHighHz = 3000.0;
    c.filterTaps = 8192;   // Hl2RxDsp::kRxFilterTaps
    c.minimumPhase = minimumPhase;
    return c;
}

// Live WDSP allocations held by one open channel of this shape: the counter
// with the channel open, minus the counter before it was opened.
std::int64_t allocationsHeldBy(WdspChannel& ch, std::uint64_t before)
{
    (void)ch;
    return static_cast<std::int64_t>(WdspChannel::outstandingAllocationsForTest())
           - static_cast<std::int64_t>(before);
}

}  // namespace

int main()
{
    std::string err;

    // ---- 1: open ----------------------------------------------------------
    const std::uint64_t beforeLin = WdspChannel::outstandingAllocationsForTest();
    auto lin = WdspChannel::create(hl2Shaped(false), &err);
    check(lin != nullptr, "the linear-phase channel opens");
    if (!lin) {
        std::fprintf(stderr, "  create failed: %s\n", err.c_str());
        return 1;
    }
    const std::int64_t heldLin = allocationsHeldBy(*lin, beforeLin);

    const std::uint64_t beforeMp = WdspChannel::outstandingAllocationsForTest();
    auto mp = WdspChannel::create(hl2Shaped(true), &err);
    check(mp != nullptr, "the minimum-phase channel opens");
    if (!mp) {
        std::fprintf(stderr, "  create failed: %s\n", err.c_str());
        return 1;
    }
    const std::int64_t heldMpOpen = allocationsHeldBy(*mp, beforeMp);
    std::printf("      MEASURED: live WDSP allocations per channel: linear %lld, "
                "minimum phase %lld\n",
                static_cast<long long>(heldLin), static_cast<long long>(heldMpOpen));
    check(heldLin > 0, "the counter sees the channel at all");
    check(heldMpOpen == heldLin,
          "open: a minimum-phase channel holds no more WDSP allocations than a "
          "linear one -- the design workspace was freed");

    // ---- 2: one re-design ---------------------------------------------------
    check(mp->setFilter(200.0, 2900.0), "the minimum-phase channel takes a filter change");
    const std::int64_t heldMpOne = allocationsHeldBy(*mp, beforeMp);
    check(heldMpOne == heldLin,
          "after a passband change the re-built workspace is freed again");

    // ---- 3: many re-designs -------------------------------------------------
    for (int i = 0; i < 20; ++i) {
        mp->setFilter(150.0 + 10.0 * (i % 5), 2800.0 + 20.0 * (i % 7));
    }
    const std::int64_t heldMpMany = allocationsHeldBy(*mp, beforeMp);
    std::printf("      MEASURED: after 1 and 21 filter changes: %lld and %lld\n",
                static_cast<long long>(heldMpOne), static_cast<long long>(heldMpMany));
    check(heldMpMany == heldMpOne, "twenty more designs accumulate nothing");

    mp.reset();
    lin.reset();
    if (g_failures == 0) {
        std::fprintf(stderr, "wdsp_minphase_workspace_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
