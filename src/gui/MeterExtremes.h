#pragma once

#include "SmartMtrStyle.h"

#include <QtGlobal>

#include <cmath>
#include <deque>
#include <functional>

namespace AetherSDR {

// Sliding-window min/max envelope tracker for the SmartMTR "extremes" markers.
//
// Sits alongside the bar's MeterSmoother but moves differently on purpose: the
// bar uses an asymmetric exponential ballistic (the analog-meter feel), while the
// extremes glide at a CONSTANT linear slew over the recent-signal envelope, so
// they read as separate sweep markers rather than a second needle.
//
// History is kept in RAW signal units (dBm for RX, dBFS for TX), not in scale
// UNITS: the fade rules are dB-based and the dBm->UNIT mapping is piecewise
// non-linear, so min/max/avg are computed in raw space and mapped to UNITS only
// for drawing. The slewed display positions live in scale UNITS.
//
// Drive it once per physics tick (any rate, even irregular) from the same timer
// that ticks the bar; pass the monotonic clock and the elapsed dt.
class MeterExtremes {
public:
    struct Tuning {
        double windowSeconds = SmartMtrExtremes::kWindowMediumSec;
        double slewUnitsPerSec = SmartMtrExtremes::kSlewUnitsPerSec;
    };

    void setTuning(const Tuning& t) { m_tuning = t; }
    const Tuning& tuning() const { return m_tuning; }

    // Clear the window and snap both markers down to the floor. Used on a kind
    // switch (dBm vs dBFS must not mix) or a hard park.
    void reset()
    {
        m_window.clear();
        m_sumRaw = 0.0;
        m_minRaw = 0.0;
        m_maxRaw = 0.0;
        m_minPos = SmartMtrUnits::kScaleMin;
        m_maxPos = SmartMtrUnits::kScaleMin;
        m_hasData = false;
    }

    // Record a raw signal sample at a monotonic timestamp (ms). Call from the
    // signal source (after clamping to the floor), only while a value is present.
    void record(double rawValue, qint64 nowMs)
    {
        m_window.push_back({nowMs, rawValue});
        m_sumRaw += rawValue;
        m_hasData = true;
    }

    // Advance one tick. Prunes expired samples, recomputes the raw window
    // min/max/avg, then slews both display markers toward their (mapped, clamped)
    // targets at constant velocity. needlePosUnits is the live bar position, used
    // for the min <= needle <= max clamp. mapToUnits maps a raw value to a
    // clamped UNIT position. Returns true while further ticks are useful (a marker
    // still moving, or window samples that may yet expire and shift the targets).
    bool tick(qint64 nowMs, qint64 elapsedMs, double needlePosUnits,
              const std::function<double(double)>& mapToUnits)
    {
        // 1) Prune expired samples and rescan min/max in a single pass.
        const qint64 cutoff = nowMs - qint64(m_tuning.windowSeconds * 1000.0);
        while (!m_window.empty() && m_window.front().t < cutoff) {
            m_sumRaw -= m_window.front().raw;
            m_window.pop_front();
        }
        if (m_window.empty()) {
            m_hasData = false;
        } else {
            double mn = m_window.front().raw, mx = mn;
            for (const Sample& s : m_window) {
                if (s.raw < mn) mn = s.raw;
                if (s.raw > mx) mx = s.raw;
            }
            m_minRaw = mn;
            m_maxRaw = mx;
        }

        // 2) Targets in UNITS. With no data left, glide both to the floor.
        const double minTgt =
            m_hasData ? mapToUnits(m_minRaw) : SmartMtrUnits::kScaleMin;
        const double maxTgt =
            m_hasData ? mapToUnits(m_maxRaw) : SmartMtrUnits::kScaleMin;

        // 3) Constant-velocity slew toward each target.
        bool moving = false;
        const double step = m_tuning.slewUnitsPerSec * double(elapsedMs) / 1000.0;
        if (step > 0.0) {
            m_minPos = slew(m_minPos, minTgt, step, moving);
            m_maxPos = slew(m_maxPos, maxTgt, step, moving);
        }

        // 4) Ordering / floor clamps: min <= needle <= max, nothing below floor.
        if (m_maxPos < needlePosUnits) m_maxPos = needlePosUnits;
        if (m_minPos > needlePosUnits) m_minPos = needlePosUnits;
        if (m_minPos < SmartMtrUnits::kScaleMin) m_minPos = SmartMtrUnits::kScaleMin;
        if (m_maxPos < SmartMtrUnits::kScaleMin) m_maxPos = SmartMtrUnits::kScaleMin;
        if (m_minPos > m_maxPos) m_minPos = m_maxPos;

        // Keep animating while a marker is mid-slew OR has not yet collapsed onto
        // the needle (a peak/trough is still standing off it). The latter keeps
        // the timer running through the whole hold->return so the window prunes
        // and the markers slew at the timer rate — matching the original app's
        // steady loop and avoiding a staircase clocked by the irregular packet
        // feed. It self-terminates once min ~= max ~= needle (markers collapsed),
        // so an idle meter still settles rather than pinning the repaint timer at
        // full rate forever (each meter repaint over the GPU panadapter forces a
        // costly recomposite that starves input).
        const bool standingOff = (m_maxPos > needlePosUnits + kConvergeEps)
                                 || (m_minPos < needlePosUnits - kConvergeEps);
        return moving || standingOff;
    }

    bool hasData() const { return m_hasData; }
    double minRaw() const { return m_minRaw; }
    double maxRaw() const { return m_maxRaw; }
    double avgRaw() const
    {
        return m_window.empty() ? 0.0 : m_sumRaw / double(m_window.size());
    }
    double minPosUnits() const { return m_minPos; }
    double maxPosUnits() const { return m_maxPos; }

private:
    struct Sample {
        qint64 t;
        double raw;
    };

    // Move cur toward tgt by at most step (constant velocity). Snaps within a
    // convergence epsilon so the timer doesn't live forever on sub-pixel motion.
    static double slew(double cur, double tgt, double step, bool& moving)
    {
        const double d = tgt - cur;
        if (std::fabs(d) <= kConvergeEps)
            return tgt;
        moving = true;
        if (d > step) return cur + step;
        if (d < -step) return cur - step;
        return tgt; // reaches target this step
    }

    static constexpr double kConvergeEps = 0.05; // UNITS

    std::deque<Sample> m_window;
    Tuning m_tuning;
    double m_sumRaw = 0.0;
    double m_minRaw = 0.0;
    double m_maxRaw = 0.0;
    double m_minPos = SmartMtrUnits::kScaleMin;
    double m_maxPos = SmartMtrUnits::kScaleMin;
    bool m_hasData = false;
};

} // namespace AetherSDR
