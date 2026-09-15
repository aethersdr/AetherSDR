#include "core/tnc/YappTransferSession.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace AetherSDR {
namespace {
constexpr quint8 kSoh = 1, kStx = 2, kEtx = 3, kEot = 4, kEnq = 5;
constexpr quint8 kAck = 6, kNak = 21, kCan = 24;

bool decimal(const QByteArray& bytes, qint64& value)
{
    if (bytes.isEmpty() || bytes.size() > 10) {
        return false;
    }
    for (char c : bytes) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    bool ok = false;
    value = bytes.toLongLong(&ok);
    return ok && value >= 0 && value <= YappFileStore::kMaximumFileBytes;
}
QByteArray dosStamp(const QDateTime& modified)
{
    const QDate date = modified.date();
    const QTime time = modified.time();
    const int year = qBound(1980, date.year(), 2107);
    const int d = ((year - 1980) << 9) | (date.month() << 5) | date.day();
    const int t = (time.hour() << 11) | (time.minute() << 5) | (time.second() / 2);
    return (QStringLiteral("%1%2").arg(d, 4, 16, QLatin1Char('0'))
            .arg(t, 4, 16, QLatin1Char('0'))).toUpper().toLatin1();
}
} // namespace

YappTransferSession::YappTransferSession(QObject* parent) : QObject(parent) {}
bool YappTransferSession::active() const { return m_state != State::Idle; }

quint8 YappTransferSession::checksum(const QByteArray& data)
{
    quint8 sum = 0;
    for (char byte : data) {
        sum = static_cast<quint8>(sum + static_cast<quint8>(byte));
    }
    return sum;
}

void YappTransferSession::initialize(const QString& peer, bool sending)
{
    m_source.close();
    m_store.close();
    m_input.clear();
    m_trailing.clear();
    m_outbound.clear();
    m_peer = peer;
    m_sending = sending;
    m_success = false;
    m_fileAccepted = false;
    m_size = m_position = m_resumeOffset = m_elapsedMs = 0;
    m_name.clear();
    m_stamp.clear();
    m_reason.clear();
    m_clock.start();
    m_idle.start();
}

bool YappTransferSession::startSend(const QString& path, const QString& peer, QString& error)
{
    if (active()) {
        error = QStringLiteral("A file transfer is already active");
        return false;
    }
    const QFileInfo info(path);
    if (!info.isFile() || !YappFileStore::safeName(info.fileName())
        || info.size() > YappFileStore::kMaximumFileBytes) {
        error = QStringLiteral("Choose a regular file up to 64 MiB with a portable ASCII filename");
        return false;
    }
    initialize(peer, true);
    m_source.setFileName(path);
    if (!m_source.open(QIODevice::ReadOnly)) {
        error = m_source.errorString();
        return false;
    }
    m_name = info.fileName();
    m_size = m_source.size();
    if (m_size > YappFileStore::kMaximumFileBytes) {
        m_source.close();
        error = QStringLiteral("Source file exceeds 64 MiB");
        return false;
    }
    m_stamp = dosStamp(info.lastModified());
    m_state = State::SendReady;
    queue(kEnq, 1);
    emit changed();
    return true;
}

bool YappTransferSession::startReceive(const QString& directory, const QString& peer,
                                      bool resume, QString& error)
{
    if (active() || !QFileInfo(directory).isDir()) {
        error = QStringLiteral("Transfer active or receive directory unavailable");
        return false;
    }
    initialize(peer, false);
    m_directory = directory;
    m_resume = resume;
    m_state = State::ReceiveInit;
    emit changed();
    return true;
}

void YappTransferSession::queue(quint8 type, quint8 code)
{
    QByteArray packet;
    packet.append(char(type));
    packet.append(char(code));
    m_outbound.enqueue(packet);
}

void YappTransferSession::queuePayload(quint8 type, const QByteArray& payload)
{
    QByteArray packet;
    packet.append(char(type));
    packet.append(char(payload.size() & 255));
    packet.append(payload);
    m_outbound.enqueue(packet);
}

QByteArray YappTransferSession::takeOutbound()
{
    if (!m_outbound.isEmpty()) {
        m_idle.restart();
        return m_outbound.dequeue();
    }
    if (m_state == State::Finishing) {
        finish(m_success, m_reason);
        return {};
    }
    if (m_state != State::Sending) {
        return {};
    }
    if (m_source.size() != m_size || dosStamp(QFileInfo(m_source).lastModified()) != m_stamp) {
        fail(QStringLiteral("Source file changed during transfer"));
        return takeOutbound();
    }
    if (m_position == m_size) {
        m_state = State::SendEof;
        queue(kEtx, 1);
        return takeOutbound();
    }
    const QByteArray data = m_source.read(qMin<qint64>(256, m_size - m_position));
    if (data.isEmpty() || m_source.error() != QFileDevice::NoError) {
        fail(QStringLiteral("Source file read failed"));
        return takeOutbound();
    }
    m_position += data.size();
    queuePayload(kStx, data);
    m_outbound.back().append(char(checksum(data)));
    emit changed();
    return takeOutbound();
}

void YappTransferSession::receive(const QByteArray& bytes)
{
    // Parse incrementally: the receive buffer never exceeds one wire packet
    // (259 bytes), even when the transport supplies a huge or coalesced chunk.
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        if (m_state == State::Finishing && m_success) {
            // A peer may concatenate its post-transfer prompt after EOT/ACK.
            // Keep a bounded tail until the final ACK is drained by the caller.
            m_trailing.append(bytes.mid(index, qMax<qsizetype>(0, 4096 - m_trailing.size())));
            break;
        }
        if (!active() || m_state == State::Finishing) { break; }
        m_input.append(bytes[index]);
        if (m_input.size() < 2) {
            continue;
        }
        const quint8 type = static_cast<quint8>(m_input[0]);
        const quint8 code = static_cast<quint8>(m_input[1]);
        int length = 2;
        if (type == kStx) {
            length += (code == 0 ? 256 : code) + 1;
        } else if (type == kSoh || type == kNak || type == kCan) {
            length += code;
        } else if (type != kAck && type != kEnq && type != kEtx && type != kEot) {
            m_input.clear();
            fail(QStringLiteral("Unrecognized YAPP-C packet"));
            break;
        }
        if (m_input.size() < length) {
            continue;
        }
        const QByteArray payload = m_input.mid(2);
        m_input.clear();
        if (m_outbound.size() >= 4) {
            fail(QStringLiteral("Peer exceeded YAPP-C response capacity"));
            break;
        }
        handle(type, code, payload);
        emit changed();
    }
}

void YappTransferSession::handle(quint8 type, quint8 code, const QByteArray& payload)
{
    if (type == kCan) {
        m_reason = QStringLiteral("Peer cancelled transfer");
        m_success = false;
        m_outbound.clear();
        queue(kAck, 5);
        m_state = State::Finishing;
        return;
    }
    if (m_state == State::Cancelling) {
        if (type == kAck && code == 5) {
            m_state = State::Finishing;
        }
        return;
    }
    m_idle.restart();
    if (m_state == State::SendReady && type == kAck && code == 1) {
        QByteArray header = m_name.toLatin1();
        header.append('\0');
        header.append(QByteArray::number(m_size));
        header.append('\0');
        header.append(m_stamp);
        header.append('\0');
        m_state = State::SendHeader;
        queuePayload(kSoh, header);
        return;
    }
    if (m_state == State::SendHeader) {
        if (type == kAck && code == 6) {
            m_state = State::Sending;
            return;
        }
        if (type == kNak) {
            const QList<QByteArray> fields = payload.split('\0');
            qint64 offset = 0;
            if (fields.size() == 4 && fields[0] == "R" && fields[2] == "C"
                && fields[3].isEmpty() && decimal(fields[1], offset)
                && offset <= m_size && m_source.seek(offset)) {
                m_position = m_resumeOffset = offset;
                m_state = State::Sending;
                return;
            }
            fail(QStringLiteral("Resume refused: invalid offset or checksum negotiation"));
            return;
        }
        if (type == kAck && code == 2) {
            fail(QStringLiteral("Peer selected plain YAPP; YAPP-C checksum mode is required"));
            return;
        }
    }
    if (m_state == State::SendEof && type == kAck && code == 3) {
        m_fileAccepted = true;
        m_state = State::SendEot;
        queue(kEot, 1);
        return;
    }
    if (m_state == State::SendEot && type == kAck && code == 4) {
        m_success = true;
        m_state = State::Finishing;
        return;
    }
    if (m_state == State::ReceiveInit && type == kEnq && code == 1) {
        m_state = State::ReceiveHeader;
        queue(kAck, 1);
        return;
    }
    if (m_state == State::ReceiveHeader && type == kEnq && code == 1) {
        queue(kAck, 1);
        return;
    }
    if (m_state == State::ReceiveHeader && type == kSoh) {
        const QList<QByteArray> fields = payload.split('\0');
        qint64 size = 0;
        static const QRegularExpression stampPattern(QStringLiteral("^[0-9A-Fa-f]{8}$"));
        if (fields.size() != 4 || !fields[3].isEmpty() || !decimal(fields[1], size)
            || !stampPattern.match(QString::fromLatin1(fields[2])).hasMatch()) {
            fail(QStringLiteral("Invalid YAPP-C file header or missing timestamp"));
            return;
        }
        const QString incomingName = QString::fromLatin1(fields[0]);
        if (!YappFileStore::safeName(incomingName)) {
            fail(QStringLiteral("Unsafe or non-portable remote filename"));
            return;
        }
        m_name = incomingName;
        m_size = size;
        m_stamp = fields[2].toUpper();
        QString error;
        if (!m_store.open(m_directory, m_peer, m_name, m_size, m_stamp, m_resume, error)) {
            fail(error);
            return;
        }
        m_position = m_resumeOffset = m_store.position();
        m_state = State::Receiving;
        if (m_resumeOffset > 0) {
            QByteArray reply("R\0", 2);
            reply.append(QByteArray::number(m_resumeOffset));
            reply.append(QByteArray("\0C\0", 3));
            queuePayload(kNak, reply);
        } else {
            queue(kAck, 6);
        }
        return;
    }
    if (m_state == State::Receiving && type == kStx) {
        const QByteArray data = payload.chopped(1);
        if (checksum(data) != static_cast<quint8>(payload.back())) {
            fail(QStringLiteral("YAPP-C block checksum mismatch; partial file retained"));
            return;
        }
        QString error;
        if (!m_store.append(data, error)) {
            fail(error);
            return;
        }
        m_position = m_store.position();
        return;
    }
    if (m_state == State::Receiving && type == kEtx && code == 1) {
        QString error;
        if (!m_store.finish(error)) {
            fail(error);
            return;
        }
        m_fileAccepted = true;
        m_state = State::ReceiveEot;
        queue(kAck, 3);
        return;
    }
    if (m_state == State::ReceiveEot && type == kEot && code == 1) {
        queue(kAck, 4);
        m_success = true;
        m_state = State::Finishing;
        return;
    }
    fail(QStringLiteral("Unexpected packet in %1").arg(phase()));
}

void YappTransferSession::fail(const QString& reason)
{
    if (!active() || m_state == State::Cancelling) {
        return;
    }
    m_reason = reason;
    m_success = false;
    m_outbound.clear();
    m_input.clear();
    m_source.close();
    m_store.close();
    m_state = State::Cancelling;
    queuePayload(kCan, reason.toLatin1().left(120));
    m_idle.restart();
    emit changed();
}

QByteArray YappTransferSession::takeTrailingText()
{
    QByteArray bytes;
    bytes.swap(m_trailing);
    return bytes;
}

void YappTransferSession::cancel()
{
    if (m_state == State::ReceiveInit) {
        stop(QStringLiteral("Receive cancelled before handshake"));
    } else {
        fail(QStringLiteral("Cancelled by operator; partial file retained"));
    }
}
void YappTransferSession::stop(const QString& reason)
{
    if (active()) {
        finish(false, reason);
    }
}
void YappTransferSession::checkTimeout(int timeoutMs)
{
    if (active() && m_idle.elapsed() >= timeoutMs) {
        // The transport owns RF retry policy. A stalled session is bounded even
        // if the peer keeps the AX.25 link alive with polls but no file progress.
        stop(m_reason.isEmpty() ? QStringLiteral("Transfer timed out") : m_reason);
    }
}
void YappTransferSession::finish(bool success, const QString& reason)
{
    m_elapsedMs = m_clock.elapsed();
    m_state = State::Idle;
    m_success = success;
    m_reason = reason;
    m_source.close();
    m_store.close();
    m_outbound.clear();
    m_input.clear();
    emit changed();
    emit finished(success, summary());
}
QString YappTransferSession::phase() const
{
    switch (m_state) {
    case State::Idle: return m_success ? QStringLiteral("Complete")
        : (m_reason.isEmpty() ? QStringLiteral("Idle") : QStringLiteral("Stopped"));
    case State::SendReady: return QStringLiteral("Waiting for receiver");
    case State::SendHeader: return QStringLiteral("Negotiating file");
    case State::Sending: return QStringLiteral("Sending");
    case State::SendEof: return QStringLiteral("Waiting for file acceptance");
    case State::SendEot: return QStringLiteral("Waiting for final confirmation");
    case State::ReceiveInit: return QStringLiteral("Receive armed");
    case State::ReceiveHeader: return QStringLiteral("Waiting for header");
    case State::Receiving: return QStringLiteral("Receiving");
    case State::ReceiveEot: return QStringLiteral("File saved; waiting for session end");
    case State::Cancelling: return QStringLiteral("Cancelling");
    case State::Finishing: return QStringLiteral("Finishing handshake");
    }
    return {};
}
QJsonObject YappTransferSession::snapshot() const
{
    const qint64 elapsed = active() ? m_clock.elapsed() : m_elapsedMs;
    const qint64 newly = m_position - m_resumeOffset;
    const double rate = elapsed > 0 ? double(newly) * 1000 / double(elapsed) : 0;
    return {{QStringLiteral("active"), active()}, {QStringLiteral("phase"), phase()},
            {QStringLiteral("direction"), m_sending ? QStringLiteral("send") : QStringLiteral("receive")},
            {QStringLiteral("file"), m_name}, {QStringLiteral("peer"), m_peer},
            {QStringLiteral("size"), double(m_size)}, {QStringLiteral("bytes"), double(m_position)},
            {QStringLiteral("resumeOffset"), double(m_resumeOffset)},
            {QStringLiteral("elapsedMs"), double(elapsed)}, {QStringLiteral("bytesPerSecond"), rate},
            {QStringLiteral("etaSeconds"), elapsed >= 3000 && rate > 0 ? (m_size - m_position) / rate : -1},
            {QStringLiteral("fileAccepted"), m_fileAccepted}, {QStringLiteral("success"), m_success},
            {QStringLiteral("reason"), m_reason},
            {QStringLiteral("savedPath"), !m_sending && m_fileAccepted ? m_store.destination() : QString()}};
}
QString YappTransferSession::summary() const
{
    const QJsonObject s = snapshot();
    QString line = QStringLiteral("YAPP-C %1 %2: %3/%4 bytes (%5 resumed), %6 s, %7 B/s")
        .arg(m_sending ? QStringLiteral("send") : QStringLiteral("receive"), m_name)
        .arg(m_position).arg(m_size).arg(m_resumeOffset)
        .arg(s.value(QStringLiteral("elapsedMs")).toDouble() / 1000, 0, 'f', 1)
        .arg(s.value(QStringLiteral("bytesPerSecond")).toDouble(), 0, 'f', 1);
    if (!active()) {
        line += m_success ? QStringLiteral(" — complete") : QStringLiteral(" — stopped: %1").arg(m_reason);
    }
    if (!m_sending && m_fileAccepted) {
        line += QStringLiteral("; saved: %1").arg(m_store.destination());
    } else if (m_sending && m_fileAccepted && !m_success) {
        line += QStringLiteral("; peer accepted file, session completion unconfirmed");
    }
    return line;
}
} // namespace AetherSDR
