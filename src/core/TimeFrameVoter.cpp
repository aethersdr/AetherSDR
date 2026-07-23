#include "TimeFrameVoter.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Cross-frame confidence-weighted voting per the AetherClock reference decoder:
// a bit's vote is the sum of its per-frame matched-filter margins (floored at
// 0.01), with older frames discounted by agingFactor^age. Markers and Unknown
// symbols never vote. The timestamp vote is NORMALIZE-then-COHERENCE-GATED-
// per-bit: every frame is first extrapolated to the newest epoch (removing the
// epoch skew that makes a stale hour dangerous), then each field is composed from
// its bits ONLY when every bit is coherent (its confidence-winner agrees with its
// aging-only count majority); an incoherent field — where sibling bits are carried
// by different frame camps — falls back to the top value some frame actually held,
// so per-bit voting can never assemble a phantom value. Quality is computed in the
// same pass (computeResolution) as the min across fields of per-bit trust
// (margin x participation) or the held-value margin, so value and quality can
// never disagree. Lock gates on consecutive +1 minute increments plus
// self-consistent static fields.

namespace AetherSDR {

namespace {

// Per-frame vote weight for one bit: max(confidence, 0.01) aged by
// agingFactor^age (age 0 = newest frame).
inline float agedWeight(float confidence, float agingFactor, int age) {
    const float base = std::max(confidence, 0.01f);
    return base * std::pow(agingFactor, static_cast<float>(age));
}

// True when the two-digit year names a Gregorian leap year, under the 20xx
// century assumption (full year = 2000 + year2). Within 2000..2099 this reduces
// to year2 % 4 == 0 — 2000 is a 400-divisible leap year and no year in that
// span is century-divisible — but the full rule is written out so the helper
// stays correct if the century assumption is ever revisited.
inline bool isLeapYear2(int year2) {
    const int y = 2000 + year2;
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

// Absolute minute count of a range-valid timestamp since 2000-001T00:00, under
// the same 20xx century assumption. Used only for plausibility DIFFERENCES, so
// the epoch choice is arbitrary as long as both sides share it.
inline long long minutesSince2000(const TimeFields& t) {
    long long days = 0;
    for (int y = 0; y < t.year2; ++y) days += isLeapYear2(y) ? 366 : 365;
    days += t.doy - 1;
    return ((days * 24 + t.hour) * 60) + t.minute;
}

} // namespace

// Forward calendar arithmetic with minute/hour/doy/year carries. `minutes` is
// small in practice (bounded by the voter window), but the loops are written to
// carry across any number of day/year boundaries.
TimeFields advanceMinutes(TimeFields t, int minutes) {
    t.minute += minutes;
    t.hour   += t.minute / 60;
    t.minute %= 60;
    int carryDays = t.hour / 24;
    t.hour %= 24;
    t.doy += carryDays;
    // doy is 1-based; wrap at the current year's length, carrying year2 (mod 100).
    for (int len = isLeapYear2(t.year2) ? 366 : 365; t.doy > len;
         len = isLeapYear2(t.year2) ? 366 : 365) {
        t.doy -= len;
        t.year2 = (t.year2 + 1) % 100;
    }
    return t;
}

TimeFrameVoter::TimeFrameVoter(Config cfg) : m_cfg(std::move(cfg)) {}

void TimeFrameVoter::setPlausibility(std::function<TimeFields()> referenceNow,
                                     int boundMinutes) {
    m_cfg.referenceNow = std::move(referenceNow);
    m_cfg.plausibilityBoundMinutes = boundMinutes;
}

void TimeFrameVoter::addFrame(const std::array<ClockSymbol, 60>& symbols,
                              const std::array<float, 60>& confidence) {
    Frame f;
    f.symbols = symbols;
    f.confidence = confidence;
    m_frames.push_back(f);
    // Drop oldest beyond the sliding window (newest kept at the back).
    while (m_frames.size() > m_cfg.window) {
        m_frames.erase(m_frames.begin());
    }
}

void TimeFrameVoter::reset() {
    m_frames.clear();
}

int TimeFrameVoter::frameCount() const {
    return static_cast<int>(m_frames.size());
}

// Per-frame BCD field decode: sum the field-map weights whose second classified
// as One. Marker/Unknown contribute nothing.
int TimeFrameVoter::decodeField(const Frame& f, FieldIndex field) const {
    int value = 0;
    for (const auto& bw : m_cfg.fields[field]) {
        if (bw.second >= 0 && bw.second < 60 &&
            f.symbols[bw.second] == ClockSymbol::One) {
            value += bw.weight;
        }
    }
    return value;
}

int TimeFrameVoter::decodeMinutes(const Frame& f) const {
    return decodeField(f, FieldMinutes);
}

std::vector<TimeFrameVoter::NormalizedFrame>
TimeFrameVoter::buildNormalizedFrames() const {
    const int n = static_cast<int>(m_frames.size());
    std::vector<NormalizedFrame> out;
    out.reserve(static_cast<std::size_t>(n));

    // Greedy descending-weight BCD encode of `value` over a field map: the maps
    // carry canonical BCD weights, so subtracting weights largest-first
    // reconstructs the exact bit pattern (One/Zero, parallel to the map).
    auto encodeField = [](const ClockFieldMap& map, int value) {
        std::vector<ClockSymbol> bits(map.size(), ClockSymbol::Zero);
        std::vector<std::size_t> order(map.size());
        for (std::size_t k = 0; k < map.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return map[a].weight > map[b].weight;
                  });
        int rem = value;
        for (std::size_t k : order) {
            if (map[k].weight > 0 && map[k].weight <= rem) {
                bits[k] = ClockSymbol::One;
                rem -= map[k].weight;
            }
        }
        return bits;
    };

    // Weakest of the original per-second confidences over one field map, floored
    // at 0.01 (a re-encoded field weights every bit by this). A BCD field VALUE is
    // exactly as reliable as its least-reliable bit — one flipped bit changes the
    // whole value — so a re-encoded value's vote must carry the weakest bit's
    // margin, never the average, which would dilute a single faded bit ~1/N.
    auto fieldMinConf = [&](const Frame& f, FieldIndex fld) -> float {
        float lo = -1.0f;
        for (const auto& bw : m_cfg.fields[fld]) {
            if (bw.second >= 0 && bw.second < 60) {
                lo = (lo < 0.0f) ? f.confidence[bw.second]
                                 : std::min(lo, f.confidence[bw.second]);
            }
        }
        return std::max(lo < 0.0f ? 0.0f : lo, 0.01f);
    };

    for (int i = 0; i < n; ++i) {
        const int age = (n - 1) - i;  // newest -> age 0
        const Frame& f = m_frames[i];

        TimeFields raw;
        raw.minute = decodeField(f, FieldMinutes);
        raw.hour   = decodeField(f, FieldHours);
        raw.doy    = decodeField(f, FieldDoy);
        raw.year2  = decodeField(f, FieldYear);

        // Whole-frame range gate — a frame decoding outside valid broadcast
        // ranges votes nothing and holds nothing (same spirit as excluding
        // Marker/Unknown from a bit vote; also guards the calendar arithmetic
        // below). It is NOT dropped from the window (WS-4.5): it keeps its slot
        // and its real per-second confidences count against participation, so a
        // run of garbage frames drags quality down instead of letting the
        // surviving stale frames dead-reckon at undiminished quality.
        // doy 366 is only ever broadcast in a leap year — accepting it in a
        // non-leap year would let advanceMinutes wrap a corrupt frame into
        // January of the next year, polluting the vote with an off-air date.
        const bool rangeValid =
            raw.minute >= 0 && raw.minute <= 59 &&
            raw.hour   >= 0 && raw.hour   <= 23 &&
            raw.doy    >= 1 && raw.doy    <= 366 &&
            raw.year2  >= 0 && raw.year2  <= 99 &&
            !(raw.doy == 366 && !isLeapYear2(raw.year2));

        if (!rangeValid) {
            NormalizedFrame nf;
            nf.age = age;
            nf.valid = false;
            nf.meanConfidence = 0.0;
            for (std::size_t fld = 0; fld < FieldCount; ++fld) {
                const ClockFieldMap& map = m_cfg.fields[fld];
                std::vector<NormalizedFrame::Bit>& dst = nf.bits[fld];
                dst.resize(map.size());
                for (std::size_t k = 0; k < map.size(); ++k) {
                    const int sec = map[k].second;
                    dst[k].symbol = ClockSymbol::Unknown;   // votes nothing
                    dst[k].confidence =
                        (sec >= 0 && sec < 60) ? f.confidence[sec] : 0.0f;
                }
            }
            out.push_back(std::move(nf));
            continue;
        }

        NormalizedFrame nf;
        nf.age = age;
        nf.ext = advanceMinutes(raw, age);

        double sum = 0.0;
        int count = 0;
        for (const FieldIndex fld : {FieldMinutes, FieldHours, FieldDoy, FieldYear}) {
            for (const auto& bw : m_cfg.fields[fld]) {
                if (bw.second >= 0 && bw.second < 60) {
                    sum += f.confidence[bw.second];
                    ++count;
                }
            }
        }
        nf.meanConfidence = count ? sum / count : 0.0;

        const int rawByField[FieldCount] = {raw.minute, raw.hour, raw.doy, raw.year2};
        const int extByField[FieldCount] = {nf.ext.minute, nf.ext.hour,
                                            nf.ext.doy, nf.ext.year2};

        for (std::size_t fld = 0; fld < FieldCount; ++fld) {
            const ClockFieldMap& map = m_cfg.fields[fld];
            std::vector<NormalizedFrame::Bit>& dst = nf.bits[fld];
            dst.resize(map.size());

            if (extByField[fld] == rawByField[fld]) {
                // Extrapolation left this field unchanged (the common case away
                // from a boundary): vote its ORIGINAL symbols with their ORIGINAL
                // per-second confidences — a faded bit carries a low matched-
                // filter margin and loses the cross-frame vote. This is the
                // reference model that rescues single-bit fades in noisy corpora.
                // Note (WS-4.5): noise-grade margins still VOTE here — silencing
                // them flips coherence verdicts and vote topology on real
                // corpora (measured 2026-07-20 on the live WWV corpus). Their
                // honesty cost is charged in TRUST instead: a bit whose winning
                // side has no confident support scores zero trust
                // (computeResolution), which is what refuses the lock.
                for (std::size_t k = 0; k < map.size(); ++k) {
                    const int sec = map[k].second;
                    const bool inRange = sec >= 0 && sec < 60;
                    dst[k].symbol = inRange ? f.symbols[sec] : ClockSymbol::Unknown;
                    dst[k].confidence = inRange ? f.confidence[sec] : 0.0f;
                }
            } else {
                // Extrapolation moved this field across a carry: re-encode the
                // NORMALIZED value and weight every bit by the field-MIN original
                // confidence, so a value whose weakest bit faded votes at that
                // faded margin. Blending is thus confined to normalized space,
                // where the frames are supposed to agree. The eligibility floor
                // deliberately does NOT apply here: the field-MIN weighting
                // already discounts a faded frame to near-zero, and silencing it
                // outright guts the fade rescue on minutes (every frame takes
                // this path for minutes) — measured on the 2026-07-19 live WWV
                // corpus, where the floor flipped the voted minute to the
                // newest frame's corrupt raw value.
                const std::vector<ClockSymbol> bits = encodeField(map, extByField[fld]);
                const float w = fieldMinConf(f, static_cast<FieldIndex>(fld));
                for (std::size_t k = 0; k < map.size(); ++k) {
                    dst[k].symbol = bits[k];
                    dst[k].confidence = w;
                }
            }
        }
        out.push_back(std::move(nf));
    }
    return out;
}

namespace {

// One field's per-bit tallies across the normalized window: confidence-weighted
// (w) decides the value; aging-only count (c) decides the count majority; the
// two together decide COHERENCE. `part` is participation (participating weight /
// potential weight) — the lone-floor-voter guard.
struct BitStat {
    double w0 = 0.0, w1 = 0.0;   // confidence-weighted (aged x max(conf,0.01))
    double c0 = 0.0, c1 = 0.0;   // aging-only count (aged, no confidence)
    double participation = 0.0;  // Pact / Ppot
};

// This bit is coherent iff its confidence-winner agrees with its count-winner,
// or the count is tied (no majority to contradict). Ties in w resolve to Zero,
// matching the compose rule (value bit set only when w1 > w0).
inline bool bitCoherent(const BitStat& b) {
    const bool wOne = b.w1 > b.w0;
    const bool cOne = b.c1 > b.c0;
    const bool cTied = b.c0 == b.c1;
    return cTied || (wOne == cOne);
}

// Normalized winning margin of a bit, 0 when nothing participated.
inline double bitMargin(const BitStat& b) {
    const double total = b.w0 + b.w1;
    constexpr double kEpsilon = 1e-6;
    return total > 0.0 ? (std::max(b.w0, b.w1) - std::min(b.w0, b.w1)) / (total + kEpsilon)
                       : 0.0;
}

} // namespace

TimeFrameVoter::Resolution TimeFrameVoter::computeResolution() const {
    Resolution r;  // value all -1, quality 0
    const std::vector<NormalizedFrame> frames = buildNormalizedFrames();
    if (frames.empty()) {
        return r;
    }

    int valueByField[FieldCount] = {0, 0, 0, 0};
    double qualityByField[FieldCount] = {1.0, 1.0, 1.0, 1.0};

    for (std::size_t fld = 0; fld < FieldCount; ++fld) {
        const ClockFieldMap& map = m_cfg.fields[fld];

        // Held-value vote: each frame votes its whole ext field VALUE, weighted by
        // aged x the frame's field-min original confidence (consistent with the
        // re-encode weighting) — a value a frame actually carried, never composed.
        std::vector<std::pair<int, double>> held;  // (value, weight)
        auto addHeld = [&](int value, double weight) {
            for (auto& hv : held) {
                if (hv.first == value) { hv.second += weight; return; }
            }
            held.push_back({value, weight});
        };

        bool allCoherent = true;
        double minCoherentTrust = 1.0;
        int composeValue = 0;

        for (const NormalizedFrame& nf : frames) {
            if (!nf.valid) continue;   // holds no value
            float fmin = -1.0f;
            for (const auto& b : nf.bits[fld]) {
                fmin = (fmin < 0.0f) ? b.confidence : std::min(fmin, b.confidence);
            }
            const int extByField[FieldCount] = {nf.ext.minute, nf.ext.hour,
                                                nf.ext.doy, nf.ext.year2};
            addHeld(extByField[fld],
                    agedWeight(fmin < 0.0f ? 0.0f : fmin, m_cfg.agingFactor, nf.age));
        }

        for (std::size_t k = 0; k < map.size(); ++k) {
            BitStat s;
            double pAct = 0.0, pPot = 0.0;
            float bestConf0 = 0.0f, bestConf1 = 0.0f;
            for (const NormalizedFrame& nf : frames) {
                const NormalizedFrame::Bit b = nf.bits[fld][k];
                const double aged =
                    std::pow(static_cast<double>(m_cfg.agingFactor), nf.age);
                const double confW = std::max(b.confidence, 0.01f) * aged;
                pPot += confW;  // potential: every frame's aged floored confidence
                if (b.symbol == ClockSymbol::One || b.symbol == ClockSymbol::Zero) {
                    (b.symbol == ClockSymbol::One ? s.w1 : s.w0) += confW;
                    (b.symbol == ClockSymbol::One ? s.c1 : s.c0) += aged;
                    pAct += confW;
                    float& best = (b.symbol == ClockSymbol::One) ? bestConf1
                                                                 : bestConf0;
                    best = std::max(best, b.confidence);
                }
            }
            s.participation = pPot > 0.0 ? pAct / pPot : 0.0;

            // Trust floor (WS-4.5): a bit whose WINNING side has not one vote
            // at a confident margin is certified by nothing but noise — a deep
            // fade misreads the SAME bits in every frame, and unanimity of
            // noise-grade reads must not score margin-1.0 trust (the
            // 2026-07-20 q100-on-2006-01-01 mechanism). The vote itself is
            // untouched; only its certification collapses.
            const float winnerBest = (s.w1 > s.w0) ? bestConf1 : bestConf0;
            const bool certified = m_cfg.minBitConfidence <= 0.0f ||
                                   winnerBest >= m_cfg.minBitConfidence;
            const double trust =
                certified ? bitMargin(s) * s.participation : 0.0;
            if (bitCoherent(s)) {
                minCoherentTrust = std::min(minCoherentTrust, trust);
            } else {
                allCoherent = false;
            }
            if (s.w1 > s.w0) {
                composeValue += map[k].weight;
            }
        }

        // Top two held values -> held-value margin.
        double topW = 0.0, runnerW = 0.0, totalHeld = 0.0;
        int topValue = 0;
        for (const auto& hv : held) {
            totalHeld += hv.second;
            if (hv.second > topW) { runnerW = topW; topW = hv.second; topValue = hv.first; }
            else if (hv.second > runnerW) { runnerW = hv.second; }
        }
        constexpr double kEpsilon = 1e-6;
        const double valueMargin = (topW - runnerW) / (totalHeld + kEpsilon);

        // Per-bit coherence is necessary but NOT sufficient to rule out a
        // synthesized value: rotating multi-bit misreads at comparable
        // confidence can win every bit from a DIFFERENT frame camp -- each bit
        // coherent, yet the composed value held by no frame (refuter round 5,
        // 2026-07-20: frames holding {9, 10, 3} compose hour 11). The composed
        // value must itself be a held value; otherwise the field demotes to
        // the held-value fallback below.
        bool composeHeld = false;
        for (const auto& hv : held) {
            if (hv.first == composeValue) { composeHeld = true; break; }
        }

        if (allCoherent && composeHeld) {
            // Fade-rescue / clean path: every bit's confidence-winner is also
            // its count majority AND the composed value is one some frame
            // actually carried -- composing cannot synthesize a phantom.
            valueByField[fld] = composeValue;
            qualityByField[fld] = minCoherentTrust;
        } else {
            // A sibling bit was carried by a DIFFERENT frame camp than its count
            // majority — per-bit composition would Frankenstein a value no frame
            // held. Fall back to the top held value; quality is the weaker of the
            // held-value consensus and the coherent bits' trust.
            valueByField[fld] = topValue;
            qualityByField[fld] = std::min(valueMargin, minCoherentTrust);
        }
    }

    TimeFields voted;
    voted.minute = valueByField[FieldMinutes];
    voted.hour   = valueByField[FieldHours];
    voted.doy    = valueByField[FieldDoy];
    voted.year2  = valueByField[FieldYear];

    // Final-range guard: an all-coherent compose can still sum to an out-of-range
    // BCD value. If it did, the window has no usable consensus — fall back to the
    // highest-aged-weight frame's tuple and report quality 0 for the emission.
    const bool inRange =
        voted.minute >= 0 && voted.minute <= 59 &&
        voted.hour   >= 0 && voted.hour   <= 23 &&
        voted.doy    >= 1 && voted.doy    <= 366 &&
        voted.year2  >= 0 && voted.year2  <= 99;
    if (!inRange) {
        const NormalizedFrame* best = nullptr;
        double bestWeight = -1.0;
        for (const NormalizedFrame& nf : frames) {
            if (!nf.valid) continue;   // never fall back onto a garbage tuple
            const double w = agedWeight(static_cast<float>(nf.meanConfidence),
                                        m_cfg.agingFactor, nf.age);
            if (w > bestWeight) { bestWeight = w; best = &nf; }
        }
        r.value = best ? best->ext : TimeFields{};
        r.quality = 0.0;
        return r;
    }

    r.value = voted;
    r.quality = std::min({qualityByField[FieldMinutes], qualityByField[FieldHours],
                          qualityByField[FieldDoy], qualityByField[FieldYear]});
    return r;
}

TimeFields TimeFrameVoter::votedTimestamp() const {
    return computeResolution().value;
}

int TimeFrameVoter::votedField(FieldIndex field) const {
    if (m_frames.empty()) {
        return -1;
    }
    const TimeFields t = votedTimestamp();
    switch (field) {
        case FieldMinutes: return t.minute;
        case FieldHours:   return t.hour;
        case FieldDoy:     return t.doy;
        case FieldYear:    return t.year2;
        default:           return -1;
    }
}

int TimeFrameVoter::lastFrameMinute() const {
    if (m_frames.empty()) {
        return -1;
    }
    return decodeMinutes(m_frames.back());
}

bool TimeFrameVoter::locked() const { return lockVerdict().locked; }

ClockLockRefusal TimeFrameVoter::lockRefusal() const {
    return lockVerdict().reason;
}

// The original locked() body, verbatim, with each refusal tagged (WS-7): same
// gates, same order, same short-circuits — the tag names the branch that was
// already taken, so tagging cannot change lock behavior (corpus-gated A/B).
TimeFrameVoter::LockVerdict TimeFrameVoter::lockVerdict() const {
    const int n = static_cast<int>(m_frames.size());
    if (n < m_cfg.minFramesForLock) {
        return {false, ClockLockRefusal::None};  // still collecting
    }

    // Count adjacent frame pairs whose decoded minutes increment by exactly +1
    // (mod 60; 59 -> 0 valid). Live per-frame decodes are routinely imperfect,
    // so increment support is COUNTED across the window rather than required of
    // every pair (cf. the reference's marker_score + 4 * increment_count) — one
    // corrupted frame must not permanently prevent or drop the lock.
    int increments = 0;
    for (int i = 1; i < n; ++i) {
        const int prev = decodeMinutes(m_frames[i - 1]);
        const int cur = decodeMinutes(m_frames[i]);
        if (((prev + 1) % 60) == cur) {
            ++increments;
        }
    }
    if (increments < m_cfg.minFramesForLock - 1) {
        return {false, ClockLockRefusal::Contested};
    }

    // Static-field self-consistency in the SAME normalized space the vote uses:
    // each static field must carry at least one confident bit vote (a strictly
    // positive winning margin). No qualifying frame (e.g. an all-Unknown window
    // whose doy decodes out of range) yields zero margin everywhere — must not
    // lock.
    const std::vector<NormalizedFrame> frames = buildNormalizedFrames();
    if (frames.empty()) {
        return {false, ClockLockRefusal::Staleness};
    }

    // Staleness bound (WS-4.5): a lock needs a range-VALID frame among the
    // newest maxNewestValidAge slots. Without this the window dead-reckons on
    // aged frames while garbage streams in — the vote keeps extrapolating
    // forward with no live support.
    if (m_cfg.maxNewestValidAge >= 0) {
        bool fresh = false;
        for (const NormalizedFrame& nf : frames) {
            if (nf.valid && nf.age <= m_cfg.maxNewestValidAge) {
                fresh = true;
                break;
            }
        }
        if (!fresh) {
            return {false, ClockLockRefusal::Staleness};
        }
    }
    for (std::size_t fi = FieldHours; fi <= FieldYear; ++fi) {
        const ClockFieldMap& map = m_cfg.fields[fi];
        double margin = 0.0;
        for (std::size_t k = 0; k < map.size(); ++k) {
            float w0 = 0.0f, w1 = 0.0f;
            for (const NormalizedFrame& nf : frames) {
                const NormalizedFrame::Bit b = nf.bits[fi][k];
                if (b.symbol == ClockSymbol::One || b.symbol == ClockSymbol::Zero) {
                    const float w = agedWeight(b.confidence, m_cfg.agingFactor, nf.age);
                    (b.symbol == ClockSymbol::One ? w1 : w0) += w;
                }
            }
            margin += std::max(w0, w1) - std::min(w0, w1);
        }
        if (!(margin > 0.0)) {
            return {false, ClockLockRefusal::Contested};
        }
    }

    // WS-4.5 honesty gates. Both consume the SAME resolution votedField and
    // lockConfidence report, so a refused lock is always explicable from the
    // published value/quality pair.
    if (m_cfg.minLockQuality > 0.0f || (m_cfg.plausibilityBoundMinutes > 0 &&
                                        m_cfg.referenceNow)) {
        const Resolution res = computeResolution();

        // Quality floor: unanimity of ineligible (noise-grade) bits shows up
        // here as collapsed participation -> collapsed trust.
        if (m_cfg.minLockQuality > 0.0f &&
            res.quality < static_cast<double>(m_cfg.minLockQuality)) {
            return {false, ClockLockRefusal::QualityFloor};
        }

        // Absolute plausibility: a self-consistent decode implausibly far from
        // the reference clock is vetoed by the only independent evidence there
        // is. Fields of `res.value` are range-valid by construction (both the
        // compose and its fallback pass the range gates), so the calendar
        // arithmetic is safe.
        if (m_cfg.plausibilityBoundMinutes > 0 && m_cfg.referenceNow) {
            const TimeFields ref = m_cfg.referenceNow();
            const bool refValid =
                ref.minute >= 0 && ref.minute <= 59 &&
                ref.hour   >= 0 && ref.hour   <= 23 &&
                ref.doy    >= 1 && ref.doy    <= 366 &&
                ref.year2  >= 0 && ref.year2  <= 99;
            if (refValid) {
                const long long diff =
                    minutesSince2000(res.value) - minutesSince2000(ref);
                const long long bound = m_cfg.plausibilityBoundMinutes;
                if (diff > bound || diff < -bound) {
                    return {false, ClockLockRefusal::Plausibility};
                }
            }
        }
    }

    return {true, ClockLockRefusal::None};
}

float TimeFrameVoter::lockConfidence() const {
    // Quality is the resolution's own quality (computed WITH the value in
    // computeResolution, so the two can never disagree) x frame-count saturation.
    // That quality is the MIN across fields of each field's contribution: for a
    // coherent field the minimum per-bit TRUST (normalized margin x participation
    // — participation demotes a bit only one frame actually voted); for an
    // incoherent field the held-value margin capped by the coherent bits' trust;
    // and 0 outright when the compose fell out of range. Per-bit statistics alone
    // (mean OR min) cannot certify a multi-bit BCD value — sibling bits can be
    // carried by different frame camps — so quality reflects the weakest CERTIFIED
    // link, not the weakest bit. Clean/clean-rollover windows: every bit coherent
    // at full participation and margin ~1.0 -> quality ~1.0, tracking saturation.
    const int n = static_cast<int>(m_frames.size());
    if (n < m_cfg.minFramesForLock) {
        return 0.0f;
    }
    if (buildNormalizedFrames().empty()) {
        return 0.0f;
    }

    const double saturation =
        std::min(1.0, static_cast<double>(n) /
                          (2.0 * static_cast<double>(m_cfg.minFramesForLock)));
    const double quality = computeResolution().quality * saturation;
    return static_cast<float>(std::clamp(quality, 0.0, 1.0));
}

} // namespace AetherSDR
