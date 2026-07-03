#include "OccupiedRegion.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
    // ── Measurement constants (RFC #3878) ───────────────────────────────────
    // Starting values, biased toward stability. Expect on-air tuning before
    // release (the maintainer signs off on the DSP feel per the RFC).
    constexpr double kScanHz        = 6500.0; // scan this far from the carrier
    // Spectral-envelope smoothing: a moving average over kEnvHz suppresses
    // narrow spikes (hets/carriers) and speech fine structure, leaving the
    // voice "hump". Edge finding runs on the envelope, NOT the raw bins, so a
    // sharp spike on top of a broad ESSB signal can't fool the threshold.
    constexpr double kEnvHz         = 300.0;
    constexpr float  kEnvGateDb     = 5.0f;   // occupied edge: env >= floor + this
    constexpr float  kSignalGateDb  = 3.0f;   // env below floor+this = no signal
    constexpr double kSilenceHz     = 600.0;  // ...for this span => a true band edge
                                              // (wide enough to bridge deep
                                              // internal QSB notches; a separate
                                              // STRONGER lobe is still cut by the
                                              // pre-gap-level rebound test)
    // Presence margin (env peak over floor) is operator-tunable via the Minimum-SNR
    // setting — see OccupiedRegionParams::minPeakDb. The time-averaged envelope keeps
    // noise excursions to ~1-3 dB, so even the most sensitive preset does not
    // false-engage on pure noise.
    // SSB-voice shape gate: voice energy starts near the carrier. If the
    // occupied band starts above this (e.g. a 1600-4000 data/het signal), it
    // isn't the SSB voice we're tuned to -> reject -> caller keeps the manual
    // filter. Generous (well above any real voice low-cut, well below 1600).
    constexpr int    kMaxVoiceLowCutHz = 600;
    // Connected run + separate-station rejection (RFC follow-up). The wanted
    // signal is the contiguous energy from the carrier that never returns to the
    // noise floor — ONE signal whatever its spectral tilt. A floor-reaching gap of
    // >= kFloorDiscHz is a real DISCONNECTION (the run boundary); a shallower or
    // merely relative dip is bridged. Past such a disconnection, energy that climbs
    // to a plateau >= kReboundDb above the in-band reference is a DISTINCT lobe (a
    // separate/splatter station) and the high-cut is cut at the valley. Anchoring
    // the peak/reference to the whole run (not a near-carrier window) is what keeps
    // a treble-dominant signal's dominant hump from being read as a neighbour.
    constexpr float  kReboundDb    = 8.0f;
    constexpr double kFloorDiscHz  = 250.0;  // floor-reaching gap this wide = a
                                             // real disconnection (run boundary)
    constexpr int    kReferencePct = 75;     // in-band reference = this percentile
                                             // of the connected run (tracks a
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

    // ── Sharp-edge precision (spec Stage G) ─────────────────────────────────
    // Where a clear steep transition exists (modern DSP rigs have near-vertical
    // skirts), snap the edge to the steepest dB/Hz bin — the most precise method.
    // Soft/gentle roll-offs need no special inward cut: the floor crossing already
    // pins them to where the energy meets the noise, and the splatter guard above
    // bounds any over-wide tail.
    constexpr float  kSteepSlopeDbPerKHz = 30.0f;

    // Value at the given percentile (0-100) of bins[a..b], via nth_element
    // (selection, not a full sort).
    float percentileOfWindow(const QVector<float>& bins, int a, int b, int pct)
    {
        QVector<float> w(bins.begin() + a, bins.begin() + b + 1);
        const int idx = std::clamp(pct * (static_cast<int>(w.size()) - 1) / 100,
                                   0, static_cast<int>(w.size()) - 1);
        std::nth_element(w.begin(), w.begin() + idx, w.end());
        return w[idx];
    }
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
    const int floorHalf = std::max(2, static_cast<int>(kFloorWindowHz / hzPerBin / 2.0));
    QVector<float> floorCurve(span);
    for (int o = 0; o < span; ++o) {
        const int c = binAt(o);
        const int a = std::clamp(c - floorHalf, 0, N - 1);
        const int b = std::clamp(c + floorHalf, 0, N - 1);
        const float pct = percentileOfWindow(binsDbm, a, b, kFloorPercentile);
        floorCurve[o] = std::clamp(pct, scalarFloor, scalarFloor + kFloorTiltMaxDb);
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
    const int  silBins    = std::max(1, static_cast<int>(kSilenceHz / hzPerBin));

    // Inner edge: the first bin clearing the floor-relative occupied gate.
    int firstO = -1;
    for (int o = 0; o <= scanBins; ++o)
        if (envAt(o) >= occThrAt(o)) { firstO = o; break; }
    if (firstO < 0) return r;  // nothing occupied above the floor-relative gate

    // ── Connected run + in-band reference (Stage F/G, tilt-robust) ──────────
    // The wanted signal is the contiguous energy from the carrier that never
    // returns to the noise floor. Bridge relative dips (formant nulls, the valley
    // between a bass and a treble hump); stop at the first floor-reaching gap of
    // kFloorDiscHz — a real disconnection. Anchor the PEAK and the in-band
    // REFERENCE to THIS run, not a fixed near-carrier window, so both reflect the
    // signal's real level even when the energy peaks well above the carrier
    // (treble-dominant voice) — otherwise a strong high hump read far above a
    // carrier-anchored reference and got cut as if it were a neighbour.
    const int floorDiscBins = std::max(1, static_cast<int>(kFloorDiscHz / hzPerBin));
    int connEndO = firstO;
    for (int o = firstO, floorRun = 0; o <= scanBins; ++o) {
        const float v = envAt(o);
        if (v >= occThrAt(o))        { connEndO = o; floorRun = 0; }
        else if (v < floorGateAt(o)) { if (++floorRun >= floorDiscBins) break; }
        else                         { floorRun = 0; }   // relative dip -> bridge
    }

    // Peak over the connected run (drives the gap logic + presence gate). Because
    // the peak now lands on the dominant hump wherever it sits, the energy up to it
    // is bridged and a treble hump is captured instead of being cut before it.
    int   peakO   = firstO;
    float envPeak = envAt(firstO);
    for (int o = firstO; o <= connEndO; ++o)
        if (envAt(o) > envPeak) { envPeak = envAt(o); peakO = o; }

    // Presence gate on the GLOBAL scalar floor (unchanged behaviour): the scalar is
    // the robust band-wide noise estimate, so weak-signal engagement is identical.
    if (envPeak < scalarFloor + params.minPeakDb) return r;  // weak / ambiguous

    // Reference = a HIGH percentile of the occupied bins across the connected run.
    // High (not the median) so it tracks the signal's level when a treble hump
    // dominates; a percentile (not the peak) so a lone transient het — a tiny
    // fraction of the run's bins — cannot inflate it.
    QVector<float> runVals;
    for (int o = firstO; o <= connEndO; ++o)
        if (envAt(o) >= occThrAt(o)) runVals.append(envAt(o));
    float referenceDbm = envPeak;  // fallback if the run is too thin
    if (runVals.size() >= 3) {
        std::sort(runVals.begin(), runVals.end());
        const int idx = std::clamp(kReferencePct * (static_cast<int>(runVals.size()) - 1) / 100,
                                   0, static_cast<int>(runVals.size()) - 1);
        referenceDbm = runVals[idx];
    }
    const float splatterLevel = referenceDbm - params.splatterDownDb;

    // Scan the contiguous occupied region from the CARRIER outward. The inner
    // edge (low-cut) is the first occupied bin; up to and past the peak all gaps
    // are bridged (near-carrier audio + the main hump are one station).
    //
    // INTERNAL GAP handling (frequency-selective fades inside the voice): when
    // energy resumes after a gap at a level CONSISTENT with the wanted signal's
    // decay (<= the pre-gap level + kReboundDb), it's the same signal recovering
    // — bridge it and keep going, even across a deep, wide notch. We only stop
    // at:
    //   (a) TRUE silence — below floorGate for silBins of contiguous spectrum
    //       (a real band edge: nothing resumes), or
    //   (b) a SEPARATE STRONGER lobe — energy resuming ABOVE the pre-gap level
    //       by kReboundDb (an adjacent / splatter station rising up).
    // Comparing the resumption to the PRE-GAP level (not to the valley floor) is
    // the fix: a deep internal fade recovers to <= its pre-gap level, so it is
    // no longer mistaken for a separate lobe and cut.
    //
    // splatterO additionally tracks the outermost occupied bin still within
    // kSplatterDownDb of the in-band reference — the reference-relative cap that
    // excludes a slowly-decaying splatter tail (Stage F.2).
    int nearO = firstO, farO = firstO, splatterO = firstO;
    bool  inGap = false, gapHitFloor = false;
    // Look-ahead span for the gap-exit plateau test (below): the envelope ramps
    // over ~envHalf bins, so the first re-occupied bin still reads near the gap
    // level; peek a couple bins further to see the level the signal resumes to.
    const int reboundLook = std::max(1, 2 * envHalf + 1);
    for (int o = nearO, silence = 0; o <= scanBins; ++o) {
        const float v = envAt(o);
        if (v >= occThrAt(o)) {
            // Separate-stronger-station cut — but ONLY across a real DISCONNECTION
            // (a gap that returned to the noise floor). A RELATIVE dip that stayed
            // above the floor is the valley between two humps of ONE spectrally-
            // tilted signal (e.g. a treble-dominant voice whose energy peaks well
            // above the carrier), so it bridges and the far hump is kept — instead
            // of being chopped as if it were a neighbour. A genuine adjacent station
            // is still separated by a floor-reaching valley, so it is still cut.
            // The plateau look-ahead (not just the first ramp bin, which the
            // envelope average holds low) makes the resumed level visible; comparing
            // to the in-band reference keeps a same-level fade recovery bridging.
            if (inGap && o > peakO && gapHitFloor) {
                float plateau = v;
                for (int k = o + 1; k <= std::min(scanBins, o + reboundLook); ++k)
                    plateau = std::max(plateau, envAt(k));
                if (plateau > referenceDbm + kReboundDb) break;
            }
            farO = o;
            if (v >= splatterLevel) splatterO = o;
            inGap = false; gapHitFloor = false;
            silence = 0;
        } else if (o > peakO) {
            inGap = true;
            if (v >= floorGateAt(o)) silence = 0;
            else { gapHitFloor = true; if (++silence > silBins) break; }
        }
    }
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
    return r;
}

} // namespace AetherSDR
