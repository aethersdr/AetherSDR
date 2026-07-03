#include "AdaptiveFilterEngine.h"
#include "models/SliceModel.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace AetherSDR {

namespace {
    // The occupied-bandwidth measurement (measureOccupiedRegion + its constants)
    // lives in OccupiedRegion.{h,cpp}, a SliceModel-free TU so it can be unit
    // tested against Qt6::Core alone. The constants below belong to the temporal
    // pipeline that turns a per-frame measurement into a stable, smooth passband.

    // Fixed guardrails for a valid voice passband (applied within operator
    // bounds): low-cut never above kMaxLowCutHz (keep warmth), high-cut never
    // below kMinHighCutHz (keep intelligibility).
    constexpr int    kMaxLowCutHz   = 400;
    constexpr int    kMinHighCutHz  = 1800;

    // ── Frame pacing (wall-clock) ────────────────────────────────────────────
    // Every constant below is counted in FRAMES and was calibrated at the
    // panadapter's historical ~25-30 fps. The pan fps ceiling is now 60
    // (perf(gui) #3958), which would halve every dwell/hold/confidence window
    // in wall-clock time AND push the frame-counted send throttle to ~15
    // filt/s — violating RFC #3878 cond. 2 (<= ~8/s). Rather than re-express
    // the whole pipeline in seconds (a redesign), frames arriving faster than
    // kMinFrameSpacingMs are simply not accepted: a native ~30 fps stream
    // (33 ms) passes untouched with jitter headroom, a 60 fps stream is paced
    // down to ~30 — the calibration rate — and the measurement cost halves as
    // a bonus. kMinSendSpacingMs additionally enforces the filt-rate condition
    // directly in wall-clock, independent of any fps assumption.
    constexpr qint64 kMinFrameSpacingNs = 25LL * 1000000LL;   // <= 40 fps accepted
    constexpr qint64 kMinSendSpacingNs  = 125LL * 1000000LL;  // <= 8 filt/s

    // ── Pipeline constants (in frames; paced to ~30 fps above) ──────────────
    // Biased toward LOCKING: once fitted, the passband should sit still and move
    // only on a real width change, not chase speech dynamics. A wider deadband
    // and longer dwell are the levers that stop the float.
    constexpr int    kMedianFrames  = 15;     // ~0.5 s single-frame outlier reject
    constexpr int    kHoldFrames    = 75;     // ~2.5 s edge peak-hold (rides QSB;
                                              // bounded, so no long-term drift)
    // Asymmetric attack/release, OPERATOR-TUNABLE via the Response-speed setting.
    // A signal APPEARING or GROWING should be fitted promptly, while a signal
    // SHRINKING/FADING is confirmed slowly so the filter rides word gaps and QSB
    // instead of pinching. So in every preset widening is faster than narrowing,
    // and the very FIRST fit out of idle (passband still at the operator's
    // baseline — someone just started transmitting) commits on a much shorter
    // dwell so there's no lag before the filter starts moving. Faster presets
    // shorten every confirm/settle window; slower presets lengthen them.
    struct ResponseTuning {
        int engage;      // dwell for the first fit from baseline (frames)
        int widen;       // dwell before WIDENING
        int narrow;      // dwell before NARROWING (always the slowest)
        int refractory;  // settle after a committed change
    };
    ResponseTuning responseTuning(int level) {
        switch (level) {
            case 0:  return {  5,  8, 24, 16 };   // Fast
            case 2:  return { 16, 24, 54, 36 };   // Slow
            default: return {  8, 14, 36, 24 };   // Normal
        }
    }

    // Minimum-SNR + Splatter-rejection settings -> measurement knobs.
    OccupiedRegionParams measureParams(int minSnrLevel, int splatterLevel) {
        OccupiedRegionParams p;
        switch (minSnrLevel) {                    // presence gate (peak over floor)
            case 0:  p.minPeakDb =  6.0f; break;  // Sensitive
            case 2:  p.minPeakDb = 14.0f; break;  // Strong only
            default: p.minPeakDb =  9.0f; break;  // Normal
        }
        switch (splatterLevel) {                  // outer-edge splatter handling
            case 0:  p.splatterDownDb = 18.0f; p.splatterGuardHz = 2700.0; break;  // Tight
            case 2:  p.splatterDownDb = 35.0f; p.splatterGuardHz = 4200.0; break;  // Wide
            default: p.splatterDownDb = 25.0f; p.splatterGuardHz = 3200.0; break;  // Normal
        }
        return p;
    }
    // After a tune, ignore filter changes for this long (band-stack restore can
    // land a frame or two after the retune) so they don't read as a manual edit.
    constexpr int    kPostTuneSettleFrames = 15;  // ~0.5 s
    // AUTO/active confidence integrator (Schmitt trigger). It both debounces the
    // AUTO badge AND decides when to hold the fit vs fall back to the default.
    // The decay is deliberately SLOW (and the ceiling high) so it rides natural
    // speech pauses — a between-words gap briefly drops envPeak below the gate
    // (reg invalid), and we must NOT fall back to the default for that. From a
    // full ceiling it now takes ~1.7 s of *continuous* invalid to release, so a
    // genuine signal loss still falls back, but ordinary pauses don't. (This is
    // the bounded replacement for the pause-riding the old spectral memory did.)
    constexpr int    kConfMax       = 60;
    constexpr int    kConfUp        = 4;
    constexpr int    kConfDown      = 1;
    constexpr int    kConfHigh      = 28;     // rise above -> AUTO latches ON (~0.25 s)
    constexpr int    kConfLow       = 10;     // fall below -> AUTO releases OFF (~1.7 s)
    constexpr int    kDeadbandHz    = 220;    // ignore sub-deadband wiggle (locks
                                              // the edge; only a real width change
                                              // > this commits a move)
    constexpr int    kSnapHz        = 50;     // 50 Hz grid
    constexpr int    kMinBwHz       = 50;     // never narrower than this
    // Glide + send throttle (RFC #3878 cond. 2 — no filt command storm). Emit
    // at most one filt per kSendIntervalFrames (~130 ms => <= ~8/s), each move
    // a proportion of the remaining distance so the passband converges smoothly
    // in a few sends rather than one write per frame.
    constexpr int    kSendIntervalFrames = 4;
    constexpr int    kGlideFracPct       = 50;  // % of remaining distance per send
    constexpr int    kGlideMinStepHz     = 60;  // ...but at least this much

    // Peak-hold percentile: hold the *sustained* wide extent without letting a
    // single brief over-wide reading dominate. 80 -> a value exceeded only ~20%
    // of the hold window, so a transient that occupies < ~20% of the window is
    // ignored (fixes the "high-cut ~200 Hz too wide for 1-3 s then snaps back"
    // — a sibilant/edge overshoot getting held by a plain max).
    constexpr int    kHoldPctHigh   = 80;     // for the high-cut (wider = bigger)
    constexpr int    kHoldPctLow    = 20;     // for the low-cut  (wider = smaller)

    int snap50(int hz) { return ((hz + kSnapHz / 2) / kSnapHz) * kSnapHz; }

    int medianOf(QVector<int> v)
    {
        if (v.isEmpty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    // Value at the given percentile (0-100) of v.
    int percentileOf(QVector<int> v, int pct)
    {
        if (v.isEmpty()) return 0;
        std::sort(v.begin(), v.end());
        const int idx = std::clamp(pct * (static_cast<int>(v.size()) - 1) / 100,
                                   0, static_cast<int>(v.size()) - 1);
        return v[idx];
    }
}

AdaptiveFilterEngine::AdaptiveFilterEngine(QObject* parent) : QObject(parent) {}

void AdaptiveFilterEngine::resetSlice(int sliceId) { m_state.remove(sliceId); }

void AdaptiveFilterEngine::processFrame(SliceModel* slice, double centerMhz,
                                        double bandwidthMhz,
                                        const QVector<float>& binsDbm,
                                        float noiseFloorDbm,
                                        qint64 emittedNs)
{
    if (!slice) return;
    const QString mode = slice->mode();
    const bool isUsb = (mode == QStringLiteral("USB"));
    const bool ssb   = isUsb || (mode == QStringLiteral("LSB"));
    if (!ssb || !slice->adaptiveFilterEnabled()) {
        if (m_state.contains(slice->sliceId())) resetSlice(slice->sliceId());
        return;
    }

    SliceState& st = m_state[slice->sliceId()];

    // ── Frame pacing: keep the pipeline at its ~30 fps calibration ──────────
    // Placed before any state update so a rejected frame is invisible — all
    // frame counters keep their calibrated meaning. Frames without a timestamp
    // (emittedNs <= 0) are accepted unpaced (no basis to space them).
    if (emittedNs > 0) {
        if (st.lastFrameNs != 0 && emittedNs - st.lastFrameNs < kMinFrameSpacingNs)
            return;
        st.lastFrameNs = emittedNs;
    }

    // Clear the per-fit smoothing/measurement state for a fresh fit, WITHOUT
    // touching the baseline (the operator's manual filter). Shared by the tune
    // and mode-change resets so both always clear the same fields.
    const auto clearFit = [](SliceState& s) {
        s.rawLow.clear(); s.rawHigh.clear();
        s.medLow.clear(); s.medHigh.clear();
        s.candLow = 0; s.candHigh = 0;
        s.dwell = 0; s.refractory = 0; s.sinceWrite = 0;
        s.confScore = 0; s.active = false;
        s.avgEnv.clear();
    };

    // ── Tune detection: re-fit fresh on a frequency jump ────────────────────
    // Rotating-QSO case — when the operator tunes to a different station, drop
    // the previous station's smoothing so the filter re-fits promptly instead of
    // dragging the old width. NOTE: the baseline is deliberately KEPT — it is the
    // operator's manually-selected filter (the weak-signal fallback) and must
    // persist across tunes (re-capturing it from the current adaptive-driven
    // filter polluted the fallback after a frequency change).
    const double freqMhz = slice->frequency();
    if (st.lastFreqMhz != 0.0 && std::abs(freqMhz - st.lastFreqMhz) > 0.0003) {
        clearFit(st);
        st.framesSinceTune = 0;                 // open the post-tune settle window
    }
    st.lastFreqMhz = freqMhz;
    if (st.framesSinceTune < 1000000) ++st.framesSinceTune;

    // ── Mode change (USB <-> LSB): full re-baseline ─────────────────────────
    // The signed-offset convention flips between sidebands (USB filterLow =
    // +low-cut; LSB filterLow = -high-cut), and the radio applies the new mode's
    // default filter. Without this, the cached signed cur/tgt/baseline keep the
    // previous sideband's wrong-signed values, so the filter stays on the old
    // side and only crawls across as it glides. Re-capture the baseline from the
    // new mode's filter (haveBaseline=false below) and drop the smoothing.
    if (!st.lastMode.isEmpty() && st.lastMode != mode) {
        clearFit(st);
        st.haveBaseline = false;     // re-capture baseline from the new mode
        st.framesSinceTune = 0;
    }
    st.lastMode = mode;

    // ── Baseline tracking (the operator's selected filter) ──────────────────
    // The baseline is what we fall back to when there's no signal to fit. It must
    // track genuine operator edits but never be corrupted by our own writes or
    // the radio's async echoes of them. We detect a real operator edit via
    // SliceModel's userFilterEpoch(), which is bumped ONLY by setFilterWidth()
    // (preset click / passband drag) — never by applyAdaptiveFilter() or status
    // echoes. So a manual change is unambiguous regardless of engine state: we
    // re-baseline, re-sync cur/tgt to it, and re-fit from there (fixes "manual
    // filter lost" and "auto-adjust stops after a manual change").
    const int fLow = slice->filterLow();
    const int fHigh = slice->filterHigh();
    const quint64 userEpoch = slice->userFilterEpoch();
    const bool epochChanged = st.haveBaseline && userEpoch != st.lastUserEpoch;

    if (epochChanged && st.framesSinceTune >= kPostTuneSettleFrames) {
        // A genuine manual filter edit on a stable station — user intent wins
        // (RFC #3878 cond. 3). Turn the feature OFF; the operator re-enables it
        // explicitly. setAdaptiveFilterEnabled(false) clears AUTO + markers via
        // the model signals; resetSlice() drops our per-slice state.
        slice->setAdaptiveFilterEnabled(false);
        resetSlice(slice->sliceId());
        return;
    }

    if (!st.haveBaseline || epochChanged) {
        // First frame, post-tune re-capture, or a filter change within the
        // post-tune settle window (band-stack restore) — adopt as baseline and
        // re-fit cleanly; do NOT disable.
        st.baseLow = fLow; st.baseHigh = fHigh;
        st.curLow = fLow;  st.curHigh = fHigh;
        st.tgtLow = fLow;  st.tgtHigh = fHigh;
        st.haveBaseline = true;
        if (epochChanged) clearFit(st);   // operator/band-stack filter change -> re-fit cleanly
    }
    st.lastUserEpoch = userEpoch;

    const int minLow  = slice->adaptiveMinLowCut();
    const int maxHigh = slice->adaptiveMaxHighCut();

    // Operator-tunable presets: Minimum SNR + Splatter rejection -> measurement
    // knobs; Response speed -> dwell/settle timing (below).
    const OccupiedRegionParams params =
        measureParams(slice->adaptiveMinSnr(), slice->adaptiveSplatter());
    const ResponseTuning resp = responseTuning(slice->adaptiveResponse());

    // While idle (no confident fit — reverted to the operator's baseline), keep
    // the temporal average fresh so a signal returning after a dropout is captured
    // at full width on its FIRST frame, like right after a tune. Otherwise the EMA
    // (persisted from the dropout at the noise floor) has to climb back up over
    // ~1 s, so the fit re-engages slowly and narrow. active stays true through
    // brief speech pauses (HOLD), so this does not disturb pause-riding.
    if (!st.active) st.avgEnv.clear();

    // ── Measure (single-signal edge-finder; see OccupiedRegion.cpp) ──────────
    const OccupiedRegion reg = measureOccupiedRegion(
        binsDbm, centerMhz, bandwidthMhz, slice->frequency(), mode, noiseFloorDbm,
        st.avgEnv, params);

    // Debounce the AUTO state with a Schmitt-trigger confidence integrator so a
    // weak/marginal signal cannot flicker the badge between AUTO and the value.
    st.confScore = reg.valid ? std::min(st.confScore + kConfUp, kConfMax)
                             : std::max(st.confScore - kConfDown, 0);
    if (!st.active && st.confScore >= kConfHigh)      st.active = true;
    else if (st.active && st.confScore <= kConfLow)   st.active = false;

    if (!reg.valid) {
        // A momentary measurement gap (e.g. a speech pause). If we are
        // confidently active, HOLD the current fit — do NOT lurch back to
        // baseline or wipe the smoothing, or the filter would oscillate on
        // every syllable. Only once confidence has truly decayed (active
        // false) do we fall back to the operator's selected filter.
        if (!st.active) {
            st.tgtLow = st.baseLow; st.tgtHigh = st.baseHigh;
            st.rawLow.clear(); st.rawHigh.clear();
            st.medLow.clear(); st.medHigh.clear();
            st.dwell = 0;
        }
        glideToward(slice, st, st.active, emittedNs);
        return;
    }

    // Clamp to operator bounds AND fixed SSB-voice guardrails, enforce MIN_BW,
    // snap to 50 Hz. low-cut in [minLow, 400] (keep warmth); high-cut in
    // [1800, maxHigh] (keep intelligibility). The combo ranges guarantee
    // minLow <= 400 < 1800 <= maxHigh, so the clamps never invert.
    int audioLow  = std::clamp(reg.lowHz,  minLow, kMaxLowCutHz);
    int audioHigh = std::clamp(reg.highHz, kMinHighCutHz, maxHigh);
    if (audioHigh - audioLow < kMinBwHz) audioHigh = audioLow + kMinBwHz;
    audioLow  = snap50(audioLow);
    audioHigh = snap50(audioHigh);

    // ── Smooth: median (outlier reject) -> peak-hold (expand fast/contract slow)
    st.rawLow.append(audioLow);   st.rawHigh.append(audioHigh);
    if (st.rawLow.size()  > kMedianFrames) st.rawLow.removeFirst();
    if (st.rawHigh.size() > kMedianFrames) st.rawHigh.removeFirst();
    st.medLow.append(medianOf(st.rawLow));
    st.medHigh.append(medianOf(st.rawHigh));
    if (st.medLow.size()  > kHoldFrames) st.medLow.removeFirst();
    if (st.medHigh.size() > kHoldFrames) st.medHigh.removeFirst();
    // Widest over the hold window: low-cut min (lower = wider), high-cut max.
    // Percentile (not min/max) so a single brief over-wide reading can't be
    // held for the whole window: low-cut takes the 20th pct (favours wider =
    // lower), high-cut the 80th pct (favours wider = higher), each ignoring the
    // outer ~20% of transient excursions.
    int holdLow  = percentileOf(st.medLow,  kHoldPctLow);
    int holdHigh = percentileOf(st.medHigh, kHoldPctHigh);
    holdLow  = std::max(holdLow,  minLow);
    holdHigh = std::min(holdHigh, maxHigh);

    // ── Inertia: candidate must hold (deadband + dwell) before committing ────
    const bool sameCandidate = std::abs(holdLow - st.candLow) <= kDeadbandHz &&
                               std::abs(holdHigh - st.candHigh) <= kDeadbandHz;
    if (sameCandidate) {
        ++st.dwell;
    } else {
        st.candLow = holdLow; st.candHigh = holdHigh; st.dwell = 0;
    }

    // Audio magnitudes -> signed filter offsets for this mode.
    int wantLo, wantHi;
    if (isUsb) { wantLo = holdLow;   wantHi = holdHigh; }
    else       { wantLo = -holdHigh; wantHi = -holdLow; }

    const bool differs = std::abs(wantLo - st.tgtLow) > kDeadbandHz ||
                         std::abs(wantHi - st.tgtHigh) > kDeadbandHz;
    // Narrowing cuts into the signal — the dangerous direction. Demand a much
    // longer confirmation for it than for widening, so a transient dip that
    // slipped past the gap-bridge + peak-hold still cannot pinch the passband.
    const int wantWidth = wantHi - wantLo;
    const int curWidth  = (st.tgtHigh == INT_MIN) ? 0 : st.tgtHigh - st.tgtLow;
    const bool narrowing = wantWidth < curWidth - kDeadbandHz;
    // First fit out of idle: the passband is still parked at the operator's
    // baseline (nobody fitted yet — a transmission just started). Commit on the
    // short engage dwell so the filter starts adapting promptly; later moves use
    // the normal widen/narrow dwell so they stay calm.
    const bool atBaseline = (st.tgtLow == st.baseLow && st.tgtHigh == st.baseHigh);
    const int needDwell = atBaseline    ? resp.engage
                        : narrowing     ? resp.narrow
                                        : resp.widen;
    // Settle after each change before allowing the next, so the filter feels
    // calm rather than continuously nudging.
    if (st.refractory > 0) --st.refractory;
    if (differs && st.dwell >= needDwell && st.refractory == 0) {
        st.tgtLow = wantLo; st.tgtHigh = wantHi;
        st.dwell = 0;
        st.refractory = resp.refractory;
    }

    glideToward(slice, st, st.active, emittedNs);
}

void AdaptiveFilterEngine::glideToward(SliceModel* slice, SliceState& st, bool active,
                                       qint64 emittedNs)
{
    // AUTO-badge state is cheap and not a radio command — update every frame.
    slice->setAdaptiveActive(active);

    // Already at target: nothing to send (steady state stays silent).
    if (st.curLow == st.tgtLow && st.curHigh == st.tgtHigh) { st.sinceWrite = 0; return; }

    // Throttle: emit at most one filt per kSendIntervalFrames (anti-storm),
    // AND at most one per kMinSendSpacingNs wall-clock — the direct RFC #3878
    // cond. 2 guarantee (<= ~8/s) whatever the pan frame rate does.
    if (++st.sinceWrite < kSendIntervalFrames) return;
    if (emittedNs > 0 && st.lastSendNs != 0 &&
        emittedNs - st.lastSendNs < kMinSendSpacingNs) return;
    st.sinceWrite = 0;

    // Proportional step toward the target — smooth, converges in a few sends.
    const auto step = [](int cur, int tgt) {
        if (cur == INT_MIN) return tgt;
        const int d = tgt - cur;
        if (std::abs(d) <= kGlideMinStepHz) return tgt;
        const int s = std::max(kGlideMinStepHz, std::abs(d) * kGlideFracPct / 100);
        return cur + (d > 0 ? s : -s);
    };
    const int nextLow  = step(st.curLow,  st.tgtLow);
    const int nextHigh = step(st.curHigh, st.tgtHigh);

    if (nextLow != st.curLow || nextHigh != st.curHigh) {
        st.curLow = nextLow; st.curHigh = nextHigh;
        st.lastSendNs = emittedNs;
        slice->applyAdaptiveFilter(nextLow, nextHigh);
    }
}

} // namespace AetherSDR
