#pragma once

// GUI event-loop tick lag for the Overview tab (#2554): how late each 50 ms
// perf-heartbeat tick fired, accumulated between reads.
//
// Fed by MainWindow's existing heartbeat lambda (one call per tick) and read
// by SystemInfoDialog on the GUI thread whenever a CPU sample arrives, so it
// needs no lock: producer and consumer are the same thread. It lives here, not
// in PerfTelemetry, because PerfTelemetry records nothing unless aether.perf
// debug logging is switched on (PerfTelemetry::enabled() is
// lcPerf().isDebugEnabled(); the category defaults to warnings) — a card that
// read "—" for every user who had not enabled a log category would be a
// diagnostic of the log settings, not of the event loop.
//
// The reading is the issue's own definition for the Overview's fourth chart:
// "actual_interval - nominal_interval for the perf heartbeat timer". Nothing
// here claims to be a frame rate; the heartbeat is a 20 Hz timer, and calling
// its cadence a frame rate would put a healthy app in the issue's "yellow"
// band by construction (see the plan, §12.2).
//
// Header-only and Qt-Core-only so the arithmetic is testable with constructed
// timestamps and no timer: tickAt(nowNs) is the seam, tick() the production
// wrapper around a steady clock.

#include <QElapsedTimer>
#include <QtGlobal>

#include <algorithm>

namespace AetherSDR {

class UiTickLagMeter {
public:
    // MainWindow's m_perfHeartbeatTimer interval. Repeated here rather than
    // read from MainWindow so this header stays free of gui/core includes;
    // MainWindow.cpp static_asserts the two agree where it starts the timer.
    static constexpr double kNominalIntervalMs = 50.0;

    struct Reading {
        int    tickCount{0};      // ticks since the previous take(); 0 = nothing measured
        double spanMs{0.0};       // wall time the ticks cover
        double lagMaxMs{0.0};     // worst (actual - nominal), floored at 0
        double lagMeanMs{0.0};    // mean of the same
    };

    // Production entry point: called on every heartbeat tick, GUI thread.
    void tick()
    {
        if (!m_clock.isValid()) {
            m_clock.start();
        }
        tickAt(m_clock.nsecsElapsed());
    }

    // The arithmetic, with the clock injected. The first tick after
    // construction or take() establishes the baseline and records no lag: a
    // gap measured against "when was the meter created" is not a tick interval.
    void tickAt(qint64 nowNs)
    {
        if (m_lastTickNs >= 0) {
            const double gapMs = static_cast<double>(nowNs - m_lastTickNs) / 1e6;
            const double lagMs = std::max(0.0, gapMs - kNominalIntervalMs);
            m_lagMaxMs = std::max(m_lagMaxMs, lagMs);
            m_lagSumMs += lagMs;
            m_spanMs += gapMs;
            ++m_tickCount;
        }
        m_lastTickNs = nowNs;
    }

    // Hand back everything since the previous take() and start over. The
    // baseline tick is kept: the next interval is measured from the last tick
    // seen, not from the moment of the read.
    Reading take()
    {
        Reading reading;
        reading.tickCount = m_tickCount;
        reading.spanMs = m_spanMs;
        reading.lagMaxMs = m_lagMaxMs;
        reading.lagMeanMs = m_tickCount > 0 ? m_lagSumMs / m_tickCount : 0.0;
        m_tickCount = 0;
        m_spanMs = 0.0;
        m_lagMaxMs = 0.0;
        m_lagSumMs = 0.0;
        return reading;
    }

    // Forget the accumulation AND the baseline: the next tick establishes a
    // fresh one. What a consumer calls when it starts reading after a period in
    // which nobody was taking — the dialog opening after an hour closed — so
    // the first reading it takes does not describe that hour.
    void reset()
    {
        take();
        m_lastTickNs = -1;
    }

private:
    QElapsedTimer m_clock;
    qint64 m_lastTickNs{-1};
    int    m_tickCount{0};
    double m_spanMs{0.0};
    double m_lagMaxMs{0.0};
    double m_lagSumMs{0.0};
};

}  // namespace AetherSDR
