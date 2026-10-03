#pragma once

#include <QtGlobal>
#include <QElapsedTimer>
#include <cmath>

namespace AetherSDR {

// Shared asymmetric attack/release follower for every UI meter bar (HGauge,
// ClientCompMeter, GR strips) so all meters move alike: 30 ms attack, 180 ms
// release, polled ~120 Hz. Values are normalised [0, 1]. Drive from a QTimer:
// setTarget() on new data and start the timer if needsAnimation(); in the
// callback, stop when tick(elapsed) returns false, then repaint with value().
class MeterSmoother {
public:
    // Ballistics are mutable per-instance so individual meters can
    // opt into different behaviour (e.g. a GR bar with a slower
    // release) while still reading off the same central name.
    struct Ballistics {
        // SmartMTR's analog ballistics are project canon: a fast attack and
        // a ~15x slower, lazy decay giving the d'Arsonval "jumps up, sags
        // down" envelope-follower feel. Ported from the SmartMTR macOS app;
        // tau from per-tick fractions k=0.60/0.06 at 60 Hz,
        // tau = -(1/60)/ln(1-k). Every meter inherits these by doing nothing;
        // override only with a reason, and say what it is.
        float attackSeconds  = 0.01818f;   // ~18.2 ms
        float releaseSeconds = 0.26940f;   // ~269 ms
        float snapEpsilon    = 0.0005f;
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
    Ballistics     m_b;
    float          m_display{0.0f};
    float          m_target{0.0f};
};

// Recommended driving-timer interval for a MeterSmoother.  8 ms ≈
// 120 Hz — tight enough that the attack time constant resolves
// smoothly, cheap enough to run on every active meter simultaneously.
constexpr int kMeterSmootherIntervalMs = 8;

// Recommended cadence for *numeric* meter readouts (dB labels next to
// or beneath a bar meter).  100 ms = 10 Hz is the eye-readable sweet
// spot — fast enough to feel live as a knob is turned, slow enough
// that digits actually settle.  Use this for QLabel::setText (or the
// cached drawText pattern in ClientCompMeter) so every numeric meter
// readout across the app updates in lockstep.  The bar fill itself
// should continue to animate at kMeterSmootherIntervalMs.
constexpr int kMeterReadoutUpdateMs = 100;

} // namespace AetherSDR
