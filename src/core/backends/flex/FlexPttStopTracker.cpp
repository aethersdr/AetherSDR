#include "FlexPttStopTracker.h"

#include <limits>

namespace AetherSDR {
namespace {

// The general presentation parser intentionally tolerates incomplete input.
// Evidence must not turn a missing/invalid result code or handle into zero.
bool number(QStringView text, int base, quint32& result)
{
    if (text.isEmpty() || text.size() > 10) {
        return false;
    }
    quint64 value = 0;
    for (const QChar ch : text) {
        const ushort c = ch.unicode();
        const int digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0 || digit >= base) {
            return false;
        }
        value = value * base + static_cast<unsigned>(digit);
        if (value > std::numeric_limits<quint32>::max()) {
            return false;
        }
    }
    result = static_cast<quint32>(value);
    return true;
}

} // namespace

bool FlexPttStopTracker::onThread() const
{
    return QThread::currentThread() == m_thread;
}

void FlexPttStopTracker::disconnect()
{
    if (!onThread()) {
        return;
    }
    m_session = 0;
    m_clientHandle = 0;
    m_lastOrdinal = 0;
    m_keySequence = 0;
    m_stopSequence = 0;
    m_lastMs = 0;
    m_deadlineMs = 0;
    m_operation = {};
    m_stop = {};
    m_stopWritten = false;
    m_stopReplied = false;
    m_phase = Phase::Disconnected;
    m_failure = Failure::None;
}

void FlexPttStopTracker::reset(quint64 session, quint32 clientHandle)
{
    if (!onThread()) {
        return;
    }
    disconnect();
    if (session == 0 || session <= m_highestSession || clientHandle == 0) {
        return;
    }
    m_highestSession = session;
    m_session = session;
    m_clientHandle = clientHandle;
    m_phase = Phase::AwaitIdle;
}

void FlexPttStopTracker::fail(Failure failure)
{
    m_phase = Phase::Failed;
    m_failure = failure;
}

bool FlexPttStopTracker::advanceClock(qint64 nowMs)
{
    if (!onThread() || m_phase == Phase::Disconnected || m_phase == Phase::Failed) {
        return false;
    }
    if (m_phase == Phase::Confirmed && !m_stop.valid() && !m_operation.permitsCleanup()) {
        m_phase = Phase::Idle;
        m_deadlineMs = 0;
        m_operation = {};
        m_stop = {};
    }
    if (nowMs < m_lastMs || nowMs < 0
        || nowMs > std::numeric_limits<qint64>::max() - kTransitionTimeoutMs) {
        fail(Failure::InvalidInput);
        return false;
    }
    m_lastMs = nowMs;
    if (m_deadlineMs != 0 && nowMs >= m_deadlineMs) {
        fail(Failure::Timeout);
        return false;
    }
    return true;
}

bool FlexPttStopTracker::accept(Stamp stamp, qint64 nowMs)
{
    // A stale queued event from a previous connection is inert, not a failure
    // of the new session. A duplicate/reordered current-session event is not.
    if (!onThread() || stamp.session == 0 || stamp.session != m_session
        || !advanceClock(nowMs)) {
        return false;
    }
    if (stamp.ordinal == 0 || stamp.ordinal <= m_lastOrdinal) {
        fail(Failure::Ordering);
        return false;
    }
    m_lastOrdinal = stamp.ordinal;
    return true;
}

bool FlexPttStopTracker::begin(const TxCoordinator::Operation& operation,
                              quint32 keySequence, qint64 nowMs)
{
    if (!advanceClock(nowMs)) {
        return false;
    }
    if (m_phase != Phase::Idle || keySequence == 0
        || keySequence <= m_keySequence || keySequence <= m_stopSequence
        || !operation.permitsDispatch(nowMs)) {
        return false;
    }
    m_operation = operation;
    m_stop = {};
    m_stopWritten = false;
    m_stopReplied = false;
    m_keySequence = keySequence;
    m_stopSequence = 0;
    m_deadlineMs = nowMs + kTransitionTimeoutMs;
    m_phase = Phase::AwaitKeyWrite;
    return true;
}

bool FlexPttStopTracker::requestStop(const TxCoordinator::Operation& operation,
                                    const TxCoordinator::StopRequest& request,
                                    quint32 stopSequence, qint64 nowMs)
{
    if (!advanceClock(nowMs)) {
        return false;
    }
    if (!m_operation.sameOperation(operation) || !request.matchesOperation(operation)
        || stopSequence <= m_keySequence) {
        fail(Failure::Identity);
        return false;
    }
    if (m_stop.valid() || m_phase < Phase::AwaitKeyReply || m_phase > Phase::Transmitting) {
        fail(Failure::State);
        return false;
    }
    m_stop = request;
    m_stopSequence = stopSequence;
    m_deadlineMs = nowMs + kTransitionTimeoutMs;
    if (m_phase == Phase::Transmitting) {
        m_phase = Phase::AwaitStopWrite;
    }
    return true;
}

void FlexPttStopTracker::commandWritten(Stamp stamp, quint32 sequence, bool keying,
                                       bool fullWrite, qint64 nowMs)
{
    if (!accept(stamp, nowMs)) {
        return;
    }
    if (!fullWrite || sequence == 0) {
        fail(Failure::Write);
    } else if (!m_operation.permitsCleanup()) {
        fail(Failure::Identity);
    } else if (m_phase == Phase::AwaitKeyWrite && keying && sequence == m_keySequence) {
        m_phase = Phase::AwaitKeyReply;
    } else if (!m_stopWritten && !keying && sequence == m_stopSequence
               && m_stop.matchesOperation(m_operation)) {
        m_stopWritten = true;
        if (m_phase == Phase::AwaitStopWrite) {
            m_phase = Phase::AwaitStopReply;
        }
    } else {
        fail(Failure::UnsupportedActivity);
    }
}

void FlexPttStopTracker::response(QStringView body)
{
    const qsizetype pipe = body.indexOf(u'|');
    quint32 sequence = 0;
    if (pipe < 0 || !number(body.left(pipe), 10, sequence) || sequence == 0) {
        fail(Failure::Reply);
        return;
    }
    if (sequence != m_keySequence && sequence != m_stopSequence) {
        return;
    }
    const QStringView rest = body.mid(pipe + 1);
    const qsizetype resultEnd = rest.indexOf(u'|');
    quint32 result = 0;
    if (resultEnd < 0 || !number(rest.left(resultEnd), 16, result) || result != 0) {
        fail(Failure::Reply);
    } else if (sequence == m_keySequence && m_phase == Phase::AwaitKeyReply) {
        m_phase = Phase::AwaitPttRequested;
    } else if (sequence == m_stopSequence && m_stopWritten && !m_stopReplied
               && m_stop.matchesOperation(m_operation)) {
        m_stopReplied = true;
        if (m_phase == Phase::AwaitStopReply) {
            m_phase = Phase::AwaitUnkey;
        }
    } else {
        fail(Failure::Ordering);
    }
}

void FlexPttStopTracker::interlock(QStringView body)
{
    QStringView state;
    QStringView source;
    QStringView handleText;
    QStringView allowed;
    QStringView reason;
    unsigned present = 0;
    bool malformed = false;
    while (!body.isEmpty()) {
        const qsizetype space = body.indexOf(u' ');
        const QStringView token = space < 0 ? body : body.left(space);
        body = space < 0 ? QStringView{} : body.mid(space + 1);
        const qsizetype eq = token.indexOf(u'=');
        if (eq < 0) {
            malformed |= !token.isEmpty();
            continue;
        }
        const QStringView key = token.left(eq);
        const QStringView value = token.mid(eq + 1);
        unsigned bit = 0;
        if (key == u"state") {
            bit = 1;
            state = value;
        } else if (key == u"source") {
            bit = 2;
            source = value;
        } else if (key == u"tx_client_handle") {
            bit = 4;
            handleText = value;
        } else if (key == u"tx_allowed") {
            bit = 8;
            allowed = value;
        } else if (key == u"reason") {
            bit = 16;
            reason = value;
        }
        if (bit != 0 && (present & bit) != 0) {
            fail(Failure::InvalidInput);
            return;
        }
        present |= bit;
    }
    if (present == 0) {
        return; // unrelated interlock timing configuration
    }
    quint32 handle = 0;
    if (malformed || ((present & 4) != 0 && (!handleText.startsWith(u"0x") || handleText.size() != 10
        || !number(handleText.mid(2), 16, handle)))
        || ((present & 8) != 0 && allowed != u"0" && allowed != u"1")) {
        fail(Failure::InvalidInput);
        return;
    }
    const bool idle = present == 31 && state == u"READY" && source.isEmpty() && handle == 0
        && allowed == u"1" && reason.isEmpty();
    if (m_phase == Phase::AwaitIdle || m_phase == Phase::Idle) {
        // FlexLib 4.2.18 ParseInterlockStatus consumes deltas. Before arming,
        // a partial sample withdraws readiness, but a later complete idle can
        // recover. Never combine fields across messages into stop evidence.
        m_phase = idle ? Phase::Idle : Phase::AwaitIdle;
        return;
    }
    if (present != 31) {
        fail(Failure::InvalidInput);
        return;
    }
    if ((handle != 0 && handle != m_clientHandle) || (!source.isEmpty() && source != u"SW")) {
        fail(Failure::Ownership);
        return;
    }
    if (allowed != u"1" || !reason.isEmpty() || !m_operation.permitsCleanup()) {
        fail(Failure::State);
        return;
    }
    if (m_phase == Phase::AwaitPttRequested && state == u"PTT_REQUESTED"
        && source == u"SW" && handle == m_clientHandle) {
        m_phase = Phase::AwaitTransmitting;
    } else if (m_phase == Phase::AwaitTransmitting && state == u"TRANSMITTING"
               && source == u"SW" && handle == m_clientHandle) {
        m_phase = Phase::Transmitting;
        if (m_stop.valid()) {
            m_phase = m_stopReplied ? Phase::AwaitUnkey
                : m_stopWritten ? Phase::AwaitStopReply : Phase::AwaitStopWrite;
        } else {
            m_deadlineMs = 0;
        }
    } else if (m_phase == Phase::Transmitting && state == u"TRANSMITTING"
               && source == u"SW" && handle == m_clientHandle) {
        // Repeated current state is not a new keying operation.
    } else if (m_phase == Phase::AwaitUnkey && state == u"UNKEY_REQUESTED"
               && source.isEmpty() && handle == m_clientHandle) {
        m_phase = Phase::AwaitReady;
    } else if (m_phase == Phase::AwaitReady && state == u"READY"
               && source.isEmpty() && handle == m_clientHandle) {
        m_phase = Phase::AwaitOwnerClear;
    } else if ((m_phase == Phase::AwaitOwnerClear || m_phase == Phase::Confirmed) && idle
               && m_stop.matchesOperation(m_operation)) {
        m_phase = Phase::Confirmed;
    } else {
        fail(Failure::State);
    }
}

void FlexPttStopTracker::observe(Stamp stamp, QStringView rawLine, qint64 nowMs)
{
    if (!accept(stamp, nowMs)) {
        return;
    }
    if (rawLine.isEmpty() || rawLine.size() > kMaximumLineSize
        || rawLine.contains(u'\n') || rawLine.contains(u'\r') || rawLine.contains(QChar(0))) {
        fail(Failure::InvalidInput);
        return;
    }
    if (rawLine.startsWith(u'R')) {
        response(rawLine.mid(1));
    } else if (rawLine.startsWith(u'S')) {
        const qsizetype pipe = rawLine.indexOf(u'|');
        if (pipe < 0) {
            fail(Failure::InvalidInput);
            return;
        }
        const QStringView body = rawLine.mid(pipe + 1);
        if (!body.startsWith(u"interlock ")) {
            return;
        }
        // Interlock band/timing configuration is not a state sample.
        if (body.startsWith(u"interlock band ")) {
            return;
        }
        quint32 envelopeHandle = 0;
        if (!number(rawLine.mid(1, pipe - 1), 16, envelopeHandle)
            || (envelopeHandle != 0 && envelopeHandle != m_clientHandle)) {
            fail(Failure::Ownership);
            return;
        }
        interlock(body.mid(10));
    }
}

void FlexPttStopTracker::poll(qint64 nowMs)
{
    (void)advanceClock(nowMs);
}

TxCoordinator::StopRequest FlexPttStopTracker::evidence(qint64 nowMs) const
{
    if (!onThread() || m_phase != Phase::Confirmed || nowMs < m_lastMs
        || nowMs >= m_deadlineMs || !m_stop.matchesOperation(m_operation)) {
        return {};
    }
    return m_stop;
}

} // namespace AetherSDR
