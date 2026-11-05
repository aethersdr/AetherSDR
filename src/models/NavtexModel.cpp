#include "NavtexModel.h"
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <utility>

namespace AetherSDR {

NavtexModel::NavtexModel(QObject* parent)
    : QObject(parent)
{}

void NavtexModel::sendMessage(QChar txIdent, QChar subjectIndicator,
                              const QString& msgText, std::optional<uint> serialNum)
{
    if (QThread::currentThread() != thread()) { return; }
    const NavtexRequest request{txIdent, subjectIndicator, msgText, serialNum};
    if (!request.valid() || m_pendingMsgs.size() + m_queuedRequests.size() >= 16) { return; }
    const quint64 seq = ++m_nextSeq;
    NavtexMsg msg;
    msg.txIdent = txIdent;
    msg.subjectIndicator = subjectIndicator;
    msg.msgText = msgText;
    msg.serialNum = serialNum.value_or(0);
    msg.session = m_session;
    msg.requestId = seq;
    m_pendingMsgs.insert(seq, msg);
    QTimer::singleShot(30000, this, [this, seq] {
        handleSendResponse(seq, {TransmitServiceReply::Outcome::Unknown, 0,
                                tr("No radio response; the radio queue is unknown.")});
    });
    emit controlRequested(request, seq);
}

void NavtexModel::appendMessage(const NavtexMsg& message)
{
    // History is presentation state, not ownership or an unbounded queue.
    if (m_msgs.size() >= 256) { m_msgs.removeFirst(); }
    m_msgs.append(message);
}

void NavtexModel::handleSendResponse(quint64 requestId, const TransmitServiceReply& reply)
{
    auto found = m_pendingMsgs.find(requestId);
    if (found == m_pendingMsgs.end()) { return; }
    NavtexMsg message = *found;
    m_pendingMsgs.erase(found);
    using Outcome = TransmitServiceReply::Outcome;
    bool indexValid = false;
    const uint idx = reply.detail.toUInt(&indexValid);
    const bool accepted = reply.outcome == Outcome::Accepted && indexValid && !m_queuedRequests.contains(idx);
    bool settled = reply.outcome == Outcome::Rejected || reply.outcome == Outcome::NotDispatched;
    message.status = settled ? NavtexMsgStatus::Error : NavtexMsgStatus::Unknown;
    if (accepted) {
        message.idx = idx;
        message.status = NavtexMsgStatus::Queued;
        // Firmware may announce sent before returning the command's index.
        for (NavtexMsg& observed : m_msgs) {
            if (observed.session == m_session && observed.idx == idx
                && observed.status == NavtexMsgStatus::Sent) {
                message.status = NavtexMsgStatus::Sent;
                message.serialNum = observed.serialNum;
                message.dateTime = observed.dateTime;
                observed = message;
                const QPointer<NavtexModel> alive(this);
                emit operationSettled(requestId);
                if (alive) { emit messagesChanged(); }
                return;
            }
        }
        m_queuedRequests.insert(idx, requestId);
    }
    appendMessage(message);
    const QPointer<NavtexModel> alive(this);
    if (settled) { emit operationSettled(requestId); }
    if (alive) { emit messagesChanged(); }
}

void NavtexModel::reset()
{
    // Retiring local intent neither retracts nor retries a radio queue.
    for (const NavtexMsg& pending : std::as_const(m_pendingMsgs)) {
        NavtexMsg retired = pending;
        retired.status = NavtexMsgStatus::Unknown;
        appendMessage(retired);
    }
    m_pendingMsgs.clear();
    m_queuedRequests.clear();
    for (NavtexMsg& message : m_msgs) {
        if (message.session == m_session && message.status == NavtexMsgStatus::Queued) {
            message.status = NavtexMsgStatus::Unknown;
        }
    }
    ++m_session;
    m_status = NavtexStatus::Inactive;
    const QPointer<NavtexModel> alive(this);
    emit statusChanged(m_status);
    if (alive) { emit messagesChanged(); }
}

void NavtexModel::parseStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    // Two forms per FlexLib:
    // 1. "navtex sent" — per-message TX confirmation with idx= and serial_num=
    // 2. "navtex" with status=<value> — global NAVTEX state
    if (object == "navtex sent") {
        parseSentStatus(kvs);
        return;
    }

    // Global status update
    if (kvs.contains("status")) {
        NavtexStatus newStatus = parseStatusString(kvs["status"]);
        if (newStatus != m_status) {
            m_status = newStatus;
            emit statusChanged(m_status);
        }
    }
}

void NavtexModel::parseSentStatus(const QMap<QString, QString>& kvs)
{
    // Parse idx and serial_num from the sent confirmation
    if (!kvs.contains("idx")) {
        qWarning() << "NavtexModel: sent status missing idx";
        return;
    }

    bool ok = false;
    uint idx = kvs["idx"].toUInt(&ok);
    if (!ok) {
        qWarning() << "NavtexModel: failed to parse idx from" << kvs["idx"];
        return;
    }

    uint serial = 0;
    if (kvs.contains("serial_num")) {
        serial = kvs["serial_num"].toUInt(&ok);
        if (!ok) { return; }
    }

    // Check for redundant status
    for (const auto& msg : m_msgs) {
        if (msg.session == m_session && msg.idx == idx && msg.status == NavtexMsgStatus::Sent) {
            return;  // Already marked as sent
        }
    }

    // Find the queued message and mark it sent
    QString dateTime = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddZHH:mm:ss");
    bool found = false;
    const quint64 settled = m_queuedRequests.take(idx);
    for (auto& msg : m_msgs) {
        if (msg.session == m_session && msg.idx == idx && msg.status == NavtexMsgStatus::Queued) {
            msg.status = NavtexMsgStatus::Sent;
            msg.dateTime = dateTime;
            msg.serialNum = serial;
            found = true;
            break;
        }
    }

    if (!found) {
        // Radio sent a confirmation for a message we don't know about
        // (e.g., sent by another client). Track it anyway.
        NavtexMsg msg;
        msg.idx = idx;
        msg.serialNum = serial;
        msg.dateTime = dateTime;
        msg.status = NavtexMsgStatus::Sent;
        msg.session = m_session;
        appendMessage(msg);
    }

    const QPointer<NavtexModel> alive(this);
    if (settled) { emit operationSettled(settled); }
    if (alive) { emit messagesChanged(); }
}

NavtexStatus NavtexModel::parseStatusString(const QString& str)
{
    // Case-insensitive match per FlexLib's Enum.TryParse behavior
    const QString lower = str.toLower();
    if (lower == "inactive")     return NavtexStatus::Inactive;
    if (lower == "active")       return NavtexStatus::Active;
    if (lower == "transmitting") return NavtexStatus::Transmitting;
    if (lower == "queuefull")    return NavtexStatus::QueueFull;
    if (lower == "unlicensed")   return NavtexStatus::Unlicensed;
    return NavtexStatus::Error;
}

} // namespace AetherSDR
