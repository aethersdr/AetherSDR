#include "core/aprs/AprsMessenger.h"

#include "core/LogManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <algorithm>

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;

namespace {
// Destination "tocall" identifying the software that originated the frame.
// APZ is the experimental allocation (APRS 1.0.1 appendix 4).
const QString kTocall = QStringLiteral("APZATH");
} // namespace

AprsMessenger::AprsMessenger(QObject* parent)
    : QObject(parent)
{
    m_retryTimer.setInterval(5000);
    connect(&m_retryTimer, &QTimer::timeout, this, &AprsMessenger::serviceRetries);

    m_saveCoalesce.setSingleShot(true);
    m_saveCoalesce.setInterval(2000);
    connect(&m_saveCoalesce, &QTimer::timeout, this, [this] { save(); });
}

AprsMessenger::~AprsMessenger()
{
    if (m_saveCoalesce.isActive()) {
        m_saveCoalesce.stop();
        save();
    }
}

void AprsMessenger::setMyAddress(const Address& addr)
{
    m_myAddress = addr;
}

void AprsMessenger::setPersistencePath(const QString& path)
{
    m_persistPath = path;
    if (!m_persistPath.isEmpty())
        load();
}

void AprsMessenger::onPacket(const aprs::Packet& packet)
{
    const QPointer<AprsMessenger> self(this);
    if (!m_myAddress.isValid())
        return;
    // Ignore our own frames coming back around a digipeater.
    if (packet.source.compare(m_myAddress.toString(), Qt::CaseInsensitive) == 0)
        return;

    if (packet.type == aprs::PacketType::MessageAck
        || packet.type == aprs::PacketType::MessageRej) {
        if (packet.addressee.compare(m_myAddress.toString(), Qt::CaseInsensitive) != 0)
            return;
        const bool acked = (packet.type == aprs::PacketType::MessageAck);
        for (Message& m : m_messages) {
            if (m.outgoing && m.msgNo == packet.messageNo
                && m.counterpart.compare(packet.source, Qt::CaseInsensitive) == 0
                && (m.state == State::Sent || m.state == State::Pending)) {
                m.state = acked ? State::Acked : State::Rejected;
                m.input = {}; // History no longer owns a live retry program.
                emit activity(QStringLiteral("APRS message %1 to %2 %3.")
                                  .arg(m.msgNo, m.counterpart,
                                       acked ? QStringLiteral("acknowledged")
                                             : QStringLiteral("rejected")));
                if (!self) { return; }
                scheduleSave();
                emit messagesChanged();
                return;
            }
        }
        return;
    }

    if (packet.type != aprs::PacketType::Message)
        return;
    if (packet.addressee.compare(m_myAddress.toString(), Qt::CaseInsensitive) != 0)
        return;

    // Always (re-)ack a numbered message — the sender retries until the ack
    // gets through, and a duplicate means our previous ack was lost.
    if (!packet.messageNo.isEmpty()) {
        transmitText(aprs::encodeAck(packet.source, packet.messageNo), m_receiveProgram);
        if (!self) { return; }
    }

    // Duplicate detection: same station re-sending the same message within a
    // few minutes. APRS clients reuse small message numbers freely (often
    // cycling {1–{99, some restarting per session), so a numbered match must be
    // bounded by both text AND time — an unbounded msgNo match would silently
    // drop a genuinely-new message that happens to reuse a number we stored
    // long ago (while still auto-acking it, so neither side notices). 30 min
    // comfortably covers ack-loss retries.
    for (const Message& m : m_messages) {
        if (m.outgoing
            || m.counterpart.compare(packet.source, Qt::CaseInsensitive) != 0)
            continue;
        const qint64 ageSecs = m.utc.secsTo(QDateTime::currentDateTimeUtc());
        if (!packet.messageNo.isEmpty()) {
            if (m.msgNo == packet.messageNo
                && m.text == packet.messageText
                && ageSecs < 1800)
                return;
        } else if (m.text == packet.messageText && ageSecs < 300) {
            return;
        }
    }

    Message msg;
    msg.counterpart = packet.source;
    msg.text = packet.messageText;
    msg.utc = QDateTime::currentDateTimeUtc();
    msg.outgoing = false;
    msg.read = false;
    msg.msgNo = packet.messageNo;
    msg.state = State::Received;
    m_messages.append(msg);
    if (m_messages.size() > kMaxStoredMessages)
        m_messages.remove(0, m_messages.size() - kMaxStoredMessages);

    emit activity(QStringLiteral("APRS message from %1: %2")
                      .arg(msg.counterpart, msg.text));
    if (!self) { return; }
    scheduleSave();
    emit messageReceived(msg);
    if (!self) { return; }
    emit messagesChanged();
    if (!self) { return; }
    emit unreadCountChanged(unreadCount());
}

bool AprsMessenger::sendMessage(const QString& to, const QString& text,
                                const TxCoordinator::Request& input)
{
    if (!m_myAddress.isValid())
        return false;
    const auto dest = Address::parse(to.trimmed().toUpper());
    if (!dest || text.trimmed().isEmpty())
        return false;

    Message msg;
    msg.counterpart = dest->toString();
    msg.input = input;
    msg.text = text.trimmed();
    msg.utc = QDateTime::currentDateTimeUtc();
    msg.outgoing = true;
    msg.read = true;
    msg.msgNo = QString::number(m_nextMsgNo);
    m_nextMsgNo = (m_nextMsgNo % 99999) + 1;
    msg.state = State::Sent;
    msg.tries = 1;
    msg.nextTryUtc = msg.utc.addSecs(kRetryIntervalSecs);
    m_messages.append(msg);

    if (!m_retryTimer.isActive())
        m_retryTimer.start();
    scheduleSave();
    const QPointer<AprsMessenger> self(this);
    transmitText(aprs::encodeMessage(msg.counterpart, msg.text, msg.msgNo), msg.input);
    if (!self) { return true; }
    emit activity(QStringLiteral("APRS message %1 to %2 sent: %3")
                      .arg(msg.msgNo, msg.counterpart, msg.text));
    if (!self) { return true; }
    emit messagesChanged();
    return true;
}

void AprsMessenger::transmitText(const QString& infoText, const TxCoordinator::Request& input)
{
    Address dest;
    dest.call = kTocall;
    const Frame frame =
        Frame::makeUI(dest, m_myAddress, m_path, infoText.toLatin1());
    emit transmitFrame(frame.encode(), input.derive());
}

void AprsMessenger::serviceRetries()
{
    if (m_servicingRetries) { return; }
    m_servicingRetries = true;
    const QPointer<AprsMessenger> self(this);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool changed = false;
    // Delivery can synchronously ack, clear, append, cancel or destroy us.
    // Snapshot the due work, then re-find each still-pending record; never
    // retain a vector reference or iterator across an application callback.
    const QVector<Message> pending = m_messages;
    for (const Message& candidate : pending) {
        if (!candidate.outgoing || candidate.state != State::Sent || candidate.nextTryUtc > now) {
            continue;
        }
        Message attempt;
        {
            const auto found = std::find_if(m_messages.begin(), m_messages.end(), [&candidate](const Message& m) {
                return m.outgoing && m.state == State::Sent && m.msgNo == candidate.msgNo
                    && m.counterpart == candidate.counterpart && m.utc == candidate.utc
                    && m.tries == candidate.tries && m.nextTryUtc == candidate.nextTryUtc;
            });
            if (found == m_messages.end()) { continue; }
            if (found->tries >= kMaxTries) {
                found->state = State::Failed;
                found->input = {};
            } else {
                ++found->tries;
                found->nextTryUtc = now.addSecs(kRetryIntervalSecs);
            }
            attempt = *found;
        }
        changed = true;
        if (attempt.state == State::Failed) {
            emit activity(QStringLiteral("APRS message %1 to %2 failed (no ack after %3 tries).")
                              .arg(attempt.msgNo, attempt.counterpart).arg(attempt.tries));
        } else {
            transmitText(aprs::encodeMessage(attempt.counterpart, attempt.text, attempt.msgNo), attempt.input);
            if (!self) { return; }
            emit activity(QStringLiteral("APRS message %1 to %2 retry %3/%4.")
                              .arg(attempt.msgNo, attempt.counterpart).arg(attempt.tries).arg(kMaxTries));
        }
        if (!self) { return; }
    }
    const bool anyOutstanding = std::any_of(m_messages.cbegin(), m_messages.cend(), [](const Message& m) {
        return m.outgoing && m.state == State::Sent;
    });
    if (!anyOutstanding) {
        m_retryTimer.stop();
    }
    m_servicingRetries = false;
    if (changed) {
        scheduleSave();
        emit messagesChanged();
    }
}

void AprsMessenger::cancelPendingTransmissions()
{
    m_retryTimer.stop();
    bool changed = false;
    for (Message& message : m_messages) {
        if (message.outgoing && (message.state == State::Sent || message.state == State::Pending)) {
            message.state = State::Failed;
            message.input = {};
            changed = true;
        }
    }
    if (changed) {
        scheduleSave();
        emit messagesChanged();
    }
}

int AprsMessenger::unreadCount() const
{
    int n = 0;
    for (const Message& m : m_messages) {
        if (!m.outgoing && !m.read)
            ++n;
    }
    return n;
}

void AprsMessenger::markAllRead()
{
    bool changed = false;
    for (Message& m : m_messages) {
        if (!m.outgoing && !m.read) {
            m.read = true;
            changed = true;
        }
    }
    if (changed) {
        scheduleSave();
        emit messagesChanged();
        emit unreadCountChanged(0);
    }
}

void AprsMessenger::clear()
{
    m_messages.clear();
    save();
    emit messagesChanged();
    emit unreadCountChanged(0);
}

void AprsMessenger::scheduleSave()
{
    if (m_persistPath.isEmpty())
        return;
    m_saveCoalesce.start();
}

void AprsMessenger::load()
{
    m_messages.clear();
    QFile f(m_persistPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    m_nextMsgNo = qBound(1, root.value(QStringLiteral("nextMsgNo")).toInt(1), 99999);
    for (const QJsonValue& v : root.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject o = v.toObject();
        Message m;
        m.counterpart = o.value(QStringLiteral("counterpart")).toString();
        if (m.counterpart.isEmpty())
            continue;
        m.text = o.value(QStringLiteral("text")).toString();
        m.utc = QDateTime::fromString(o.value(QStringLiteral("utc")).toString(),
                                      Qt::ISODate);
        m.outgoing = o.value(QStringLiteral("outgoing")).toBool();
        m.read = o.value(QStringLiteral("read")).toBool(true);
        m.msgNo = o.value(QStringLiteral("msgNo")).toString();
        m.tries = o.value(QStringLiteral("tries")).toInt();
        const int state = o.value(QStringLiteral("state")).toInt();
        m.state = State(qBound(0, state, int(State::Failed)));
        // Don't resurrect retry loops across restarts: anything still
        // awaiting an ack from a previous session is closed out as Failed.
        if (m.outgoing && (m.state == State::Sent || m.state == State::Pending))
            m.state = State::Failed;
        m_messages.append(m);
    }
}

void AprsMessenger::save() const
{
    if (m_persistPath.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_persistPath).absolutePath());
    QJsonArray arr;
    for (const Message& m : m_messages) {
        QJsonObject o;
        o.insert(QStringLiteral("counterpart"), m.counterpart);
        o.insert(QStringLiteral("text"), m.text);
        o.insert(QStringLiteral("utc"), m.utc.toString(Qt::ISODate));
        o.insert(QStringLiteral("outgoing"), m.outgoing);
        o.insert(QStringLiteral("read"), m.read);
        o.insert(QStringLiteral("msgNo"), m.msgNo);
        o.insert(QStringLiteral("tries"), m.tries);
        o.insert(QStringLiteral("state"), int(m.state));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("messages"), arr);
    root.insert(QStringLiteral("nextMsgNo"), m_nextMsgNo);
    // Atomic write (Constitution Principle XIV) — a crash mid-write must not
    // truncate or corrupt the message history.
    QSaveFile f(m_persistPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcAx25).noquote()
            << "AprsMessenger: could not write" << m_persistPath
            << "—" << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson());
    if (!f.commit())
        qCWarning(lcAx25).noquote()
            << "AprsMessenger: could not commit" << m_persistPath
            << "—" << f.errorString();
}

} // namespace AetherSDR
