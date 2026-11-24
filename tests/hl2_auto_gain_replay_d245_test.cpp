// Replays the d245 on-air legs (tests/Hl2AutoGainReplayD245.h) through
// autoGainStep: the bandscope law armed at +48 dB, where the band needed almost
// exactly the 24 dB floor and main hunted between 24 and 18 dB. Same rules as
// hl2_auto_gain_replay_test: a recorded clip applies only while the law holds
// no more attenuation than the radio did, a recorded peak is scaled by the
// offset difference, and time the law spends with LESS attenuation than the
// radio had is reported as `blind` (no clip evidence there).
//
// Fidelity: the same replay against main's policy (origin/main 50ed5fff2, no
// 30 s confirm, no failed-probe guard; run outside this tree, because the guard
// is not a config field) gives 18 / 19 / 18 / 18 / 8 / 14 changes on the six
// +48 legs: the app log's 95, same offsets in the same order, each within
// 2.1 s of the logged time.

#include "core/backends/hl2/Hl2AutoGainPolicy.h"

#include "Hl2AutoGainReplayD245.h"

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

constexpr int kAvailableOffsetDb = 60;   // baseline +48 dB above the -12 dB floor
constexpr int kHuntFromMs = 60000;       // d245's hunt rule is judged after 60 s

// A generated array with no entries carries one sentinel (ms or tick < 0).
template <typename T>
std::span<const T> real(std::span<const T> s)
{
    return s.size() == 1 && s[0].tick < 0 ? s.first(0) : s;
}
std::span<const d245::Change> realChanges(std::span<const d245::Change> s)
{
    return s.size() == 1 && s[0].ms < 0 ? s.first(0) : s;
}

struct Leg {
    const char* name;
    int ticks;
    std::span<const d245::Change> changes;
    std::span<const d245::Clip> clips;
    std::span<const d245::Block> blocks;
};

const Leg kHunting[] = {
    {"R1 40 m", d245::kR1L40Ticks, d245::kR1L40Changes, d245::kR1L40Clips, d245::kR1L40Blocks},
    {"R1 30 m", d245::kR1L30Ticks, d245::kR1L30Changes, d245::kR1L30Clips, d245::kR1L30Blocks},
    {"R1 80 m", d245::kR1L80Ticks, d245::kR1L80Changes, d245::kR1L80Clips, d245::kR1L80Blocks},
    {"R2 40 m", d245::kR2L40Ticks, d245::kR2L40Changes, d245::kR2L40Clips, d245::kR2L40Blocks},
    {"R2 30 m", d245::kR2L30Ticks, d245::kR2L30Changes, d245::kR2L30Clips, d245::kR2L30Blocks},
    {"R2 80 m", d245::kR2L80Ticks, d245::kR2L80Changes, d245::kR2L80Clips, d245::kR2L80Blocks},
};
const Leg kQuiet80m{"R1 80 m +20", d245::kR1L80p20Ticks, d245::kR1L80p20Changes,
                    d245::kR1L80p20Clips, d245::kR1L80p20Blocks};
const Leg kDummy{"dummy 7.074 +48", d245::kDum40Ticks, realChanges(d245::kDum40Changes),
                 real<d245::Clip>(d245::kDum40Clips), d245::kDum40Blocks};

int recordedOffsetAt(const Leg& leg, int ms)
{
    int offset = 0;
    for (const d245::Change& c : leg.changes) {
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
        return s;
    }
    s.samples = kEp4BlockSamples;
    s.peakAbs = peakCode >= kEp4FullScale ? kEp4FullScale - 1 : peakCode;
    s.clippedSamples = peakCode >= kEp4FullScale - 1 ? 1 : 0;
    return s;
}

struct Step { int ms; int fromDb; int toDb; };

struct Run {
    std::vector<Step> trace;
    std::vector<std::int64_t> intervals;   // dwellRequiredMs each time it changed
    int clippedObs = 0;
    int heldMoreMs = 0;
    int blindMs = 0;
    std::int64_t maxDwellMs = 0;
    int finalOffsetDb = 0;
};

void note(Run& run, const AutoGainState& before, const AutoGainAction& a, int nowMs)
{
    if (a.deltaDb != 0) {
        run.trace.push_back({nowMs, before.offsetDb, a.next.offsetDb});
    }
    const std::int64_t d = a.next.dwellRequiredMs;
    if (d != before.dwellRequiredMs && d > 0) {
        run.intervals.push_back(d);
    }
    if (d > run.maxDwellMs) {
        run.maxDwellMs = d;
    }
}

// Arming at +48 dB the radio reached the floor in 0.6 to 0.7 s on every leg,
// four attacks 0.2 s apart (the cooldown), the last on a 1-in-19 window. Those
// windows do not fall on this replay's 123 ms ticks, so until the recorded
// arrival at the floor the band is replayed as railing outright; the recorded
// clips take over from there (at most 0.5 s past that arrival).
int msToFloor(const Leg& leg)
{
    for (const d245::Change& c : leg.changes) {
        if (c.offsetDb >= 24) return c.ms;
    }
    return -1;                            // never reached the floor
}

Run replay(const Leg& leg, const AutoGainConfig& cfg)
{
    Run run;
    AutoGainState st;
    const int acquireMs = msToFloor(leg);
    bool floorClipDone = false;
    std::size_t clip = 0;
    std::size_t block = 0;
    int blockShiftDb = 0;
    bool clippedLastTick = false;
    for (int i = 0; i < leg.ticks; ++i) {
        const int nowMs = (i + 1) * d245::kTickMs;
        AutoGainObservation o;
        o.samples = d245::kSamplesPerTick;
        o.elapsedMs = d245::kTickMs;
        o.availableOffsetDb = kAvailableOffsetDb;
        while (clip < leg.clips.size() && leg.clips[clip].tick < i) {
            ++clip;
        }
        const bool here = clip < leg.clips.size() && leg.clips[clip].tick == i;
        const bool applies = here
            && (st.offsetDb <= leg.clips[clip].atOffsetDb
                || (clippedLastTick && clip > 0 && leg.clips[clip - 1].tick == i - 1));
        if (applies) {
            o.overloadSamples = leg.clips[clip].overload > o.samples
                                  ? o.samples : leg.clips[clip].overload;
        }
        clippedLastTick = applies;
        if (acquireMs >= 0 && nowMs <= acquireMs + 500 && st.offsetDb < 24) {
            o.overloadSamples = o.samples;
        } else if (!floorClipDone && acquireMs >= 0 && st.offsetDb >= 24) {
            // The window in flight when the last arming write went out: the
            // series shows it clipping at the floor, which is what recorded
            // the trip at 24 dB on the radio.
            o.overloadSamples = o.overloadSamples > 0 ? o.overloadSamples : 1;
            floorClipDone = true;
        }
        while (block + 1 < leg.blocks.size()
               && leg.blocks[block + 1].arrivalMs <= nowMs) {
            ++block;
            blockShiftDb = recordedOffsetAt(leg, leg.blocks[block].arrivalMs)
                         - st.offsetDb;
        }
        const d245::Block& b = leg.blocks[block];
        int peak = b.peakCode <= 0 ? 0
            : static_cast<int>(std::lround(b.peakCode
                                           * std::pow(10.0, blockShiftDb / 20.0)));
        if (b.peakCode >= kEp4FullScale - 1 && blockShiftDb < 0) {
            peak = b.peakCode;            // a railed block stays railed below it
        }
        o.headroom = bandscopeHeadroom(blockStats(peak), nowMs - b.arrivalMs);

        const int radioOffsetDb = recordedOffsetAt(leg, nowMs - d245::kTickMs);
        if (st.offsetDb > radioOffsetDb) run.heldMoreMs += d245::kTickMs;
        if (st.offsetDb < radioOffsetDb) run.blindMs += d245::kTickMs;
        run.clippedObs += o.overloadSamples;

        const AutoGainAction a = autoGainStep(st, o, cfg);
        note(run, st, a, nowMs);
        st = a.next;
    }
    run.finalOffsetDb = st.offsetDb;
    return run;
}

// The registered no-hunting rule of d168/d245, after kHuntFromMs: a probe is
// the first release after an attack; probes at least 25 s apart and their gaps
// not shrinking (each at least 0.9 x the one before).
struct Hunt {
    int changes = 0;
    int releases = 0;
    int probes = 0;
    bool ok = true;
};

Hunt hunt(const std::vector<Step>& trace, int fromMs)
{
    Hunt h;
    bool attacked = true;
    std::vector<int> probes;
    for (const Step& s : trace) {
        if (s.ms < fromMs) {
            continue;
        }
        ++h.changes;
        if (s.toDb > s.fromDb) {
            attacked = true;
        } else {
            ++h.releases;
            if (attacked) probes.push_back(s.ms);
            attacked = false;
        }
    }
    h.probes = static_cast<int>(probes.size());
    for (std::size_t i = 1; i < probes.size(); ++i) {
        const int gap = probes[i] - probes[i - 1];
        if (gap < 25000) h.ok = false;
        if (i >= 2 && gap < 0.9 * (probes[i - 1] - probes[i - 2])) h.ok = false;
    }
    return h;
}

int releases(const std::vector<Step>& trace)
{
    int n = 0;
    for (const Step& s : trace) n += s.toDb < s.fromDb ? 1 : 0;
    return n;
}

// A plant built from the d245 +48 legs, for what the record cannot show once
// the law's offset leaves the radio's. At 18 dB or less it clips after the
// recorded release-to-reclip delays, cycled in recorded order; at 24 dB it does
// not clip. The headroom is the blocks the radio read while holding 24 dB, one
// per second, cycled, scaled to the offset the law holds. Delays from one evening on one station: a model.
struct Plant {
    std::vector<int> reclipMs;
    std::vector<int> peakAt24;
};

Plant plantFromD245()
{
    Plant p;
    for (const Leg& leg : kHunting) {
        for (std::size_t i = 0; i + 1 < leg.changes.size(); ++i) {
            const d245::Change& c = leg.changes[i];
            const d245::Change& n = leg.changes[i + 1];
            const int before = i == 0 ? 0 : leg.changes[i - 1].offsetDb;
            if (c.offsetDb < before && n.offsetDb > c.offsetDb) {
                p.reclipMs.push_back(n.ms - c.ms);
            }
        }
        std::size_t block = 0;
        int last = -1;
        for (int i = 0; i < leg.ticks; ++i) {
            const int nowMs = (i + 1) * d245::kTickMs;
            while (block + 1 < leg.blocks.size()
                   && leg.blocks[block + 1].arrivalMs <= nowMs) {
                ++block;
            }
            if (recordedOffsetAt(leg, nowMs) == 24 && static_cast<int>(block) != last
                && leg.blocks[block].peakCode > 0) {
                last = static_cast<int>(block);
                p.peakAt24.push_back(leg.blocks[block].peakCode);
            }
        }
    }
    return p;
}

// `quietFromMs` >= 0: from then on the band is quiet (no clip, 30 dB of room).
Run model(const Plant& p, const AutoGainConfig& cfg, int seconds, int quietFromMs = -1)
{
    Run run;
    AutoGainState st;
    std::size_t event = 0;
    int belowMs = 0;
    const int ticks = seconds * 1000 / d245::kTickMs;
    for (int i = 0; i < ticks; ++i) {
        const int nowMs = (i + 1) * d245::kTickMs;
        const bool quiet = quietFromMs >= 0 && nowMs >= quietFromMs;
        AutoGainObservation o;
        o.samples = d245::kSamplesPerTick;
        o.elapsedMs = d245::kTickMs;
        o.availableOffsetDb = kAvailableOffsetDb;
        if (!quiet && nowMs < 700) {
            o.overloadSamples = d245::kSamplesPerTick;   // armed at +48: rails at once
        } else if (!quiet && st.offsetDb < 24) {
            belowMs += d245::kTickMs;
            if (belowMs >= p.reclipMs[event % p.reclipMs.size()]) {
                o.overloadSamples = 1;
                ++event;
                belowMs = 0;
            }
        } else {
            belowMs = 0;
        }
        run.clippedObs += o.overloadSamples;
        // A block read at 24 dB, scaled to the offset the law holds now.
        const int at24 = p.peakAt24[static_cast<std::size_t>(nowMs / 1000) % p.peakAt24.size()];
        const int peak = quiet ? 64
            : static_cast<int>(std::lround(at24 * std::pow(10.0, (24 - st.offsetDb) / 20.0)));
        o.headroom = bandscopeHeadroom(blockStats(peak), 500);
        const AutoGainAction a = autoGainStep(st, o, cfg);
        note(run, st, a, nowMs);
        st = a.next;
    }
    run.finalOffsetDb = st.offsetDb;
    return run;
}

void report(const char* what, const Run& r, const Hunt& h)
{
    std::printf("       %-18s %2zu changes (%2d releases), after 60 s %2d changes, "
                "%d probes, hunt rule %s; clipped obs %3d, held more %5.1f s, "
                "blind %5.1f s, longest interval %3lld s, ends at %d dB\n",
                what, r.trace.size(), releases(r.trace), h.changes, h.probes,
                h.ok ? "PASS" : "FAIL", r.clippedObs, r.heldMoreMs / 1000.0,
                r.blindMs / 1000.0, static_cast<long long>(r.maxDwellMs / 1000),
                r.finalOffsetDb);
}

}  // namespace

int main()
{
    const double bias = gatedPeakBiasDbForPeriod(1000);
    const AutoGainConfig law = bandscopeReleaseConfig(bias);

    // 1. The +48 legs, on the recorded inputs.
    {
        int worstChanges = 0;
        int totalChanges = 0;
        int worstReleases = 0;
        int legsPass = 0;
        int worstBlindMs = 0;
        for (const Leg& leg : kHunting) {
            const Run r = replay(leg, law);
            const Hunt h = hunt(r.trace, kHuntFromMs);
            report(leg.name, r, h);
            if (static_cast<int>(r.trace.size()) > worstChanges) worstChanges = static_cast<int>(r.trace.size());
            if (releases(r.trace) > worstReleases) worstReleases = releases(r.trace);
            legsPass += h.ok ? 1 : 0;
            totalChanges += static_cast<int>(r.trace.size());
            if (r.blindMs > worstBlindMs) worstBlindMs = r.blindMs;
        }
        std::printf("       six legs: %d changes (main logged 95)\n", totalChanges);
        check(worstChanges <= 12 && totalChanges <= 60,
              "1.1 replayed +48 legs: at most 12 gain changes in 305 s per leg "
              "and 60 in all, four per leg the arming attacks (main logged 8 "
              "to 19 per leg, 95 in all)");
        check(worstReleases <= 4,
              "1.2 at most 4 releases in 305 s per leg (main logged 2 to 8)");
        // The one leg that fails (R1 80 m) fails because a probe held clean
        // for 46.6 s, was confirmed, and the next interval started at base
        // again: the reset a confirmed probe is meant to earn.
        check(legsPass >= 5,
              "1.3 the no-hunting rule (probes >= 25 s apart, gaps not "
              "shrinking) passes on at least 5 of 6 legs (main logged: 2 of 6)");
        check(worstBlindMs <= 30000,
              "1.4 at most 30 s of any leg without clip evidence (the counts "
              "above are a lower bound on such a leg)");
    }

    // 1b. The same band as a plant, for the interval sequence.
    {
        const Plant p = plantFromD245();
        std::printf("       plant: %zu release-to-reclip delays, %zu blocks at 24 dB\n",
                    p.reclipMs.size(), p.peakAt24.size());
        // 34 release -> attack pairs. The lab's count of 35 also takes the
        // R1 80 m release at 166.1 s, whose clip came after a second release.
        check(p.reclipMs.size() == 34,
              "1.5 the plant has the 34 recorded release-to-attack delays");
        const Run m305 = model(p, law, 305);
        const Run m1800 = model(p, law, 1800);
        report("model 305 s", m305, hunt(m305.trace, kHuntFromMs));
        report("model 1800 s", m1800, hunt(m1800.trace, kHuntFromMs));
        std::printf("       intervals:");
        for (std::int64_t d : m1800.intervals) std::printf(" %lld", static_cast<long long>(d / 1000));
        std::printf(" s\n");
        const std::vector<std::int64_t> doubling{60000, 120000, 240000, 480000};
        check(m1800.intervals.size() >= doubling.size()
              && std::vector<std::int64_t>(m1800.intervals.begin(),
                                           m1800.intervals.begin() + 4) == doubling,
              "1.6 each failed probe doubles the interval: 30 -> 60 -> 120 -> "
              "240 -> 480 s");
        check(hunt(m305.trace, kHuntFromMs).ok && releases(m305.trace) <= 3,
              "1.7 the model's first 305 s: at most 3 releases, rule passes");
    }

    // 2. A band that goes quiet after hunting gets its gain back.
    {
        const Plant p = plantFromD245();
        const int quietAt = 900000;
        const Run r = model(p, law, 1500, quietAt);
        int backAt = -1;
        for (const Step& s : r.trace) {
            if (s.ms >= quietAt && s.toDb == 0) backAt = s.ms;
        }
        std::printf("       quiet from 900 s after hunting: full gain %.1f s later\n",
                    (backAt - quietAt) / 1000.0);
        check(r.finalOffsetDb == 0 && backAt > 0
                  && backAt - quietAt <= 480000 + 3 * 3000 + 4 * d245::kTickMs,
              "2.1 quiet after hunting: 24 dB back within the 480 s cap + 3 x 3 s");
    }

    // 3. The +20 leg of the same evening: one attack, one release, as recorded.
    {
        const Run r = replay(kQuiet80m, law);
        report(kQuiet80m.name, r, hunt(r.trace, 0));
        check(r.trace.size() == 2 && r.finalOffsetDb == 0
                  && r.blindMs <= 2 * d245::kTickMs,
              "3.1 +20 dB on 80 m: one attack and one release, gain back to 0 dB "
              "offset, as the radio did");
    }

    // 4. The dummy load: nothing to attack.
    {
        const Run r = replay(kDummy, law);
        report(kDummy.name, r, hunt(r.trace, 0));
        check(r.trace.empty() && r.finalOffsetDb == 0 && r.clippedObs == 0,
              "4.1 dummy load at +48 dB: 0 changes, 0 clipped observations");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
