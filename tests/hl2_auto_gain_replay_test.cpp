// Replays two recorded on-air legs (tests/Hl2AutoGainReplayD168.h) through
// autoGainStep and counts gain changes. A recorded clip applies only while the
// law holds no more attenuation than the radio did then, and a recorded peak is
// scaled by the difference. Where the law holds LESS than the radio did the
// record has no clip evidence: that time is reported as `blind`, not hidden.

#include "core/backends/hl2/Hl2AutoGainPolicy.h"

#include "Hl2AutoGainReplayD168.h"

#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

using AetherSDR::hl2::AutoGainAction;
using AetherSDR::hl2::AutoGainConfig;
using AetherSDR::hl2::AutoGainObservation;
using AetherSDR::hl2::AutoGainState;
using AetherSDR::hl2::Ep4Stats;
using AetherSDR::hl2::autoGainStep;
using AetherSDR::hl2::bandscopeHeadroom;
using AetherSDR::hl2::bandscopeReleaseConfig;
using AetherSDR::hl2::gatedPeakBiasDbForPeriod;
using AetherSDR::hl2::kEp4BlockSamples;
using AetherSDR::hl2::kEp4FullScale;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

struct Leg {
    const char* name;
    int ticks;
    std::span<const d168::Change> changes;
    std::span<const d168::Clip> clips;
    std::span<const d168::Block> blocks;
};

const Leg k80m{"80 m", d168::k80mTicks, d168::k80mChanges, d168::k80mClips,
               d168::k80mBlocks};
const Leg k30m{"30 m", d168::k30mTicks, d168::k30mChanges, d168::k30mClips,
               d168::k30mBlocks};

// The policy tick that carries a logged change: the first one ending at or
// after it.
int tickOf(int ms)
{
    return ms <= 0 ? 0 : (ms + d168::kTickMs - 1) / d168::kTickMs - 1;
}

// The offset the radio held at `ms` after arming, from the logged changes.
int recordedOffsetAt(const Leg& leg, int ms)
{
    int offset = 0;
    for (const d168::Change& c : leg.changes) {
        if (c.ms <= ms) {
            offset = c.offsetDb;
        }
    }
    return offset;
}

Ep4Stats blockStats(int peakCode)
{
    Ep4Stats s;
    if (peakCode <= 0) {
        return s;                         // no reading
    }
    s.samples = kEp4BlockSamples;
    s.peakAbs = peakCode >= kEp4FullScale ? kEp4FullScale - 1 : peakCode;
    s.clippedSamples = peakCode >= kEp4FullScale - 1 ? 1 : 0;
    return s;
}

struct Run {
    std::vector<d168::Change> trace;      // the law's own changes
    int clippedObs = 0;
    int attenuatedMs = 0;                 // law holds any attenuation
    int heldMoreMs = 0;                   // law holds more than the radio did
    int blindMs = 0;                      // law holds less: no clip evidence
    std::int64_t maxDwellMs = 0;
    int finalOffsetDb = 0;
};

Run replay(const Leg& leg, const AutoGainConfig& cfg)
{
    Run run;
    AutoGainState st;
    std::size_t clip = 0;
    std::size_t block = 0;
    int blockShiftDb = 0;                 // radio offset minus law offset, at arrival
    bool clippedLastTick = false;
    for (int i = 0; i < leg.ticks; ++i) {
        const int nowMs = (i + 1) * d168::kTickMs;
        AutoGainObservation o;
        o.samples = d168::kSamplesPerTick;
        o.elapsedMs = d168::kTickMs;
        o.availableOffsetDb = 32;         // baseline +20 dB above the -12 dB floor
        while (clip < leg.clips.size() && leg.clips[clip].tick < i) {
            ++clip;
        }
        // The tick after an attack is the window in flight when the gain
        // write went out: it clips if the attack's own tick did.
        const bool here = clip < leg.clips.size() && leg.clips[clip].tick == i;
        const bool applies = here
            && (st.offsetDb <= leg.clips[clip].atOffsetDb
                || (clippedLastTick && clip > 0 && leg.clips[clip - 1].tick == i - 1));
        if (applies) {
            o.overloadSamples = leg.clips[clip].overload;
        }
        clippedLastTick = applies;
        while (block + 1 < leg.blocks.size()
               && leg.blocks[block + 1].arrivalMs <= nowMs) {
            ++block;
            blockShiftDb = recordedOffsetAt(leg, leg.blocks[block].arrivalMs)
                         - st.offsetDb;
        }
        const d168::Block& b = leg.blocks[block];
        const int peak = b.peakCode <= 0 ? 0
            : static_cast<int>(std::lround(b.peakCode
                                           * std::pow(10.0, blockShiftDb / 20.0)));
        o.headroom = bandscopeHeadroom(blockStats(peak), nowMs - b.arrivalMs);

        const int radioOffsetDb = recordedOffsetAt(leg, nowMs - d168::kTickMs);
        if (st.offsetDb > 0) run.attenuatedMs += d168::kTickMs;
        if (st.offsetDb > radioOffsetDb) run.heldMoreMs += d168::kTickMs;
        if (st.offsetDb < radioOffsetDb) run.blindMs += d168::kTickMs;
        run.clippedObs += o.overloadSamples;

        const AutoGainAction a = autoGainStep(st, o, cfg);
        if (a.deltaDb != 0) {
            run.trace.push_back({nowMs, a.next.offsetDb});
        }
        if (a.next.dwellRequiredMs > run.maxDwellMs) {
            run.maxDwellMs = a.next.dwellRequiredMs;
        }
        st = a.next;
    }
    run.finalOffsetDb = st.offsetDb;
    return run;
}

// Same changes as the app logged, in order, each within one tick.
bool matchesRecord(const Leg& leg, const Run& run)
{
    if (run.trace.size() != leg.changes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < run.trace.size(); ++i) {
        const int late = run.trace[i].ms - leg.changes[i].ms;
        if (run.trace[i].offsetDb != leg.changes[i].offsetDb
            || late < -d168::kTickMs || late > d168::kTickMs) {
            return false;
        }
    }
    return true;
}

// A closed-loop plant from the 80 m leg, for durations the record does not
// cover. At 0 dB it clips after the seven recorded times-to-clip, cycled, with
// the recorded next-tick remainder; at 6 dB or more it never clips. The
// headroom is the blocks the radio read while holding 6 dB, one per second,
// cycled. Seven intervals from one evening: a model, not a measurement.
struct Plant {
    std::vector<int> timeToClipMs;
    std::vector<int> overload;
    std::vector<int> remainder;
    std::vector<int> peakAt6Db;
};

Plant plantFrom80m()
{
    Plant p;
    int releasedAtMs = 0;
    int offset = 0;
    for (const d168::Change& c : k80m.changes) {
        if (c.offsetDb > offset) {
            p.timeToClipMs.push_back(c.ms - releasedAtMs);
            const int tick = tickOf(c.ms);
            int first = 0;
            int next = 0;
            for (const d168::Clip& k : k80m.clips) {
                if (k.tick == tick) first = k.overload;
                if (k.tick == tick + 1) next = k.overload;
            }
            p.overload.push_back(first);
            p.remainder.push_back(next);
        } else {
            releasedAtMs = c.ms;
        }
        offset = c.offsetDb;
    }
    std::size_t block = 0;
    int last = -1;
    for (int i = 0; i < k80m.ticks; ++i) {
        const int nowMs = (i + 1) * d168::kTickMs;
        while (block + 1 < k80m.blocks.size()
               && k80m.blocks[block + 1].arrivalMs <= nowMs) {
            ++block;
        }
        if (recordedOffsetAt(k80m, nowMs) == 6 && static_cast<int>(block) != last) {
            last = static_cast<int>(block);
            p.peakAt6Db.push_back(k80m.blocks[block].peakCode);
        }
    }
    return p;
}

struct ModelRun {
    int changes = 0;
    int clippedObs = 0;
    std::int64_t attenuatedMs = 0;
    std::int64_t maxDwellMs = 0;
};

ModelRun model(const Plant& p, const AutoGainConfig& cfg, int seconds)
{
    ModelRun out;
    AutoGainState st;
    std::size_t event = 0;
    int atZeroMs = 0;
    int pending = 0;
    const int ticks = seconds * 1000 / d168::kTickMs;
    for (int i = 0; i < ticks; ++i) {
        const int nowMs = (i + 1) * d168::kTickMs;
        AutoGainObservation o;
        o.samples = d168::kSamplesPerTick;
        o.elapsedMs = d168::kTickMs;
        o.availableOffsetDb = 32;
        const std::size_t n = p.timeToClipMs.size();
        if (pending > 0) {
            o.overloadSamples = pending;
            pending = 0;
        } else if (st.offsetDb == 0) {
            atZeroMs += d168::kTickMs;
            if (atZeroMs >= p.timeToClipMs[event % n]) {
                o.overloadSamples = p.overload[event % n];
                pending = p.remainder[event % n];
                ++event;
                atZeroMs = 0;
            }
        }
        out.clippedObs += o.overloadSamples;
        const int peak = p.peakAt6Db[static_cast<std::size_t>(nowMs / 1000)
                                     % p.peakAt6Db.size()];
        o.headroom = bandscopeHeadroom(blockStats(peak), 500);
        const AutoGainAction a = autoGainStep(st, o, cfg);
        if (a.deltaDb != 0) ++out.changes;
        if (a.next.offsetDb > 0) out.attenuatedMs += d168::kTickMs;
        if (a.next.dwellRequiredMs > out.maxDwellMs) {
            out.maxDwellMs = a.next.dwellRequiredMs;
        }
        st = a.next;
    }
    return out;
}

void report(const char* variant, const Leg& leg, const Run& r)
{
    std::printf("       %-26s %s: %2zu changes, %3d clipped obs, attenuated "
                "%5.1f s, holds more than the radio did %5.1f s, blind %5.1f s, "
                "longest dwell %lld s, ends at %d dB\n",
                variant, leg.name, r.trace.size(), r.clippedObs,
                r.attenuatedMs / 1000.0, r.heldMoreMs / 1000.0,
                r.blindMs / 1000.0,
                static_cast<long long>(r.maxDwellMs / 1000), r.finalOffsetDb);
}

void report(const char* variant, int seconds, const ModelRun& m)
{
    std::printf("       %-26s model %4d s: %2d changes, %3d clipped obs, "
                "attenuated %2.0f %%, longest dwell %lld s\n",
                variant, seconds, m.changes, m.clippedObs,
                100.0 * static_cast<double>(m.attenuatedMs) / (seconds * 1000.0),
                static_cast<long long>(m.maxDwellMs / 1000));
}

}  // namespace

int main()
{
    const double bias = gatedPeakBiasDbForPeriod(1000);
    const AutoGainConfig law = bandscopeReleaseConfig(bias);
    const Plant plant = plantFrom80m();

    // ---- 1. THE RECORD, REPRODUCED ----------------------------------------
    {
        const Run r80 = replay(k80m, law);
        const Run r30 = replay(k30m, law);
        report("as shipped", k80m, r80);
        report("as shipped", k30m, r30);
        check(r80.trace.size() == 13 && matchesRecord(k80m, r80),
              "1.1 80 m: the law takes the 13 recorded changes, each within "
              "one tick of the app log");
        check(r30.trace.size() == 10 && matchesRecord(k30m, r30),
              "1.2 30 m: the law takes the 10 recorded changes, each within "
              "one tick of the app log");
        check(r80.blindMs + r80.heldMoreMs <= 13 * d168::kTickMs
              && r30.blindMs + r30.heldMoreMs <= 10 * d168::kTickMs,
              "1.3 the law's offset leaves the radio's for at most one tick "
              "per change, so every recorded input applies as recorded");
        check(r80.maxDwellMs == 60000 && r30.maxDwellMs == 60000,
              "1.4 the probe interval widens once, to 60 s, in each leg");
    }

    // ---- 2. THE SAME BAND FOR HALF AN HOUR --------------------------------
    {
        check(plant.timeToClipMs.size() == 7 && plant.peakAt6Db.size() >= 200,
              "2.0 the plant has the seven recorded times-to-clip and the "
              "blocks read at 6 dB");
        const ModelRun m300 = model(plant, law, 300);
        const ModelRun m1800 = model(plant, law, 1800);
        report("as shipped", 300, m300);
        report("as shipped", 1800, m1800);
        check(m1800.changes > 60,
              "2.1 on the modelled band the law changes gain more than 60 "
              "times in 30 minutes");
        check(m1800.maxDwellMs <= 60000,
              "2.2 and the probe interval never widens past 60 s");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
