// Pins the packed TX chain order (TxChainLayout) that AudioEngine hands the
// audio thread: 16 four-bit slots, no silent truncation, and the settings
// migration that appends stages an upgrade adds. Socket-free.

#include "core/AudioEngine.h"
#include "core/TxChainLayout.h"
#include "core/TxVoiceProcessor.h"

#include <QVector>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::AudioEngine;
using AetherSDR::TxVoiceProcessor;
namespace Layout = AetherSDR::TxChainLayout;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-66s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// A stand-in for the chain once more stage types exist than today: values
// 1..15 are every type a slot can carry.
enum class FutureStage : uint8_t {
    None = 0, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11, S12, S13, S14, S15, Overflow
};

std::vector<FutureStage> futureRange(int from, int to)
{
    std::vector<FutureStage> out;
    for (int v = from; v <= to; ++v) {
        out.push_back(static_cast<FutureStage>(v));
    }
    return out;
}

void testRoundTripAllSlots()
{
    // Sixteen slots, fifteen types: the sixteenth slot repeats a type.
    auto stages = futureRange(1, 15);
    stages.push_back(FutureStage::S7);
    uint64_t packed = 0;
    const bool fits = Layout::pack(stages, packed);
    const auto back = Layout::unpack<std::vector<FutureStage>>(packed);
    report("all 16 slots pack", fits);
    report("all 16 slots unpack to the same order", back == stages,
           std::to_string(back.size()) + " stages back");

    bool positions = true;
    for (int i = 0; i < Layout::kSlots; ++i) {
        positions = positions && Layout::slot<FutureStage>(packed, i) == stages[static_cast<std::size_t>(i)]
            && ((packed >> (i * 4)) & 0xF) == static_cast<uint64_t>(stages[static_cast<std::size_t>(i)]);
    }
    report("slot i occupies bits 4i..4i+3", positions);
}

void testOverflowIsReported()
{
    auto seventeen = futureRange(1, 15);
    seventeen.push_back(FutureStage::S1);
    seventeen.push_back(FutureStage::S2);
    uint64_t packed = 0;
    report("17 stages report that they do not fit", !Layout::pack(seventeen, packed));
    report("the 16 that fit are still packed in order",
           Layout::unpack<std::vector<FutureStage>>(packed)
               == std::vector<FutureStage>(seventeen.begin(), seventeen.begin() + 16));

    const std::vector<FutureStage> tooBig{FutureStage::S1, FutureStage::Overflow, FutureStage::S2};
    report("a value above 15 reports that it does not fit", !Layout::pack(tooBig, packed));
    report("a value above 15 is left out, not wrapped into a neighbour",
           Layout::unpack<std::vector<FutureStage>>(packed)
               == std::vector<FutureStage>{FutureStage::S1, FutureStage::S2});

    const std::vector<FutureStage> withNone{FutureStage::S1, FutureStage::None, FutureStage::S2};
    report("a None inside the list reports that it does not fit", !Layout::pack(withNone, packed));
    report("a None inside the list cannot end the chain early",
           Layout::unpack<std::vector<FutureStage>>(packed)
               == std::vector<FutureStage>{FutureStage::S1, FutureStage::S2});
}

void testUpgradeFromSevenStageList()
{
    // An operator's saved order of today's seven stages, as values 1..7 in
    // their own order, against a canonical list that has grown to ten.
    const QVector<FutureStage> saved{FutureStage::S4, FutureStage::S1, FutureStage::S2,
                                     FutureStage::S3, FutureStage::S5, FutureStage::S6,
                                     FutureStage::S7};
    const auto canonicalTen = futureRange(1, 10);
    const QVector<FutureStage> canonical(canonicalTen.begin(), canonicalTen.end());
    const auto merged = Layout::withMissingAppended(saved, canonical);
    uint64_t packed = 0;
    const bool fits = Layout::pack(merged, packed);
    const auto live = Layout::unpack<QVector<FutureStage>>(packed);

    QVector<FutureStage> expected = saved;
    expected << FutureStage::S8 << FutureStage::S9 << FutureStage::S10;
    report("upgrade: saved order first, the three new stages after it", merged == expected);
    report("upgrade: all ten survive packing", fits && live == expected,
           std::to_string(live.size()) + " live");
}

void testDuplicatesAreDropped()
{
    const std::vector<FutureStage> repeated{FutureStage::S3, FutureStage::S1, FutureStage::S3,
                                            FutureStage::S2, FutureStage::S1};
    report("repeats are dropped, first occurrence kept",
           Layout::withoutDuplicates(repeated)
               == std::vector<FutureStage>{FutureStage::S3, FutureStage::S1, FutureStage::S2});

    // Forty entries of three types: too long to pack as stored, three after.
    std::vector<FutureStage> forty;
    for (int i = 0; i < 40; ++i) {
        forty.push_back(static_cast<FutureStage>(1 + i % 3));
    }
    uint64_t packed = 0;
    report("a long repeated list packs once its repeats are dropped",
           !Layout::pack(forty, packed) && Layout::pack(Layout::withoutDuplicates(forty), packed));
}

void testFreshInstall()
{
    const auto canonicalTen = futureRange(1, 10);
    const QVector<FutureStage> canonical(canonicalTen.begin(), canonicalTen.end());
    const auto merged = Layout::withMissingAppended(QVector<FutureStage>{}, canonical);
    uint64_t packed = 0;
    const bool fits = Layout::pack(merged, packed);
    report("fresh install: the canonical order verbatim, all ten live",
           fits && Layout::unpack<QVector<FutureStage>>(packed) == canonical);
}

void testTodaysStages()
{
    const QVector<AudioEngine::TxChainStage> today{
        AudioEngine::TxChainStage::Tube, AudioEngine::TxChainStage::Eq,
        AudioEngine::TxChainStage::Comp, AudioEngine::TxChainStage::Enh,
        AudioEngine::TxChainStage::Reverb};
    uint64_t packed = 0;
    const bool fits = Layout::pack(today, packed);
    report("today's five stages pack and round-trip",
           fits && Layout::unpack<QVector<AudioEngine::TxChainStage>>(packed) == today);
    report("the engine and the voice processor read the same slot count",
           AudioEngine::kMaxTxChainStages == 16 && TxVoiceProcessor::kMaxStages == 16);

    // The processor reads the engine's packed value directly.
    bool same = true;
    for (int i = 0; i < today.size(); ++i) {
        same = same && static_cast<uint8_t>(Layout::slot<TxVoiceProcessor::Stage>(packed, i))
                           == static_cast<uint8_t>(today[i]);
    }
    report("the voice processor decodes the engine's order", same);
}

void testRetiredStageIsDropped()
{
    using S = AudioEngine::TxChainStage;
    const QVector<S> saved{S::Comp, S::Gate, S::Tube, S::Gate};
    const QVector<S> kept = Layout::withoutRetired(saved, S::Gate);
    report("a retired stage is dropped and the rest keep their order",
           kept == QVector<S>{S::Comp, S::Tube});
    report("an order without it is unchanged",
           Layout::withoutRetired(kept, S::Gate) == kept);
}

}  // namespace

int main()
{
    testRoundTripAllSlots();
    testOverflowIsReported();
    testUpgradeFromSevenStageList();
    testDuplicatesAreDropped();
    testFreshInstall();
    testTodaysStages();
    testRetiredStageIsDropped();
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
