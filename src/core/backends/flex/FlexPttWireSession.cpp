#include "FlexPttWireSession.h"

#include <limits>
#include <QVersionNumber>

namespace AetherSDR {

FlexPttWireSession::FlexPttWireSession(Writer writer, EvidenceSink sink)
    : m_writer(std::move(writer)), m_sink(std::move(sink))
{
}

FlexPttWireSession::~FlexPttWireSession()
{
    invalidateEvidence();
}

bool FlexPttWireSession::supportsProtocol(QStringView version)
{
    // SmartSDR's TCP prologue identifies the interface independently of the
    // firmware. Its last two components are explicitly not compatibility
    // selectors. The source-backed contract here is API 1.4, across models;
    // another major/minor needs protocol review, not a per-radio test allowlist.
    if (version.size() > 32) {
        return false;
    }
    qsizetype suffix = 0;
    const QVersionNumber parsed = QVersionNumber::fromString(version, &suffix);
    return suffix == version.size() && parsed.segmentCount() == 4
        && parsed.majorVersion() == 1 && parsed.minorVersion() == 4
        && parsed.toString() == version;
}

void FlexPttWireSession::rejectProtocol()
{
    m_ready.store(false, std::memory_order_release);
    invalidateEvidence();
    m_tracker.disconnect();
    // Retain m_session/m_operation/m_enteredKeyWrite: an already-entered key
    // still needs authorized unkey. Loss of qualification is not a teardown.
}

void FlexPttWireSession::invalidateEvidence()
{
    if (m_evidence) {
        m_evidence->store(false, std::memory_order_release);
        m_evidence.reset();
    }
}

void FlexPttWireSession::disconnect()
{
    m_ready.store(false, std::memory_order_release);
    invalidateEvidence();
    m_tracker.disconnect();
    m_session = 0;
    m_ordinal = 0;
    m_operation = {};
    m_enteredKeyWrite = false;
    m_localStop = {};
}

void FlexPttWireSession::reset(quint64 session, quint32 handle)
{
    disconnect();
    m_tracker.reset(session, handle);
    if (m_tracker.phase() != FlexPttStopTracker::Phase::Disconnected) {
        m_session = session;
    }
}

FlexPttStopTracker::Stamp FlexPttWireSession::stamp()
{
    if (m_ordinal == std::numeric_limits<quint64>::max()) {
        disconnect();
        return {};
    }
    return {m_session, ++m_ordinal};
}

void FlexPttWireSession::publish(qint64 now)
{
    using Phase = FlexPttStopTracker::Phase;
    m_ready.store(m_tracker.phase() == Phase::Idle || m_tracker.phase() == Phase::Confirmed,
                  std::memory_order_release);
    const TxCoordinator::StopRequest proof = m_tracker.evidence(now);
    if (!proof.valid()) {
        if (m_localStop.matchesOperation(m_operation) && !m_enteredKeyWrite) {
            return;
        }
        m_localStop = {};
        invalidateEvidence();
        return;
    }
    if (!m_evidence) {
        m_evidence = std::make_shared<std::atomic<bool>>(true);
        m_sink({proof, m_evidence, m_tracker.evidenceDeadlineMs()});
    }
}

void FlexPttWireSession::key(quint32 sequence, const TxCoordinator::Command& command, qint64 now)
{
    if (!command.operation.independent() || !command.keying || m_session == 0) {
        command.completion.finish();
        return;
    }
    // Remember even a canceled queued on: the following stop can distinguish
    // that exact operation from an earlier write which might have reached RF.
    if (!m_operation.sameOperation(command.operation)) {
        if (m_operation.permitsCleanup()) {
            command.completion.finish();
            return;
        }
        invalidateEvidence();
        m_operation = command.operation;
        m_enteredKeyWrite = false;
        m_localStop = {};
    }
    const TxCoordinator::Dispatch dispatch = command.beginDispatch(now);
    if (dispatch && m_tracker.begin(command.operation, sequence, now)) {
        m_ready.store(false, std::memory_order_release);
        m_enteredKeyWrite = true; // BEFORE calling the actual writer, including partial failure
        const bool written = m_writer(sequence, QStringLiteral("xmit 1"));
        m_tracker.commandWritten(stamp(), sequence, true, written, now);
    }
    command.completion.finish();
    publish(now);
}

void FlexPttWireSession::stop(quint32 sequence, const TxCoordinator::Operation& operation,
                             const TxCoordinator::StopRequest& request, qint64 now)
{
    if (m_session == 0 || !operation.independent() || !request.matchesOperation(operation)) {
        return;
    }
    if (!m_operation.sameOperation(operation)) {
        // No queued key for this operation has been sent to this transport.
        // The backend posts key/stop in order on this one worker queue.
        if (m_operation.permitsCleanup()) {
            return;
        }
        m_operation = operation;
        m_enteredKeyWrite = false;
    }
    if (!m_enteredKeyWrite) {
        invalidateEvidence();
        m_localStop = request;
        m_evidence = std::make_shared<std::atomic<bool>>(true);
        m_sink({request, m_evidence}); // local no-dispatch proof, NOT observed radio idle
        return;
    }
    const bool tracked = m_tracker.requestStop(operation, request, sequence, now);
    const TxCoordinator::Dispatch dispatch = operation.beginDispatch(now, false);
    if (dispatch) {
        const bool written = m_writer(sequence, QStringLiteral("xmit 0"));
        if (tracked) {
            m_tracker.commandWritten(stamp(), sequence, false, written, now);
        }
    }
    // Always attempt authorized unkey, even if evidence has already failed.
    // Never turn that write, or its local completion, into an idle claim.
    publish(now);
}

void FlexPttWireSession::observe(QStringView line, qint64 now)
{
    m_tracker.observe(stamp(), line, now);
    publish(now);
}

void FlexPttWireSession::otherCommand(const QString& command, qint64 now)
{
    if (command.startsWith(QLatin1String("xmit "))
        || command.startsWith(QLatin1String("transmit tune "))
        || command.startsWith(QLatin1String("atu "))
        || command.startsWith(QLatin1String("cwx "))) {
        // A desktop/legacy keying writer cannot contribute to this proof.
        // Do not disturb ordinary desktop operation when no grant is active.
        if (m_operation.permitsCleanup() && m_enteredKeyWrite) {
            m_tracker.commandWritten(stamp(), 0, true, false, now);
            publish(now);
        }
    }
}

void FlexPttWireSession::poll(qint64 now)
{
    m_tracker.poll(now);
    publish(now);
}

bool FlexPttWireSession::needsPolling() const
{
    using Phase = FlexPttStopTracker::Phase;
    const Phase phase = m_tracker.phase();
    return phase >= Phase::AwaitKeyWrite && phase <= Phase::Confirmed && phase != Phase::Transmitting;
}

} // namespace AetherSDR
