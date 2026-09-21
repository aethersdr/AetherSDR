#pragma once

#include "core/TxCoordinator.h"

#include <QStringView>

namespace AetherSDR {

// Candidate LAN software-PTT evidence path, NOT backend qualification by itself.
// See docs/aetherd-flex-ptt-stop-evidence.md. No socket, timer, command generation
// or coordinator acknowledgment lives here. The eventual transport adapter must
// supply one ordered stream of actual writes and raw replies/status, stamped at
// the transport (not when a delayed model/GUI notification is delivered).
class FlexPttStopTracker final {
public:
    struct Stamp {
        quint64 session{0};
        quint64 ordinal{0};
    };
    enum class Phase {
        Disconnected, AwaitIdle, Idle, AwaitKeyWrite, AwaitKeyReply,
        AwaitPttRequested, AwaitTransmitting, Transmitting, AwaitStopWrite,
        AwaitStopReply, AwaitUnkey, AwaitReady, AwaitOwnerClear, Confirmed, Failed
    };
    enum class Failure {
        None, InvalidInput, Ordering, Timeout, Write, Reply, State,
        Ownership, Identity, UnsupportedActivity
    };
    static constexpr qint64 kTransitionTimeoutMs = 5000;
    static constexpr qsizetype kMaximumLineSize = 4096;

    FlexPttStopTracker() = default;
    FlexPttStopTracker(const FlexPttStopTracker&) = delete;
    FlexPttStopTracker& operator=(const FlexPttStopTracker&) = delete;

    // Strictly increasing, nonzero transport session identity, never reused.
    // Reconnect clears all observations and pending evidence. Handle zero is
    // not a client identity. Calls and inspection belong to the owning thread.
    void reset(quint64 session, quint32 clientHandle);
    void disconnect();
    [[nodiscard]] bool begin(const TxCoordinator::Operation& operation,
                             quint32 keySequence, qint64 nowMs);
    [[nodiscard]] bool requestStop(const TxCoordinator::Operation& operation,
                                   const TxCoordinator::StopRequest& request,
                                   quint32 stopSequence, qint64 nowMs);
    // Only a full terminal write of the exact PTT command may report success.
    // Sequence zero is reserved. Unexpected TX writes poison this attempt;
    // ordinary non-TX commands are not inputs to this method.
    void commandWritten(Stamp stamp, quint32 sequence, bool keying,
                        bool fullWrite, qint64 nowMs);
    void observe(Stamp stamp, QStringView rawLine, qint64 nowMs);
    void poll(qint64 nowMs);

    [[nodiscard]] Phase phase() const { return m_phase; }
    [[nodiscard]] Failure failure() const { return m_failure; }
    [[nodiscard]] qint64 evidenceDeadlineMs() const { return m_deadlineMs; }
    // Still only a candidate certificate. The composition must independently
    // establish protocol/transport qualification and retain the coordinator's
    // entered-writer barrier. No caller may manufacture an acknowledgment by
    // attaching a new token to an already-observed READY.
    [[nodiscard]] TxCoordinator::StopRequest evidence(qint64 nowMs) const;

private:
    [[nodiscard]] bool onThread() const;
    [[nodiscard]] bool accept(Stamp stamp, qint64 nowMs);
    [[nodiscard]] bool advanceClock(qint64 nowMs);
    void fail(Failure failure);
    void interlock(QStringView body);
    void response(QStringView body);

    QThread* const m_thread{QThread::currentThread()};
    quint64 m_session{0};
    quint64 m_highestSession{0};
    quint64 m_lastOrdinal{0};
    quint32 m_clientHandle{0};
    quint32 m_keySequence{0};
    quint32 m_stopSequence{0};
    qint64 m_lastMs{0};
    qint64 m_deadlineMs{0};
    Phase m_phase{Phase::Disconnected};
    Failure m_failure{Failure::None};
    TxCoordinator::Operation m_operation;
    TxCoordinator::StopRequest m_stop;
    bool m_stopWritten{false};
    bool m_stopReplied{false};
};

} // namespace AetherSDR
