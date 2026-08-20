#include "PskReporterClient.h"

#ifdef HAVE_MQTT
#include "MqttClient.h"
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <QLoggingCategory>

#include <algorithm>
#include <limits>

Q_LOGGING_CATEGORY(lcPskReporter, "aether.pskreporter")

namespace AetherSDR {

namespace {
// Stall timeout for PSK Reporter queries (#4688 §6). 30 s rather than the 15 s
// the other interactive clients use: Qt's clock also covers the wait for the
// FIRST byte, and the initial backfill can ask for the full 24 h window
// (kMaxLookbackSec), which retrieve.pskreporter.info spends real server-side
// time assembling before it writes anything. The polling floor is already five
// minutes (kMinPollMs) for the same reason.
constexpr int kTransferTimeoutMs = 30000;
constexpr int kCoordinationRetryMs = 1000;
} // namespace

PskReporterClient::PskReporterClient(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTransferTimeoutMs);
    connect(&m_timer, &QTimer::timeout, this, &PskReporterClient::poll);
    m_httpThrottleTimer.setSingleShot(true);
    connect(&m_httpThrottleTimer, &QTimer::timeout,
            this, &PskReporterClient::poll);
    loadHttpThrottleState();

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
    const QString normalized = callsign.trimmed().toUpper().left(32);
    const QueryScope scope = normalized.isEmpty() ? QueryScope::Anyone
                                                   : QueryScope::Callsign;
    if (m_callsign == normalized && m_scope == scope) {
        return;
    }
    if (m_cacheDirty) {
        saveCache();
    }
    m_callsign = normalized;
    m_scope = scope;
    m_spots.clear();
    m_monitors.clear();
    m_resultsLimited = false;
    m_lastSeqNo = -1;
    emit spotsUpdated();
}

void PskReporterClient::setLookbackSeconds(int seconds)
{
    const int clamped = std::clamp(seconds, 60, kMaxLookbackSec);
    if (clamped == m_lookbackSec) {
        return;
    }
    m_lookbackSec = clamped;
    // Only hit the network when the new window is DEEPER than anything we've
    // already fetched this session. Narrowing — or revisiting a window we've
    // covered — is just a display change (the dialog filters spots() by the
    // current lookback), so it costs nothing and won't trip PSK Reporter's
    // rate limiter.
    if (m_running && clamped > m_fetchedLookbackSec) {
        m_lastSeqNo = -1;  // force a deep backfill at the new depth
        poll();
    }
    pruneOldSpots();
    emit spotsUpdated();
}

bool PskReporterClient::isMqttConnected() const
{
#ifdef HAVE_MQTT
    return m_mqtt != nullptr && m_mqtt->isConnected();
#else
    return false;
#endif
}

QString PskReporterClient::transport() const
{
    return (isLive() && isMqttConnected()) ? QStringLiteral("MQTT")
                                           : QStringLiteral("HTTP");
}

QDateTime PskReporterClient::lastHttpRequestAt() const
{
    return m_lastHttpRequestEpochMs > 0
        ? QDateTime::fromMSecsSinceEpoch(m_lastHttpRequestEpochMs)
        : QDateTime();
}

QDateTime PskReporterClient::nextHttpRequestAt() const
{
    return m_nextHttpAllowedEpochMs > 0
        ? QDateTime::fromMSecsSinceEpoch(m_nextHttpAllowedEpochMs)
        : QDateTime();
}

void PskReporterClient::start(int intervalMs)
{
    stop();
    // The global MQTT wildcard is hundreds of reports per second.  "Anyone"
    // therefore uses the bounded, policy-compliant HTTP snapshot even when
    // the UI's update selector is set to Live.
    m_intervalMs = (intervalMs == kLiveMqtt && m_scope == QueryScope::Anyone)
                       ? kMinPollMs
                       : (intervalMs == kLiveMqtt)
                       ? kLiveMqtt
                       : std::max(intervalMs, kMinPollMs);
    m_running = true;

    // Always do a fresh deep HTTP backfill on (re)start — including in Live
    // mode — so opening the window immediately repopulates the lookback
    // window instead of waiting for new live spots. (Without this, a reopen
    // kept the prior session's lastSeqNo and only fetched newer records.)
    m_lastSeqNo = -1;
    m_fetchedLookbackSec = 0;  // the open's first query establishes the depth

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
    ++m_queryGeneration;
    m_running = false;
    m_timer.stop();
    m_saveTimer.stop();
    m_httpThrottleTimer.stop();
    stopMqtt();
    if (m_queryReply != nullptr) {
        m_queryReply->abort();
    }
    if (m_cacheDirty) {
        saveCache();
    }
}

void PskReporterClient::poll()
{
    if (m_fetchInFlight) {
        return;
    }
    {
        // Coordinate every AetherSDR process through one short-held lock.
        // Without this claim, two instances whose timers expire together can
        // both read the old deadline and send simultaneously.
        const QString throttlePath = httpThrottleStateFilePath();
        QDir().mkpath(QFileInfo(throttlePath).absolutePath());
        QLockFile requestLock(throttlePath + QStringLiteral(".lock"));
        requestLock.setStaleLockTime(kTransferTimeoutMs * 2);
        if (!requestLock.tryLock(0)) {
            m_nextHttpAllowedEpochMs = qMax(
                m_nextHttpAllowedEpochMs,
                QDateTime::currentMSecsSinceEpoch() + kCoordinationRetryMs);
            scheduleHttpPollAtAllowedTime();
            emit statusChanged(tr("HTTP refresh queued behind another "
                                  "AetherSDR instance"));
            emit connectionStateChanged();
            return;
        }

        // Another process may have queried since this client started. Merge
        // its shared deadline while holding the claim lock.
        loadHttpThrottleState();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs < m_nextHttpAllowedEpochMs) {
            scheduleHttpPollAtAllowedTime();
            const int cachedStations = m_spots.size() + m_monitors.size();
            const QString nextTime = nextHttpRequestAt().toLocalTime().toString(
                QStringLiteral("hh:mm:ss"));
            emit statusChanged(cachedStations > 0
                ? tr("Showing %1 cached station(s) — HTTP refresh throttled until %2")
                      .arg(cachedStations).arg(nextTime)
                : tr("HTTP refresh throttled until %1").arg(nextTime));
            emit connectionStateChanged();
            return;
        }
        m_lastHttpRequestEpochMs = nowMs;
        m_nextHttpAllowedEpochMs = nowMs + kMinPollMs;
        saveHttpThrottleState();
    }
    m_fetchInFlight = true;
    m_httpThrottleTimer.stop();
    m_lastHttpStatus = 0;
    m_lastHttpError.clear();

    QUrlQuery query;
    if (m_scope == QueryScope::Callsign) {
        // Generic callsign matches both sent-by and received-by reports, as
        // the public PSK Reporter map does.
        query.addQueryItem(QStringLiteral("callsign"), m_callsign);
        query.addQueryItem(QStringLiteral("rronly"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("noactive"), QStringLiteral("1"));
    } else {
        query.addQueryItem(QStringLiteral("rptlimit"),
                           QString::number(kMaxSpots));
    }
    query.addQueryItem(QStringLiteral("appcontact"),
                       QStringLiteral("ki6bcj@aethersdr.com"));
    const bool initial = m_lastSeqNo < 0;
    const int fetchDepth = m_lookbackSec;  // depth this query backfills
    if (!initial) {
        // Incremental: only records newer than the last sequence number.
        query.addQueryItem(QStringLiteral("lastseqno"),
                           QString::number(m_lastSeqNo));
    } else {
        // Initial fetch: backfill the full selected lookback window.
        query.addQueryItem(QStringLiteral("flowStartSeconds"),
                           QString::number(-fetchDepth));
    }

    QUrl url{ QString::fromLatin1(kQueryUrl) };
    url.setQuery(query);
    QNetworkRequest req{ url };
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("AetherSDR/%1")
                      .arg(QCoreApplication::applicationVersion()));
    // Do NOT set Accept-Encoding manually: Qt auto-negotiates gzip/deflate
    // and transparently decompresses the reply, but only if we leave the
    // header alone. Setting it ourselves disables that, leaving raw gzip
    // bytes that fail XML parsing ("incorrectly encoded content").

    qCInfo(lcPskReporter) << "HTTP query"
                          << (initial ? "(initial)" : "(incremental)")
                          << "scope"
                          << (m_scope == QueryScope::Anyone ? "anyone" : "callsign")
                          << m_callsign
                          << "url" << url.toString(QUrl::RemoveQuery);
    emit statusChanged(tr("Updating…"));
    emit connectionStateChanged();
    QNetworkReply* reply = m_nam.get(req);
    m_queryReply = reply;
    const quint64 generation = m_queryGeneration;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, initial, fetchDepth, generation] {
        m_fetchInFlight = false;
        if (m_queryReply == reply) {
            m_queryReply = nullptr;
        }
        reply->deleteLater();
        if (generation != m_queryGeneration) {
            if (m_running) {
                QTimer::singleShot(0, this, &PskReporterClient::poll);
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            const int httpStatus = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_lastHttpStatus = httpStatus;
            m_lastHttpError = reply->errorString();
            qCWarning(lcPskReporter)
                << "HTTP error:" << reply->errorString()
                << "status" << httpStatus;
            m_lastHttpOk = false;
            m_sawError = true;
            emit connectionStateChanged();
            if (httpStatus == 429) {
                ++m_consecutiveRateLimits;
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                qint64 retryAfterMs = 0;
                const QByteArray retryHeader =
                    reply->rawHeader("Retry-After").trimmed();
                bool retrySecondsOk = false;
                const qint64 retrySeconds =
                    retryHeader.toLongLong(&retrySecondsOk);
                if (retrySecondsOk && retrySeconds > 0) {
                    retryAfterMs = retrySeconds * 1000;
                } else if (!retryHeader.isEmpty()) {
                    const QDateTime retryDate = QDateTime::fromString(
                        QString::fromLatin1(retryHeader), Qt::RFC2822Date);
                    if (retryDate.isValid()) {
                        retryAfterMs = qMax<qint64>(
                            0, retryDate.toMSecsSinceEpoch() - nowMs);
                    }
                }
                // Retry-After is optional. Without a fallback, a server that
                // omits it is hit again at the same five-minute cadence that
                // just failed. Persist an exponential 15m..2h backoff so all
                // AetherSDR processes share the more conservative deadline.
                const qint64 delayMs = qMax(
                    retryAfterMs, rateLimitBackoffMs(m_consecutiveRateLimits));
                m_nextHttpAllowedEpochMs = qMax(
                    m_nextHttpAllowedEpochMs, nowMs + delayMs);
                saveHttpThrottleState();
                scheduleHttpPollAtAllowedTime();
                const int cachedStations = m_spots.size() + m_monitors.size();
                emit statusChanged(cachedStations > 0
                    ? tr("PSK Reporter rate limit — showing %1 cached "
                         "station(s); retrying at the next update")
                          .arg(cachedStations)
                    : tr("PSK Reporter rate limit — retrying at the next update"));
            } else {
                emit statusChanged(tr("PSK Reporter error: %1")
                                       .arg(reply->errorString()));
            }
            return;
        }
        m_lastHttpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        m_lastHttpError.clear();
        if (m_consecutiveRateLimits != 0) {
            m_consecutiveRateLimits = 0;
            saveHttpThrottleState();
        }
        if (initial) {
            // We now hold history back to this depth; record it so narrowing
            // and re-widening within it won't re-query.
            m_fetchedLookbackSec = qMax(m_fetchedLookbackSec, fetchDepth);
        }
        m_lastHttpOk = true;
        m_sawError = false;
        emit connectionStateChanged();
        const QByteArray body = reply->readAll();
        qCInfo(lcPskReporter) << "HTTP reply" << body.size() << "bytes";
        handleQueryReply(body);
    });
}

void PskReporterClient::scheduleHttpPollAtAllowedTime()
{
    if (!m_running) {
        return;
    }
    const qint64 remainingMs = qMax<qint64>(
        0, m_nextHttpAllowedEpochMs - QDateTime::currentMSecsSinceEpoch());
    m_httpThrottleTimer.start(static_cast<int>(
        qMin<qint64>(remainingMs, std::numeric_limits<int>::max())));
}

qint64 PskReporterClient::rateLimitBackoffMs(int consecutiveRateLimits)
{
    const int exponent = std::clamp(consecutiveRateLimits - 1, 0, 8);
    return qMin(kRateLimitBaseMs * (1LL << exponent), kRateLimitMaxMs);
}

QString PskReporterClient::httpThrottleStateFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
         + QDir::separator()
         + QStringLiteral("psk-reporter-http-throttle.json");
}

void PskReporterClient::loadHttpThrottleState()
{
    QFile file(httpThrottleStateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const qint64 lastRequest = static_cast<qint64>(
        root.value(QLatin1String("lastRequestMs")).toDouble());
    const qint64 nextAllowed = static_cast<qint64>(
        root.value(QLatin1String("nextAllowedMs")).toDouble());
    const int rateLimitCount =
        root.value(QLatin1String("rateLimitCount")).toInt();
    m_lastHttpRequestEpochMs = qMax(m_lastHttpRequestEpochMs, lastRequest);
    m_nextHttpAllowedEpochMs = qMax(m_nextHttpAllowedEpochMs, nextAllowed);
    m_consecutiveRateLimits = qMax(
        m_consecutiveRateLimits, rateLimitCount);
}

void PskReporterClient::saveHttpThrottleState() const
{
    const QString path = httpThrottleStateFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcPskReporter) << "could not write HTTP throttle state:"
                                << path;
        return;
    }
    QJsonObject root;
    root[QLatin1String("lastRequestMs")] =
        static_cast<double>(m_lastHttpRequestEpochMs);
    root[QLatin1String("nextAllowedMs")] =
        static_cast<double>(m_nextHttpAllowedEpochMs);
    root[QLatin1String("rateLimitCount")] = m_consecutiveRateLimits;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        qCWarning(lcPskReporter) << "could not commit HTTP throttle state:"
                                << path;
    }
}

void PskReporterClient::handleQueryReply(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    int added = 0;
    int parsedReports = 0;
    QVector<PskReporterMonitor> monitors;
    QSet<QString> monitorKeys;
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
        if (reader.name() == QLatin1String("activeReceiver")) {
            PskReporterMonitor monitor;
            monitor.callsign = attrs.value(QLatin1String("callsign"))
                                   .toString().trimmed().toUpper().left(32);
            monitor.locator = attrs.value(QLatin1String("locator"))
                                  .toString().trimmed().toUpper().left(12);
            monitor.mode = attrs.value(QLatin1String("mode"))
                               .toString().trimmed().left(32);
            monitor.decoderSoftware =
                attrs.value(QLatin1String("decoderSoftware"))
                    .toString().trimmed().left(96);
            monitor.frequencyHz =
                attrs.value(QLatin1String("frequency")).toLongLong();
            const QString key = monitor.callsign + QLatin1Char('|')
                              + monitor.locator;
            if (!monitor.callsign.isEmpty() && !monitor.locator.isEmpty()
                && !monitorKeys.contains(key)
                && monitors.size() < kMaxMonitors) {
                monitorKeys.insert(key);
                monitors.append(monitor);
            }
            continue;
        }
        if (reader.name() != QLatin1String("receptionReport")) {
            continue;
        }
        ++parsedReports;
        if (parsedReports > kMaxSpots) {
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
    if (m_scope == QueryScope::Anyone) {
        m_monitors = monitors;
        m_cacheDirty = true;
    }
    m_resultsLimited = parsedReports > kMaxSpots;
    qCInfo(lcPskReporter) << "HTTP parsed" << added << "reception reports,"
                          << m_spots.size() << "total, lastSeqNo" << m_lastSeqNo;
    if (reader.hasError()) {
        qCWarning(lcPskReporter) << "XML parse error:" << reader.errorString();
    }
    emit statusChanged(m_scope == QueryScope::Anyone
                           ? tr("Updated %1 (%2 reports, %3 active monitors)%4")
                                 .arg(QDateTime::currentDateTime()
                                          .toString(QStringLiteral("hh:mm")))
                                 .arg(m_spots.size())
                                 .arg(m_monitors.size())
                                 .arg(m_resultsLimited ? tr(" — report limit reached")
                                                       : QString())
                           : tr("Updated %1 (%2 new, %3 total)")
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
            // Live feed is up — stop the HTTP fallback poller.
            m_timer.stop();
            m_sawError = false;
            emit connectionStateChanged();
            emit statusChanged(tr("Live (MQTT) — connected"));
        });
        connect(m_mqtt, &MqttClient::disconnected, this, [this] {
            qCWarning(lcPskReporter) << "MQTT disconnected from" << kMqttHost;
            startFallbackPolling();
            emit connectionStateChanged();
            emit statusChanged(tr("Live — reconnecting (polling meanwhile)…"));
        });
        connect(m_mqtt, &MqttClient::connectionError, this,
                [this](const QString& err) {
                    qCWarning(lcPskReporter) << "MQTT error:" << err
                                             << "— falling back to HTTP polling";
                    m_sawError = true;
                    startFallbackPolling();
                    emit connectionStateChanged();
                    emit statusChanged(tr("Live unavailable — polling every 5 min"));
                });
    }
    // Live feed has no backfill; seed the window with one HTTP query.
    poll();
    m_mqtt->setSubscriptions({
        QStringLiteral("pskr/filter/v2/+/+/%1/#").arg(m_callsign),
        QStringLiteral("pskr/filter/v2/+/+/+/%1/#").arg(m_callsign)
    });
    m_mqtt->connectToBroker(QString::fromLatin1(kMqttHost), kMqttPort,
                            {}, {}, /*useTls=*/false);
    m_mqttMsgWindow = 0;
    m_mqttHealthTimer.start();
    // Safety net: poll over HTTP until MQTT confirms it's connected (and if
    // it never does — e.g. the network blocks MQTT — keep polling). The
    // connected() handler stops this timer.
    startFallbackPolling();
    emit statusChanged(tr("Live (MQTT) — connecting…"));
#else
    emit statusChanged(tr("MQTT support not built in"));
#endif
}

void PskReporterClient::startFallbackPolling()
{
    // Only meaningful in Live mode; the explicit poll tiers manage m_timer
    // themselves. Don't double-start.
    if (m_intervalMs != kLiveMqtt || m_timer.isActive()) {
        return;
    }
    m_timer.start(kFallbackPollMs);
    poll();
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
    // Retain one latest report for each sender/receiver pair.  Generic
    // callsign queries can return reports in either direction, and the
    // anyone view needs distinct paths rather than one receiver-only record.
    auto it = std::find_if(m_spots.begin(), m_spots.end(),
                           [&spot](const PskReporterSpot& s) {
                               return s.receiverCallsign == spot.receiverCallsign
                                   && s.senderCallsign == spot.senderCallsign;
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
    // Retain spots back to the deepest window we've fetched (not just the
    // current lookback), so narrowing then widening shows the data again
    // without another network query. The dialog filters the *display* to the
    // current lookback.
    const int retainSec = qMax(m_lookbackSec, m_fetchedLookbackSec);
    const qint64 cutoff =
        QDateTime::currentSecsSinceEpoch() - retainSec;
    const int before = m_spots.size();
    m_spots.erase(std::remove_if(m_spots.begin(), m_spots.end(),
                                 [cutoff](const PskReporterSpot& s) {
                                     return s.flowStartSeconds < cutoff;
                                 }),
                  m_spots.end());
    while (m_spots.size() > kMaxSpots) {
        const auto oldest = std::min_element(
            m_spots.begin(), m_spots.end(),
            [](const PskReporterSpot& a, const PskReporterSpot& b) {
                return a.flowStartSeconds < b.flowStartSeconds;
            });
        m_spots.erase(oldest);
    }
    if (m_spots.size() != before) {
        m_cacheDirty = true;
    }
}

QString PskReporterClient::cacheFilePath() const
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString scopeKey = m_scope == QueryScope::Anyone
        ? QStringLiteral("anyone")
        : QString::fromLatin1(QCryptographicHash::hash(
              m_callsign.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    return dir + QDir::separator()
         + QStringLiteral("psk-reporter-spots-%1.json").arg(scopeKey);
}

void PskReporterClient::loadCache()
{
    const QString scopedPath = cacheFilePath();
    QFile f(scopedPath);
    bool legacyCache = false;
    if (!f.open(QIODevice::ReadOnly)) {
        // One-time migration from the original shared cache. Its embedded
        // callsign still prevents cross-scope data from being restored.
        f.setFileName(QFileInfo(scopedPath).absolutePath()
                      + QDir::separator()
                      + QStringLiteral("psk-reporter-spots.json"));
        if (!f.open(QIODevice::ReadOnly)) {
            return;
        }
        legacyCache = true;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    // Only restore the cache when it belongs to the current callsign.
    if (root.value(QLatin1String("callsign")).toString() != m_callsign) {
        return;
    }
    const qint64 cutoff =
        QDateTime::currentSecsSinceEpoch() - m_lookbackSec;
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
    const qint64 savedAt = static_cast<qint64>(
        root.value(QLatin1String("saved")).toDouble());
    const bool monitorSnapshotFresh = savedAt > 0
        && savedAt >= QDateTime::currentSecsSinceEpoch()
                         - (2 * kMinPollMs / 1000);
    if (m_scope == QueryScope::Anyone && monitorSnapshotFresh) {
        const QJsonArray monitorArray =
            root.value(QLatin1String("monitors")).toArray();
        for (const QJsonValue& value : monitorArray) {
            const QJsonObject o = value.toObject();
            PskReporterMonitor monitor;
            monitor.callsign = o.value(QLatin1String("c")).toString();
            monitor.locator = o.value(QLatin1String("l")).toString();
            monitor.mode = o.value(QLatin1String("m")).toString();
            monitor.decoderSoftware =
                o.value(QLatin1String("s")).toString();
            monitor.frequencyHz = static_cast<qint64>(
                o.value(QLatin1String("f")).toDouble());
            if (!monitor.callsign.isEmpty() && !monitor.locator.isEmpty()
                && m_monitors.size() < kMaxMonitors) {
                m_monitors.append(monitor);
            }
        }
    }
    m_cacheDirty = legacyCache;
    qCInfo(lcPskReporter) << "loaded" << loaded << "cached spots for"
                          << m_callsign << "and" << m_monitors.size()
                          << "cached active monitors";
    if (loaded > 0 || !m_monitors.isEmpty()) {
        emit spotsUpdated();
    }
}

void PskReporterClient::saveCache()
{
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
    if (m_scope == QueryScope::Anyone) {
        QJsonArray monitorArray;
        for (const PskReporterMonitor& monitor : std::as_const(m_monitors)) {
            QJsonObject o;
            o[QLatin1String("c")] = monitor.callsign;
            o[QLatin1String("l")] = monitor.locator;
            o[QLatin1String("m")] = monitor.mode;
            o[QLatin1String("s")] = monitor.decoderSoftware;
            o[QLatin1String("f")] = static_cast<double>(monitor.frequencyHz);
            monitorArray.append(o);
        }
        root[QLatin1String("monitors")] = monitorArray;
    }

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
