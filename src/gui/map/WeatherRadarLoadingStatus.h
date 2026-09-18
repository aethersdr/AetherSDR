#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Presentation only: hiding a notification must never cancel its downloads or
// retries. One failure notice per loading episode avoids flashing a new badge
// on every background retry. Live imagery retains the notice because its scan
// age is unknown. A fully successful idle state rearms it.
class WeatherRadarLoadingStatus {
public:
    enum class State { Hidden, Loading, Failed };

    State update(qint64 nowMs, bool busy, bool failed, int pending, int ready,
                 bool retainFailure = false)
    {
        if (!busy && !failed) {
            reset();
            return State::Hidden;
        }
        if (m_startedMs < 0) {
            m_startedMs = nowMs;
            m_progressMs = nowMs;
        }
        if (pending != m_pending || ready != m_ready) {
            m_progressMs = nowMs;
            m_pending = pending;
            m_ready = ready;
        }
        // Slow but progressing batches are not failures. Count completed work,
        // not total batch age; the network also has its own inactivity timeout.
        if (m_failedMs < 0 && (failed || nowMs - m_progressMs >= 15000)) {
            m_failedMs = nowMs;
        }
        if (m_failedMs >= 0) {
            return retainFailure || nowMs - m_failedMs < 3000
                ? State::Failed : State::Hidden;
        }
        return nowMs - m_startedMs < 300 ? State::Hidden : State::Loading;
    }

    void reset() { *this = WeatherRadarLoadingStatus{}; }

private:
    qint64 m_startedMs{-1};
    qint64 m_progressMs{-1};
    qint64 m_failedMs{-1};
    int m_pending{-1};
    int m_ready{-1};
};

} // namespace AetherSDR
