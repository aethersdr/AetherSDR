#include "PskReporterClient.h"

#ifdef HAVE_MQTT
#include "MqttClient.h"
#endif

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace AetherSDR {

PskReporterClient::PskReporterClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &PskReporterClient::poll);
}

void PskReporterClient::setCallsign(const QString& callsign)
{
    if (m_callsign == callsign.trimmed().toUpper()) {
        return;
    }
    m_callsign = callsign.trimmed().toUpper();
    m_spots.clear();
    m_lastSeqNo = -1;
    emit spotsUpdated();
    if (m_running) {
        // Restart on the new callsign.
        const int interval = m_intervalMs;
        stop();
        start(interval);
    }
}

void PskReporterClient::start(int intervalMs)
{
    stop();
    if (m_callsign.isEmpty()) {
        emit statusChanged(tr("No callsign — connect to a radio first"));
        return;
    }
    m_intervalMs = (intervalMs == kLiveMqtt)
                       ? kLiveMqtt
                       : std::max(intervalMs, kMinPollMs);
    m_running = true;

    if (m_intervalMs == kLiveMqtt) {
        startMqtt();
        return;
    }
    m_timer.start(m_intervalMs);
    poll();
}

void PskReporterClient::stop()
{
    m_running = false;
    m_timer.stop();
    stopMqtt();
}

void PskReporterClient::poll()
{
    if (m_fetchInFlight || m_callsign.isEmpty()) {
        return;
    }
    m_fetchInFlight = true;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("senderCallsign"), m_callsign);
    query.addQueryItem(QStringLiteral("rronly"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("noactive"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("appcontact"),
                       QStringLiteral("ki6bcj@aethersdr.com"));
    if (m_lastSeqNo >= 0) {
        query.addQueryItem(QStringLiteral("lastseqno"),
                           QString::number(m_lastSeqNo));
    } else {
        // First fetch: backfill up to the selected window (max 24h allowed;
        // an hour covers every offered interval).
        query.addQueryItem(QStringLiteral("flowStartSeconds"),
                           QStringLiteral("-3600"));
    }

    QUrl url{ QString::fromLatin1(kQueryUrl) };
    url.setQuery(query);
    QNetworkRequest req{ url };
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("AetherSDR/%1")
                      .arg(QCoreApplication::applicationVersion()));
    req.setRawHeader("Accept-Encoding", "gzip");

    emit statusChanged(tr("Updating…"));
    auto* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_fetchInFlight = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit statusChanged(tr("PSK Reporter error: %1")
                                   .arg(reply->errorString()));
            return;
        }
        handleQueryReply(reply->readAll());
    });
}

void PskReporterClient::handleQueryReply(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    int added = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }
        const auto& attrs = reader.attributes();
        if (reader.name() == QLatin1String("lastSequenceNumber")) {
            m_lastSeqNo = attrs.value(QLatin1String("value")).toLongLong();
            continue;
        }
        if (reader.name() != QLatin1String("receptionReport")) {
            continue;
        }
        PskReporterSpot spot;
        spot.receiverCallsign =
            attrs.value(QLatin1String("receiverCallsign")).toString();
        spot.receiverLocator =
            attrs.value(QLatin1String("receiverLocator")).toString();
        spot.senderCallsign =
            attrs.value(QLatin1String("senderCallsign")).toString();
        spot.senderLocator =
            attrs.value(QLatin1String("senderLocator")).toString();
        spot.mode = attrs.value(QLatin1String("mode")).toString();
        spot.frequencyHz =
            attrs.value(QLatin1String("frequency")).toLongLong();
        if (attrs.hasAttribute(QLatin1String("sNR"))) {
            spot.snr = attrs.value(QLatin1String("sNR")).toInt();
        }
        spot.flowStartSeconds =
            attrs.value(QLatin1String("flowStartSeconds")).toLongLong();
        if (!spot.receiverCallsign.isEmpty()) {
            appendSpot(spot);
            ++added;
        }
    }
    pruneOldSpots();
    emit statusChanged(tr("Updated %1 (%2 new, %3 total)")
                           .arg(QDateTime::currentDateTime()
                                    .toString(QStringLiteral("hh:mm")))
                           .arg(added)
                           .arg(m_spots.size()));
    emit spotsUpdated();
}

void PskReporterClient::startMqtt()
{
#ifdef HAVE_MQTT
    if (m_mqtt == nullptr) {
        m_mqtt = new MqttClient(this);
        connect(m_mqtt, &MqttClient::messageReceived,
                this, &PskReporterClient::handleMqttMessage);
        connect(m_mqtt, &MqttClient::connected, this, [this] {
            emit statusChanged(tr("Live (MQTT) — connected"));
        });
        connect(m_mqtt, &MqttClient::connectionError, this,
                [this](const QString& err) {
                    emit statusChanged(tr("MQTT error: %1").arg(err));
                });
    }
    // Live feed has no backfill; seed the window with one HTTP query.
    poll();
    m_mqtt->setSubscriptions(
        { QStringLiteral("pskr/filter/v2/+/+/%1/#").arg(m_callsign) });
    m_mqtt->connectToBroker(QString::fromLatin1(kMqttHost), kMqttTlsPort,
                            {}, {}, /*useTls=*/true);
    emit statusChanged(tr("Live (MQTT) — connecting…"));
#else
    emit statusChanged(tr("MQTT support not built in"));
#endif
}

void PskReporterClient::stopMqtt()
{
#ifdef HAVE_MQTT
    if (m_mqtt != nullptr) {
        m_mqtt->disconnect();
    }
#endif
}

void PskReporterClient::handleMqttMessage(const QString& topic,
                                          const QByteArray& payload)
{
    Q_UNUSED(topic);
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    PskReporterSpot spot;
    spot.senderCallsign = o.value(QLatin1String("sc")).toString();
    spot.senderLocator = o.value(QLatin1String("sl")).toString();
    spot.receiverCallsign = o.value(QLatin1String("rc")).toString();
    spot.receiverLocator = o.value(QLatin1String("rl")).toString();
    spot.mode = o.value(QLatin1String("md")).toString();
    spot.frequencyHz =
        static_cast<qint64>(o.value(QLatin1String("f")).toDouble());
    spot.snr = o.value(QLatin1String("rp")).toInt(-999);
    spot.flowStartSeconds =
        static_cast<qint64>(o.value(QLatin1String("t")).toDouble());
    if (spot.receiverCallsign.isEmpty()) {
        return;
    }
    appendSpot(spot);
    pruneOldSpots();
    emit spotsUpdated();
}

void PskReporterClient::appendSpot(const PskReporterSpot& spot)
{
    // Replace any older report from the same receiver so the map shows one
    // marker per reporting station.
    auto it = std::find_if(m_spots.begin(), m_spots.end(),
                           [&spot](const PskReporterSpot& s) {
                               return s.receiverCallsign
                                   == spot.receiverCallsign;
                           });
    if (it != m_spots.end()) {
        if (spot.flowStartSeconds >= it->flowStartSeconds) {
            *it = spot;
        }
        return;
    }
    m_spots.append(spot);
}

void PskReporterClient::pruneOldSpots()
{
    const qint64 cutoff =
        QDateTime::currentSecsSinceEpoch() - 24 * 60 * 60;
    m_spots.erase(std::remove_if(m_spots.begin(), m_spots.end(),
                                 [cutoff](const PskReporterSpot& s) {
                                     return s.flowStartSeconds < cutoff;
                                 }),
                  m_spots.end());
    while (m_spots.size() > kMaxSpots) {
        m_spots.removeFirst();
    }
}

} // namespace AetherSDR
