#include "PskReporterClient.h"

#ifdef HAVE_MQTT
#include "MqttClient.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPskReporter, "aether.pskreporter")

namespace AetherSDR {

PskReporterClient::PskReporterClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &PskReporterClient::poll);

    // Five-minute MQTT health summary — enough to spot a dead or flapping
    // feed in the logs without per-spot noise.
    m_mqttHealthTimer.setInterval(5 * 60 * 1000);
    connect(&m_mqttHealthTimer, &QTimer::timeout, this, [this] {
#ifdef HAVE_MQTT
        const qint64 lastAge = m_mqttLastMsgEpoch > 0
            ? QDateTime::currentSecsSinceEpoch() - m_mqttLastMsgEpoch
            : -1;
        qCInfo(lcPskReporter)
            << "MQTT health:" << m_mqttMsgWindow << "spots in last 5m,"
            << m_mqttMsgTotal << "total,"
            << "connected=" << (m_mqtt != nullptr && m_mqtt->isConnected())
            << "lastSpotAgeSec=" << lastAge;
        m_mqttMsgWindow = 0;
#endif
    });

    // Throttled disk persistence: flush the rolling spot set at most once
    // per kSaveIntervalMs while it is changing.
    m_saveTimer.setInterval(kSaveIntervalMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_cacheDirty) {
            saveCache();
        }
    });
}

PskReporterClient::~PskReporterClient()
{
    if (m_cacheDirty) {
        saveCache();
    }
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

    // Repopulate from the on-disk cache so the map isn't blank while the
    // first fetch / live feed warms up.
    if (m_spots.isEmpty()) {
        loadCache();
    }
    m_saveTimer.start();

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
    m_saveTimer.stop();
    stopMqtt();
    if (m_cacheDirty) {
        saveCache();
    }
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
    const bool initial = m_lastSeqNo < 0;
    if (!initial) {
        // Incremental: only records newer than the last sequence number.
        query.addQueryItem(QStringLiteral("lastseqno"),
                           QString::number(m_lastSeqNo));
    }
    // On the first fetch we deliberately omit flowStartSeconds so PSK
    // Reporter returns its default window (the last 100 reception records,
    // up to 6 hours) — a friendly initial population when the window opens.

    QUrl url{ QString::fromLatin1(kQueryUrl) };
    url.setQuery(query);
    QNetworkRequest req{ url };
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("AetherSDR/%1")
                      .arg(QCoreApplication::applicationVersion()));
    req.setRawHeader("Accept-Encoding", "gzip");

    qCInfo(lcPskReporter) << "HTTP query"
                          << (initial ? "(initial)" : "(incremental)")
                          << "senderCallsign" << m_callsign
                          << "url" << url.toString(QUrl::RemoveQuery);
    emit statusChanged(tr("Updating…"));
    auto* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_fetchInFlight = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPskReporter)
                << "HTTP error:" << reply->errorString()
                << "status"
                << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            emit statusChanged(tr("PSK Reporter error: %1")
                                   .arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        qCInfo(lcPskReporter) << "HTTP reply" << body.size() << "bytes";
        handleQueryReply(body);
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
    qCInfo(lcPskReporter) << "HTTP parsed" << added << "reception reports,"
                          << m_spots.size() << "total, lastSeqNo" << m_lastSeqNo;
    if (reader.hasError()) {
        qCWarning(lcPskReporter) << "XML parse error:" << reader.errorString();
    }
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
            qCInfo(lcPskReporter) << "MQTT connected to" << kMqttHost
                                  << "topic filter callsign" << m_callsign;
            emit statusChanged(tr("Live (MQTT) — connected"));
        });
        connect(m_mqtt, &MqttClient::disconnected, this, [this] {
            qCWarning(lcPskReporter) << "MQTT disconnected from" << kMqttHost;
            emit statusChanged(tr("Live (MQTT) — reconnecting…"));
        });
        connect(m_mqtt, &MqttClient::connectionError, this,
                [this](const QString& err) {
                    qCWarning(lcPskReporter) << "MQTT error:" << err;
                    emit statusChanged(tr("MQTT error: %1").arg(err));
                });
    }
    // Live feed has no backfill; seed the window with one HTTP query.
    poll();
    m_mqtt->setSubscriptions(
        { QStringLiteral("pskr/filter/v2/+/+/%1/#").arg(m_callsign) });
    m_mqtt->connectToBroker(QString::fromLatin1(kMqttHost), kMqttTlsPort,
                            {}, {}, /*useTls=*/true);
    m_mqttMsgWindow = 0;
    m_mqttHealthTimer.start();
    emit statusChanged(tr("Live (MQTT) — connecting…"));
#else
    emit statusChanged(tr("MQTT support not built in"));
#endif
}

void PskReporterClient::stopMqtt()
{
    m_mqttHealthTimer.stop();
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
    ++m_mqttMsgTotal;
    ++m_mqttMsgWindow;
    m_mqttLastMsgEpoch = QDateTime::currentSecsSinceEpoch();
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
            m_cacheDirty = true;
        }
        return;
    }
    m_spots.append(spot);
    m_cacheDirty = true;
}

void PskReporterClient::pruneOldSpots()
{
    const qint64 cutoff =
        QDateTime::currentSecsSinceEpoch() - kSpotTtlSeconds;
    const int before = m_spots.size();
    m_spots.erase(std::remove_if(m_spots.begin(), m_spots.end(),
                                 [cutoff](const PskReporterSpot& s) {
                                     return s.flowStartSeconds < cutoff;
                                 }),
                  m_spots.end());
    while (m_spots.size() > kMaxSpots) {
        m_spots.removeFirst();
    }
    if (m_spots.size() != before) {
        m_cacheDirty = true;
    }
}

QString PskReporterClient::cacheFilePath() const
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return dir + QDir::separator()
         + QStringLiteral("psk-reporter-spots.json");
}

void PskReporterClient::loadCache()
{
    if (m_callsign.isEmpty()) {
        return;
    }
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    // Only restore the cache when it belongs to the current callsign.
    if (root.value(QLatin1String("callsign")).toString() != m_callsign) {
        return;
    }
    const qint64 cutoff =
        QDateTime::currentSecsSinceEpoch() - kSpotTtlSeconds;
    const QJsonArray arr = root.value(QLatin1String("spots")).toArray();
    int loaded = 0;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        PskReporterSpot spot;
        spot.flowStartSeconds =
            static_cast<qint64>(o.value(QLatin1String("t")).toDouble());
        if (spot.flowStartSeconds < cutoff) {
            continue;  // tombstoned
        }
        spot.receiverCallsign = o.value(QLatin1String("rc")).toString();
        spot.receiverLocator = o.value(QLatin1String("rl")).toString();
        spot.senderCallsign = o.value(QLatin1String("sc")).toString();
        spot.senderLocator = o.value(QLatin1String("sl")).toString();
        spot.mode = o.value(QLatin1String("md")).toString();
        spot.frequencyHz =
            static_cast<qint64>(o.value(QLatin1String("f")).toDouble());
        spot.snr = o.value(QLatin1String("snr")).toInt(-999);
        if (!spot.receiverCallsign.isEmpty()) {
            appendSpot(spot);
            ++loaded;
        }
    }
    m_cacheDirty = false;
    qCInfo(lcPskReporter) << "loaded" << loaded << "cached spots for"
                          << m_callsign;
    if (loaded > 0) {
        emit spotsUpdated();
    }
}

void PskReporterClient::saveCache()
{
    if (m_callsign.isEmpty()) {
        return;
    }
    QJsonArray arr;
    for (const PskReporterSpot& s : std::as_const(m_spots)) {
        QJsonObject o;
        o[QLatin1String("rc")] = s.receiverCallsign;
        o[QLatin1String("rl")] = s.receiverLocator;
        o[QLatin1String("sc")] = s.senderCallsign;
        o[QLatin1String("sl")] = s.senderLocator;
        o[QLatin1String("md")] = s.mode;
        o[QLatin1String("f")] = static_cast<double>(s.frequencyHz);
        o[QLatin1String("snr")] = s.snr;
        o[QLatin1String("t")] = static_cast<double>(s.flowStartSeconds);
        arr.append(o);
    }
    QJsonObject root;
    root[QLatin1String("callsign")] = m_callsign;
    root[QLatin1String("saved")] =
        static_cast<double>(QDateTime::currentSecsSinceEpoch());
    root[QLatin1String("spots")] = arr;

    const QString path = cacheFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qCWarning(lcPskReporter) << "could not write spot cache:" << path;
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.commit();
    m_cacheDirty = false;
}

} // namespace AetherSDR
