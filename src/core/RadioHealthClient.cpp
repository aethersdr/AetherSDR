#include "RadioHealthClient.h"

#include "RadioHealthRelease.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTcpSocket>
#include <QTimer>

namespace AetherSDR {

namespace {
constexpr int kTimeoutMs = 3000;
constexpr qsizetype kMaxReply = 4096;   // the agent's line is under 1 KB
}  // namespace

namespace radiohealth {

std::optional<RadioHealth> parse(const QByteArray& line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("agent")).isString()
        || !o.value(QStringLiteral("storage")).isArray()) {
        return std::nullopt;
    }
    RadioHealth h;
    h.agentVersion = o.value(QStringLiteral("agent")).toString().left(32);
    const int idle = o.value(QStringLiteral("cpu_idle_pct")).toInt(-1);
    h.cpuIdlePct = idle >= 0 && idle <= 100 ? idle : -1;
    h.cpuCores = qBound(0, o.value(QStringLiteral("cpu_cores")).toInt(0), 1024);
    h.memTotalKb = o.value(QStringLiteral("mem_total_kb")).toInteger(-1);
    h.memAvailableKb = o.value(QStringLiteral("mem_available_kb")).toInteger(-1);
    h.memEstimated = o.value(QStringLiteral("mem_estimated")).toBool(false);
    for (const QJsonValue& v : o.value(QStringLiteral("storage")).toArray()) {
        const QJsonObject s = v.toObject();
        const qint64 free = s.value(QStringLiteral("free_bytes")).toInteger(-1);
        const qint64 total = s.value(QStringLiteral("total_bytes")).toInteger(-1);
        if (free < 0 || total < free || h.storage.size() >= 4) {
            continue;
        }
        h.storage.append({s.value(QStringLiteral("path")).toString().left(200),
                          quint64(free), quint64(total)});
    }
    return h;
}

QString summary(const RadioHealth& h)
{
    const QLocale locale;
    QStringList parts;
    if (!h.storage.isEmpty()) {
        parts << QObject::tr("Storage %1 free")
                     .arg(locale.formattedDataSize(qint64(h.storage.first().freeBytes), 0,
                                                   QLocale::DataSizeSIFormat));
    }
    if (h.memAvailableKb >= 0) {
        const QString size = locale.formattedDataSize(h.memAvailableKb * 1000, 1,
                                                      QLocale::DataSizeSIFormat);
        parts << (h.memEstimated ? QObject::tr("Memory about %1 free").arg(size)
                                 : QObject::tr("Memory %1 free").arg(size));
    }
    if (h.cpuIdlePct >= 0) {
        parts << QObject::tr("CPU %1% idle").arg(h.cpuIdlePct);
    }
    return parts.join(QStringLiteral(" · "));
}

}  // namespace radiohealth

RadioHealthClient::RadioHealthClient(QObject* parent)
    : QObject(parent)
{
}

void RadioHealthClient::fetch()
{
    if (m_socket || m_address.isNull()) {
        return;
    }
    m_buffer.clear();
    auto* socket = new QTcpSocket(this);
    m_socket = socket;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        m_buffer += socket->read(kMaxReply - m_buffer.size());
        if (m_buffer.contains('\n') || m_buffer.size() >= kMaxReply) {
            finish();
        }
    });
    // The agent closes after its line, so a close is how a reply ends.
    connect(socket, &QTcpSocket::disconnected, this, &RadioHealthClient::finish);
    connect(socket, &QTcpSocket::errorOccurred, this, &RadioHealthClient::finish);
    QTimer::singleShot(kTimeoutMs, socket, [this] { finish(); });
    socket->connectToHost(m_address, kRadioHealthPort);
}

void RadioHealthClient::finish()
{
    QTcpSocket* socket = m_socket;
    if (!socket) {
        return;
    }
    m_socket = nullptr;
    // Whatever arrived before the close or the timeout; parse() wants a
    // whole line, so a cut-off reply fails.
    if (m_buffer.size() < kMaxReply) {
        m_buffer += socket->read(kMaxReply - m_buffer.size());
    }
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    const qsizetype end = m_buffer.indexOf('\n');
    if (const auto health = end < 0 ? std::nullopt : radiohealth::parse(m_buffer.left(end))) {
        emit received(*health);
    } else {
        emit failed(tr("The radio health tools didn't answer."));
    }
}

}  // namespace AetherSDR
