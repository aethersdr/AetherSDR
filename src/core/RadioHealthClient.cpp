#include "RadioHealthClient.h"

#include "RadioHealthPackage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTimer>

namespace AetherSDR {

namespace {
constexpr int kTimeoutMs = 3000;
constexpr qsizetype kMaxReply = 4096;   // the agent's line is under 1 KB
// /proc/meminfo counts KiB. 2^40 KiB (1 PiB) is far beyond any radio and
// keeps the byte conversion (x 1024) well inside qint64.
constexpr qint64 kMaxMemKb = qint64(1) << 40;
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
    // Out-of-range memory is dropped, not formatted: a total outside
    // 1..kMaxMemKb, or available memory outside 0..total.
    const qint64 total = o.value(QStringLiteral("mem_total_kb")).toInteger(-1);
    h.memTotalKb = total >= 1 && total <= kMaxMemKb ? total : -1;
    const qint64 avail = o.value(QStringLiteral("mem_available_kb")).toInteger(-1);
    const qint64 ceiling = h.memTotalKb > 0 ? h.memTotalKb : kMaxMemKb;
    h.memAvailableKb = avail >= 0 && avail <= ceiling ? avail : -1;
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

QString storageSummary(const RadioHealth& h)
{
    if (h.storage.isEmpty()) {
        return {};
    }
    return QObject::tr("Storage %1 free")
        .arg(QLocale().formattedDataSize(qint64(h.storage.first().freeBytes), 0,
                                         QLocale::DataSizeSIFormat));
}

QString summary(const RadioHealth& h)
{
    QStringList parts{storageSummary(h), resourceSummary(h)};
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

QString resourceSummary(const RadioHealth& h)
{
    const QLocale locale;
    QStringList parts;
    if (h.memAvailableKb >= 0) {
        const QString size = locale.formattedDataSize(h.memAvailableKb * 1024, 1,
                                                      QLocale::DataSizeSIFormat);
        parts << (h.memEstimated ? QObject::tr("Memory about %1 free").arg(size)
                                 : QObject::tr("Memory %1 free").arg(size));
    }
    if (h.cpuIdlePct >= 0) {
        parts << QObject::tr("CPU %1% idle").arg(h.cpuIdlePct);
    }
    return parts.join(QStringLiteral(" · "));
}

namespace {

bool matchesPin(const QByteArray& bytes)
{
    return bytes.size() == RadioHealthPackage::kSize
        && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
               == QByteArray(RadioHealthPackage::kSha256);
}

}  // namespace

QString writeBundledPackage(const QString& dir, QString* error, const QString& resource)
{
    const QString target = QDir(dir).filePath(QStringLiteral("AetherHealth.ssdr_waveform"));
    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == RadioHealthPackage::kSize
        && matchesPin(existing.readAll())) {
        return target;
    }
    existing.close();
    QFile bundled(resource);
    const QByteArray bytes = bundled.open(QIODevice::ReadOnly)
        ? bundled.read(RadioHealthPackage::kSize + 1) : QByteArray();
    if (!matchesPin(bytes)) {
        if (error) {
            *error = QObject::tr("This AetherSDR's radio health tools are damaged, so nothing "
                                 "was installed.");
        }
        return {};
    }
    QSaveFile out(target);
    if (!QDir().mkpath(dir) || !out.open(QIODevice::WriteOnly)
        || out.write(bytes) != bytes.size() || !out.commit()) {
        if (error) {
            *error = QObject::tr("Can't write the radio health tools to %1.")
                         .arg(QDir::toNativeSeparators(target));
        }
        return {};
    }
    return target;
}

bool isLanIpv4(const QHostAddress& address)
{
    return !address.isNull() && address.protocol() == QAbstractSocket::IPv4Protocol
        && (address.isPrivateUse() || address.isLinkLocal());
}

}  // namespace radiohealth

RadioHealthClient::RadioHealthClient(QObject* parent)
    : QObject(parent)
{
}

void RadioHealthClient::setRadioAddress(const QHostAddress& address)
{
    if (address != m_address) {
        cancel();
        m_address = address;
    }
}

void RadioHealthClient::cancel()
{
    ++m_generation;
    if (QTcpSocket* socket = m_socket) {
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

void RadioHealthClient::fetch()
{
    if (m_socket || m_address.isNull()) {
        return;
    }
    m_buffer.clear();
    m_readGeneration = m_generation;
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
    deliver(m_buffer, m_readGeneration);
}

void RadioHealthClient::deliver(const QByteArray& reply, quint64 generation)
{
    if (generation != m_generation) {
        return;   // retired by cancel() or a new address
    }
    const qsizetype end = reply.indexOf('\n');
    if (const auto health = end < 0 ? std::nullopt : radiohealth::parse(reply.left(end))) {
        emit received(*health);
    } else {
        emit failed(tr("The radio health tools didn't answer."));
    }
}

}  // namespace AetherSDR
