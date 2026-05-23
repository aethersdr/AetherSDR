#pragma once

#include <QtGlobal>
#include <cmath>

namespace MasterSDR {

// Shared asymmetric attack / release envelope follower for UI meter
// bars.  Every metering surface in the app (HGauge, ClientCompMeter,
// ClientCompApplet's GR strip, any future meter) runs the same
// motion so the interface reads as one consistent instrument rather
// than a grab-bag of different bar behaviours.
//
// "MeterSmoother ballistics" — 30 ms attack, 180 ms release, polled
// at ~120 Hz.  Fast enough to feel responsive on speech peaks; slow
// enough that the bar doesn't flicker on per-block noise.  Between a
// classic VU (300 ms integration) and a fast PPM (a few ms) — suited
// for ham-radio-style level / GR indication rather than broadcast
// loudness compliance.
//
// Value domain is normalised [0, 1]; callers map physical units
// (dBFS, dB of GR, etc.) to a fraction before calling setTarget().
//
// Usage (driven by a QTimer on the owning widget):
//
//     MeterSmoother m_smooth;
//     QTimer        m_timer;
//     QElapsedTimer m_clock;
//
//     // On new data:
//     m_smooth.setTarget(fracFromDb(...));
//     if (m_smooth.needsAnimation() && !m_timer.isActive()) {
//         m_clock.restart();
//         m_timer.start();
//     }
//
//     // In the timer callback:
//     if (!m_smooth.tick(m_clock.restart())) {
//         m_timer.stop();   // settled
//     }
//     update();             // repaint, read m_smooth.value()
class MeterSmoother {
public:
    // Ballistics are mutable per-instance so individual meters can
    // opt into different behaviour (e.g. a GR bar with a slower
    // release) while still reading off the same central name.
    struct Ballistics {
        float attackSeconds  = 0.030f;
        float releaseSeconds = 0.180f;
        float snapEpsilon    = 0.001f;
    };

    MeterSmoother() = default;
    explicit MeterSmoother(Ballistics b) : m_b(b) {}

    // Set the animation target.  Snaps immediately when already
    // within snapEpsilon of the target, so small-delta moves don't
    // trigger a pointless animation frame.
    void setTarget(float target)
    {
        m_target = target;
        if (std::fabs(m_target - m_display) <= m_b.snapEpsilon)
            m_display = m_target;
    }
    float target() const { return m_target; }

    // Current display value [0, 1].  Read on every paint.
    float value() const { return m_display; }

    // Snap to the current target with no animation (e.g. mode reset).
    void snapToTarget() { m_display = m_target; }

    // True when the bar is still catching up to the target.  Caller
    // uses this to decide whether to keep its animation timer running.
    bool needsAnimation() const
    {
        return std::fabs(m_target - m_display) > m_b.snapEpsilon;
    }

    // Advance the smoother by wall-clock milliseconds.  Returns true
    // while still animating, false once the display has reached the
    // target — caller can stop its driving timer when it returns false.
    bool tick(qint64 elapsedMs)
    {
        if (elapsedMs <= 0) return needsAnimation();
        const float delta = m_target - m_display;
        if (std::fabs(delta) <= m_b.snapEpsilon) {
            m_display = m_target;
            return false;
        }
        const float tau = (delta >= 0.0f) ? m_b.attackSeconds
                                          : m_b.releaseSeconds;
        const float alpha = 1.0f - std::exp(
            -static_cast<float>(elapsedMs) / 1000.0f / tau);
        m_display += delta * alpha;
        return true;
    }

    void setBallistics(const Ballistics& b) { m_b = b; }
    const Ballistics& ballistics() const { return m_b; }

private:
    Ballistics m_b;
    float      m_display{0.0f};
    float      m_target{0.0f};
};

// Recommended driving-timer interval for a MeterSmoother.  8 ms ≈
// 120 Hz — tight enough that the attack time constant resolves
// smoothly, cheap enough to run on every active meter simultaneously.
constexpr int kMeterSmootherIntervalMs = 8;

} // namespace MasterSDR
