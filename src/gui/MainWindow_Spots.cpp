// MainWindow_Spots.cpp — spot-subsystem wiring for MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 2b). Holds
// wireSpotSubsystem(), extracted verbatim from the constructor:
//
//   • DX Cluster / RBN / WSJT-X / SpotCollector / POTA clients on the
//     dedicated spot worker thread
//   • HF propagation forecast client
//   • Spot forwarding to the radio: dedup + batch queue + 1/sec flush
//
// Runs once at construction, at the original constructor position.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumWidget.h"
#include "core/SpotCommandPolicy.h"
#ifdef HAVE_MQTT
#include "MqttApplet.h"
#include "core/MqttAntennaAlias.h"
#include "core/MqttSettings.h"
#endif
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/SpotModel.h"

#include <QDateTime>
#ifdef HAVE_MQTT
#include <QJsonDocument>
#include <QJsonObject>
#endif
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

namespace AetherSDR {

void MainWindow::wireSpotSubsystem()
{
    // ── DX Cluster — forward parsed spots to radio ──────────────────────
    // ── Spot clients on worker thread ─────────────────────────────────────
    m_dxCluster = new DxClusterClient;
    m_rbnClient = new DxClusterClient;
    m_rbnClient->setLogFileName("rbn.log");
    m_rbnClient->setStartupCommandsKey("RbnStartupCommands");
    m_wsjtxClient = new WsjtxClient;
    m_spotCollectorClient = new SpotCollectorClient;
    m_potaClient = new PotaClient;
#ifdef HAVE_WEBSOCKETS
    m_freedvClient = new FreeDvClient;
#endif
#ifdef HAVE_MQTT
    m_mqttClient = new MqttClient(this);
    m_appletPanel->mqttApplet()->setMqttClient(m_mqttClient);

    connect(m_appletPanel->mqttApplet(), &MqttApplet::connectRequested,
            this, [this](const QString& host, quint16 port,
                         const QString& user, const QString& pass,
                         const QStringList& topics,
                         bool useTls, const QString& caFile) {
        m_mqttClient->setSubscriptions(mqttSubscriptionTopics(topics));
        m_mqttClient->connectToBroker(host, port, user, pass, useTls, caFile);
    });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::disconnectRequested,
            this, [this] { m_mqttClient->disconnect(); });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::settingsRequested,
            this, &MainWindow::showMqttSettingsDialog);
    m_appletPanel->mqttApplet()->restoreConnectionState();

    // CW decode → MQTT topic "aethersdr/cw/decode".
    // Publishes {"text":"K","freq":14.025,"rx":true} per decoded character.
    // RX decoder uses rx:true; TX sidetone decoder uses rx:false.
    // Any MQTT subscriber (e.g. a contest logger) receives the stream
    // without additional AetherSDR interfaces.
    connect(&m_cwDecoder,   &CwDecoder::textDecoded, this,
            [this](const QString& t, float cost) { publishCwDecodeMqtt(t, cost, true);  });
    connect(&m_cwDecoderTx, &CwDecoder::textDecoded, this,
            [this](const QString& t, float cost) { publishCwDecodeMqtt(t, cost, false); });

    // aethersdr/radio/state — publish on PTT transitions.
    // Slice freq/mode changes are wired per-slice in setActiveSliceInternal().
    m_radioStateCoalesceTimer.setSingleShot(true);
    m_radioStateCoalesceTimer.setInterval(150);
    connect(&m_radioStateCoalesceTimer, &QTimer::timeout,
            this, &MainWindow::publishRadioStateMqtt);
    connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
            this, [this](bool) { publishRadioStateMqtt(); });
    // Debounce timer for end-of-CWX detection (queueEmpty unreliable with sync_cwx=0).
    // Fires 1 s after the last tx:false with no intervening tx:true = transmission done.
    m_cwxTxEndTimer.setSingleShot(true);
    connect(&m_cwxTxEndTimer, &QTimer::timeout, this, [this]() {
        if (m_cwxSavedWpm > 0 && m_radioModel.cwxModel().speed() == m_cwxSentWpm)
            m_radioModel.cwxModel().setSpeed(m_cwxSavedWpm);
        if (m_cwxSavedHz  > 0 && m_radioModel.transmitModel().cwPitch() == m_cwxSentHz)
            m_radioModel.transmitModel().setCwPitch(m_cwxSavedHz);
        m_cwxSavedWpm = 0;
        m_cwxSavedHz  = 0;
        m_cwxSentWpm  = 0;
        m_cwxSentHz   = 0;
        m_cwxTransmitting = false;
        m_cwxPublishedTxTrue = false;
        publishRadioStateMqtt();
    });

    // aethersdr/cw/transmit → CWX keyer.
    // Payload: {"text":"de k5ptb","speed_wpm":28,"pitch_hz":600}
    // speed_wpm and pitch_hz are optional; absent = use current radio settings.
    connect(m_mqttClient, &MqttClient::messageReceived,
            this, [this](const QString& topic, const QByteArray& payload) {
        if (topic != QString::fromLatin1(kCwTransmitTopic)) return;
        if (!isMqttTopicEnabled(QString::fromLatin1(kCwTransmitTopic))) return;
        const QJsonObject obj =
            QJsonDocument::fromJson(payload).object();
        const QString text = obj.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) return;
        auto& tx = m_radioModel.transmitModel();
        const int wpm = obj.value(QStringLiteral("speed_wpm")).toInt(0);
        const int hz  = obj.value(QStringLiteral("pitch_hz")).toInt(0);
        const bool changeWpm = (wpm >= 5 && wpm <= 100);
        const bool changeHz  = (hz >= 100 && hz <= 6000);
        if (!m_cwxTransmitting) {
            m_cwxSavedWpm = changeWpm ? m_radioModel.cwxModel().speed() : 0;
            m_cwxSavedHz  = changeHz  ? tx.cwPitch() : 0;
        }
        if (changeWpm) { m_radioModel.cwxModel().setSpeed(wpm); m_cwxSentWpm = wpm; }
        if (changeHz)  { tx.setCwPitch(hz);                   m_cwxSentHz  = hz;  }
        m_cwxTxEndTimer.stop();
        m_cwxPublishedTxTrue = false;
        m_cwxTransmitting = true;
        m_radioModel.cwxModel().send(text);
        disconnect(m_cwxSpeedRestoreConn);
        m_cwxSpeedRestoreConn = connect(
            &m_radioModel.cwxModel(), &CwxModel::queueEmpty,
            this, [this]() {
                // Fast path if queueEmpty fires (sync_cwx=1 or firmware sends queue=0).
                // Identical work to m_cwxTxEndTimer.timeout — whichever fires first wins.
                if (m_cwxSavedWpm > 0 && m_radioModel.cwxModel().speed() == m_cwxSentWpm)
                    m_radioModel.cwxModel().setSpeed(m_cwxSavedWpm);
                if (m_cwxSavedHz  > 0 && m_radioModel.transmitModel().cwPitch() == m_cwxSentHz)
                    m_radioModel.transmitModel().setCwPitch(m_cwxSavedHz);
                m_cwxSavedWpm = 0;
                m_cwxSavedHz  = 0;
                m_cwxSentWpm  = 0;
                m_cwxSentHz   = 0;
                m_cwxTxEndTimer.stop();
                m_cwxTransmitting = false;
                m_cwxPublishedTxTrue = false;
                if (!m_radioModel.isRadioTransmitting())
                    publishRadioStateMqtt();
                disconnect(m_cwxSpeedRestoreConn);
            });
    });

    // MQTT → panadapter overlay display
    connect(m_appletPanel->mqttApplet(), &MqttApplet::displayValueChanged,
            this, [this](const QString& key, const QString& value) {
        if (auto* sw = m_panStack->activeSpectrum()) {
            sw->setMqttDisplayValue(key, value);
        }
    });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::displayCleared,
            this, [this] {
        if (auto* sw = m_panStack->activeSpectrum()) {
            sw->clearMqttDisplay();
        }
    });
    auto mqttAntennaAliasQueue = std::make_shared<MqttAntennaAliasQueue>();
    auto hasStableRadioAliasKey = [this] {
        return m_radioModel.isConnected()
            && (!m_radioModel.chassisSerial().trimmed().isEmpty()
                || !m_radioModel.serial().trimmed().isEmpty());
    };
    auto applyMqttAntennaAlias = [this](const QString& token, const QString& alias) {
        if (alias.trimmed().isEmpty())
            m_radioModel.clearAntennaAlias(token);
        else
            m_radioModel.setAntennaAlias(token, alias);
    };
    auto flushPendingMqttAntennaAliases = [mqttAntennaAliasQueue,
                                           hasStableRadioAliasKey,
                                           applyMqttAntennaAlias] {
        for (const auto& update : mqttAntennaAliasQueue->flush(hasStableRadioAliasKey()))
            applyMqttAntennaAlias(update.token, update.alias);
    };
    connect(m_appletPanel->mqttApplet(), &MqttApplet::antennaAliasRequested,
            this, [mqttAntennaAliasQueue,
                   hasStableRadioAliasKey,
                   applyMqttAntennaAlias,
                   this](const QString& token,
                         const QString& alias) {
        const MqttAntennaAliasUpdate update{token, alias};
        for (const auto& ready : mqttAntennaAliasQueue->receive(
                 update, m_radioModel.isConnected(), hasStableRadioAliasKey()))
            applyMqttAntennaAlias(ready.token, ready.alias);
    });
    connect(&m_radioModel, &RadioModel::connectionStateChanged,
            this, [mqttAntennaAliasQueue,
                   flushPendingMqttAntennaAliases](bool connected) {
        if (connected)
            flushPendingMqttAntennaAliases();
        else
            mqttAntennaAliasQueue->clear();
    });
    connect(&m_radioModel, &RadioModel::infoChanged,
            this, [flushPendingMqttAntennaAliases] { flushPendingMqttAntennaAliases(); });
#endif

    m_spotThread = new QThread(this);
    m_spotThread->setObjectName("SpotClients");
    m_dxCluster->moveToThread(m_spotThread);
    m_rbnClient->moveToThread(m_spotThread);
    m_wsjtxClient->moveToThread(m_spotThread);
    m_spotCollectorClient->moveToThread(m_spotThread);
    m_potaClient->moveToThread(m_spotThread);
#ifdef HAVE_WEBSOCKETS
    m_freedvClient->moveToThread(m_spotThread);
#endif
    m_spotThread->start();

    // Construct each client's sockets/timers on the SpotClients thread (#1929).
    // On Windows, QTcpSocket / QUdpSocket / QWebSocket bind their internal
    // QSocketNotifier to the construction thread's Win32 message loop, so
    // creating them on the main thread before moveToThread() trips a
    // cross-thread sendEvent assert when socket events fire on disconnect.
    QMetaObject::invokeMethod(m_dxCluster, &DxClusterClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_rbnClient, &DxClusterClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_wsjtxClient, &WsjtxClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_spotCollectorClient, &SpotCollectorClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_potaClient, &PotaClient::initialize,
                              Qt::QueuedConnection);
#ifdef HAVE_WEBSOCKETS
    QMetaObject::invokeMethod(m_freedvClient, &FreeDvClient::initialize,
                              Qt::QueuedConnection);
#endif

    // ── HF Propagation Forecast ────────────────────────────────────────────
    m_propForecast = new PropForecastClient(this);
    connect(m_propForecast, &PropForecastClient::forecastUpdated,
            this, [this](const PropForecast& fc) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            applet->spectrumWidget()->setPropForecast(fc.kIndex, fc.aIndex, fc.sfi);
        }
    });
    // Restore persisted setting — timer only arms if enabled
    if (AppSettings::instance().value("PropForecastEnabled", "False").toString() == "True") {
        m_propForecast->setEnabled(true);
    }

    // ── Spot forwarding: dedup + batch queue + 1/sec flush ────────────────

    // Dedup helper — returns true if spot should be skipped
    auto isDuplicateSpot = [this](const DxSpot& spot) -> bool {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto& as = AppSettings::instance();
        int lifetimeMs;
        if (spot.lifetimeSec > 0)
            lifetimeMs = spot.lifetimeSec * 1000;                      // source-provided
        else if (spot.source == "WSJT-X")
            lifetimeMs = as.value("WsjtxSpotLifetime", 120).toInt() * 1000;
        else if (spot.source == "FreeDV")
            lifetimeMs = as.value("FreeDvSpotLifetime", 120).toInt() * 1000;  // HAVE_WEBSOCKETS
        else
        {
            int sec = as.value("DxClusterSpotLifetimeSec", 0).toInt();
            if (sec <= 0) sec = as.value("DxClusterSpotLifetime", 30).toInt() * 60;
            lifetimeMs = sec * 1000;
        }
        auto it = m_spotDedup.find(spot.dxCall);
        if (it != m_spotDedup.end()) {
            bool sameFreq = std::abs(it->freqMhz - spot.freqMhz) < 0.001;
            bool expired = (now - it->addedMs) > lifetimeMs;
            if (sameFreq && !expired)
                return true;
        }
        m_spotDedup[spot.dxCall] = {spot.freqMhz, now};
        return false;
    };

    auto spotLifetimeSeconds = [](const DxSpot& spot, const QString& source) {
        if (spot.lifetimeSec > 0)
            return spot.lifetimeSec;

        auto& as = AppSettings::instance();
        if (source == "WSJT-X")
            return as.value("WsjtxSpotLifetime", 120).toInt();
        if (source == "FreeDV")
            return as.value("FreeDvSpotLifetime", 120).toInt();

        int sec = as.value("DxClusterSpotLifetimeSec", 0).toInt();
        if (sec <= 0)
            sec = as.value("DxClusterSpotLifetime", 30).toInt() * 60;
        return sec;
    };

    auto spotColorForSource = [](const DxSpot& spot, const QString& source) {
        QString spotColor = spot.color;
        if (spotColor.isEmpty()) {
            auto& as = AppSettings::instance();
            if (source == "DXCluster")
                spotColor = as.value("DxClusterSpotColor", "#D2B48C").toString();
            else if (source == "RBN")
                spotColor = as.value("RbnSpotColor", "#4488FF").toString();
            else if (source == "SpotCollector")
                spotColor = as.value("SpotCollectorSpotColor", "#FFD700").toString();
            else if (source == "FreeDV")
                spotColor = as.value("FreeDvSpotColor", "#FF8C00").toString();  // HAVE_WEBSOCKETS
        }
        if (spotColor.length() == 7)
            spotColor = "#FF" + spotColor.mid(1);
        return spotColor;
    };

    auto addPassiveSpotToModel = [this](const DxSpot& spot, const QString& source,
                                        const QString& color, int lifetimeSec) {
        if (spot.dxCall.trimmed().isEmpty() || spot.freqMhz <= 0.0)
            return;

        QMap<QString, QString> kvs;
        kvs["callsign"] = QString(spot.dxCall).replace(' ', QChar(0x7f));
        kvs["rx_freq"] = QString::number(spot.freqMhz, 'f', 6);
        kvs["tx_freq"] = QString::number(spot.freqMhz, 'f', 6);
        kvs["source"] = source;
        kvs["spotter_callsign"] = spot.spotterCall;
        kvs["lifetime_seconds"] = QString::number(lifetimeSec);
        kvs["timestamp"] = QString::number(QDateTime::currentSecsSinceEpoch());
        if (!spot.comment.isEmpty())
            kvs["comment"] = QString(spot.comment).replace(' ', QChar(0x7f));
        if (!color.isEmpty())
            kvs["color"] = color;

        const int spotId = m_nextPassiveSpotId--;
        m_radioModel.spotModel().applySpotStatus(spotId, kvs);
        if (lifetimeSec > 0) {
            const qint64 expiresAt = QDateTime::currentMSecsSinceEpoch()
                                   + qint64(lifetimeSec) * 1000;
            m_passiveSpotExpiryMs.insert(spotId, expiresAt);
        }
    };

    // Build spot add command and queue for batch send
    auto queueSpotCmd = [this, isDuplicateSpot, spotLifetimeSeconds,
                         spotColorForSource, addPassiveSpotToModel]
                        (const DxSpot& spot, const QString& source) {
        if (!m_radioModel.isConnected()) return;
        if (isDuplicateSpot(spot)) return;
        const int lifetimeSec = spotLifetimeSeconds(spot, source);
        const QString spotColor = spotColorForSource(spot, source);
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            addPassiveSpotToModel(spot, source, spotColor, lifetimeSec);
            return;
        }

        QString call = QString(spot.dxCall).replace(' ', QChar(0x7f));
        QString freq = QString::number(spot.freqMhz, 'f', 6);
        // trigger_action=none disables the radio's internal tune/mode-set on
        // spot click. AetherSDR handles freq via frequencyClicked and mode
        // via SpotAutoSwitchMode client-side, so the radio's stored-mode
        // path (which mishandles non-Flex tokens like "SSB") never fires.
        // Clicks still emit SpotTriggered for external loggers (#341, #1846).
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=" + source
                     + " spotter_callsign=" + spot.spotterCall
                     + " trigger_action=none"
                     + " lifetime_seconds=" + QString::number(lifetimeSec);
        if (!spot.comment.isEmpty())
            cmd += " comment=" + QString(spot.comment).replace(' ', QChar(0x7f));
        if (!spotColor.isEmpty())
            cmd += " color=" + spotColor;
        m_spotCmdBatch.append(cmd);
    };

    // Flush batch: send queued spot commands to radio (1/sec)
    auto* spotCmdTimer = new QTimer(this);
    spotCmdTimer->start(1000);
    connect(spotCmdTimer, &QTimer::timeout, this, [this] {
        if (m_spotCmdBatch.isEmpty() || !m_radioModel.isConnected()) return;
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            m_spotCmdBatch.clear();
            return;
        }
        // Send up to RbnRateLimit commands per tick
        int limit = AppSettings::instance().value("RbnRateLimit", 10).toInt();
        int count = std::min(static_cast<int>(m_spotCmdBatch.size()), limit);
        for (int i = 0; i < count; ++i)
            m_radioModel.sendCommand(m_spotCmdBatch[i]);
        m_spotCmdBatch.remove(0, count);
    });

    auto* passiveSpotExpiryTimer = new QTimer(this);
    passiveSpotExpiryTimer->start(1000);
    connect(passiveSpotExpiryTimer, &QTimer::timeout, this, [this] {
        if (m_passiveSpotExpiryMs.isEmpty())
            return;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QVector<int> expired;
        for (auto it = m_passiveSpotExpiryMs.cbegin(); it != m_passiveSpotExpiryMs.cend(); ++it) {
            if (it.value() <= now)
                expired.append(it.key());
        }

        for (int spotId : expired) {
            m_passiveSpotExpiryMs.remove(spotId);
            m_radioModel.spotModel().removeSpot(spotId);
        }
    });
    connect(&m_radioModel.spotModel(), &SpotModel::spotsCleared,
            this, [this] { m_passiveSpotExpiryMs.clear(); });

    connect(m_dxCluster, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "DXCluster");
    });

    connect(m_rbnClient, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "RBN");
    });

    connect(m_wsjtxClient, &WsjtxClient::spotReceived,
            this, [this, isDuplicateSpot, spotLifetimeSeconds, addPassiveSpotToModel](const DxSpot& spot) {
        if (!m_radioModel.isConnected()) return;
        if (isDuplicateSpot(spot)) return;

        auto& as = AppSettings::instance();
        const QString& msg = spot.comment;
        bool isCQ = msg.startsWith("CQ ");
        bool isPOTA = msg.contains("CQ POTA");
        bool isCallingMe = false;
        {
            QString myCall = as.value("DxClusterCallsign").toString();
            if (!myCall.isEmpty()) {
                QStringList parts = msg.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 2 && parts[0] == myCall)
                    isCallingMe = true;
            }
        }

        // Filter
        bool fCQ   = as.value("WsjtxFilterCQ", "True").toString() == "True";
        bool fPOTA = as.value("WsjtxFilterPOTA", "True").toString() == "True";
        bool fMe   = as.value("WsjtxFilterCallingMe", "True").toString() == "True";
        bool anyFilter = fCQ || fPOTA || fMe;
        if (anyFilter) {
            bool pass = false;
            if (fCQ && isCQ) pass = true;
            if (fPOTA && isPOTA) pass = true;
            if (fMe && isCallingMe) pass = true;
            if (!pass) return;
        }

        // Color
        DxSpot colored = spot;
        if (isCallingMe)
            colored.color = as.value("WsjtxColorCallingMe", "#FF0000").toString();
        else if (isPOTA)
            colored.color = as.value("WsjtxColorPOTA", "#00FFFF").toString();
        else if (isCQ)
            colored.color = as.value("WsjtxColorCQ", "#00FF00").toString();
        else
            colored.color = as.value("WsjtxColorDefault", "#FFFFFF").toString();

        // Compute alpha from SNR: -24→64, 0→192, +10→255 (linear interpolation)
        int alpha;
        if (colored.snr <= -24)
            alpha = 64;
        else if (colored.snr >= 10)
            alpha = 255;
        else if (colored.snr <= 0)
            alpha = 64 + (colored.snr + 24) * (192 - 64) / 24;   // -24..0 → 64..192
        else
            alpha = 192 + colored.snr * (255 - 192) / 10;         // 0..+10 → 192..255

        // Convert to #AARRGGBB format for radio
        if (colored.color.length() == 7)  // #RRGGBB → #AARRGGBB
            colored.color = QString("#%1%2").arg(alpha, 2, 16, QChar('0')).arg(colored.color.mid(1));

        QString call = QString(colored.dxCall).replace(' ', QChar(0x7f));
        QString freq = QString::number(colored.freqMhz, 'f', 6);
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=WSJT-X"
                     + " spotter_callsign=" + colored.spotterCall
                     + " trigger_action=none"  // see comment at queueSpotCmd (#1846)
                     + " lifetime_seconds=" + QString::number(
                           spotLifetimeSeconds(colored, "WSJT-X"));
        if (!colored.comment.isEmpty())
            cmd += " comment=" + QString(colored.comment).replace(' ', QChar(0x7f));
        if (!colored.color.isEmpty())
            cmd += " color=" + colored.color;
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            addPassiveSpotToModel(colored, "WSJT-X", colored.color,
                                  spotLifetimeSeconds(colored, "WSJT-X"));
            return;
        }
        m_spotCmdBatch.append(cmd);
    });

    connect(m_spotCollectorClient, &SpotCollectorClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "SpotCollector");
    });

    connect(m_potaClient, &PotaClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "POTA");
    });

#ifdef HAVE_WEBSOCKETS
    connect(m_freedvClient, &FreeDvClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "FreeDV");
    });
#endif

}

} // namespace AetherSDR
