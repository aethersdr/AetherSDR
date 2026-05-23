#include "FreeDvClient.h"
#include "LogManager.h"
#include "AppSettings.h"
#include <QCoreApplication>

#include <QJsonDocument>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <cmath>

namespace MasterSDR {

FreeDvClient::FreeDvClient(QObject* parent)
    : QObject(parent)
{
    // WebSocket + timers are created in initialize() on the SpotClients thread (#1929).
}

void FreeDvClient::initialize()
{
    if (m_ws) return;

    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_pingTimer = new QTimer(this);
    m_reconnectTimer = new QTimer(this);
    m_snrTimer = new QTimer(this);

    m_pingTimer->setSingleShot(false);
    m_reconnectTimer->setSingleShot(true);
    m_snrTimer->setSingleShot(false);
    m_snrTimer->setInterval(1000);

    connect(m_ws, &QWebSocket::connected, this, &FreeDvClient::onWsConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &FreeDvClient::onWsDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &FreeDvClient::onWsTextMessage);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_ws, &QWebSocket::errorOccurred, this, &FreeDvClient::onWsError);
#else
    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &FreeDvClient::onWsError);
#endif
    connect(m_reconnectTimer, &QTimer::timeout, this, &FreeDvClient::onReconnectTimer);
    connect(m_snrTimer,       &QTimer::timeout, this, &FreeDvClient::onSnrTimer);

    // Engine.IO ping keepalive — reply is handled in handleEngineIO
    connect(m_pingTimer, &QTimer::timeout, this, [this] {
        if (m_ws->state() == QAbstractSocket::ConnectedState)
            m_ws->sendTextMessage("2");  // client-side ping
    });
}

FreeDvClient::~FreeDvClient()
{
    stopConnection();
    m_logFile.close();
}

QString FreeDvClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/freedv.log";
}

void FreeDvClient::startConnection()
{
    if (m_connected.load()) return;

    qCDebug(lcDxCluster) << "FreeDvClient: connecting to qso.freedv.org";

    // Open log file (truncate on each start)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- FreeDV Reporter connected at %1 ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC")).toUtf8());
        m_logFile.flush();
    }

    m_stations.clear();
    m_intentionalDisconnect = false;
    m_reconnectAttempts = 0;
    m_ws->open(QUrl(WsUrl));
    emit started();
}

void FreeDvClient::stopConnection()
{
    m_intentionalDisconnect = true;
    if (m_pingTimer) m_pingTimer->stop();
    if (m_reconnectTimer) m_reconnectTimer->stop();

    if (m_ws && m_ws->state() != QAbstractSocket::UnconnectedState)
        m_ws->close();

    m_connected.store(false);
    m_stations.clear();
    emit stopped();
}

// ── WebSocket slots ─────────────────────────────────────────────────────

void FreeDvClient::onWsConnected()
{
    qCDebug(lcDxCluster) << "FreeDvClient: WebSocket connected";
    // Engine.IO open packet will arrive as the first text message
}

void FreeDvClient::onWsDisconnected()
{
    m_connected.store(false);
    m_pingTimer->stop();
    m_stations.clear();

    if (m_intentionalDisconnect) {
        qCDebug(lcDxCluster) << "FreeDvClient: disconnected (intentional)";
        return;
    }

    if (m_authNeedsRefresh) {
        // Reconnect immediately with updated auth (view↔full switch due to RADE activation)
        m_authNeedsRefresh = false;
        m_reconnectAttempts = 0;
        qCDebug(lcDxCluster) << "FreeDvClient: reconnecting with updated auth";
        m_ws->open(QUrl(WsUrl));
        return;
    }

    // Auto-reconnect with exponential backoff
    int delay = std::min(InitialReconnectDelayMs * (1 << m_reconnectAttempts),
                         MaxReconnectDelayMs);
    qCDebug(lcDxCluster) << "FreeDvClient: disconnected, reconnecting in" << delay << "ms";
    emit rawLineReceived(QString("--- Disconnected, reconnecting in %1s ---").arg(delay / 1000));
    m_reconnectTimer->start(delay);
}

void FreeDvClient::onWsError(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err);
    QString msg = m_ws->errorString();
    qCWarning(lcDxCluster) << "FreeDvClient: WebSocket error:" << msg;
    emit connectionError(msg);
}

void FreeDvClient::onReconnectTimer()
{
    if (m_intentionalDisconnect) return;
    m_reconnectAttempts++;
    qCDebug(lcDxCluster) << "FreeDvClient: reconnect attempt" << m_reconnectAttempts;
    m_ws->open(QUrl(WsUrl));
}

// ── Engine.IO / Socket.IO framing ───────────────────────────────────────

void FreeDvClient::onWsTextMessage(const QString& message)
{
    if (message.isEmpty()) return;
    handleEngineIO(message);
}

void FreeDvClient::handleEngineIO(const QString& raw)
{
    QChar type = raw.at(0);

    if (type == '0') {
        // Engine.IO Open — server sends JSON with sid, pingInterval, pingTimeout
        QJsonDocument doc = QJsonDocument::fromJson(raw.mid(1).toUtf8());
        QJsonObject obj = doc.object();
        m_pingIntervalMs = obj.value("pingInterval").toInt(25000);
        m_pingTimer->start(m_pingIntervalMs);

        // Send Socket.IO Connect packet ("40" = Engine.IO Message + Socket.IO Connect).
        // role="report" is required for full-participant auth — without it the server
        // immediately disconnects. rx_only and os are also required by the server.
        // version carries "MasterSDR <ver> RADE v<N>" so both app and engine version
        // are visible on the FreeDV Reporter website.
        QJsonObject auth;
        if (m_reportingEnabled && !m_myCallsign.isEmpty()) {
            auth["role"]             = QString("report");
            auth["callsign"]         = m_myCallsign;
            auth["grid_square"]      = m_myGrid;
            auth["version"]          = m_mySoftwareVersion;
            auth["rx_only"]          = false;
#if defined(Q_OS_WIN)
            auth["os"]               = QString("windows");
#elif defined(Q_OS_MAC)
            auth["os"]               = QString("macos");
#else
            auth["os"]               = QString("linux");
#endif
            auth["protocol_version"] = 2;
        } else {
            auth["role"]             = QString("view");
            auth["protocol_version"] = 2;
        }
        m_ws->sendTextMessage("40" + QString::fromUtf8(
            QJsonDocument(auth).toJson(QJsonDocument::Compact)));
        return;
    }

    if (type == '2') {
        // Engine.IO Ping → reply with Pong
        m_ws->sendTextMessage("3");
        return;
    }

    if (type == '3') {
        // Engine.IO Pong — response to our keepalive ping
        return;
    }

    if (type == '4') {
        // Engine.IO Message — contains Socket.IO payload
        handleSocketIO(raw.mid(1));
        return;
    }
    // type '6' = noop, others = ignore
}

void FreeDvClient::handleSocketIO(const QString& payload)
{
    if (payload.isEmpty()) return;

    QChar sioType = payload.at(0);

    if (sioType == '0') {
        // Socket.IO Connect ACK — "0{"sid":"..."}"
        m_connected.store(true);
        m_reconnectAttempts = 0;
        qCDebug(lcDxCluster) << "FreeDvClient: Socket.IO connected";
        emit rawLineReceived("Connected to qso.freedv.org");
        if (m_reportingEnabled && !m_myCallsign.isEmpty()) {
            sendInitialFreqChange();
            if (!m_myMessage.isEmpty())
                sendEvent("message_update", QJsonObject{{"message", m_myMessage}});
        }
        return;
    }

    if (sioType == '2') {
        // Socket.IO Event — "2["event_name",{...}]"
        QString json = payload.mid(1);
        QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        if (!doc.isArray()) return;
        QJsonArray arr = doc.array();
        if (arr.size() < 2) return;

        QString eventName = arr[0].toString();
        if (eventName == "bulk_update") {
            onBulkUpdate(arr[1].toArray());
        } else {
            handleEvent(eventName, arr[1].toObject());
        }
        return;
    }

    if (sioType == '1') {
        // Socket.IO Disconnect
        qCDebug(lcDxCluster) << "FreeDvClient: server disconnected us";
        m_connected.store(false);
        return;
    }
}

// ── Event dispatch ──────────────────────────────────────────────────────

void FreeDvClient::handleEvent(const QString& eventName, const QJsonObject& data)
{
    if      (eventName == "new_connection")    onNewConnection(data);
    else if (eventName == "freq_change")       onFreqChange(data);
    else if (eventName == "rx_report")         onRxReport(data);
    else if (eventName == "tx_report")         onTxReport(data);
    else if (eventName == "remove_connection") onRemoveConnection(data);
    // Silently ignore: connection_successful, message_update, chat_*, etc.
}

void FreeDvClient::onNewConnection(const QJsonObject& data)
{
    QString sid = data["sid"].toString();
    StationInfo info;
    info.callsign   = data["callsign"].toString();
    info.gridSquare  = data["grid_square"].toString();
    info.rxOnly      = data["rx_only"].toBool();
    m_stations[sid] = info;
}

void FreeDvClient::onFreqChange(const QJsonObject& data)
{
    QString sid = data["sid"].toString();
    double freqMhz = data["freq"].toDouble() / 1.0e6;

    // Update station map (create entry if new_connection hasn't arrived yet)
    if (!m_stations.contains(sid)) {
        StationInfo info;
        info.callsign = data["callsign"].toString();
        info.gridSquare = data["grid_square"].toString();
        m_stations[sid] = info;
    }
    auto& info = m_stations[sid];
    info.freqMhz = freqMhz;

    // Also pick up callsign/grid if present (bulk_update freq_change includes them)
    if (info.callsign.isEmpty())
        info.callsign = data["callsign"].toString();
    if (info.gridSquare.isEmpty())
        info.gridSquare = data["grid_square"].toString();

    if (freqMhz <= 0.0 || info.callsign.isEmpty()) return;

    // Emit a spot for every station with a known frequency — this is
    // the primary spot source.  rx_report only fires when one station
    // actually decodes another, which is rare.  freq_change fires on
    // connect (bulk_update) and whenever a station retunes.
    DxSpot spot;
    spot.dxCall      = info.callsign;
    spot.spotterCall = info.callsign;  // self-reported
    spot.freqMhz     = freqMhz;
    spot.source      = "FreeDV";
    spot.comment     = info.mode.isEmpty() ? "FreeDV" : info.mode;
    if (!info.gridSquare.isEmpty())
        spot.comment += " " + info.gridSquare;
    spot.utcTime     = QDateTime::currentDateTimeUtc().time();
    spot.lifetimeSec = 0;

    QString freedvColor = AppSettings::instance().value("FreeDvSpotColor", "#FF8C00").toString();
    if (freedvColor.length() == 7)
        freedvColor = "#FF" + freedvColor.mid(1);
    spot.color = freedvColor;

    QString logLine = QString("%1  %2  %3 MHz  %4")
        .arg(spot.utcTime.toString("HH:mm"), info.callsign,
             QString::number(freqMhz, 'f', 4), spot.comment);
    if (m_logFile.isOpen()) {
        m_logFile.write((logLine + "\n").toUtf8());
        m_logFile.flush();
    }
    emit rawLineReceived(logLine);
    emit spotReceived(spot);
}

void FreeDvClient::onTxReport(const QJsonObject& data)
{
    QString sid = data["sid"].toString();
    if (!m_stations.contains(sid)) return;
    auto& info = m_stations[sid];
    QString mode = data["mode"].toString();
    if (!mode.isEmpty())
        info.mode = mode;
}

void FreeDvClient::onRemoveConnection(const QJsonObject& data)
{
    QString sid = data["sid"].toString();
    m_stations.remove(sid);
}

void FreeDvClient::onBulkUpdate(const QJsonArray& pairs)
{
    for (const auto& item : pairs) {
        QJsonArray pair = item.toArray();
        if (pair.size() < 2) continue;
        handleEvent(pair[0].toString(), pair[1].toObject());
    }
}

void FreeDvClient::onRxReport(const QJsonObject& data)
{
    QString sid = data["sid"].toString();

    // Look up the receiving station's frequency from our state map
    double freqMhz = 0.0;
    if (m_stations.contains(sid))
        freqMhz = m_stations[sid].freqMhz;
    if (freqMhz <= 0.0)
        return;  // cannot spot without a frequency

    QString receiverCall = data["receiver_callsign"].toString();
    QString txCall       = data["callsign"].toString();
    QString mode         = data["mode"].toString();
    double  snr          = data["snr"].toDouble();
    QString grid         = data["receiver_grid_square"].toString();

    if (txCall.isEmpty()) return;

    DxSpot spot;
    spot.dxCall      = txCall;
    spot.spotterCall = receiverCall;
    spot.freqMhz     = freqMhz;
    spot.snr         = static_cast<int>(std::round(snr));
    spot.source      = "FreeDV";
    spot.comment     = mode;
    if (!grid.isEmpty())
        spot.comment += " " + grid;
    spot.utcTime     = QDateTime::currentDateTimeUtc().time();
    spot.lifetimeSec = 0;  // use source default from AppSettings

    // Apply FreeDV spot color
    QString freedvColor = AppSettings::instance().value("FreeDvSpotColor", "#FF8C00").toString();
    if (freedvColor.length() == 7)
        freedvColor = "#FF" + freedvColor.mid(1);
    spot.color = freedvColor;

    // Log
    QString logLine = QString("%1  %2 heard %3  %4 MHz  SNR %5 dB  %6")
        .arg(spot.utcTime.toString("HH:mm"), receiverCall, txCall,
             QString::number(freqMhz, 'f', 4),
             QString::number(snr, 'f', 1), mode);
    if (m_logFile.isOpen()) {
        m_logFile.write((logLine + "\n").toUtf8());
        m_logFile.flush();
    }
    emit rawLineReceived(logLine);
    emit spotReceived(spot);
}

// ── Station reporting ───────────────────────────────────────────────────

void FreeDvClient::sendEvent(const QString& name, const QJsonObject& data)
{
    if (m_ws->state() != QAbstractSocket::ConnectedState) return;
    QJsonArray arr;
    arr.append(name);
    arr.append(data);
    m_ws->sendTextMessage("42" + QString::fromUtf8(
        QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void FreeDvClient::sendInitialFreqChange()
{
    if (m_myFreqMhz <= 0.0) return;
    // Server knows our callsign/grid from auth — freq_change only needs the frequency.
    sendEvent("freq_change", QJsonObject{
        {"freq", static_cast<qint64>(m_myFreqMhz * 1.0e6)}
    });
}

void FreeDvClient::enableReporting(const QString& callsign, const QString& grid,
                                    const QString& message,
                                    const QString& softwareVersion, double freqMhz)
{
    m_myCallsign         = callsign;
    m_myGrid             = grid;
    m_mySoftwareVersion  = softwareVersion;
    m_myFreqMhz          = freqMhz;
    m_myMessage          = message;
    m_reportingEnabled   = true;
    m_snrTimer->start();

    qCDebug(lcDxCluster) << "FreeDvClient: reporting enabled for" << callsign
                          << "grid" << grid << "freq" << freqMhz << "MHz";

    if (m_connected.load() &&
        m_ws->state() != QAbstractSocket::ClosingState) {
        // Already connected as view — reconnect immediately with full-participant auth
        m_authNeedsRefresh = true;
        m_pingTimer->stop();
        m_ws->close();
    }
    // If not yet connected, the next connect will use full auth automatically
}

void FreeDvClient::disableReporting()
{
    if (!m_reportingEnabled) return;
    m_reportingEnabled = false;
    m_snrTimer->stop();
    m_radeSynced = false;
    m_lastSnr    = -99.0f;

    qCDebug(lcDxCluster) << "FreeDvClient: reporting disabled";

    if (m_connected.load() &&
        m_ws->state() != QAbstractSocket::ClosingState) {
        sendEvent("message_update", QJsonObject{{"message", QString()}});
        // freq=0 signals departure — server removes us from the map
        sendEvent("freq_change", QJsonObject{
            {"freq", static_cast<qint64>(0)}
        });
        // Reconnect as view-only
        m_authNeedsRefresh = true;
        m_pingTimer->stop();
        m_ws->close();
    }
    m_myMessage.clear();
}

void FreeDvClient::updateMessage(const QString& message)
{
    m_myMessage = message;
    if (!m_reportingEnabled || !m_connected.load() || m_myCallsign.isEmpty()) return;
    sendEvent("message_update", QJsonObject{{"message", message}});
}

void FreeDvClient::reportFreqChange(double freqMhz)
{
    m_myFreqMhz = freqMhz;
    if (!m_reportingEnabled || !m_connected.load()) return;
    sendEvent("freq_change", QJsonObject{
        {"freq", static_cast<qint64>(freqMhz * 1.0e6)}
    });
}

void FreeDvClient::reportTxState(bool transmitting)
{
    if (!m_reportingEnabled || !m_connected.load()) return;
    sendEvent("tx_report", QJsonObject{
        {"mode",         QString("RADEV1")},
        {"transmitting", transmitting}
    });
}

void FreeDvClient::updateRxSnr(float snrDb)
{
    m_lastSnr = snrDb;
}

void FreeDvClient::updateRxSynced(bool synced)
{
    m_radeSynced = synced;
    if (!synced) {
        m_lastSnr = -99.0f;
        if (!m_rxCallsign.isEmpty()) {
            qCDebug(lcDxCluster) << "FreeDvClient: sync lost — clearing rx callsign" << m_rxCallsign;
        }
        m_rxCallsign.clear();
    }
}

void FreeDvClient::updateRxCallsign(const QString& callsign)
{
    qCDebug(lcDxCluster) << "FreeDvClient: EOO callsign received:" << callsign
                        << "reporting=" << m_reportingEnabled
                        << "connected=" << m_connected.load()
                        << "synced=" << m_radeSynced
                        << "snr=" << m_lastSnr;
    m_rxCallsign = callsign;

    // Send an immediate rx_report on EOO receipt — do not wait for the 1-second
    // timer. The EOO frame is the last frame of the transmission; sync drops
    // within ~100–500ms afterward, which clears m_rxCallsign before the timer
    // can fire. Reporting is not gated on m_radeSynced here because we just
    // decoded a valid callsign from the signal, so clearly reception was good.
    if (!m_reportingEnabled || !m_connected.load()) return;
    if (m_lastSnr <= -99.0f) return;
    qCDebug(lcDxCluster) << "FreeDvClient: sending immediate rx_report for EOO callsign" << callsign;
    sendEvent("rx_report", QJsonObject{
        {"callsign", m_rxCallsign},
        {"mode",     QString("RADEV1")},
        {"snr",      static_cast<int>(m_lastSnr)}
    });
}

void FreeDvClient::onSnrTimer()
{
    if (!m_reportingEnabled || !m_connected.load() || !m_radeSynced) return;
    if (m_lastSnr <= -99.0f) return;
    qCDebug(lcDxCluster) << "FreeDvClient: periodic rx_report — callsign='"
                         << m_rxCallsign << "' snr=" << m_lastSnr;
    sendEvent("rx_report", QJsonObject{
        {"callsign", m_rxCallsign},
        {"mode",     QString("RADEV1")},
        {"snr",      static_cast<int>(m_lastSnr)}
    });
}

} // namespace MasterSDR
