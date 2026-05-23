#include "NavtexModel.h"
#include <QDebug>
#include <QDateTime>

namespace MasterSDR {

NavtexModel::NavtexModel(QObject* parent)
    : QObject(parent)
{}

void NavtexModel::sendMessage(QChar txIdent, QChar subjectIndicator,
                              const QString& msgText, std::optional<uint> serialNum)
{
    // Format per FlexLib v4.2.18:
    // navtex send tx_ident=<C> subject_indicator=<C> [serial_num=<N>] msg_text="..."
    QString cmd = QString("navtex send tx_ident=%1 subject_indicator=%2")
                      .arg(txIdent)
                      .arg(subjectIndicator);
    if (serialNum.has_value()) {
        cmd += QString(" serial_num=%1").arg(serialNum.value());
    }
    // Quote-escape so an embedded `"` in the user's text doesn't break the
    // radio's command parser.  Backslash needs escaping first (otherwise the
    // replacement double-escapes existing backslashes).
    QString escaped = msgText;
    escaped.replace('\\', "\\\\");
    escaped.replace('"', "\\\"");
    cmd += QString(" msg_text=\"%1\"").arg(escaped);

    int seq = m_nextSeq++;

    // Track as pending until radio responds with index
    NavtexMsg msg;
    msg.txIdent = txIdent;
    msg.subjectIndicator = subjectIndicator;
    msg.msgText = msgText;
    msg.status = NavtexMsgStatus::Pending;
    if (serialNum.has_value()) {
        msg.serialNum = serialNum.value();
    }
    m_pendingMsgs[seq] = msg;

    emit replyCommandReady(cmd, seq);
}

void NavtexModel::handleSendResponse(int seq, uint respVal, const QString& indexStr)
{
    auto it = m_pendingMsgs.find(seq);
    if (it == m_pendingMsgs.end()) {
        qWarning() << "NavtexModel: no pending message for seq=" << seq;
        return;
    }

    // Failure path: error responses come with respVal != 0 and an empty body
    // (radio returns `R<seq>|<errcode>|`).  Promote to Error and surface to the
    // messages list so the UI can show what happened.
    if (respVal != 0) {
        qWarning() << "NavtexModel: send failed, resp=" << respVal << "seq=" << seq;
        it->status = NavtexMsgStatus::Error;
        m_msgs.append(*it);
        m_pendingMsgs.erase(it);
        emit messagesChanged();
        return;
    }

    // Success path requires the index in the body.
    if (indexStr.isEmpty()) {
        qWarning() << "NavtexModel: send response missing index, seq=" << seq;
        m_pendingMsgs.erase(it);
        return;
    }

    bool ok = false;
    uint idx = indexStr.toUInt(&ok);
    if (!ok) {
        qWarning() << "NavtexModel: failed to parse index from" << indexStr;
        m_pendingMsgs.erase(it);
        return;
    }

    it->idx = idx;
    it->status = NavtexMsgStatus::Queued;
    m_msgs.append(*it);
    m_pendingMsgs.erase(it);
    emit messagesChanged();
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
        serial = kvs["serial_num"].toUInt();
    }

    // Check for redundant status
    for (const auto& msg : m_msgs) {
        if (msg.idx == idx && msg.status == NavtexMsgStatus::Sent) {
            return;  // Already marked as sent
        }
    }

    // Find the queued message and mark it sent
    QString dateTime = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddZHH:mm:ss");
    bool found = false;
    for (auto& msg : m_msgs) {
        if (msg.idx == idx) {
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
        m_msgs.append(msg);
    }

    emit messagesChanged();
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

} // namespace MasterSDR
