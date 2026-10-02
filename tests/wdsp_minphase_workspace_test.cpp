// WDSP patch 14: minimum-phase design scratch is released after each design.
// Count only WDSP's live allocations at open, after filter/notch edits and
// after teardown. A minimum-phase 8192-tap RX channel holds the same count as a
// linear-phase channel. Socket-free; no audio is processed.

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

    // Compare live allocations after opening at each phase.
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

    // A filter edit rebuilds and releases the design scratch.
    check(mp->setFilter(200.0, 2900.0), "the minimum-phase channel takes a filter change");
    const std::int64_t heldMpOne = allocationsHeldBy(*mp, beforeMp);
    check(heldMpOne == heldLin,
          "after a passband change the re-built workspace is freed again");

    // Repeated designs must not accumulate allocations.
    for (int i = 0; i < 20; ++i) {
        check(mp->setFilter(150.0 + 10.0 * (i % 5), 2800.0 + 20.0 * (i % 7)),
              "each repeated minimum-phase filter change is accepted");
    }
    const std::int64_t heldMpMany = allocationsHeldBy(*mp, beforeMp);
    std::printf("      MEASURED: after 1 and 21 filter changes: %lld and %lld\n",
                static_cast<long long>(heldMpOne), static_cast<long long>(heldMpMany));
    check(heldMpMany == heldMpOne, "twenty more designs accumulate nothing");

    // Tuning rebuilds minimum-phase masks when an active notch is in passband.
    constexpr double kTuneHz = 7100000.0;
    check(mp->setNotchTuneFrequency(kTuneHz), "the notch tune frequency is accepted");
    check(mp->addNotch(0, kTuneHz + 1500.0, 100.0, true), "an active notch is accepted");
    check(mp->setNotchesEnabled(true), "the notch stage is enabled");
    const std::uint64_t beforeTune = WdspChannel::allocationSequenceForTest();
    check(mp->setShift(50.0), "a notch shift is accepted");
    check(mp->setNotchTuneFrequency(kTuneHz + 100.0), "a notch retune is accepted");
    check(WdspChannel::allocationSequenceForTest() > beforeTune,
          "tuning through an active notch actually rebuilds the filter");
    check(allocationsHeldBy(*mp, beforeMp) == heldMpMany,
          "notch edits and tuning release their minimum-phase scratch");

    mp.reset();
    lin.reset();
    check(WdspChannel::outstandingAllocationsForTest() == beforeLin,
          "closing both channels releases every WDSP allocation");
    if (g_failures == 0) {
        std::fprintf(stderr, "wdsp_minphase_workspace_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
