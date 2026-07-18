#pragma once

#include <QtGlobal>

#include <algorithm>

namespace AetherSDR {

// Hardware-free state machine for TX capture observability. QAudioSource starts
// in Idle on some backends, so Idle alone is not a fault. PipeWire can also keep
// reporting Active after its pull device fills, so capacity exhaustion is the
// primary signature; Active -> Idle with unread bytes remains a fallback when a
// backend does not report a useful capacity. Keeping this policy separate makes
// the field diagnostic testable without an audio device or PipeWire server.
class TxCaptureHealthTracker {
public:
    enum class CaptureState {
        Stopped,
        Active,
        Idle,
        Suspended,
    };

    enum class Event {
        None,
        BufferSaturatedDuringTci,
        LocalTxWhileSaturated,
    };

    struct Snapshot {
        qint64 lifecycleMs{0};
        qint64 lastMicReadAgeMs{-1};
        quint64 tciSuppressedCallbacks{0};
        qint64 suppressedBufferPeakBytes{0};
        quint64 fullBufferDuringTciObservations{0};
        quint64 idleDuringTciTransitions{0};
        quint64 postTciLocalTxWhileSaturated{0};
        bool sourceWasActive{false};
        bool saturationObserved{false};
    };

    void reset(CaptureState initialState, qint64 nowMs = 0)
    {
        m_startedAtMs = nowMs;
        m_lastMicReadMs = -1;
        m_lastState = initialState;
        m_sourceWasActive = initialState == CaptureState::Active;
        m_currentlySaturated = false;
        m_saturationReported = false;
        m_stalledLocalTxReported = false;
        m_tciSuppressedCallbacks = 0;
        m_suppressedBufferPeakBytes = 0;
        m_fullBufferDuringTciObservations = 0;
        m_idleDuringTciTransitions = 0;
        m_postTciLocalTxWhileSaturated = 0;
    }

    Event recordSuppressedCallback(qint64 bufferedBytes, qint64 bufferCapacityBytes)
    {
        ++m_tciSuppressedCallbacks;
        m_suppressedBufferPeakBytes = std::max(m_suppressedBufferPeakBytes,
                                                std::max<qint64>(0, bufferedBytes));

        if (!bufferIsFull(bufferedBytes, bufferCapacityBytes)) {
            return Event::None;
        }

        ++m_fullBufferDuringTciObservations;
        m_currentlySaturated = true;
        if (m_saturationReported) {
            return Event::None;
        }
        m_saturationReported = true;
        return Event::BufferSaturatedDuringTci;
    }

    void recordMicRead(qint64 nowMs)
    {
        m_lastMicReadMs = nowMs;
        // A successful consume proves the pull device is making progress again.
        // Preserve the lifecycle-level observation/rate limit, but do not use a
        // historical saturation to classify later healthy TX attempts.
        m_currentlySaturated = false;
    }

    Event observeState(CaptureState state, bool tciAudioFresh, qint64 bufferedBytes)
    {
        const bool changed = state != m_lastState;
        m_lastState = state;

        if (state == CaptureState::Active) {
            m_sourceWasActive = true;
        }

        const bool saturatedDuringTci = changed
            && state == CaptureState::Idle
            && m_sourceWasActive
            && tciAudioFresh
            && m_tciSuppressedCallbacks > 0
            && bufferedBytes > 0;
        if (!saturatedDuringTci) {
            return Event::None;
        }

        ++m_idleDuringTciTransitions;
        m_currentlySaturated = true;
        if (m_saturationReported) {
            return Event::None;
        }
        m_saturationReported = true;
        return Event::BufferSaturatedDuringTci;
    }

    Event recordLocalTxAttempt(CaptureState state,
                               bool localTxOwned,
                               bool daxTxMode,
                               bool tciAudioFresh,
                               qint64 bufferedBytes,
                               qint64 bufferCapacityBytes)
    {
        const bool sourceRunning = state == CaptureState::Active
            || state == CaptureState::Idle;
        const bool saturated = m_currentlySaturated
            || (m_tciSuppressedCallbacks > 0
                && bufferIsFull(bufferedBytes, bufferCapacityBytes));
        const bool stalled = localTxOwned
            && !daxTxMode
            && !tciAudioFresh
            && sourceRunning
            && m_sourceWasActive
            && saturated
            && bufferedBytes > 0;
        if (!stalled) {
            return Event::None;
        }

        m_currentlySaturated = true;
        m_saturationReported = true;
        ++m_postTciLocalTxWhileSaturated;
        if (m_stalledLocalTxReported) {
            return Event::None;
        }
        m_stalledLocalTxReported = true;
        return Event::LocalTxWhileSaturated;
    }

    Snapshot snapshot(qint64 nowMs) const
    {
        Snapshot out;
        out.lifecycleMs = std::max<qint64>(0, nowMs - m_startedAtMs);
        out.lastMicReadAgeMs = m_lastMicReadMs >= 0
            ? std::max<qint64>(0, nowMs - m_lastMicReadMs)
            : -1;
        out.tciSuppressedCallbacks = m_tciSuppressedCallbacks;
        out.suppressedBufferPeakBytes = m_suppressedBufferPeakBytes;
        out.fullBufferDuringTciObservations = m_fullBufferDuringTciObservations;
        out.idleDuringTciTransitions = m_idleDuringTciTransitions;
        out.postTciLocalTxWhileSaturated = m_postTciLocalTxWhileSaturated;
        out.sourceWasActive = m_sourceWasActive;
        out.saturationObserved = m_saturationReported;
        return out;
    }

private:
    static bool bufferIsFull(qint64 bufferedBytes, qint64 bufferCapacityBytes)
    {
        return bufferCapacityBytes > 0 && bufferedBytes >= bufferCapacityBytes;
    }

    qint64 m_startedAtMs{0};
    qint64 m_lastMicReadMs{-1};
    CaptureState m_lastState{CaptureState::Stopped};
    bool m_sourceWasActive{false};
    bool m_currentlySaturated{false};
    bool m_saturationReported{false};
    bool m_stalledLocalTxReported{false};
    quint64 m_tciSuppressedCallbacks{0};
    qint64 m_suppressedBufferPeakBytes{0};
    quint64 m_fullBufferDuringTciObservations{0};
    quint64 m_idleDuringTciTransitions{0};
    quint64 m_postTciLocalTxWhileSaturated{0};
};

} // namespace AetherSDR
