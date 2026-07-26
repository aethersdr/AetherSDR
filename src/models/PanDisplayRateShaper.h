#pragma once

#include <QVector>

#include <cmath>

namespace AetherSDR {

// Coalesces a raw spectrum stream down to a target display rate.
//
// WHY THIS EXISTS. A backend that streams cooked spectra (HL2) emits one frame
// per FFT block, so its frame rate is the IQ SAMPLE RATE divided by the FFT
// size — nothing to do with what the operator asked for. On a Hermes-Lite 2
// that is 48000/1024 = 47 fps at the narrowest span and 384000/1024 = 375 fps
// at the widest, so zooming out multiplied the render load eightfold while the
// Display→FFT FPS and Display→Waterfall Rate sliders governed neither. (Those
// sliders emitted Flex wire text, which on that radio reached nothing.)
//
// For the waterfall it was a CORRECTNESS bug rather than only load: the widget
// scales its time axis from line_duration, so rows arriving at 375/s against a
// 100 ms calibration made the visible history up to 37x shorter than it
// claimed.
//
// AVERAGING IN THE POWER DOMAIN is the load-bearing choice, not an
// implementation detail:
//
//   - Power-domain mean: the expected value of the mean of N noise frames
//     equals that of one frame, so the noise floor does not move as the
//     operator zooms and N changes.
//   - Max-hold would LIFT the floor at wide spans (the max of N noise samples
//     grows with N).
//   - Log-domain (dB) averaging would DEPRESS it (Jensen's inequality).
//
// The trace is calibrated to dBm, so a level that shifted with zoom would be
// the display lying about signal strength — the failure this codebase has been
// bitten by before. Dropping frames outright was the other option and was
// rejected for the same reason it would lose brief signals entirely: at 384 kHz
// a 30 fps display would discard ~92% of the spectrum.
//
// A Flex radio never uses this: its display engine shapes on the radio side and
// its frames arrive through PanadapterStream.
struct PanDisplayRateShaper {
    QVector<double> accum;      // linear power, summed over the interval
    int             count{0};
    qint64          lastEmitNs{0};
    // False until the first frame has been let through. The first frame is
    // ALWAYS emitted: the interval describes the gap between frames, not a delay
    // before the first one, and holding it back would leave the panadapter blank
    // for an interval after connect (or after a span change rebuilt the FFT) for
    // no benefit.
    bool            primed{false};

    // Feed one frame of DC-centred dBFS/dBm bins. Returns true when the
    // interval has elapsed, leaving the coalesced frame in `out`.
    //
    // intervalMs <= 0 means "no shaping requested": every frame is passed
    // through rather than dividing by zero or stalling the display.
    bool feed(const QVector<float>& bins, qint64 nowNs, int intervalMs,
              QVector<float>& out)
    {
        constexpr qint64 kNsPerMs = 1000000;

        // A bin-count change (the operator zoomed and the FFT was rebuilt) makes
        // the partial sum meaningless — averaging across it would blend two
        // different frequency grids into a single frame.
        if (accum.size() != bins.size()) {
            accum.assign(bins.size(), 0.0);
            count = 0;
        }

        // Accumulated as a SUM and divided at emit time, so a frame count that
        // varies between intervals (the block rate is not a multiple of the
        // display rate) still yields a correct mean.
        for (qsizetype i = 0; i < bins.size(); ++i)
            accum[i] += std::pow(10.0, static_cast<double>(bins[i]) / 10.0);
        ++count;

        const qint64 dueNs = static_cast<qint64>(intervalMs) * kNsPerMs;
        if (primed && dueNs > 0 && (nowNs - lastEmitNs) < dueNs)
            return false;

        out.resize(bins.size());
        if (count == 1) {
            // The common case at narrow spans, where the block rate is already
            // at or below the display rate. Pass the frame through untouched: a
            // pow/log10 round trip on a single frame is wasted work and a
            // needless rounding of a value that was already exact.
            out = bins;
        } else {
            const double inv = 1.0 / static_cast<double>(count);
            for (qsizetype i = 0; i < bins.size(); ++i) {
                out[i] = static_cast<float>(
                    10.0 * std::log10(accum[i] * inv + 1e-30));
            }
        }

        accum.fill(0.0);
        count = 0;

        // Advance the deadline BY the interval rather than resetting it to now.
        // Resetting quantizes the output rate down onto the input grid — frames
        // arrive on their own cadence, so "first frame at or after the deadline"
        // always rounds up by up to one input period, and a 33 ms target fed at
        // 25 ms intervals emits every 50 ms (20 fps, not 30). Measured, not
        // reasoned: that is exactly what the first cut of this did.
        if (!primed) {
            // The priming emit has no previous deadline to advance from.
            primed = true;
            lastEmitNs = nowNs;
        } else if (dueNs > 0) {
            lastEmitNs += dueNs;
            // Catch-up has a limit. After a stall (a rate change rebuilding the
            // DSP, the app backgrounded) the deadline can sit arbitrarily far in
            // the past, and letting it chase would emit a burst of frames as
            // fast as they arrive — the exact render spike this shaping exists
            // to prevent. Give up the backlog beyond one interval.
            if (nowNs - lastEmitNs > dueNs)
                lastEmitNs = nowNs - dueNs;
        } else {
            lastEmitNs = nowNs;
        }
        return true;
    }
};

}  // namespace AetherSDR
