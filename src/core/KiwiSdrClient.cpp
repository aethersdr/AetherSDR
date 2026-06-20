#include "KiwiSdrClient.h"

#include "KiwiSdrProtocol.h"
#include "LogManager.h"
#include "Resampler.h"

#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#ifdef HAVE_WEBSOCKETS
#include <QAbstractSocket>
#include <QWebSocket>
#include <QWebSocketProtocol>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace AetherSDR {
namespace {
constexpr int kAudioReadyTimeoutMs = 12000;
constexpr quint16 kDefaultKiwiSdrPort = 8073;
constexpr double kDefaultWaterfallCenterMhz = 15.0;
constexpr double kDefaultWaterfallBandwidthMhz = 30.0;
constexpr int kSpecSoundHeaderBytes = 6;
constexpr int kObservedSoundHeaderBytes = 10;
constexpr int kObservedSoundFrameBytes = 1034;
constexpr int kSpecWaterfallHeaderBytes = 4;
constexpr int kExtendedWaterfallHeaderBytes = 16;
constexpr int kZoomedWaterfallPrefixBytes = 5;
constexpr int kDefaultWaterfallZoomCap = 14;
constexpr int kDefaultWaterfallFftBins = 1024;
constexpr quint64 kMaxSequenceGapPaddingFrames = 8;
constexpr double kWaterfallStartFixedPointScale = 16777216.0; // 2^24
constexpr quint32 kWaterfallStartServerSnapTolerance = 16;

quint32 readLittleEndianU32(const char* data)
{
    const auto* bytes = reinterpret_cast<const uchar*>(data);
    return static_cast<quint32>(bytes[0])
        | (static_cast<quint32>(bytes[1]) << 8)
        | (static_cast<quint32>(bytes[2]) << 16)
        | (static_cast<quint32>(bytes[3]) << 24);
}

int sanitizedWaterfallFftBins(int binCount)
{
    return std::clamp(binCount, 16, 65536);
}

bool plausibleWaterfallFftBins(int binCount)
{
    return binCount >= 128
        && binCount <= 65536
        && (binCount & (binCount - 1)) == 0;
}

double waterfallZoomScale(int zoom)
{
    return std::ldexp(1.0, std::clamp(zoom, 0, 30));
}

double waterfallRowSpanMhz(double fullBandwidthMhz, int zoom)
{
    return fullBandwidthMhz / waterfallZoomScale(zoom);
}

quint32 waterfallStartFixedPoint(double fullLowMhz, double fullBandwidthMhz,
                                 double rowLowMhz)
{
    const double requested = fullBandwidthMhz > 0.0
        ? ((rowLowMhz - fullLowMhz) / fullBandwidthMhz)
              * kWaterfallStartFixedPointScale
        : 0.0;
    return static_cast<quint32>(std::clamp(
        std::isfinite(requested) ? std::round(requested) : 0.0,
        0.0,
        std::min(kWaterfallStartFixedPointScale - 1.0,
                 static_cast<double>(std::numeric_limits<quint32>::max()))));
}

double waterfallStartFixedPointToLowMhz(double fullLowMhz,
                                        double fullBandwidthMhz,
                                        quint32 start)
{
    return fullLowMhz
        + (static_cast<double>(start) / kWaterfallStartFixedPointScale)
            * fullBandwidthMhz;
}

double waterfallMetadataValueToMhz(double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return 0.0;
    }
    if (value >= 1000000.0) {
        return value / 1000000.0;
    }
    if (value >= 1000.0) {
        return value / 1000.0;
    }
    return value;
}

KiwiSdrReceiverControls normalizedControls(
    const KiwiSdrReceiverControls& controls)
{
    KiwiSdrReceiverControls normalized = controls;
    normalized.agcGainDb = std::clamp(normalized.agcGainDb, 0, 100);
    normalized.agcThresholdDb = std::clamp(normalized.agcThresholdDb, -100, -10);
    normalized.agcDecayMs = std::clamp(normalized.agcDecayMs, 200, 5000);
    normalized.squelchLevelDbm =
        std::clamp(normalized.squelchLevelDbm, -160.0, 0.0);
    return normalized;
}

bool controlsEqual(const KiwiSdrReceiverControls& a,
                   const KiwiSdrReceiverControls& b)
{
    return a.agcEnabled == b.agcEnabled
        && a.agcGainDb == b.agcGainDb
        && a.agcHang == b.agcHang
        && a.agcThresholdDb == b.agcThresholdDb
        && a.agcDecayMs == b.agcDecayMs
        && a.squelchEnabled == b.squelchEnabled
        && std::abs(a.squelchLevelDbm - b.squelchLevelDbm) < 0.001;
}

QString redactedKiwiCommand(const QString& command)
{
    if (!command.startsWith(QStringLiteral("SET auth "))) {
        return command;
    }

    const QString passwordMarker = QStringLiteral(" p=");
    const int passwordStart = command.indexOf(passwordMarker);
    if (passwordStart < 0) {
        return command;
    }

    const int valueStart = passwordStart + passwordMarker.size();
    const int valueEnd = command.indexOf(QLatin1Char(' '), valueStart);
    const QString password = valueEnd < 0
        ? command.mid(valueStart)
        : command.mid(valueStart, valueEnd - valueStart);
    if (password == QStringLiteral("#")) {
        return command;
    }

    QString redacted = command;
    redacted.replace(valueStart, password.size(), QStringLiteral("<redacted>"));
    return redacted;
}

QString firstBytesHex(const QByteArray& frame, int limit = 16)
{
    return QString::fromLatin1(frame.left(std::max(0, limit)).toHex(' '));
}

}

KiwiSdrClient::KiwiSdrClient(QObject* parent)
    : QObject(parent)
{
    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(10000);
    connect(m_keepaliveTimer, &QTimer::timeout,
            this, &KiwiSdrClient::sendKeepalive);

    m_audioReadyTimer = new QTimer(this);
    m_audioReadyTimer->setSingleShot(true);
    m_audioReadyTimer->setInterval(kAudioReadyTimeoutMs);
    connect(m_audioReadyTimer, &QTimer::timeout, this, [this]() {
        if (m_soundAudioReady || m_userDisconnecting
            || m_state != State::Connecting) {
            return;
        }

        setState(State::Error,
                 m_soundSampleRateCommandsSent
                     ? tr("KiwiSDR sound setup completed but no SND audio frames arrived.")
                     : tr("No KiwiSDR audio channel became available."));
        cleanupSockets();
    });
}

KiwiSdrClient::~KiwiSdrClient()
{
    m_userDisconnecting = true;
    cleanupSockets();
}

void KiwiSdrClient::setOperatorCallsign(const QString& callsign)
{
    const QString normalized = callsign.trimmed().toUpper();
    if (m_operatorCallsign == normalized) {
        return;
    }

    m_operatorCallsign = normalized;
    sendSoundIdentityToServer();
    sendWaterfallIdentityToServer();
}

void KiwiSdrClient::setReceiverControls(
    const KiwiSdrReceiverControls& controls)
{
    const KiwiSdrReceiverControls normalized = normalizedControls(controls);
    if (controlsEqual(m_receiverControls, normalized)) {
        return;
    }

    m_receiverControls = normalized;
    sendReceiverControlsToServer();
}

void KiwiSdrClient::connectToEndpoint(const QString& endpoint)
{
    QString host;
    quint16 port = 0;
    if (!parseEndpoint(endpoint, &host, &port)) {
        setState(State::Error, tr("Enter a KiwiSDR endpoint as hostname or hostname:port."));
        return;
    }

    m_endpoint = QStringLiteral("%1:%2").arg(host).arg(port);
    m_host = host;
    m_port = port;
    m_secureWebSocket = false;
    m_secureWebSocketRetryAttempted = false;
    m_soundSocketConnected = false;
    m_waterfallSocketConnected = false;
    m_soundAudioReady = false;
    m_soundAudioRateAcked = false;
    m_soundSampleRateCommandsSent = false;
    m_soundSampleRatePending = false;
    m_soundFrameSeen = false;
    m_loggedSoundFrameShape = false;
    m_loggedWaterfallFrameShape = false;
    m_soundSampleRateHz = 12000.0;
    m_soundResamplerRateHz = 0.0;
    m_soundResampler.reset();
    m_haveSoundAudioRate = false;
    m_telemetry = {};
    m_telemetryPending = false;
    m_lastSoundIdentityCallsign.clear();
    m_lastWaterfallIdentityCallsign.clear();
    m_lastDecodedSoundPcm.clear();
    m_lastWaterfallBins.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;
    m_waterfallServerCenterMhz = kDefaultWaterfallCenterMhz;
    m_waterfallServerBandwidthMhz = kDefaultWaterfallBandwidthMhz;
    m_waterfallZoomCap = kDefaultWaterfallZoomCap;
    m_waterfallFftBins = kDefaultWaterfallFftBins;
    m_waterfallRequestValid = false;
    m_waterfallRequestPanId.clear();
    m_waterfallRequestLowMhz = 0.0;
    m_waterfallRequestHighMhz = 0.0;
    m_userDisconnecting = true;
    cleanupSockets();
    m_userDisconnecting = false;
    const QString callsign = kiwiIdentityCallsign();
    setState(State::Connecting,
             callsign.isEmpty()
                 ? tr("Connecting to %1 without a radio callsign.").arg(m_endpoint)
                 : tr("Connecting to %1 as %2.").arg(m_endpoint, callsign));

#ifdef HAVE_WEBSOCKETS
    openWebSockets();
#else
    setState(State::Error, tr("Qt WebSockets support is required for KiwiSDR."));
#endif
}

#ifdef HAVE_WEBSOCKETS
void KiwiSdrClient::openWebSockets()
{
    const QString scheme = m_secureWebSocket
        ? QStringLiteral("wss")
        : QStringLiteral("ws");
    const quint16 socketPort = m_secureWebSocket ? 443 : m_port;
    const QString sessionId = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString secureAwareBase = QStringLiteral("%1://%2:%3/%4")
        .arg(scheme)
        .arg(m_host)
        .arg(socketPort)
        .arg(sessionId);
    const QString soundUrl = secureAwareBase + QStringLiteral("/SND");
    const QString waterfallUrl = secureAwareBase + QStringLiteral("/W/F");

    m_soundSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_soundSocket, &QWebSocket::connected, this, [this]() {
        m_soundSocketConnected = true;
        sendSoundSetupCommands();
        m_keepaliveTimer->start();
        m_audioReadyTimer->start();
    });
    connect(m_soundSocket, &QWebSocket::textMessageReceived,
            this, &KiwiSdrClient::handleTextMessage);
    connect(m_soundSocket, &QWebSocket::binaryMessageReceived,
            this, &KiwiSdrClient::handleBinaryMessage);
    connect(m_soundSocket, &QWebSocket::disconnected, this, [this]() {
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR SND closed code="
            << (m_soundSocket
                    ? static_cast<int>(m_soundSocket->closeCode())
                    : -1)
            << "reason="
            << (m_soundSocket ? m_soundSocket->closeReason() : QString())
            << "saw_snd="
            << (m_soundFrameSeen ? QStringLiteral("yes") : QStringLiteral("no"));
        if (!m_userDisconnecting) {
            if (m_state == State::Connecting) {
                if (retryWithSecureWebSocket(m_soundSocketConnected)) {
                    return;
                }
                setState(State::Error,
                         tr("KiwiSDR sound connection closed during setup. %1")
                             .arg(identityDiagnosticText()));
                cleanupSockets();
            } else if (m_state == State::Connected) {
                if (retryWithSecureWebSocket(m_soundSocketConnected)) {
                    return;
                }
                const QString detail = tr("KiwiSDR sound connection closed.");
                setState(State::Error, detail);
                cleanupSockets();
                emit recoverableDisconnect(detail);
            }
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_soundSocket, &QWebSocket::errorOccurred, this,
#else
    connect(m_soundSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
#endif
            [this](QAbstractSocket::SocketError) {
                handleSocketError(m_soundSocket ? m_soundSocket->errorString()
                                                : tr("KiwiSDR sound socket error."),
                                  m_soundSocketConnected);
            });
    qCInfo(lcKiwiSdr).noquote() << "KiwiSDR SND URL" << soundUrl;
    m_soundSocket->open(QUrl(soundUrl));

    m_waterfallSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_waterfallSocket, &QWebSocket::connected, this, [this]() {
        m_waterfallSocketConnected = true;
        sendWaterfallSetupCommands();
    });
    connect(m_waterfallSocket, &QWebSocket::textMessageReceived,
            this, &KiwiSdrClient::handleTextMessage);
    connect(m_waterfallSocket, &QWebSocket::binaryMessageReceived,
            this, &KiwiSdrClient::handleBinaryMessage);
    connect(m_waterfallSocket, &QWebSocket::disconnected, this, [this]() {
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR W/F closed code="
            << (m_waterfallSocket
                    ? static_cast<int>(m_waterfallSocket->closeCode())
                    : -1)
            << "reason="
            << (m_waterfallSocket ? m_waterfallSocket->closeReason() : QString());
        if (!m_userDisconnecting) {
            if (retryWithSecureWebSocket(m_waterfallSocketConnected)) {
                return;
            }
            const QString detail = tr("KiwiSDR waterfall connection closed.");
            const bool wasConnected = m_state == State::Connected;
            setState(State::Error, detail);
            cleanupSockets();
            if (wasConnected) {
                emit recoverableDisconnect(detail);
            }
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_waterfallSocket, &QWebSocket::errorOccurred, this,
#else
    connect(m_waterfallSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
#endif
            [this](QAbstractSocket::SocketError) {
                handleSocketError(m_waterfallSocket ? m_waterfallSocket->errorString()
                                                    : tr("KiwiSDR waterfall socket error."),
                                  m_waterfallSocketConnected);
            });
    qCInfo(lcKiwiSdr).noquote() << "KiwiSDR W/F URL" << waterfallUrl;
    m_waterfallSocket->open(QUrl(waterfallUrl));
}
#endif

void KiwiSdrClient::disconnectFromEndpoint()
{
    m_userDisconnecting = true;
    setAudioActive(false);
    cleanupSockets();
    setState(State::Disconnected, QString());
}

void KiwiSdrClient::setTrackedSlice(int sliceId, double frequencyMhz,
                                    const QString& mode, int filterLowHz,
                                    int filterHighHz, const QString& panId)
{
    if (m_trackedSliceId == sliceId
        && qFuzzyCompare(m_trackedFrequencyMhz, frequencyMhz)
        && m_trackedMode == mode
        && m_trackedFilterLowHz == filterLowHz
        && m_trackedFilterHighHz == filterHighHz
        && m_trackedPanId == panId) {
        return;
    }

    m_trackedSliceId = sliceId;
    m_trackedFrequencyMhz = frequencyMhz;
    m_trackedMode = mode;
    m_trackedFilterLowHz = filterLowHz;
    m_trackedFilterHighHz = filterHighHz;
    m_trackedPanId = panId;

    emit trackedSliceChanged(sliceId, frequencyMhz, mode,
                             filterLowHz, filterHighHz, panId);
    sendTrackedSliceToServer();
    sendWaterfallViewToServer();
}

void KiwiSdrClient::setWaterfallView(const QString& panId, double centerMhz,
                                     double bandwidthMhz)
{
    if (panId.isEmpty() || centerMhz <= 0.0 || bandwidthMhz <= 0.0) {
        return;
    }

    if (m_waterfallViewPanId == panId
        && qFuzzyCompare(m_waterfallViewCenterMhz, centerMhz)
        && qFuzzyCompare(m_waterfallViewBandwidthMhz, bandwidthMhz)) {
        return;
    }

    m_waterfallViewPanId = panId;
    m_waterfallViewCenterMhz = centerMhz;
    m_waterfallViewBandwidthMhz = bandwidthMhz;
    sendWaterfallViewToServer();
}

void KiwiSdrClient::setWaterfallLineDurationMs(int lineDurationMs)
{
    const int clamped = std::clamp(lineDurationMs, 1, 100);
    if (m_waterfallLineDurationMs == clamped) {
        return;
    }

    m_waterfallLineDurationMs = clamped;
    sendWaterfallRateToServer();
}

void KiwiSdrClient::setWaterfallDisplayAdjustments(int cellDb, int floorDb)
{
    const int clampedCell = std::clamp(cellDb, -30, 30);
    const int clampedFloor = std::clamp(floorDb, -30, 30);
    if (m_waterfallCellDb == clampedCell
        && m_waterfallFloorDb == clampedFloor) {
        return;
    }

    m_waterfallCellDb = clampedCell;
    m_waterfallFloorDb = clampedFloor;
    sendWaterfallDisplayAdjustmentsToServer();
}

void KiwiSdrClient::setWaterfallRateOverride(int rate)
{
    const int clamped = std::clamp(rate, 0, 5);
    if (m_waterfallRateOverride == clamped) {
        return;
    }

    m_waterfallRateOverride = clamped;
    sendWaterfallRateToServer();
}

void KiwiSdrClient::setAudioActive(bool active)
{
    const bool allowed = active && m_state == State::Connected;
    if (m_audioActive == allowed) {
        return;
    }

    m_audioActive = allowed;
    emit audioActiveChanged(m_audioActive);
}

void KiwiSdrClient::setDecodeAudioWhenInactive(bool decode)
{
    m_decodeAudioWhenInactive = decode;
}

bool KiwiSdrClient::parseEndpoint(const QString& endpoint,
                                  QString* host,
                                  quint16* port)
{
    const QString trimmed = normalizeEndpoint(endpoint);
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('/'))
        || trimmed.contains(QLatin1Char('\\'))) {
        return false;
    }

    const int colon = trimmed.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == trimmed.size() - 1) {
        return false;
    }

    const QString parsedHost = trimmed.left(colon).trimmed();
    const QString parsedPortText = trimmed.mid(colon + 1).trimmed();
    if (parsedHost.isEmpty() || parsedHost.contains(QLatin1Char(' '))
        || parsedPortText.isEmpty()) {
        return false;
    }

    bool ok = false;
    const int parsedPort = parsedPortText.toInt(&ok);
    if (!ok || parsedPort <= 0 || parsedPort > 65535) {
        return false;
    }

    if (host) {
        *host = parsedHost;
    }
    if (port) {
        *port = static_cast<quint16>(parsedPort);
    }
    return true;
}

QString KiwiSdrClient::normalizeEndpoint(const QString& endpoint)
{
    QString normalized = endpoint.trimmed();

    if (normalized.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        normalized.remove(0, 7);
    } else if (normalized.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        normalized.remove(0, 8);
    }

    while (normalized.endsWith(QLatin1Char('/'))) {
        normalized.chop(1);
    }

    normalized = normalized.trimmed();
    if (normalized.isEmpty()
        || normalized.contains(QLatin1Char('/'))
        || normalized.contains(QLatin1Char('\\'))
        || normalized.contains(QLatin1Char(':'))) {
        return normalized;
    }

    return QStringLiteral("%1:%2").arg(normalized).arg(kDefaultKiwiSdrPort);
}

void KiwiSdrClient::cleanupSockets()
{
    if (m_keepaliveTimer) {
        m_keepaliveTimer->stop();
    }
    if (m_audioReadyTimer) {
        m_audioReadyTimer->stop();
    }
    m_soundAudioReady = false;
    m_soundAudioRateAcked = false;
    m_soundSampleRateCommandsSent = false;
    m_soundSampleRatePending = false;
    m_soundFrameSeen = false;
    m_loggedSoundFrameShape = false;
    m_loggedWaterfallFrameShape = false;
    m_lastDecodedSoundPcm.clear();
    m_lastWaterfallBins.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;

#ifdef HAVE_WEBSOCKETS
    auto cleanup = [this](QWebSocket*& socket) {
        if (!socket) {
            return;
        }
        socket->disconnect(this);
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->close();
        }
        socket->deleteLater();
        socket = nullptr;
    };
    cleanup(m_soundSocket);
    cleanup(m_waterfallSocket);
#endif
}

void KiwiSdrClient::sendSoundSetupCommands()
{
    sendSoundCommand(QStringLiteral("SET auth t=kiwi p=#"));
    sendSoundIdentityToServer();
    sendSoundCommand(QStringLiteral("SET compression=0"));
}

void KiwiSdrClient::sendSoundAudioRateAck()
{
    if (m_soundAudioRateAcked) {
        return;
    }

    m_soundAudioRateAcked = true;
    sendSoundCommand(QStringLiteral("SET AR OK in=%1 out=24000")
        .arg(m_soundSampleRateHz, 0, 'f', 0));
}

void KiwiSdrClient::sendSoundSampleRateCommands()
{
    if (m_soundSampleRateCommandsSent) {
        return;
    }

    if (!m_soundAudioRateAcked) {
        m_soundSampleRatePending = true;
        return;
    }

    m_soundSampleRatePending = false;
    m_soundSampleRateCommandsSent = true;
    sendSoundCommand(QStringLiteral("SERVER DE CLIENT AetherSDR SND"));
    const KiwiSdrReceiverControls c = normalizedControls(m_receiverControls);
    sendSoundCommand(QStringLiteral("SET squelch=%1 max=%2")
        .arg(c.squelchEnabled ? 1 : 0)
        .arg(c.squelchEnabled ? c.squelchLevelDbm : 0.0, 0, 'f', 2));
    sendSoundCommand(QStringLiteral("SET genattn=0"));
    sendSoundCommand(QStringLiteral("SET gen=0 mix=-1"));
    sendSoundCommand(QStringLiteral("SET agc=%1 hang=%2 thresh=%3 slope=6 decay=%4 manGain=%5")
        .arg(c.agcEnabled ? 1 : 0)
        .arg(c.agcHang ? 1 : 0)
        .arg(c.agcThresholdDb)
        .arg(c.agcDecayMs)
        .arg(c.agcGainDb));
    sendTrackedSliceToServer();
    sendKeepalive();
}

void KiwiSdrClient::sendWaterfallSetupCommands()
{
    sendWaterfallCommand(QStringLiteral("SET auth t=kiwi p=#"));
    sendWaterfallIdentityToServer();
    sendWaterfallCommand(QStringLiteral("SERVER DE CLIENT AetherSDR W/F"));
    sendWaterfallCommand(QStringLiteral("SET wf_comp=0"));
    sendWaterfallCommand(QStringLiteral("SET send_dB=1"));
    sendWaterfallViewToServer();
    sendWaterfallDisplayAdjustmentsToServer();
    sendWaterfallCommand(QStringLiteral("SET interp=13"));
    sendWaterfallCommand(QStringLiteral("SET window_func=2"));
    sendWaterfallRateToServer();
}

void KiwiSdrClient::sendTrackedSliceToServer()
{
    if (m_trackedSliceId < 0 || m_trackedFrequencyMhz <= 0.0) {
        return;
    }
#ifdef HAVE_WEBSOCKETS
    if (!m_soundSocket
        || m_soundSocket->state() != QAbstractSocket::ConnectedState
        || !m_soundSampleRateCommandsSent) {
        return;
    }
#endif

    const double freqKhz = m_trackedFrequencyMhz * 1000.0;
    const QString mode = kiwiMode();
    const int lowCutHz = kiwiLowCutHz();
    const int highCutHz = kiwiHighCutHz();
    if (lowCutHz >= highCutHz) {
        qCWarning(lcKiwiSdr).noquote()
            << "KiwiSDR refusing invalid passband mode=" << mode
            << "freq_khz=" << QString::number(freqKhz, 'f', 3)
            << "low_cut=" << lowCutHz
            << "high_cut=" << highCutHz;
        return;
    }
    sendSoundCommand(QStringLiteral("SET mod=%1 low_cut=%2 high_cut=%3 freq=%4")
        .arg(mode)
        .arg(lowCutHz)
        .arg(highCutHz)
        .arg(freqKhz, 0, 'f', 3));
}

void KiwiSdrClient::sendReceiverControlsToServer()
{
#ifdef HAVE_WEBSOCKETS
    if (!m_soundSocket
        || m_soundSocket->state() != QAbstractSocket::ConnectedState
        || !m_soundSampleRateCommandsSent) {
        return;
    }
#endif
    const KiwiSdrReceiverControls c = normalizedControls(m_receiverControls);
    sendSoundCommand(QStringLiteral("SET squelch=%1 max=%2")
        .arg(c.squelchEnabled ? 1 : 0)
        .arg(c.squelchEnabled ? c.squelchLevelDbm : 0.0, 0, 'f', 2));
    sendSoundCommand(QStringLiteral("SET agc=%1 hang=%2 thresh=%3 slope=6 decay=%4 manGain=%5")
        .arg(c.agcEnabled ? 1 : 0)
        .arg(c.agcHang ? 1 : 0)
        .arg(c.agcThresholdDb)
        .arg(c.agcDecayMs)
        .arg(c.agcGainDb));
}

void KiwiSdrClient::sendWaterfallViewToServer()
{
#ifdef HAVE_WEBSOCKETS
    if (!m_waterfallSocket
        || m_waterfallSocket->state() != QAbstractSocket::ConnectedState) {
        m_waterfallRequestValid = false;
        return;
    }
#else
    m_waterfallRequestValid = false;
    return;
#endif

    double viewCenterMhz = m_waterfallViewCenterMhz;
    double viewBandwidthMhz = m_waterfallViewBandwidthMhz;
    if (viewCenterMhz <= 0.0 || viewBandwidthMhz <= 0.0) {
        viewCenterMhz = m_trackedFrequencyMhz > 0.0
            ? m_trackedFrequencyMhz
            : m_waterfallServerCenterMhz;
        viewBandwidthMhz = 0.2;
    }

    const double fullBandwidthMhz = std::max(
        0.001,
        m_waterfallServerBandwidthMhz > 0.0
            ? m_waterfallServerBandwidthMhz
            : kDefaultWaterfallBandwidthMhz);
    const double fullCenterMhz = m_waterfallServerCenterMhz > 0.0
        ? m_waterfallServerCenterMhz
        : kDefaultWaterfallCenterMhz;
    const double fullLowMhz = fullCenterMhz - fullBandwidthMhz * 0.5;
    const double fullHighMhz = fullCenterMhz + fullBandwidthMhz * 0.5;
    const int zoomCap = std::clamp(m_waterfallZoomCap, 0, 20);
    const double halfBandwidthMhz = std::max(0.0005, viewBandwidthMhz * 0.5);
    const double viewLowMhz = std::clamp(viewCenterMhz - halfBandwidthMhz,
                                         fullLowMhz,
                                         fullHighMhz);
    const double viewHighMhz = std::clamp(viewCenterMhz + halfBandwidthMhz,
                                          fullLowMhz,
                                          fullHighMhz);

    const double viewSpanMhz = std::max(0.0, viewHighMhz - viewLowMhz);
    const double requiredSpanMhz = std::min(fullBandwidthMhz, viewSpanMhz);
    const double viewMidMhz = (viewLowMhz + viewHighMhz) * 0.5;
    int zoom = 0;
    quint32 start = 0;
    double requestLowMhz = fullLowMhz;
    double requestHighMhz = fullHighMhz;
    bool selectedRequest = false;
    for (int candidate = zoomCap; candidate >= 0; --candidate) {
        const double candidateRowSpanMhz = std::min(
            fullBandwidthMhz,
            waterfallRowSpanMhz(fullBandwidthMhz, candidate));
        if (candidateRowSpanMhz + 1.0e-9 < requiredSpanMhz) {
            continue;
        }

        double candidateLowMhz = std::clamp(
            viewMidMhz - candidateRowSpanMhz * 0.5,
            fullLowMhz,
            std::max(fullLowMhz, fullHighMhz - candidateRowSpanMhz));
        const quint32 candidateStart = waterfallStartFixedPoint(
            fullLowMhz, fullBandwidthMhz, candidateLowMhz);
        candidateLowMhz = waterfallStartFixedPointToLowMhz(
            fullLowMhz, fullBandwidthMhz, candidateStart);
        const double candidateHighMhz = candidateLowMhz
            + std::min(fullBandwidthMhz,
                       waterfallRowSpanMhz(fullBandwidthMhz, candidate));

        // Kiwi W/F rows are discrete fixed-point windows with a fixed bin
        // count. Prefer the highest resolution row that fully covers the
        // AetherSDR viewport after start quantization; otherwise zooming near
        // a row boundary leaves black side bands until the next coarser row is
        // requested.
        const double coverEpsilonMhz = std::max(
            1.0e-9,
            (fullBandwidthMhz / kWaterfallStartFixedPointScale) * 2.0);
        if (candidateLowMhz <= viewLowMhz + coverEpsilonMhz
            && candidateHighMhz + coverEpsilonMhz >= viewHighMhz) {
            zoom = candidate;
            start = candidateStart;
            requestLowMhz = candidateLowMhz;
            requestHighMhz = candidateHighMhz;
            selectedRequest = true;
            break;
        }
    }
    if (!selectedRequest) {
        zoom = 0;
        start = waterfallStartFixedPoint(fullLowMhz, fullBandwidthMhz,
                                         fullLowMhz);
        requestLowMhz = fullLowMhz;
        requestHighMhz = fullHighMhz;
    }

    if (m_waterfallRequestValid
        && m_waterfallRequestPanId == m_waterfallViewPanId
        && m_waterfallRequestStart == start
        && m_waterfallRequestZoom == zoom
        && std::abs(m_waterfallRequestLowMhz - requestLowMhz) <= 1.0e-9
        && std::abs(m_waterfallRequestHighMhz - requestHighMhz) <= 1.0e-9) {
        return;
    }

    m_waterfallRequestValid = true;
    m_waterfallRequestPanId = m_waterfallViewPanId;
    m_waterfallRequestStart = start;
    m_waterfallRequestZoom = zoom;
    m_waterfallRequestLowMhz = requestLowMhz;
    m_waterfallRequestHighMhz = requestHighMhz;
    m_lastWaterfallBins.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;

    const double centerFreqKhz = ((requestLowMhz + requestHighMhz) * 0.5)
        * 1000.0;
    sendWaterfallCommand(QStringLiteral("SET zoom=%1 cf=%2 start=%3")
        .arg(zoom)
        .arg(centerFreqKhz, 0, 'f', 3)
        .arg(start));
}

void KiwiSdrClient::sendWaterfallDisplayAdjustmentsToServer()
{
    const float maxDb = m_waterfallMaxDbm
        + static_cast<float>(m_waterfallCellDb);
    const float minDb = std::min(
        maxDb - 1.0f,
        m_waterfallMinDbm + static_cast<float>(m_waterfallFloorDb));
    sendWaterfallCommand(QStringLiteral("SET maxdb=%1 mindb=%2")
        .arg(static_cast<double>(maxDb), 0, 'f', 0)
        .arg(static_cast<double>(minDb), 0, 'f', 0));
}

QString KiwiSdrClient::kiwiIdentityCallsign() const
{
    QString identity;
    const QString source = m_operatorCallsign.trimmed().toUpper();
    identity.reserve(source.size());
    for (const QChar ch : source) {
        if (ch.isLetterOrNumber()
            || ch == QLatin1Char('-')
            || ch == QLatin1Char('/')) {
            identity.append(ch);
        }
    }
    return identity.left(32);
}

void KiwiSdrClient::sendSoundIdentityToServer()
{
    const QString callsign = kiwiIdentityCallsign();
    m_lastSoundIdentityCallsign = callsign;
    if (!callsign.isEmpty()) {
        sendSoundCommand(QStringLiteral("SET ident_user=%1").arg(callsign));
    }
}

void KiwiSdrClient::sendWaterfallIdentityToServer()
{
    const QString callsign = kiwiIdentityCallsign();
    m_lastWaterfallIdentityCallsign = callsign;
    if (!callsign.isEmpty()) {
        sendWaterfallCommand(QStringLiteral("SET ident_user=%1").arg(callsign));
    }
}

QString KiwiSdrClient::identityDiagnosticText() const
{
    const QString callsign = !m_lastSoundIdentityCallsign.isEmpty()
        ? m_lastSoundIdentityCallsign
        : kiwiIdentityCallsign();
    if (callsign.isEmpty()) {
        return tr("No radio callsign was available to send.");
    }
    return tr("Radio callsign sent: %1.").arg(callsign);
}

QString KiwiSdrClient::kiwiMode() const
{
    const QString mode = m_trackedMode.trimmed().toUpper();
    if (mode == QStringLiteral("LSB") || mode == QStringLiteral("DIGL")) {
        return QStringLiteral("lsb");
    }
    if (mode == QStringLiteral("USB") || mode == QStringLiteral("DIGU")
        || mode == QStringLiteral("RTTY")) {
        return QStringLiteral("usb");
    }
    if (mode == QStringLiteral("CW") || mode == QStringLiteral("CWU")
        || mode == QStringLiteral("CWL")) {
        return QStringLiteral("cw");
    }
    if (mode == QStringLiteral("NFM") || mode == QStringLiteral("FM")) {
        return QStringLiteral("nfm");
    }
    return QStringLiteral("am");
}

int KiwiSdrClient::kiwiLowCutHz() const
{
    if (m_trackedFilterLowHz < m_trackedFilterHighHz) {
        return m_trackedFilterLowHz;
    }

    const QString mode = kiwiMode();
    if (mode == QStringLiteral("lsb")) {
        return -2900;
    }
    if (mode == QStringLiteral("usb")) {
        return 100;
    }
    if (mode == QStringLiteral("cw")) {
        return 400;
    }
    if (mode == QStringLiteral("nfm")) {
        return -6000;
    }
    return -4900;
}

int KiwiSdrClient::kiwiHighCutHz() const
{
    if (m_trackedFilterLowHz < m_trackedFilterHighHz) {
        return m_trackedFilterHighHz;
    }

    const QString mode = kiwiMode();
    if (mode == QStringLiteral("lsb")) {
        return -100;
    }
    if (mode == QStringLiteral("usb")) {
        return 2900;
    }
    if (mode == QStringLiteral("cw")) {
        return 800;
    }
    if (mode == QStringLiteral("nfm")) {
        return 6000;
    }
    return 4900;
}

bool KiwiSdrClient::isSupportedPcmSoundFrame(
    const QByteArray& frame,
    const KiwiSdrProtocol::SoundFrameHeader& header) const
{
    if (!header.valid) {
        return false;
    }

    const bool observedExtendedFrame = frame.size() == kObservedSoundFrameBytes;
    if (observedExtendedFrame) {
        return true;
    }

    const int sampleBytes = frame.size() - kSpecSoundHeaderBytes;
    if (sampleBytes <= 0 || (sampleBytes % 2) != 0) {
        return false;
    }

    return true;
}

QByteArray KiwiSdrClient::decodeSoundFrame(const QByteArray& frame)
{
    if (frame.size() <= kSpecSoundHeaderBytes || !frame.startsWith("SND")) {
        return {};
    }

    int payloadOffset = kSpecSoundHeaderBytes;
    const bool observedExtendedFrame = frame.size() == kObservedSoundFrameBytes;
    if (observedExtendedFrame) {
        payloadOffset = kObservedSoundHeaderBytes;
    }
    const int sampleBytes = frame.size() - payloadOffset;
    const int inSamples = sampleBytes / 2;
    if (inSamples <= 0 || (sampleBytes % 2) != 0) {
        return {};
    }

    std::vector<float> mono(static_cast<std::size_t>(inSamples));
    const auto* data = reinterpret_cast<const uchar*>(frame.constData() + payloadOffset);
    for (int i = 0; i < inSamples; ++i) {
        int sample = 0;
        if (observedExtendedFrame) {
            sample = (static_cast<int>(data[2 * i]) << 8)
                   | static_cast<int>(data[2 * i + 1]);
        } else {
            sample = static_cast<int>(data[2 * i])
                   | (static_cast<int>(data[2 * i + 1]) << 8);
        }
        if (sample & 0x8000) {
            sample -= 0x10000;
        }
        mono[static_cast<std::size_t>(i)] = std::clamp(
            static_cast<float>(sample) / 32768.0f, -1.0f, 1.0f);
    }
    if (!m_soundResampler || std::abs(m_soundResamplerRateHz - m_soundSampleRateHz) > 1.0) {
        m_soundResampler = std::make_unique<Resampler>(
            m_soundSampleRateHz, 24000.0);
        m_soundResamplerRateHz = m_soundSampleRateHz;
    }
    return m_soundResampler->processMonoToStereo(mono.data(), inSamples);
}

bool KiwiSdrClient::parseWaterfallFrameHeader(const QByteArray& frame,
                                              quint32* start,
                                              int* zoom) const
{
    if (frame.size() < kExtendedWaterfallHeaderBytes || !frame.startsWith("W/F")) {
        return false;
    }
    if (frame.size() == kSpecWaterfallHeaderBytes + kDefaultWaterfallFftBins) {
        // User-provided protocol correction: normal W/F frames are a 4-byte
        // tag/sequence header followed by exactly 1024 bins. Do not interpret
        // early bin bytes as the older observed extended start/zoom fields.
        return false;
    }

    const quint32 parsedStart = readLittleEndianU32(frame.constData() + 4);
    const int parsedZoom = static_cast<uchar>(frame[8]);
    if (parsedZoom < 0 || parsedZoom > m_waterfallZoomCap) {
        return false;
    }
    if (start) {
        *start = parsedStart;
    }
    if (zoom) {
        *zoom = parsedZoom;
    }
    return true;
}

QVector<float> KiwiSdrClient::decodeWaterfallFrame(const QByteArray& frame) const
{
    if (frame.size() <= kSpecWaterfallHeaderBytes || !frame.startsWith("W/F")) {
        return {};
    }

    int payloadOffset = kSpecWaterfallHeaderBytes;
    int frameZoom = 0;
    const bool haveExtendedHeader = parseWaterfallFrameHeader(
        frame, nullptr, &frameZoom);
    if (haveExtendedHeader) {
        payloadOffset = kExtendedWaterfallHeaderBytes;
    }
    int payloadBytes = frame.size() - payloadOffset;
    if (payloadBytes == kZoomedWaterfallPrefixBytes) {
        return {};
    }
    const auto* payload = reinterpret_cast<const uchar*>(
        frame.constData() + payloadOffset);
    if (haveExtendedHeader
        && frameZoom > 0
        && payloadBytes > kZoomedWaterfallPrefixBytes
        && payload[0] == 0x77
        && payload[1] == 0x01) {
        // Clean-room W/F captures on 2026-06-18 showed this zoom-row
        // prefix marks the compact encoded W/F format. It is not one
        // byte-per-bin waterfall data; rendering it caused the speckled
        // display. We request wf_comp=0 during setup, and drop encoded rows
        // from endpoints that ignore that request until a clean decoder is
        // available.
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR W/F compressed row ignored len=" << frame.size()
            << "zoom=" << frameZoom
            << "first=" << firstBytesHex(frame);
        return {};
    }
    if (payloadBytes <= 0) {
        return {};
    }

    QVector<float> bins;
    bins.reserve(payloadBytes);
    const auto* data = reinterpret_cast<const uchar*>(frame.constData() + payloadOffset);
    for (int i = 0; i < payloadBytes; ++i) {
        bins.append(KiwiSdrProtocol::waterfallByteToDisplayLevel(*data++));
    }
    return bins;
}

void KiwiSdrClient::handleBinaryMessage(const QByteArray& frame)
{
    if (frame.startsWith("MSG")) {
        handleMessage(frame);
    } else if (frame.startsWith("SND")) {
        handleSoundFrame(frame);
    } else if (frame.startsWith("W/F")) {
        handleWaterfallFrame(frame);
    } else if (frame.startsWith("EXT")) {
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR EXT ignored len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    } else {
        const QString tag = QString::fromLatin1(frame.left(3));
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR unknown binary tag=" << tag
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
}

void KiwiSdrClient::handleSoundFrame(const QByteArray& frame)
{
    m_soundFrameSeen = true;
    if (!m_loggedSoundFrameShape) {
        m_loggedSoundFrameShape = true;
        qCDebug(lcKiwiSdrAudio).noquote()
            << "KiwiSDR SND frame len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
    const KiwiSdrProtocol::SoundFrameHeader header =
        KiwiSdrProtocol::parseSoundFrameHeader(frame);
    if (!isSupportedPcmSoundFrame(frame, header)) {
        qCWarning(lcKiwiSdrAudio).noquote()
            << "KiwiSDR SND unsupported frame shape len=" << frame.size()
            << "first=" << firstBytesHex(frame);
        return;
    }
    const quint64 sequenceGaps = header.valid
        ? KiwiSdrProtocol::sequenceGapCount(m_telemetry.soundSequence,
                                            header.sequence)
        : 0;
    updateSoundTelemetry(frame);
    const KiwiSdrProtocol::MeterReading meterReading =
        KiwiSdrProtocol::extractMeterFromSndVerifiedLayout(
            frame, KiwiSdrProtocol::MeterContext{});
    markSoundAudioReady();
    if (!m_audioActive && !m_decodeAudioWhenInactive) {
        return;
    }

    const QByteArray pcm = decodeSoundFrame(frame);
    if (!pcm.isEmpty()) {
        const quint64 padFrames = std::min(sequenceGaps,
                                          kMaxSequenceGapPaddingFrames);
        if (!m_lastDecodedSoundPcm.isEmpty()) {
            for (quint64 i = 0; i < padFrames; ++i) {
                emit decodedAudioReady(m_lastDecodedSoundPcm);
            }
        }
        emit decodedAudioReady(pcm);
        m_lastDecodedSoundPcm = pcm;
        emit meterReadingReady(meterReading);
    }
}

void KiwiSdrClient::handleWaterfallFrame(const QByteArray& frame)
{
    if (!m_loggedWaterfallFrameShape) {
        m_loggedWaterfallFrameShape = true;
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR W/F frame len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
    const KiwiSdrProtocol::WaterfallLineHeader header =
        KiwiSdrProtocol::parseWaterfallLineHeader(frame);
    const quint64 sequenceGaps = header.valid
        ? KiwiSdrProtocol::sequenceGapCount(m_telemetry.waterfallSequence,
                                            header.sequence)
        : 0;
    updateWaterfallTelemetry(frame);
    quint32 frameStart = 0;
    int frameZoom = 0;
    const bool hasExtendedHeader =
        parseWaterfallFrameHeader(frame, &frameStart, &frameZoom);
    if (!m_waterfallRequestValid
        || m_waterfallRequestLowMhz <= 0.0
        || m_waterfallRequestHighMhz <= m_waterfallRequestLowMhz) {
        return;
    }
    if (hasExtendedHeader
        && frameZoom != m_waterfallRequestZoom) {
        return;
    }
    if (hasExtendedHeader) {
        const quint32 startDelta =
            frameStart > m_waterfallRequestStart
                ? frameStart - m_waterfallRequestStart
                : m_waterfallRequestStart - frameStart;
        if (startDelta > kWaterfallStartServerSnapTolerance) {
            return;
        }
    }

    const QVector<float> bins = decodeWaterfallFrame(frame);
    if (bins.isEmpty()) {
        return;
    }
    if (updateWaterfallFftBins(bins.size())) {
        return;
    }

    const QString panId = !m_waterfallRequestPanId.isEmpty()
        ? m_waterfallRequestPanId
        : (!m_waterfallViewPanId.isEmpty()
              ? m_waterfallViewPanId
              : m_trackedPanId);
    if (panId.isEmpty()) {
        return;
    }
    double rowLowMhz = m_waterfallRequestLowMhz;
    double rowHighMhz = m_waterfallRequestHighMhz;
    if (hasExtendedHeader) {
        const double fullBandwidthMhz = std::max(
            0.001,
            m_waterfallServerBandwidthMhz > 0.0
                ? m_waterfallServerBandwidthMhz
                : kDefaultWaterfallBandwidthMhz);
        const double fullCenterMhz = m_waterfallServerCenterMhz > 0.0
            ? m_waterfallServerCenterMhz
            : kDefaultWaterfallCenterMhz;
        const double fullLowMhz = fullCenterMhz - fullBandwidthMhz * 0.5;
        rowLowMhz = waterfallStartFixedPointToLowMhz(
            fullLowMhz, fullBandwidthMhz, frameStart);
        rowHighMhz = rowLowMhz
            + std::min(fullBandwidthMhz,
                       waterfallRowSpanMhz(fullBandwidthMhz, frameZoom));
    }
    const quint64 padRows = std::min(sequenceGaps,
                                     kMaxSequenceGapPaddingFrames);
    if (m_lastWaterfallRowValid
        && m_lastWaterfallPanId == panId
        && std::abs(m_lastWaterfallLowMhz - rowLowMhz) <= 1.0e-9
        && std::abs(m_lastWaterfallHighMhz - rowHighMhz) <= 1.0e-9) {
        for (quint64 i = 0; i < padRows; ++i) {
            emit waterfallRowReady(panId, m_lastWaterfallBins,
                                   m_lastWaterfallLowMhz,
                                   m_lastWaterfallHighMhz,
                                   0);
        }
    }
    emit waterfallRowReady(panId, bins, rowLowMhz, rowHighMhz, 0);
    m_lastWaterfallBins = bins;
    m_lastWaterfallPanId = panId;
    m_lastWaterfallLowMhz = rowLowMhz;
    m_lastWaterfallHighMhz = rowHighMhz;
    m_lastWaterfallRowValid = true;
}

void KiwiSdrClient::handleMessage(const QByteArray& frame)
{
    const QString text = QString::fromLatin1(frame.constData(), frame.size());
    handleTextMessage(text);
}

void KiwiSdrClient::handleTextMessage(const QString& text)
{
    const QString message = text.startsWith(QStringLiteral("MSG"))
        ? text.mid(3).trimmed()
        : text.trimmed();

    const QStringList parts = message.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    auto setSoundSampleRate = [this](double value) {
        if (!std::isfinite(value) || value < 8000.0 || value > 48000.0) {
            return;
        }
        if (std::abs(value - m_soundSampleRateHz) > 1.0) {
            qCInfo(lcKiwiSdr).noquote()
                << "KiwiSDR audio rate negotiated" << value
                << "Hz (resampler rebuild)";
            m_soundSampleRateHz = value;
            m_soundResamplerRateHz = 0.0;
            m_soundResampler.reset();
        }
    };
    for (const QString& part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString key = part.left(eq);
        const QString valueText = part.mid(eq + 1);
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR MSG" << key << "=" << valueText;
        if (key == QStringLiteral("too_busy")) {
            bool busyOk = false;
            const int busyValue = valueText.toInt(&busyOk);
            if (!busyOk || busyValue != 0) {
                setState(State::Error, tr("KiwiSDR endpoint is busy."));
                cleanupSockets();
                return;
            }
            continue;
        }
        if (key == QStringLiteral("reason_disabled")) {
            const QString reason =
                QUrl::fromPercentEncoding(valueText.toUtf8()).trimmed();
            setState(State::Error,
                     reason.isEmpty()
                         ? tr("KiwiSDR endpoint is disabled.")
                         : tr("KiwiSDR endpoint is disabled: %1").arg(reason));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("down")) {
            bool downOk = false;
            const int downValue = valueText.toInt(&downOk);
            if (!downOk || downValue != 0) {
                setState(State::Error, tr("KiwiSDR endpoint is down."));
                cleanupSockets();
                return;
            }
            continue;
        }
        if (key == QStringLiteral("camp_disconnect")) {
            setState(State::Error,
                     tr("KiwiSDR camping queue disconnected this client."));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("redirect")) {
            const QString target =
                QUrl::fromPercentEncoding(valueText.toUtf8()).trimmed();
            setState(State::Error,
                     target.isEmpty()
                         ? tr("KiwiSDR endpoint requested redirect.")
                         : tr("KiwiSDR endpoint requested redirect to %1.")
                               .arg(target));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("audio_init")) {
            markSoundAudioReady();
            continue;
        }

        bool ok = false;
        const double value = valueText.toDouble(&ok);
        if (!ok) {
            continue;
        }
        if (key == QStringLiteral("badp")) {
            const int badp = static_cast<int>(value);
            if (badp != 0) {
                setState(
                    State::Error,
                    tr("KiwiSDR rejected public receive access (badp=%1). "
                       "%2 This endpoint may require a password or site-specific login.")
                        .arg(badp)
                        .arg(identityDiagnosticText()));
                cleanupSockets();
                return;
            }
        } else if (key == QStringLiteral("center_freq")) {
            const double centerMhz = waterfallMetadataValueToMhz(value);
            if (centerMhz > 0.0) {
                m_waterfallServerCenterMhz = centerMhz;
                m_waterfallRequestValid = false;
                sendWaterfallViewToServer();
            }
        } else if (key == QStringLiteral("bandwidth")) {
            const double bandwidthMhz = waterfallMetadataValueToMhz(value);
            if (bandwidthMhz > 0.0) {
                m_waterfallServerBandwidthMhz = bandwidthMhz;
                m_waterfallRequestValid = false;
                sendWaterfallViewToServer();
            }
        } else if (key == QStringLiteral("zoom_cap")
                   || key == QStringLiteral("zoom_max")) {
            m_waterfallZoomCap = std::clamp(static_cast<int>(value), 0, 20);
            m_waterfallRequestValid = false;
            sendWaterfallViewToServer();
        } else if (key == QStringLiteral("wf_fft_size")) {
            updateWaterfallFftBins(static_cast<int>(value));
        } else if (key == QStringLiteral("audio_rate")) {
            m_haveSoundAudioRate = true;
            setSoundSampleRate(value);
            sendSoundAudioRateAck();
            if (m_soundSampleRatePending) {
                sendSoundSampleRateCommands();
            }
        } else if (key == QStringLiteral("sample_rate")) {
            sendSoundSampleRateCommands();
        } else if (key == QStringLiteral("users")) {
            const int users = static_cast<int>(value);
            if (m_telemetry.users != users) {
                m_telemetry.users = users;
                emitTelemetryChanged();
            }
        } else if (key == QStringLiteral("freq")) {
            if (std::abs(m_telemetry.reportedFrequencyKhz - value) > 0.001) {
                m_telemetry.reportedFrequencyKhz = value;
                emitTelemetryChanged();
            }
        } else if (key == QStringLiteral("adc_clipping")) {
            const bool clipping = value != 0.0;
            if (!m_telemetry.hasAdcClipping
                || m_telemetry.adcClipping != clipping) {
                m_telemetry.hasAdcClipping = true;
                m_telemetry.adcClipping = clipping;
                emitTelemetryChanged();
            }
        } else if (key == QStringLiteral("gps_good")) {
            const bool good = value != 0.0;
            if (!m_telemetry.hasGpsGood || m_telemetry.gpsGood != good) {
                m_telemetry.hasGpsGood = true;
                m_telemetry.gpsGood = good;
                emitTelemetryChanged();
            }
        }
    }
}

bool KiwiSdrClient::updateWaterfallFftBins(int binCount)
{
    if (!plausibleWaterfallFftBins(binCount)) {
        return false;
    }

    const int fftBins = sanitizedWaterfallFftBins(binCount);
    if (m_waterfallFftBins == fftBins) {
        return false;
    }

    m_waterfallFftBins = fftBins;
    m_waterfallRequestValid = false;
    sendWaterfallViewToServer();
    return true;
}

void KiwiSdrClient::updateSoundTelemetry(const QByteArray& frame)
{
    const KiwiSdrProtocol::SoundFrameHeader header =
        KiwiSdrProtocol::parseSoundFrameHeader(frame);
    if (!header.valid) {
        return;
    }

    const quint64 gaps = header.sequence >= 0
        ? KiwiSdrProtocol::sequenceGapCount(m_telemetry.soundSequence,
                                            header.sequence)
        : 0;
    if (header.sequence >= 0) {
        m_telemetry.soundSequenceGaps += gaps;
        m_telemetry.soundSequence = header.sequence;
    }
    bool changed = gaps > 0;
    if (header.hasRssi
        && (!m_telemetry.hasSoundRssi
            || std::abs(m_telemetry.soundRssiDbm - header.rssiDbm) > 0.05f)) {
        m_telemetry.hasSoundRssi = true;
        m_telemetry.soundRssiDbm = header.rssiDbm;
        changed = true;
    }
    if (changed) {
        emitTelemetryChanged();
    }
}

void KiwiSdrClient::updateWaterfallTelemetry(const QByteArray& frame)
{
    const KiwiSdrProtocol::WaterfallLineHeader header =
        KiwiSdrProtocol::parseWaterfallLineHeader(frame);
    if (!header.valid) {
        return;
    }

    const quint64 gaps =
        KiwiSdrProtocol::sequenceGapCount(m_telemetry.waterfallSequence,
                                          header.sequence);
    m_telemetry.waterfallSequence = header.sequence;
    if (gaps > 0) {
        m_telemetry.waterfallSequenceGaps += gaps;
        emitTelemetryChanged();
    }
}

void KiwiSdrClient::emitTelemetryChanged()
{
    if (m_telemetryPending) {
        return;
    }

    m_telemetryPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_telemetryPending = false;
        emit telemetryChanged();
    });
}

void KiwiSdrClient::sendWaterfallRateToServer()
{
    if (m_waterfallRateOverride > 0) {
        sendWaterfallCommand(QStringLiteral("SET wf_speed=%1")
            .arg(m_waterfallRateOverride));
        return;
    }

    const int flexRate = std::clamp(m_waterfallLineDurationMs, 1, 100);
    // Clean-room W/F probes on public endpoints showed that the supported
    // speed control is a compact 1..4 range. On 22033.proxy.kiwisdr.com,
    // values 5 and above echoed wf_fps=0 and produced no W/F rows.
    const int speed = std::clamp(
        1 + static_cast<int>(std::lround((flexRate - 1) * 3.0 / 99.0)),
        1,
        4);
    sendWaterfallCommand(QStringLiteral("SET wf_speed=%1").arg(speed));
}

void KiwiSdrClient::markSoundAudioReady()
{
    if (m_soundAudioReady) {
        return;
    }

    m_soundAudioReady = true;
    if (m_audioReadyTimer) {
        m_audioReadyTimer->stop();
    }
    setState(State::Connected, tr("Connected to %1.").arg(m_endpoint));
}

void KiwiSdrClient::sendKeepalive()
{
    sendSoundCommand(QStringLiteral("SET keepalive"));
    sendWaterfallCommand(QStringLiteral("SET keepalive"));
}

#ifdef HAVE_WEBSOCKETS
bool KiwiSdrClient::retryWithSecureWebSocket(bool transportEstablished)
{
    if (transportEstablished
        || m_soundSocketConnected || m_waterfallSocketConnected
        || m_secureWebSocket || m_secureWebSocketRetryAttempted
        || m_host.isEmpty()) {
        return false;
    }

    const QString lowerHost = m_host.toLower();
    if (!lowerHost.endsWith(QStringLiteral(".proxy.kiwisdr.com"))) {
        return false;
    }

    m_secureWebSocketRetryAttempted = true;
    m_secureWebSocket = true;
    m_soundSocketConnected = false;
    m_waterfallSocketConnected = false;
    m_soundAudioReady = false;
    m_soundAudioRateAcked = false;
    m_soundSampleRateCommandsSent = false;
    m_soundSampleRatePending = false;
    m_soundFrameSeen = false;
    m_loggedSoundFrameShape = false;
    m_loggedWaterfallFrameShape = false;
    m_haveSoundAudioRate = false;
    m_soundResamplerRateHz = 0.0;
    m_soundResampler.reset();

    m_userDisconnecting = true;
    cleanupSockets();
    m_userDisconnecting = false;
    setState(State::Connecting,
             tr("Retrying %1 over secure KiwiSDR WebSocket.")
                 .arg(m_endpoint));

    QTimer::singleShot(0, this, [this]() {
        if (!m_userDisconnecting) {
            openWebSockets();
        }
    });
    return true;
}

void KiwiSdrClient::sendSoundCommand(const QString& command)
{
    if (m_soundSocket && m_soundSocket->state() == QAbstractSocket::ConnectedState) {
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR SND ->" << redactedKiwiCommand(command);
        m_soundSocket->sendTextMessage(command);
    }
}

void KiwiSdrClient::sendWaterfallCommand(const QString& command)
{
    if (m_waterfallSocket
        && m_waterfallSocket->state() == QAbstractSocket::ConnectedState) {
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR W/F ->" << redactedKiwiCommand(command);
        m_waterfallSocket->sendTextMessage(command);
    }
}

void KiwiSdrClient::handleSocketError(const QString& detail,
                                      bool transportEstablished)
{
    if (!m_userDisconnecting) {
        if (retryWithSecureWebSocket(transportEstablished)) {
            return;
        }
        const bool wasConnected = m_state == State::Connected;
        setState(State::Error, detail);
        cleanupSockets();
        if (wasConnected) {
            emit recoverableDisconnect(detail);
        }
    }
}
#else
void KiwiSdrClient::sendSoundCommand(const QString&)
{
}

void KiwiSdrClient::sendWaterfallCommand(const QString&)
{
}
#endif

QString KiwiSdrClient::stateLabel(State state)
{
    switch (state) {
    case State::Disconnected: return tr("Disconnected");
    case State::Connecting:   return tr("Connecting");
    case State::Connected:    return tr("Connected");
    case State::Error:        return tr("Error");
    }
    return tr("Disconnected");
}

void KiwiSdrClient::setState(State state, const QString& detail)
{
    if (m_state == state && m_stateDetail == detail) {
        return;
    }

    m_state = state;
    m_stateDetail = detail;
    if (m_audioReadyTimer && m_state != State::Connecting) {
        m_audioReadyTimer->stop();
    }
    if (m_state != State::Connected) {
        setAudioActive(false);
    }

    emit stateChanged(m_state, detail);
}

} // namespace AetherSDR
