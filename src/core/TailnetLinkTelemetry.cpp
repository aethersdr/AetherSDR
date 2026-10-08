#include "TailnetLinkTelemetry.h"

#include "TailnetAddress.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr int kPollIntervalMs = 2000;
// One poll in flight at a time; a reply slower than this is a failed poll.
constexpr int kTransferTimeoutMs = 3000;
// Three missed polls: the shim stopped answering, or the tunnel went away.
constexpr qint64 kStaleAfterMs = 6500;
// A report is a few KB; a shim that sends more is not trusted to.
constexpr qint64 kMaxReplyBytes = 256 * 1024;
}  // namespace

namespace tailnetshim {

std::optional<TailnetSessionReport> parseSessionReport(const QByteArray& json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("version")).isString()
        || !o.value(QStringLiteral("sessions")).isArray()) {
        return std::nullopt;
    }
    TailnetSessionReport r;
    r.version = o.value(QStringLiteral("version")).toString().left(128);
    r.path = o.value(QStringLiteral("path")).toString().left(128);
    r.endpoint = o.value(QStringLiteral("endpoint")).toString().left(128);
    r.relay = o.value(QStringLiteral("relay")).toString().left(128);
    r.rttMs = o.value(QStringLiteral("rtt_ms")).toDouble(-1.0);
    r.rttVia = o.value(QStringLiteral("rtt_via")).toString().left(128);
    r.rttAgeS = o.value(QStringLiteral("rtt_age_s")).toDouble();
    r.pathChanges = o.value(QStringLiteral("path_changes")).toInt();
    r.pathSinceS = o.value(QStringLiteral("path_since_s")).toDouble();
    r.toClientKbps = o.value(QStringLiteral("to_client_kbps")).toDouble();
    r.fromClientKbps = o.value(QStringLiteral("from_client_kbps")).toDouble();
    r.lastHandshakeS = o.value(QStringLiteral("last_handshake_s")).toDouble();
    r.shimCpuPct = o.value(QStringLiteral("shim_cpu_pct")).toDouble();
    r.shimRssKb = o.value(QStringLiteral("shim_rss_kb")).toInteger();
    r.mtuClamp = o.value(QStringLiteral("mtu_clamp")).toInt();
    const QJsonArray sessions = o.value(QStringLiteral("sessions")).toArray();
    r.sessions = sessions.size();
    for (const QJsonValue& sv : sessions) {
        const QJsonObject s = sv.toObject();
        r.sendFailures += static_cast<quint64>(
            std::max<qint64>(0, s.value(QStringLiteral("to_client_failures")).toInteger()));
        for (const QJsonValue& stv : s.value(QStringLiteral("streams")).toArray()) {
            const QJsonObject st = stv.toObject();
            r.radioPackets += static_cast<quint64>(
                std::max<qint64>(0, st.value(QStringLiteral("packets")).toInteger()));
            r.radioBreaks += static_cast<quint64>(
                std::max<qint64>(0, st.value(QStringLiteral("breaks")).toInteger()));
            r.radioGaps += static_cast<quint64>(
                std::max<qint64>(0, st.value(QStringLiteral("gaps")).toInteger()));
        }
    }
    return r;
}

double tunnelBreakPercent(qint64 clientPackets, qint64 clientBreaks,
                          qint64 radioPackets, qint64 radioBreaks)
{
    if (clientPackets <= 0) {
        return 0.0;
    }
    const double client = std::max<qint64>(0, clientBreaks) * 100.0 / clientPackets;
    const double radio = radioPackets > 0
        ? std::max<qint64>(0, radioBreaks) * 100.0 / radioPackets
        : 0.0;
    return std::max(0.0, client - radio);
}

}  // namespace tailnetshim

TailnetLinkTelemetry::TailnetLinkTelemetry(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTransferTimeoutMs);
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_timer.setInterval(kPollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &TailnetLinkTelemetry::poll);
}

void TailnetLinkTelemetry::setRadioAddress(const QHostAddress& address)
{
    const bool tailnet = !address.isNull() && isTailnetAddress(address);
    if (tailnet && address == m_address && m_timer.isActive()) {
        return;
    }
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_last.reset();
    m_lastOkMs = 0;
    if (!tailnet) {
        m_address = QHostAddress();
        m_timer.stop();
        return;
    }
    m_address = address;
    m_timer.start();
    poll();
}

std::optional<TailnetSessionReport> TailnetLinkTelemetry::current() const
{
    if (!m_last || QDateTime::currentMSecsSinceEpoch() - m_lastOkMs > kStaleAfterMs) {
        return std::nullopt;
    }
    return m_last;
}

void TailnetLinkTelemetry::poll()
{
    if (m_reply || m_address.isNull()) {
        return;
    }
    // The same address the radio connection uses, so the shim sees this
    // client at the address its relay session is keyed by.
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimTelemetryPort);
    url.setPath(QStringLiteral("/v1/session"));
    m_reply = m_nam.get(QNetworkRequest(url));
    connect(m_reply, &QNetworkReply::downloadProgress, m_reply.data(),
            [reply = m_reply.data()](qint64 received, qint64) {
        if (received > kMaxReplyBytes) {
            reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data()] {
        if (reply != m_reply) {
            return;
        }
        m_reply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;   // current() goes stale on its own
        }
        if (auto report = tailnetshim::parseSessionReport(reply->readAll())) {
            m_last = std::move(report);
            m_lastOkMs = QDateTime::currentMSecsSinceEpoch();
            ++m_serial;
        }
    });
}

}  // namespace AetherSDR
