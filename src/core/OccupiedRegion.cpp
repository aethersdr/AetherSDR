#include "OccupiedRegion.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>

namespace AetherSDR {

namespace {
    // ── Measurement constants (RFC #3878) ───────────────────────────────────
    // Starting values, biased toward stability. Expect on-air tuning before
    // release (the maintainer signs off on the DSP feel per the RFC).
    constexpr double kScanHz        = 6500.0; // scan this far from the carrier
    // Measurement resolution cap. Every loop is O(kScanHz / hzPerBin), and hzPerBin
    // shrinks without bound on zoom while downstream is far coarser (50 Hz snap,
    // 150 Hz margin, 220 Hz deadband). Finer input is decimated by the dB mean of D
    // bins so the effective bin stays in [kMin, 2*kMin) and cost is O(1) in zoom.
    // Mean, not max (biases noise up ~5 dB) or stride (jitters narrow hets); it is
    // the first stage of the kEnvHz average. Coarser pans use D=1 unchanged.
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
    // Presence margin (env peak over floor) is operator-tunable via
    // OccupiedRegionParams::minPeakDb; the time-averaged envelope keeps noise within
    // ~1-3 dB. SSB voice shape gate: voice starts near the carrier, so a band
    // starting above kMaxVoiceLowCutHz (e.g. a 1600-4000 Hz data signal) is rejected
    // and the caller keeps the manual filter.
    constexpr int    kMaxVoiceLowCutHz = 600;
    // Extent and separate-station rejection. The wanted signal is the energy from the
    // carrier that never returns to the floor. A floor gap >= kFloorDiscHz, measured
    // on RAW bins (the ~300 Hz envelope smear erodes ~150 Hz per side), ARMS a
    // disconnection; shallower dips are bridged. When energy resumes across an armed
    // gap with a plateau > kReboundDb above both the pre-gap level and the reference:
    //   * run so far CONFIDENT (cleared presence) -> separate station, cut at valley;
    //   * run NOT confident (weak bass lobe of a mid-scooped ESSB signal) ->
    //     RE-ANCHOR into the lobe, if it starts within kReanchorMaxStartHz.
    // Peak/reference/presence are computed over the full kept extent (anchor pass).
    constexpr float  kReboundDb    = 8.0f;
    constexpr double kFloorDiscHz  = 250.0;  // raw floor gap this wide arms a
                                             // real disconnection (in Hz — no
                                             // bin truncation)
    constexpr double kReanchorMaxStartHz = 2000.0;  // re-anchor only into lobes
                                             // starting by here (voice humps
                                             // rise by ~1.2-2 kHz)
    // Cut-vs-reanchor hysteresis: cutting requires the pre-gap run to clear the gate
    // by kRunConfidentHystDb AND the new lobe to exceed kReboundDb by
    // kSeparateMarginDb, so a ~0.2 dB wiggle can't flip the high-cut by a whole hump
    // width; otherwise re-anchor (keeping signal is the safer error).
    constexpr float  kRunConfidentHystDb = 2.0f;   // over the presence gate to CUT
    constexpr float  kSeparateMarginDb   = 2.0f;   // over kReboundDb to CUT
    // Weak-run re-anchor guard: an unconfident run scans toward kScanHz, so a
    // re-anchor also needs a real inner lobe (>= kInnerReanchorMinWidthHz occupied
    // and >= floor + kEnvGateDb + kInnerReanchorMinDb) and an at-floor gap no wider
    // than kUnconfBridgeMaxHz (also its silence-stop). Occupied width under-reads a
    // weak lobe by the envelope smear (600-700 Hz raw bass reads ~290-390 Hz; a
    // carrier blip ~0), so 250 Hz separates them.
    constexpr double kInnerReanchorMinWidthHz = 250.0;
    constexpr float  kInnerReanchorMinDb      = 1.0f;   // over the occupied gate
    constexpr double kUnconfBridgeMaxHz       = 1300.0;
    constexpr int    kReferencePct = 75;     // in-band reference = this percentile
                                             // of the kept extent (tracks a
                                             // treble hump; robust to a transient)
    constexpr int    kMarginHz      = 150;    // intelligibility margin
    constexpr int    kMinBwHz       = 50;     // never narrower than this
    // Temporal envelope: per-offset fast-attack / slow bounded-release EMA (a leaky
    // peak hold). SSB voice fills its upper 1.5-3 kHz only intermittently, so a
    // symmetric average sagged in word gaps and the high edge jittered. The ~1.1 s
    // release holds reach across gaps while real narrowing resolves in ~1-2 s.
    constexpr float  kEnvAttackAlpha  = 0.30f;  // fast rise  (~0.1 s time constant)
    constexpr float  kEnvReleaseAlpha = 0.03f;  // slow fall  (~1.1 s time constant)

    // Edge-het rejection (opt-in, OccupiedRegionParams::hetReject): a narrow strong
    // interferer shows as a RAW bin far (> kHetExcessDb) above the smoothed envelope,
    // which a voice hump or formant never does. A het within kHetSearchHz of a cut
    // pulls the cut kHetGuardHz inboard of it, never past the signal peak.
    // Single-frame and stateless; the engine's temporal pipeline supplies stability.
    // Mid-band hets are left to notch/ANF.
    constexpr double kHetSearchHz = 300.0;   // scan this far in/out of each cut
    constexpr float  kHetExcessDb = 15.0f;   // raw over local envelope = a het
    constexpr double kHetGuardHz  = 150.0;   // place the cut this far below it

    // Per-frequency noise floor: a sliding low percentile of raw bins over a wide
    // window (kFloorWindowHz), so a tilted floor doesn't drag the high-cut into
    // noise. Clamped to [scalar, scalar + kFloorTiltMaxDb]: it may only rise above
    // the global scalar floor and never far enough to swallow a signal. The presence
    // gate stays on the scalar; the curve only sharpens edge placement.
    constexpr double kFloorWindowHz   = 5000.0;  // sliding window (half = 2500 Hz)
    constexpr int    kFloorPercentile = 20;      // low pct over the window
    constexpr float  kFloorTiltMaxDb  = 10.0f;   // curve may rise at most this far

    // Splatter cap: normal voice legitimately rolls off 20-25 dB below the core, so
    // the natural floor-return high-cut is trusted; the reference-relative cap
    // (referenceDbm - kSplatterDownDb) applies only when the floor crossing runs past
    // kSplatterGuardHz (an over-driven splatterer). Both are operator-tunable
    // (OccupiedRegionParams).

    // Level-invariant outer edge: on a soft skirt of S dB/kHz the floor crossing moves
    // ~1000/S Hz per dB, so width breathed with QSB. Inside the splatter guard the
    // edge is also capped at the outermost bin within
    // (splatterDownDb + kOccupiedCapExtraDb) of the in-band reference (Tight 23 /
    // Normal 30 / Wide 40 dB). ITU-R SM.443 puts SSB 99% bandwidth ~26 dB down; the
    // extra 5 dB covers reference-below-peak and smear. Engages only when
    // referenceDbm >= scalar floor + depth + kCapHeadroomMarginDb; weaker signals
    // follow the floor crossing, which always remains the outer limit.
    constexpr float  kOccupiedCapExtraDb   = 5.0f;
    constexpr float  kCapHeadroomMarginDb  = 5.0f;

    // ── Sharp-edge precision (spec Stage G) ─────────────────────────────────
    // Where a clear steep transition exists (modern DSP rigs have near-vertical
    // skirts), snap the edge to the steepest dB/Hz bin — the most precise method.
    // Soft/gentle roll-offs need no special inward cut: the floor crossing already
    // pins them to where the energy meets the noise, and the splatter guard above
    // bounds any over-wide tail.
    constexpr float  kSteepSlopeDbPerKHz = 30.0f;

    // Sliding-window percentile for the floor curve: the window (+/-2500 Hz) slides
    // one bin per offset, so a histogram with one remove + one add per step gives the
    // percentile in O(buckets) instead of O(span x window) copies per frame. 256
    // buckets over [floor - kFloorHistBelowDb, floor + kFloorHistAboveDb] = ~0.23
    // dB/bucket; input is display-quantized (~0.13 dB) and the result is clamped, so
    // rounding stays far below the 3/5 dB gates.
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

    // Per-frequency floor curve: sliding low percentile of raw bins, clamped to only
    // rise above the scalar floor. One histogram slides carrier-outward one bin per
    // offset; edge clamps at the pan boundary mean each edge moves 0..1 step (hence
    // the while-loops).
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

    // Extent pass: one outward scan decides how far the signal extends, bridging
    // relative dips and fades, cutting at a separate station, re-anchoring into a
    // mid-scooped signal's dominant hump (rules at kFloorDiscHz). Peak/reference/
    // presence come from the anchor pass over the full kept extent, never a
    // bass-only reference. reboundLook: the envelope ramps over ~envHalf bins, so
    // peek past it to see the level the signal resumes to.
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
                    // CUT as a separate station only when BOTH conditions clear
                    // their dead-bands (F3) — a razor pivot flipped the high-cut
                    // by the whole treble width on a ~0.2 dB bass wiggle.
                    const bool runEstablished =
                        runMaxEnv >= scalarFloor + params.minPeakDb + kRunConfidentHystDb;
                    const bool clearlySeparate =
                        plateau > std::max(preGapLevel, refSoFar) + kReboundDb + kSeparateMarginDb;
                    if (runEstablished && clearlySeparate) break;   // separate stn -> cut
                    if (o * hzPerBin > kReanchorMaxStartHz) break;  // adjacent stn
                    // Re-anchor into the tuned signal's dominant hump — but for a
                    // WEAK (unestablished) run guard against grabbing a neighbour
                    // (F4): the inner pre-gap lobe must be a real sustained lobe,
                    // not a near-carrier blip.
                    if (!runEstablished) {
                        const double innerWidthHz = (keptEndO - firstO) * hzPerBin;
                        const bool innerReal =
                            innerWidthHz >= kInnerReanchorMinWidthHz &&
                            runMaxEnv >= scalarFloor + kEnvGateDb + kInnerReanchorMinDb;
                        if (!innerReal) break;   // weak blip + neighbour -> don't grab
                    }
                    // else: re-anchor
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
                // Confident run: kSilenceHz of contiguous at-floor spectrum is a
                // true band edge. Unconfident run: keep looking past a mid scoop
                // for the dominant hump, but bound the bridge (kUnconfBridgeMaxHz,
                // F4) so the scan can't run across dead air to a neighbour.
                const bool confident = runMaxEnv >= scalarFloor + params.minPeakDb;
                if (silenceHz > (confident ? kSilenceHz : kUnconfBridgeMaxHz)) break;
            } else {
                silenceHz = 0.0;
            }
        }
        // Disconnection arming on RAW bins, for every bin: the raw valley starts ~150 Hz
        // before the envelope gap and belongs to the disconnection (see kFloorDiscHz).
        // Accumulated in Hz so coarse pans can't truncate it. An envelope-occupied bin
        // clears the ARM; the raw run resets only on a raw bin above the floor gate.
        // binAt(o) can pass the pan edge near it, so clamp like env().
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

    // ── Edge-het rejection (opt-in) ─────────────────────────────────────────
    // Find a narrow strong interferer (het/carrier) within kHetSearchHz of a
    // finalised edge and record the audio frequency to cut at — applied AFTER
    // the intelligibility margin below, so the margin cannot re-cross the het.
    // Never cuts past the signal peak (a het inboard of the peak is left to the
    // notch — a bandpass edge can't remove it without gutting voice).
    int hetHighCutHz = INT_MAX;   // clamp audioHigh at/below this
    int hetLowCutHz  = 0;         // clamp audioLow  at/above this
    if (params.hetReject) {
        const int searchBins = std::max(1, static_cast<int>(kHetSearchHz / hzPerBin));
        const int hetGuard   = static_cast<int>(kHetGuardHz);
        const auto isHet = [&](int o) -> bool {
            return o >= 0 && o <= scanBins &&
                   binsDbm[std::clamp(binAt(o), 0, N - 1)] - envAt(o) >= kHetExcessDb;
        };
        // High edge: the innermost het within reach of farO and above the peak.
        for (int o = std::max(peakO + 1, farO - searchBins);
             o <= std::min(scanBins, farO + searchBins); ++o) {
            if (isHet(o)) {
                hetHighCutHz = std::max(static_cast<int>(peakO * hzPerBin),
                                        static_cast<int>(o * hzPerBin) - hetGuard);
                break;
            }
        }
        // Low edge: a het just above the low cut (below the peak) raises it.
        for (int o = std::min(peakO - 1, nearO + searchBins);
             o >= std::max(0, nearO - searchBins); --o) {
            if (isHet(o)) {
                hetLowCutHz = std::min(static_cast<int>(peakO * hzPerBin),
                                       static_cast<int>(o * hzPerBin) + hetGuard);
                break;
            }
        }
    }

    // Offset indices -> audio cut magnitudes (Hz), plus intelligibility margin.
    int audioLow  = static_cast<int>(std::floor(nearO * hzPerBin)) - kMarginHz;
    int audioHigh = static_cast<int>(std::ceil (farO  * hzPerBin)) + kMarginHz;
    audioLow = std::max(0, audioLow);
    // Edge-het cut wins over the margin (opt-in; sentinels no-op when disabled).
    audioHigh = std::min(audioHigh, hetHighCutHz);
    audioLow  = std::max(audioLow,  hetLowCutHz);
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
