#pragma once

#include <QtGlobal>
#include <QElapsedTimer>
#include <cmath>

namespace AetherSDR {

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

    // ── Lean-mode per-widget repaint gate (#3283) ──────────────────────────
    // Every meter animates its MeterSmoother at ~120 Hz; each tick repaints,
    // and on a window that also hosts the GPU panadapter every repaint forces a
    // full-window backing-store→texture re-upload (the dominant pooled cost on
    // large/5K displays). Lean mode keeps the smoother integrating at the full
    // rate (so ballistics stay smooth) but gates the *repaint* to ~kLeanRepaintHz
    // per widget.
    //
    // The gate is per-instance (each MeterSmoother carries its own clock),
    // because every active meter owns its own MeterSmoother. A previous
    // shared-static version of this gate underdelivered on its own claim: with
    // N meters racing into the gate, the first to tick each ~83 ms window got
    // through and the other N−1 were starved that window, so each individual
    // meter only saw the green light at roughly (kLeanRepaintHz / N) Hz —
    // visibly stuttery on GR bars and S-meter needles. Per-instance gates let
    // every meter independently hit the target rate.
    //
    // Usage at the meter's timer callback:
    //     const bool settled = !m_smooth.tick(elapsed);
    //     if (settled) m_timer.stop();
    //     if (settled || m_smooth.shouldRepaint()) update();
    // (Always paint the settled frame so the final value is never dropped.)
    static void setLeanThrottle(bool on) { s_leanThrottle = on; }
    static bool leanThrottle() { return s_leanThrottle; }
    bool shouldRepaint()
    {
        if (!s_leanThrottle) {
            return true;
        }
        constexpr qint64 kGateMs = 1000 / kLeanRepaintHz;
        if (!m_gate.isValid()) {
            m_gate.start();
        }
        const qint64 now = m_gate.elapsed();
        if (m_lastMs < 0 || now - m_lastMs >= kGateMs) {
            m_lastMs = now;
            return true;
        }
        return false;
    }

private:
    static constexpr int kLeanRepaintHz = 12;  // gated meter repaint rate in lean
    static inline bool s_leanThrottle = false;

    Ballistics     m_b;
    float          m_display{0.0f};
    float          m_target{0.0f};
    QElapsedTimer  m_gate;
    qint64         m_lastMs{-1};
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
