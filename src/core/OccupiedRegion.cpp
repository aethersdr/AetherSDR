#include "OccupiedRegion.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {
    // ── Measurement constants (RFC #3878) ───────────────────────────────────
    // Starting values, biased toward stability. Expect on-air tuning before
    // release (the maintainer signs off on the DSP feel per the RFC).
    constexpr double kScanHz        = 6500.0; // scan this far from the carrier
    // Measurement resolution cap (zoom-invariant cost). Every loop below is
    // O(kScanHz / hzPerBin), and the pan's hzPerBin shrinks without bound as
    // the operator zooms in (xpixels is tied to widget width, not span) — the
    // measurement got quadratically slower with zoom while producing edges no
    // better: everything downstream is far coarser (50 Hz snap grid, 150 Hz
    // margin, 220 Hz engine deadband). Below this resolution the input is
    // decimated (mean in dB of D consecutive bins) so the effective bin stays
    // in [kMinMeasureHzPerBin, 2*kMinMeasureHzPerBin) and the whole
    // measurement is O(1) in zoom. Mean — not max (extreme-value bias inflates
    // averaged noise ~5 dB and risks false-engage at the Sensitive preset) and
    // not stride (a narrow het hopping in/out of the sample comb jitters the
    // edges); the dB mean is literally the first stage of the kEnvHz box
    // average, so envelope semantics are unchanged. Coarser pans (hzPerBin
    // already >= the cap) take the D=1 path, bit-identical to before.
    constexpr double kMinMeasureHzPerBin = 25.0;
    // Spectral-envelope smoothing: a moving average over kEnvHz suppresses
    // narrow spikes (hets/carriers) and speech fine structure, leaving the
    // voice "hump". Edge finding runs on the envelope, NOT the raw bins, so a
    // sharp spike on top of a broad ESSB signal can't fool the threshold.
    constexpr double kEnvHz         = 300.0;
    constexpr float  kEnvGateDb     = 5.0f;   // occupied edge: env >= floor + this
    constexpr float  kSignalGateDb  = 3.0f;   // env below floor+this = no signal
    constexpr double kSilenceHz     = 600.0;  // ...for this span => a true band
                                              // edge, once the run is CONFIDENT
                                              // (wide enough to bridge deep
                                              // internal QSB notches; a separate
                                              // STRONGER lobe is still cut by the
                                              // pre-gap-level rebound test; an
                                              // unconfident run scans on — see
                                              // the extent pass)
    // Presence margin (env peak over floor) is operator-tunable via the Minimum-SNR
    // setting — see OccupiedRegionParams::minPeakDb. The time-averaged envelope keeps
    // noise excursions to ~1-3 dB, so even the most sensitive preset does not
    // false-engage on pure noise.
    // SSB-voice shape gate: voice energy starts near the carrier. If the
    // occupied band starts above this (e.g. a 1600-4000 data/het signal), it
    // isn't the SSB voice we're tuned to -> reject -> caller keeps the manual
    // filter. Generous (well above any real voice low-cut, well below 1600).
    constexpr int    kMaxVoiceLowCutHz = 600;
    // Extent + separate-station rejection (RFC follow-up). The wanted signal is
    // the contiguous energy from the carrier that never returns to the noise
    // floor — ONE signal whatever its spectral tilt. A floor-reaching gap of
    // >= kFloorDiscHz ARMS a disconnection (measured on the RAW work-grid bins,
    // not the envelope: the ~300 Hz envelope smear erodes ~150 Hz off each side
    // of a real valley, so an envelope-measured width would under-read a
    // genuine floor gap — the envelope measures LEVEL, the raw bins measure
    // CONNECTIVITY); a shallower or merely relative dip is bridged. When energy
    // resumes across an armed disconnection, a plateau more than kReboundDb
    // above BOTH the pre-gap level and the reference-so-far is a DISTINCT lobe:
    //   * if the run so far was CONFIDENT (cleared the presence preset), the
    //     lobe is a separate/splatter station -> cut at the valley;
    //   * if the run so far was NOT confident (a weak bass lobe that never
    //     itself cleared the gate), the lobe IS the tuned signal's dominant
    //     hump (smiley-EQ / presence-boosted ESSB whose mid scoop fades to the
    //     floor) -> RE-ANCHOR: keep the inner edge, continue the extent into
    //     the lobe — provided it starts within kReanchorMaxStartHz (energy
    //     first appearing beyond that above a silent low band is an adjacent
    //     station, not a voice hump).
    // Peak/reference/presence are then computed over the FULL kept extent
    // (anchor pass below) so a treble-dominant hump can never be judged, gated
    // or splatter-capped against a bass-only reference.
    constexpr float  kReboundDb    = 8.0f;
    constexpr double kFloorDiscHz  = 250.0;  // raw floor gap this wide arms a
                                             // real disconnection (in Hz — no
                                             // bin truncation)
    constexpr double kReanchorMaxStartHz = 2000.0;  // re-anchor only into lobes
                                             // starting by here (voice humps
                                             // rise by ~1.2-2 kHz)
    constexpr int    kReferencePct = 75;     // in-band reference = this percentile
                                             // of the kept extent (tracks a
                                             // treble hump; robust to a transient)
    constexpr int    kMarginHz      = 150;    // intelligibility margin
    constexpr int    kMinBwHz       = 50;     // never narrower than this
    // Temporal envelope (video peak-hold with leak): per-offset EMA that rises
    // FAST when energy appears and decays SLOWLY when it goes — a bounded
    // maximum-hold, not a symmetric average. Rationale: SSB voice only fills its
    // upper 1.5-3 kHz INTERMITTENTLY (sibilants, formant peaks), so a symmetric
    // slow average let the upper-band envelope sag below the occupied gate in
    // every word gap — the measured high edge then collapsed to the low formants
    // and snapped back out when speech resumed, jittering between "narrow voice"
    // and "full width" frame to frame. A fast attack captures the signal's reach
    // the instant it appears; a slow, BOUNDED (~1 s) release holds that reach
    // across gaps, so the high edge reflects the SUSTAINED occupied width. The
    // release is short enough that a genuine narrowing still resolves in ~1-2 s,
    // and noise is still averaged down (a near-floor spike, held briefly, stays
    // near the floor — it cannot walk the edge far past the signal).
    constexpr float  kEnvAttackAlpha  = 0.30f;  // fast rise  (~0.1 s time constant)
    constexpr float  kEnvReleaseAlpha = 0.03f;  // slow fall  (~1.1 s time constant)

    // ── Per-frequency noise floor (spec Stage B) ────────────────────────────
    // A single scalar floor mis-thresholds a TILTED floor: with a global 10th-pct
    // scalar, the busy/high side reads "occupied" far past the signal and the
    // high-cut runs out into noise. Track a floor CURVE instead: a sliding LOW
    // PERCENTILE of the raw bins across frequency. A low percentile over a wide
    // window returns the surrounding noise even with signal present (signal is
    // the high minority). Two safety properties make this non-regressive vs the
    // scalar:
    //   * the window is WIDE (kFloorWindowHz) so it almost always spans noise
    //     beyond the voice band — it cannot collapse onto a wide signal's level;
    //   * the curve is CLAMPED to [scalar, scalar + kFloorTiltMaxDb] — it may only
    //     RISE above the global scalar (to follow genuinely-louder noise), never
    //     fall below it, and never rise far enough to swallow a real signal.
    // The presence gate stays on the global scalar (below), so weak-signal
    // engagement is unchanged; the curve only sharpens per-bin EDGE placement.
    constexpr double kFloorWindowHz   = 5000.0;  // sliding window (half = 2500 Hz)
    constexpr int    kFloorPercentile = 20;      // low pct over the window
    constexpr float  kFloorTiltMaxDb  = 10.0f;   // curve may rise at most this far

    // ── Splatter cap (spec Stages F.2, G) ───────────────────────────────────
    // The natural high-cut is where the signal returns to the noise floor (the
    // scan's far edge). For NORMAL voice that is the correct answer and must be
    // trusted: real SSB voice rolls off gradually, so the upper voice legitimately
    // sits 20-25 dB below the core — a bare referenceDbm - kSplatterDownDb cut
    // would chop useful audio (it cut a ~3 kHz signal to ~1.8 kHz on air). So the
    // reference-relative splatter cap is applied ONLY when the floor crossing runs
    // past kSplatterGuardHz, i.e. the signal never returns to the floor within the
    // plausible voice band — the signature of a dirty, over-driven splatterer.
    // kSplatterDownDb / kSplatterGuardHz are operator-tunable via the Splatter-
    // rejection setting — see OccupiedRegionParams.

    // ── Level-invariant outer edge (in-guard reference cap) ─────────────────
    // A floor-relative crossing on a SOFT skirt is a property of the SNR, not
    // of the TX signal: on a skirt of S dB/kHz the floor+5 crossing moves
    // ~1000/S Hz per dB of signal-level change, so the measured width breathed
    // with QSB and collapsed as stations weakened. The level-invariant answer
    // is reference-relative (ITU-R SM.443 puts the 99% occupied bandwidth of
    // SSB voice ~26 dB below the peak): INSIDE the splatter guard the edge is
    // additionally capped at the outermost bin within
    // (splatterDownDb + kOccupiedCapExtraDb) of the in-band reference — the
    // SAME rule family as the splatter cap, at a deeper depth (Tight 23 /
    // Normal 30 / Wide 40 dB), so the operator's Tight/Wide intent scales both
    // regimes and the past-guard splatter depth is always the tighter of the
    // two where both apply. The extra 5 dB over ITU's 26 accounts for the
    // reference sitting a few dB under the peak plus envelope-smear headroom
    // (a bare 26 re-chopped the declining-voice shape the splatter guard was
    // built to protect). The cap only engages with real headroom —
    // referenceDbm at least (depth + kCapHeadroomMarginDb) above the scalar
    // floor — because below that the cap level is within a few dB of the
    // noise and indistinguishable from it: weak signals stay governed by the
    // floor crossing (that regime is handled by the engine's widen-only
    // low-SNR behaviour instead). The floor+5 crossing remains the absolute
    // outer limit by construction (the cap only ranges over occupied bins).
    constexpr float  kOccupiedCapExtraDb   = 5.0f;
    constexpr float  kCapHeadroomMarginDb  = 5.0f;

    // ── Sharp-edge precision (spec Stage G) ─────────────────────────────────
    // Where a clear steep transition exists (modern DSP rigs have near-vertical
    // skirts), snap the edge to the steepest dB/Hz bin — the most precise method.
    // Soft/gentle roll-offs need no special inward cut: the floor crossing already
    // pins them to where the energy meets the noise, and the splatter guard above
    // bounds any over-wide tail.
    constexpr float  kSteepSlopeDbPerKHz = 30.0f;

    // ── Sliding-window percentile (floor curve) ────────────────────────────
    // The floor curve needs a low percentile of a wide (±2500 Hz) raw-bin
    // window at EVERY scan offset. Copying the window and running nth_element
    // per offset is O(span × window) — quadratic in zoom (span and window both
    // grow as hzPerBin shrinks) and an allocation storm on the GUI thread; on
    // a zoomed-in Retina pan it reached millions of float copies per frame
    // (the "waterfall chokes when zoomed" regression). The window slides by
    // exactly one bin per offset, so a histogram over quantized dB with one
    // remove + one add per step gives the same percentile in O(buckets).
    //
    // Quantization: 256 buckets over [scalarFloor - kFloorHistBelowDb,
    // scalarFloor + kFloorHistAboveDb] ≈ 0.23 dB/bucket. The incoming bins are
    // display-quantized (~0.13 dB) and the result is clamped to
    // [scalarFloor, scalarFloor + kFloorTiltMaxDb] before use, so the ≤0.24 dB
    // bucket rounding is absorbed long before the 3/5 dB gates can notice.
    constexpr int   kFloorHistBuckets = 256;
    constexpr float kFloorHistBelowDb = 20.0f;   // histogram floor headroom
    constexpr float kFloorHistAboveDb = 40.0f;   // histogram signal headroom

    struct FloorHistogram {
        std::array<int, kFloorHistBuckets> counts{};
        int   total{0};
        float lo{0.0f};
        float bucketDb{1.0f};

        explicit FloorHistogram(float scalarFloor)
            : lo(scalarFloor - kFloorHistBelowDb),
              bucketDb((kFloorHistBelowDb + kFloorHistAboveDb) / kFloorHistBuckets) {}

        int bucketOf(float v) const {
            // Guard NaN before the cast: static_cast<int>(NaN) is UB and could
            // land outside [0, buckets) after clamp, indexing counts[] OOB.
            // (#3945 review)
            if (!std::isfinite(v)) return 0;
            return std::clamp(static_cast<int>((v - lo) / bucketDb),
                              0, kFloorHistBuckets - 1);
        }
        void add(float v)    { ++counts[bucketOf(v)]; ++total; }
        void remove(float v) { --counts[bucketOf(v)]; --total; }

        // Value at the given percentile (0-100), replicating the previous
        // copy+nth_element semantics: idx = pct*(n-1)/100 (integer division),
        // then the idx-th smallest sample -> its bucket's lower edge.
        float percentile(int pct) const {
            if (total <= 0) return lo;
            const int idx = std::clamp(pct * (total - 1) / 100, 0, total - 1);
            int cum = 0;
            for (int i = 0; i < kFloorHistBuckets; ++i) {
                cum += counts[i];
                if (cum > idx) return lo + i * bucketDb;
            }
            return lo + (kFloorHistBuckets - 1) * bucketDb;
        }
    };
}

OccupiedRegion measureOccupiedRegion(const QVector<float>& binsDbm,
                                     double centerMhz, double bandwidthMhz,
                                     double carrierMhz, const QString& mode,
                                     float noiseFloorDbm,
                                     QVector<float>& avgEnv,
                                     const OccupiedRegionParams& params)
{
    OccupiedRegion r;
    const int N = binsDbm.size();
    if (N < 32 || bandwidthMhz <= 0.0) return r;

    const double hzPerBin = bandwidthMhz * 1.0e6 / N;
    if (hzPerBin <= 0.0) return r;

    // ── Resolution cap: decimate over-fine pans, then measure normally ──────
    // (see kMinMeasureHzPerBin). One level of recursion: the decimated grid is
    // >= the cap, so the recursive call always takes the D=1 path. The ragged
    // tail (< D bins, < 50 Hz of span at the far pan edge) is dropped and the
    // center re-derived so the carrier<->bin mapping stays exact. avgEnv needs
    // no special handling: span is derived from the effective grid and the
    // existing size-mismatch reseed covers the geometry change (zoom already
    // reseeds it today).
    if (hzPerBin < kMinMeasureHzPerBin) {
        const int D  = static_cast<int>(std::ceil(kMinMeasureHzPerBin / hzPerBin));
        const int Nd = N / D;
        if (Nd >= 32) {
            QVector<float> deci(Nd);
            for (int i = 0; i < Nd; ++i) {
                double sum = 0.0;
                const int base = i * D;
                for (int k = 0; k < D; ++k) sum += binsDbm[base + k];
                deci[i] = static_cast<float>(sum / D);
            }
            const double bwDeciMhz  = bandwidthMhz * (static_cast<double>(Nd) * D / N);
            const double startMhz   = centerMhz - bandwidthMhz / 2.0;
            return measureOccupiedRegion(deci, startMhz + bwDeciMhz / 2.0, bwDeciMhz,
                                         carrierMhz, mode, noiseFloorDbm, avgEnv, params);
        }
    }

    const double startMhz = centerMhz - bandwidthMhz / 2.0;
    const bool   isUsb    = (mode != QStringLiteral("LSB"));  // USB-family default

    const int carrierBin = static_cast<int>(
        std::lround((carrierMhz - startMhz) / bandwidthMhz * N));
    const int scanBins = std::max(8, static_cast<int>(kScanHz / hzPerBin));

    // Energy side: USB above the carrier (higher bins), LSB below (lower bins).
    int lo = isUsb ? carrierBin : carrierBin - scanBins;
    int hi = isUsb ? carrierBin + scanBins : carrierBin;
    lo = std::clamp(lo, 0, N - 1);
    hi = std::clamp(hi, 0, N - 1);
    if (hi - lo < 8) return r;

    // Bin index for an audio offset o (carrier-outward on the energy side).
    const auto binAt = [&](int o) -> int { return isUsb ? carrierBin + o : carrierBin - o; };

    // Scalar floor: prefer the caller's rolling value; else local 10th percentile.
    // It seeds and cross-checks the per-frequency floor curve below.
    float scalarFloor = noiseFloorDbm;
    if (scalarFloor <= -500.0f) {
        QVector<float> w(binsDbm.begin() + lo, binsDbm.begin() + hi + 1);
        std::sort(w.begin(), w.end());
        scalarFloor = w[w.size() / 10];
    }

    // Spectral envelope: a moving average (in dB) over ~kEnvHz. dB-domain
    // averaging compresses spikes, so a narrow het/carrier sitting on top of a
    // broad ESSB signal barely shifts the envelope — fixing the under-fit where
    // a sharp spike's peak-relative threshold excluded the wider voice energy.
    // Prefix sum makes the per-bin average O(1).
    const int W = hi - lo + 1;
    QVector<double> pref(W + 1, 0.0);
    for (int i = 0; i < W; ++i) pref[i + 1] = pref[i] + binsDbm[lo + i];
    const int envHalf = std::max(1, static_cast<int>(kEnvHz / hzPerBin / 2.0));
    const auto env = [&](int bin) -> float {
        const int a = std::clamp(bin - envHalf, lo, hi) - lo;
        const int b = std::clamp(bin + envHalf, lo, hi) - lo;
        return static_cast<float>((pref[b + 1] - pref[a]) / (b - a + 1));
    };
    // Materialise the instantaneous envelope over the energy-side offsets.
    const int span = scanBins + 1;
    QVector<float> envInst(span);
    for (int o = 0; o < span; ++o)
        envInst[o] = env(binAt(o));

    // ── Temporal envelope: video peak-hold with leak (fast attack, slow release) ─
    // Per-offset asymmetric EMA. Rising energy is tracked quickly (attack) so the
    // edge extends the instant the signal reaches out; falling energy decays slowly
    // (release) so a word gap or a between-sibilant lull does NOT collapse the high
    // edge. The first frame seeds avgEnv to the instantaneous envelope (so a
    // single call stays reproducible for the unit tests and a fresh fit engages at
    // full width immediately).
    if (avgEnv.size() != span) {
        avgEnv = envInst;
    } else {
        for (int o = 0; o < span; ++o) {
            const float d = envInst[o] - avgEnv[o];
            avgEnv[o] += (d > 0.0f ? kEnvAttackAlpha : kEnvReleaseAlpha) * d;
        }
    }
    const auto envAt = [&](int o) -> float { return avgEnv[o]; };

    // ── Per-frequency floor curve (Stage B) ──────────────────────────────
    // Sliding low percentile of the RAW bins across frequency, indexed by offset,
    // clamped to only rise above the global scalar (never below, never far enough
    // to swallow a signal). Tracks a tilted floor at the signal/noise boundary
    // where edge accuracy matters, while the clamp guarantees it can never make
    // the occupied threshold lower than the proven scalar behaviour.
    // One histogram slides across the scan: the window center moves by exactly
    // one bin per offset (carrier-outward), so each step removes at most one
    // leaving bin and adds at most one entering bin (the clamps freeze an edge
    // at the pan boundary, hence the while-loops that move each edge 0..1 step).
    const int floorHalf = std::max(2, static_cast<int>(kFloorWindowHz / hzPerBin / 2.0));
    QVector<float> floorCurve(span);
    {
        FloorHistogram hist(scalarFloor);
        int a = std::clamp(binAt(0) - floorHalf, 0, N - 1);
        int b = std::clamp(binAt(0) + floorHalf, 0, N - 1);
        for (int i = a; i <= b; ++i) hist.add(binsDbm[i]);
        for (int o = 0; o < span; ++o) {
            const int c  = binAt(o);
            const int a2 = std::clamp(c - floorHalf, 0, N - 1);
            const int b2 = std::clamp(c + floorHalf, 0, N - 1);
            while (b < b2) hist.add(binsDbm[++b]);
            while (a > a2) hist.add(binsDbm[--a]);
            while (a < a2) hist.remove(binsDbm[a++]);
            while (b > b2) hist.remove(binsDbm[b--]);
            floorCurve[o] = std::clamp(hist.percentile(kFloorPercentile),
                                       scalarFloor, scalarFloor + kFloorTiltMaxDb);
        }
    }
    const auto floorAt = [&](int o) -> float { return floorCurve[o]; };

    // Occupied threshold: FLOOR-relative (a fixed margin above the per-bin noise
    // floor). It must NOT be peak-relative — the TX filter edge is fixed, but a
    // peak-relative threshold rises/falls with the speaker's loudness, so the
    // measured edge would creep narrower on loud syllables and wider on quiet
    // ones. Floor-relative pins the crossing to the actual TX cliff regardless
    // of level. Splatter is handled by the rebound cut + silence-stop + the
    // reference-relative cap below, not by clamping this threshold.
    const auto occThrAt   = [&](int o) -> float { return floorAt(o) + kEnvGateDb; };
    const auto floorGateAt = [&](int o) -> float { return floorAt(o) + kSignalGateDb; };

    // Inner edge: the first bin clearing the floor-relative occupied gate.
    int firstO = -1;
    for (int o = 0; o <= scanBins; ++o)
        if (envAt(o) >= occThrAt(o)) { firstO = o; break; }
    if (firstO < 0) return r;  // nothing occupied above the floor-relative gate

    // ── Extent pass (Stage F/G, tilt-robust) ────────────────────────────────
    // ONE outward scan decides how far the tuned signal's energy extends —
    // bridging relative dips (formant nulls, the valley between a bass and a
    // treble hump) and internal fades, cutting at a separate station, and
    // re-anchoring into the dominant hump of a mid-scooped signal whose lows
    // never cleared the presence preset (see the kFloorDiscHz block comment for
    // the full decision rules). Peak/reference/presence are deliberately NOT
    // computed here: they come from the anchor pass below, over the FULL kept
    // extent, so a treble-dominant hump can never be judged against a
    // bass-only reference (that mis-anchoring both amputated smiley-EQ signals
    // at the mid scoop and rejected strong treble-dominant signals whose weak
    // bass lobe alone failed the presence gate).
    //
    // Look-ahead span for the gap-exit plateau test: the envelope ramps over
    // ~envHalf bins, so the first re-occupied bin still reads near the gap
    // level; peek a couple bins further to see the level the signal resumes to.
    const int reboundLook = std::max(1, 2 * envHalf + 1);

    int keptEndO = firstO;            // outermost kept occupied bin
    QVector<float> runVals;           // occupied env values over the kept extent
    float  runMaxEnv    = envAt(firstO);
    double rawFloorRunHz = 0.0;       // contiguous RAW bins < floorGate (arming)
    double silenceHz     = 0.0;       // contiguous ENV bins < floorGate (band edge)
    bool   inGap = false, armed = false;

    for (int o = firstO; o <= scanBins; ++o) {
        const float v = envAt(o);
        if (v >= occThrAt(o)) {
            if (inGap && armed) {
                // Resuming across a REAL disconnection: same signal, separate
                // station, or the dominant hump of the tuned signal?
                float plateau = v;
                for (int k = o + 1; k <= std::min(scanBins, o + reboundLook); ++k)
                    plateau = std::max(plateau, envAt(k));
                // Pre-gap level: what the signal decayed FROM going into the
                // gap — a deep internal fade recovers to <= this, so it is not
                // mistaken for a separate lobe (the reference alone would cut
                // a fade recovery on a tilted signal).
                float preGapLevel = envAt(keptEndO);
                for (int k = std::max(firstO, keptEndO - reboundLook + 1); k < keptEndO; ++k)
                    preGapLevel = std::max(preGapLevel, envAt(k));
                float refSoFar = runMaxEnv;
                if (runVals.size() >= 3) {
                    QVector<float> w = runVals;
                    const int idx = std::clamp(
                        kReferencePct * (static_cast<int>(w.size()) - 1) / 100,
                        0, static_cast<int>(w.size()) - 1);
                    std::nth_element(w.begin(), w.begin() + idx, w.end());
                    refSoFar = w[idx];
                }
                if (plateau > std::max(preGapLevel, refSoFar) + kReboundDb) {
                    const bool runConfident =
                        runMaxEnv >= scalarFloor + params.minPeakDb;
                    if (runConfident) break;   // separate stronger lobe -> cut
                    if (o * hzPerBin > kReanchorMaxStartHz) break;  // adjacent stn
                    // else: re-anchor into the tuned signal's dominant hump
                }
            }
            keptEndO = o;
            runVals.append(v);
            runMaxEnv = std::max(runMaxEnv, v);
            inGap = false; armed = false;
            silenceHz = 0.0;
        } else {
            inGap = true;
            // True-silence band edge: contiguous ENVELOPE silence wider than
            // kSilenceHz on a CONFIDENT run means nothing resumes — stop. An
            // unconfident run keeps scanning (bounded by kScanHz): its dominant
            // hump may still be ahead beyond a wide at-floor mid scoop, and the
            // resume decision above adjudicates whatever is found (this is the
            // bounded look-ahead that used to be cut short at 600 Hz with the
            // AUTO badge confidently showing a bass-only fit).
            if (v < floorGateAt(o)) {
                silenceHz += hzPerBin;
                if (silenceHz > kSilenceHz &&
                    runMaxEnv >= scalarFloor + params.minPeakDb) break;
            } else {
                silenceHz = 0.0;
            }
        }
        // Disconnection arming on the RAW bins, tracked for EVERY bin — the
        // raw valley starts under the envelope's smear shoulder, ~150 Hz
        // before the envelope gap opens, and that width is part of the real
        // disconnection (accumulating only inside the envelope gap would
        // re-shrink the standard by the very smear the raw domain is meant to
        // bypass — see kFloorDiscHz). Hz-accumulated, so coarse pans cannot
        // truncate it. An envelope-occupied bin clears the ARM (same-signal
        // energy resumed and was adjudicated above), while the raw run itself
        // only resets on a raw bin back above the floor gate. binAt(o) can run
        // past the pan when the slice sits within kScanHz of a pan edge —
        // clamp like env() does (edge bin repeats).
        if (binsDbm[std::clamp(binAt(o), 0, N - 1)] < floorGateAt(o)) {
            rawFloorRunHz += hzPerBin;
            if (rawFloorRunHz >= kFloorDiscHz) armed = true;
        } else {
            rawFloorRunHz = 0.0;
        }
    }

    // ── Anchor pass: peak / presence / reference over the FULL kept extent ──
    int   peakO   = firstO;
    float envPeak = envAt(firstO);
    for (int o = firstO; o <= keptEndO; ++o)
        if (envAt(o) > envPeak) { envPeak = envAt(o); peakO = o; }

    // Presence gate on the GLOBAL scalar floor: the scalar is the robust
    // band-wide noise estimate. Judged on the kept extent's peak, so a strong
    // dominant hump beyond a faded mid scoop counts (a bass-only judgement
    // rejected such signals outright and lurched the filter to baseline).
    if (envPeak < scalarFloor + params.minPeakDb) return r;  // weak / ambiguous

    // Reference = a HIGH percentile of the occupied bins across the kept extent.
    // High (not the median) so it tracks the signal's level when a treble hump
    // dominates; a percentile (not the peak) so a lone transient het — a tiny
    // fraction of the extent's bins — cannot inflate it.
    float referenceDbm = envPeak;  // fallback if the extent is too thin
    if (runVals.size() >= 3) {
        std::sort(runVals.begin(), runVals.end());
        const int idx = std::clamp(kReferencePct * (static_cast<int>(runVals.size()) - 1) / 100,
                                   0, static_cast<int>(runVals.size()) - 1);
        referenceDbm = runVals[idx];
    }
    const float splatterLevel = referenceDbm - params.splatterDownDb;

    // ── Cap pass ─────────────────────────────────────────────────────────────
    // splatterO tracks the outermost occupied bin still within kSplatterDownDb
    // of the in-band reference — the reference-relative cap that excludes a
    // slowly-decaying splatter tail (Stage F.2). refCapO tracks the outermost
    // occupied bin within the deeper in-guard depth — the level-invariant edge
    // (see the kOccupiedCapExtraDb block comment). Both computed against the
    // FINAL reference (anchor pass), not a run-so-far value.
    const float capDepthDb  = params.splatterDownDb + kOccupiedCapExtraDb;
    const float refCapLevel = referenceDbm - capDepthDb;
    int nearO = firstO, farO = keptEndO, splatterO = firstO, refCapO = firstO;
    for (int o = firstO; o <= keptEndO; ++o) {
        const float v = envAt(o);
        if (v < occThrAt(o)) continue;
        if (v >= splatterLevel) splatterO = o;
        if (v >= refCapLevel)   refCapO   = o;
    }

    // Level-invariant edge: with real headroom, the occupied width ends where
    // the envelope has fallen capDepthDb below the in-band reference — the
    // same Hz whatever the absolute signal level, so QSB stops breathing the
    // passband on soft skirts. Without headroom the floor crossing governs.
    if (referenceDbm - scalarFloor >= capDepthDb + kCapHeadroomMarginDb)
        farO = std::min(farO, std::max(refCapO, peakO));
    // ── Outer-edge refinement (Stage F.2 / G) ───────────────────────────────
    // Steep slope, in dB per bin (kSteepSlopeDbPerKHz is dB/kHz).
    const float steepPerBin = kSteepSlopeDbPerKHz * static_cast<float>(hzPerBin) / 1000.0f;

    // (1) Splatter guard: trust the floor crossing within the plausible voice
    // band; only when it runs PAST kSplatterGuardHz (the signal never returned to
    // the floor in-band — a dirty over-driven tail) pull the edge back to the
    // reference-relative cap (the last bin within kSplatterDownDb of the core).
    if (farO * hzPerBin > params.splatterGuardHz)
        farO = std::min(farO, std::max(splatterO, peakO));

    // (2) Sharp-edge precision: if a clear cliff sits at/just inside farO, latch
    // the steepest bin (modern steep skirts). Gentle roll-offs keep the floor
    // crossing — it already pins them to where the energy meets the noise.
    {
        const int gradWin = std::max(1, envHalf);
        float maxDrop = 0.0f; int steepO = farO;
        for (int o = std::max(peakO + 1, farO - gradWin); o <= farO && o + 1 <= scanBins; ++o) {
            const float drop = envAt(o) - envAt(o + 1);   // positive = falling outward
            if (drop > maxDrop) { maxDrop = drop; steepO = o + 1; }
        }
        if (maxDrop >= steepPerBin) farO = std::min(farO, std::max(steepO, peakO));
    }

    // Inner edge: snap to a steep rise when the signal climbs sharply out of the
    // carrier region; otherwise keep the floor-relative crossing. The refined
    // inner edge may never move INSIDE (closer to the carrier than) the
    // floor-relative occupancy crossing — that crossing stays the binding lower
    // bound (preserves the no-creep property).
    {
        const int gradWin = std::max(1, envHalf);
        float maxRise = 0.0f; int steepO = nearO;
        for (int o = nearO; o <= std::min(peakO, nearO + gradWin) && o + 1 <= scanBins; ++o) {
            const float rise = envAt(o + 1) - envAt(o);   // positive = rising outward
            if (rise > maxRise) { maxRise = rise; steepO = o; }
        }
        if (maxRise >= steepPerBin) nearO = std::max(nearO, steepO);
    }
    if (farO < nearO) farO = nearO;

    // Offset indices -> audio cut magnitudes (Hz), plus intelligibility margin.
    int audioLow  = static_cast<int>(std::floor(nearO * hzPerBin)) - kMarginHz;
    int audioHigh = static_cast<int>(std::ceil (farO  * hzPerBin)) + kMarginHz;
    audioLow = std::max(0, audioLow);
    if (audioHigh - audioLow < kMinBwHz) return r;
    // SSB-voice shape gate: the energy must start near the carrier. A band that
    // starts well above it (e.g. a 1600-4000 data/het signal) isn't the SSB
    // voice we're tuned to -> reject (r.valid stays false) so the engine keeps
    // the operator's manual filter.
    if (audioLow > kMaxVoiceLowCutHz) return r;

    r.valid        = true;
    r.lowHz        = audioLow;
    r.highHz       = audioHigh;
    r.peakDbm      = envPeak;
    r.referenceDbm = referenceDbm;
    r.floorDbm     = scalarFloor;
    return r;
}

} // namespace AetherSDR
