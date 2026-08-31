// aetherd HL2 — notch seeding must be idempotent.
//
// Hl2Backend::seedNotches() pushes the radio-wide notch set into one receiver's
// chain. It does NOT run once per connect: pushInitialState() calls it on every
// linkUp, and MetisClient re-emits linkUp after an EP6 silence timeout without
// any new connectRadio() — buildReceivers() only runs from connectRadio(), so
// on that path the Hl2RxDsp objects, their notch mirrors, and WDSP's own notch
// database all survive.
//
// A seed that only appended therefore gave every notch a second copy in WDSP
// while the backend's record still held one. The positional index the whole
// stable-id mapping rests on then addressed the wrong notch, and the duplicate
// kept notching a frequency with nothing on screen to remove it — a failure
// with no error anywhere, visible only in the audio.
//
// This pins the property that fixes it: seeding REPLACES. Seeding the same set
// twice leaves exactly one copy, in both the mirror and WDSP, and the two
// counts agree — which is what the index map requires.

#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct SeedNotch {
    double centerHz;
    double widthHz;
};

// The same sequence Hl2Backend::seedNotches() performs, minus the queued-call
// plumbing: clear, then replay in index order.
void seed(Hl2RxDsp& dsp, const std::vector<SeedNotch>& notches, double tuneHz)
{
    dsp.setNotchTuneFrequency(tuneHz);
    dsp.clearNotches();
    for (std::size_t index = 0; index < notches.size(); ++index) {
        dsp.addNotch(static_cast<int>(index), notches[index].centerHz,
                     notches[index].widthHz, true);
    }
    dsp.setNotchesEnabled(true);
}

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    Hl2RxDsp::Config config;
    config.inputSampleRateHz = 48000;
    config.audioSampleRateHz = 48000;
    config.dspBlockSize = 1024;
    std::string error;
    if (!dsp.configure(config, &error)) {
        std::fprintf(stderr, "FAIL: could not configure Hl2RxDsp: %s\n",
                     error.c_str());
        return 1;
    }

    const double tuneHz = 7'000'000.0;
    const std::vector<SeedNotch> set {
        {tuneHz + 1500.0, 400.0},
        {tuneHz + 2500.0, 400.0},
    };

    seed(dsp, set, tuneHz);
    check(dsp.notchCount() == 2, "first seed did not place two notches");
    check(dsp.wdspNotchCount() == 2, "first seed did not reach WDSP");

    // The linkUp re-entry. Before the fix this produced four notches in both
    // places, with the duplicates interleaved and unaddressable.
    seed(dsp, set, tuneHz);
    check(dsp.notchCount() == 2, "re-seeding duplicated the mirror");
    check(dsp.wdspNotchCount() == 2, "re-seeding duplicated WDSP's database");
    check(dsp.notchCount() == dsp.wdspNotchCount(),
          "mirror and WDSP disagree after a re-seed; the index map is broken");

    // A shorter set replaces rather than merges — the reconnect case, where the
    // operator removed a notch while the link was down.
    seed(dsp, {set.front()}, tuneHz);
    check(dsp.notchCount() == 1, "re-seeding a shorter set left stale entries");
    check(dsp.wdspNotchCount() == 1, "WDSP kept a notch the new set dropped");

    // And an empty set empties both, which is what a fresh session seeds.
    seed(dsp, {}, tuneHz);
    check(dsp.notchCount() == 0, "seeding an empty set left mirror entries");
    check(dsp.wdspNotchCount() == 0, "seeding an empty set left WDSP entries");

    // --- Plan 4.1: notch-tap gating -----------------------------------------
    // The RX FIR runs the low-latency length until a notch is placed, switches
    // to the notch-capable length for as long as any notch exists, and drops
    // back — all in place, without closing and reopening the channel.
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsShort,
          "no notch -> short (low-latency) RX filter");
    const int chanId = dsp.wdspChannelId();

    dsp.addNotch(0, tuneHz + 1500.0, 50.0, true);
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsLong,
          "first notch -> long (50 Hz-capable) RX filter");
    check(dsp.wdspChannelId() == chanId,
          "the tap change kept the same WDSP channel (no close/reopen)");
    check(dsp.wdspNotchCount() == 1, "the notch survived the tap change");

    dsp.addNotch(1, tuneHz + 2500.0, 400.0, true);
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsLong,
          "a second notch keeps the long filter");

    dsp.removeNotch(1);
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsLong,
          "one notch remaining -> still long");

    dsp.removeNotch(0);
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsShort,
          "last notch removed -> back to the short filter");
    check(dsp.wdspChannelId() == chanId,
          "dropping back to short kept the same WDSP channel");

    dsp.addNotch(0, tuneHz + 1000.0, 50.0, true);
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsLong,
          "a re-added notch pulls the filter long again");
    dsp.clearNotches();
    check(dsp.channelConfig().filterTaps == Hl2RxDsp::kRxFilterTapsShort,
          "clearNotches() lands on the short filter");

    if (g_failures == 0)
        std::printf("hl2_notch_seed_test: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
