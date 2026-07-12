#include "KiwiSdrClient.h"

#include "KiwiSdrRedirectPolicy.h"
#include "KiwiSdrProtocol.h"
#include "LogManager.h"
#include "Resampler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QList>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#ifdef HAVE_WEBSOCKETS
#include <QAbstractSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QWebSocket>
#include <QWebSocketProtocol>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace AetherSDR {
namespace {
constexpr int kAudioReadyTimeoutMs = 12000;
constexpr int kKeepaliveIntervalMs = 10000;
constexpr int kStatusPreflightTimeoutMs = 5000;
constexpr int kStatusPreflightMaxRedirects = 8;
constexpr quint16 kDefaultKiwiSdrPort = 8073;
constexpr double kDefaultWaterfallCenterMhz = 15.0;
constexpr double kDefaultWaterfallBandwidthMhz = 30.0;
constexpr int kSpecSoundHeaderBytes = 6;
constexpr int kObservedSoundHeaderBytes = 10;
constexpr quint8 kSoundCompressedFlag = 0x10;
constexpr quint8 kSoundRestartFlag = 0x20;
constexpr int kSpecWaterfallHeaderBytes = 4;
constexpr int kExtendedWaterfallHeaderBytes = 16;
constexpr int kDefaultWaterfallZoomCap = 14;
constexpr int kDefaultWaterfallFftBins = 1024;
constexpr int kWaterfallMinDbmLimit = -260;
constexpr int kWaterfallMaxDbmLimit = 30;
constexpr int kWaterfallAutoHistoryRows = 20;
constexpr int kWaterfallAutoMinRows = kWaterfallAutoHistoryRows;
constexpr float kWaterfallAutoReuseToleranceDb = 5.0f;
constexpr quint64 kMaxSequenceGapPaddingFrames = 8;
constexpr int kWaterfallGuiMinIntervalMs = 33;
constexpr double kWaterfallStartFixedPointScale = 16777216.0; // 2^24
constexpr quint64 kWebSocketSessionIdBase = 1ULL << 62;
constexpr const char* kSoundCompressionEnv = "AETHER_KIWI_SND_COMP";
constexpr const char* kWaterfallCompressionEnv = "AETHER_KIWI_WF_COMP";

QString trKiwiSdrClient(const char* sourceText)
{
    return QCoreApplication::translate("KiwiSdrClient", sourceText);
}

KiwiSdrProtocol::WaterfallDisplayRange clampedWaterfallDisplayRange(
    KiwiSdrProtocol::WaterfallDisplayRange range)
{
    if (!range.valid) {
        return range;
    }

    range.minDbm = std::clamp(range.minDbm,
                              static_cast<float>(kWaterfallMinDbmLimit),
                              static_cast<float>(kWaterfallMaxDbmLimit - 1));
    range.maxDbm = std::clamp(range.maxDbm,
                              range.minDbm + 1.0f,
                              static_cast<float>(kWaterfallMaxDbmLimit));
    return range;
}

QString statusPreflightFailureMessage(int firstHttpStatus,
                                      const QString& firstError,
                                      int finalHttpStatus,
                                      const QString& finalError)
{
    const int usefulStatus =
        firstHttpStatus > 0 ? firstHttpStatus : finalHttpStatus;
    const QString usefulError =
        !firstError.isEmpty() ? firstError : finalError;
    if (usefulStatus == 401 || usefulStatus == 403) {
        return trKiwiSdrClient("This KiwiSDR denied access to its status page "
                               "(HTTP %1), so AetherSDR won't connect. The server "
                               "or proxy may be blocking this IP address, which can "
                               "happen after a per-IP time limit is reached, or it "
                               "may be denying public access for another reason.")
            .arg(usefulStatus);
    }
    if (usefulStatus >= 400) {
        return trKiwiSdrClient("This KiwiSDR's status page returned HTTP %1, so "
                               "AetherSDR couldn't verify its access policy and "
                               "won't connect.")
            .arg(usefulStatus);
    }
    if (!usefulError.isEmpty()) {
        return trKiwiSdrClient("Couldn't verify this KiwiSDR's access policy "
                               "(status page error: %1), so AetherSDR won't "
                               "connect. Try again later.")
            .arg(usefulError);
    }
    return trKiwiSdrClient("Couldn't verify this KiwiSDR's access policy (its "
                           "status page is unreachable), so AetherSDR won't "
                           "connect. Try again later.");
}

// Source-attributed badp labels are documented in the KiwiSDR design note.
QString badpMessage(int badp, const QString& identityDiagnostic)
{
    switch (badp) {
    case 1:
        return trKiwiSdrClient("KiwiSDR rejected this authentication attempt "
                               "(badp=1). %1 This endpoint may require a "
                               "password or site-specific login.")
            .arg(identityDiagnostic);
    case 2:
        return trKiwiSdrClient("This KiwiSDR is still determining whether this "
                               "client is local (badp=2). Try again in a few "
                               "moments.");
    case 3:
        return trKiwiSdrClient("This KiwiSDR does not allow access from this IP "
                               "address (badp=3).");
    case 4:
        return trKiwiSdrClient("This KiwiSDR does not allow remote admin access "
                               "because no admin password is set (badp=4).");
    case 5:
        return trKiwiSdrClient("KiwiSDR rejected public receive access (badp=5). "
                               "This can mean the server does not allow another "
                               "connection from this IP address; some deployed "
                               "receivers have also returned it for public-access "
                               "rejection. %1 This endpoint may require a password "
                               "or site-specific login.")
            .arg(identityDiagnostic);
    case 6:
        return trKiwiSdrClient("This KiwiSDR is temporarily unavailable while "
                               "its database is updating (badp=6). Try again "
                               "in about a minute.");
    case 7:
        return trKiwiSdrClient("This KiwiSDR already has an admin connection "
                               "open (badp=7).");
    default:
        return trKiwiSdrClient("KiwiSDR rejected public receive access "
                               "(badp=%1). %2 This endpoint may require a "
                               "password, site-specific login, or a different "
                               "public access policy.")
            .arg(badp)
            .arg(identityDiagnostic);
    }
}

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
    normalized.agcGainDb = std::clamp(
        normalized.agcGainDb,
        KiwiSdrProtocol::kAgcManualGainMinDb,
        KiwiSdrProtocol::kAgcManualGainMaxDb);
    normalized.agcThresholdDb = std::clamp(
        normalized.agcThresholdDb,
        KiwiSdrProtocol::kAgcThresholdMinDb,
        KiwiSdrProtocol::kAgcThresholdMaxDb);
    normalized.agcDecayMs = std::clamp(
        normalized.agcDecayMs,
        KiwiSdrProtocol::kAgcDecayMinMs,
        KiwiSdrProtocol::kAgcDecayMaxMs);
    if (normalized.squelchEnabled) {
        normalized.squelchThresholdDb = std::clamp(
            normalized.squelchThresholdDb,
            KiwiSdrProtocol::kSquelchServerMinMarginDb,
            KiwiSdrProtocol::kSquelchServerMaxMarginDb);
        if (normalized.squelchThresholdDb == KiwiSdrProtocol::kSquelchOffLevel) {
            normalized.squelchThresholdDb = 1;
        }
    } else {
        normalized.squelchThresholdDb = KiwiSdrProtocol::kSquelchOffLevel;
    }
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
        && a.squelchThresholdDb == b.squelchThresholdDb;
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

QString abbreviatedMsgValue(const QString& value)
{
    constexpr qsizetype kMaxLoggedMsgValueChars = 160;
    if (value.size() <= kMaxLoggedMsgValueChars) {
        return value;
    }

    return value.left(kMaxLoggedMsgValueChars)
        + QStringLiteral("...(len=")
        + QString::number(value.size())
        + QLatin1Char(')');
}

bool soundFrameCompressed(quint8 flags)
{
    return (flags & kSoundCompressedFlag) != 0;
}

bool soundFrameRestart(quint8 flags)
{
    return (flags & kSoundRestartFlag) != 0;
}

int soundPayloadOffset(const QByteArray& frame)
{
    return frame.size() >= kObservedSoundHeaderBytes
        ? kObservedSoundHeaderBytes
        : kSpecSoundHeaderBytes;
}

#ifdef HAVE_WEBSOCKETS
QString webSocketOrigin(const QString& scheme, const QString& host, quint16 port)
{
    const QString originScheme = scheme == QStringLiteral("wss")
        ? QStringLiteral("https")
        : QStringLiteral("http");
    return QStringLiteral("%1://%2:%3")
        .arg(originScheme)
        .arg(host)
        .arg(port);
}

QNetworkRequest kiwiWebSocketRequest(const QString& url,
                                     const QString& origin)
{
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("AetherSDR"));
    request.setRawHeader("Origin", origin.toUtf8());
    return request;
}

QUrl kiwiStatusUrl(const QString& host, quint16 port, bool secure)
{
    QUrl url;
    // Plain Kiwis serve /status over http on their own port; proxied/TLS-only
    // Kiwis serve it over https on 443 (mirrors the wss-on-443 socket retry).
    url.setScheme(secure ? QStringLiteral("https") : QStringLiteral("http"));
    url.setHost(host);
    url.setPort(secure ? 443 : port);
    url.setPath(QStringLiteral("/status"));
    return url;
}
#endif

}

KiwiSdrReceiverTelemetry::KiwiSdrReceiverTelemetry()
    : protocol(KiwiSdrProtocol::defaultProtocolState())
{
    if (KiwiSdrClient::diagnosticSoundCompressionRequested()) {
        protocol.sound.compressedRequested = true;
        protocol.sound.uncompressedRequested = false;
    }
    if (KiwiSdrClient::diagnosticWaterfallCompressionRequested()) {
        protocol.waterfall.compressedRequested = true;
        protocol.waterfall.uncompressedRequested = false;
    }
}

KiwiSdrClient::KiwiSdrClient(QObject* parent)
    : QObject(parent)
{
    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(kKeepaliveIntervalMs);
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

        setState(State::Error, setupTimeoutDetail());
        cleanupSockets();
    });

    m_waterfallRowFlushTimer = new QTimer(this);
    m_waterfallRowFlushTimer->setSingleShot(true);
    connect(m_waterfallRowFlushTimer, &QTimer::timeout,
            this, &KiwiSdrClient::flushPendingWaterfallRow);
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

void KiwiSdrClient::connectToEndpoint(const QString& endpoint,
                                      const QString& password)
{
    if (!KiwiSdrProtocol::authPasswordFitsServerLimit(password)) {
        setState(
            State::Error,
            tr("The KiwiSDR password is too long after URL encoding "
               "(maximum %1 characters).")
                .arg(KiwiSdrProtocol::kAuthPasswordEncodedMaxLength));
        return;
    }

    QString host;
    quint16 port = 0;
    if (!parseEndpoint(endpoint, &host, &port)) {
        setState(State::Error, tr("Enter a KiwiSDR endpoint as hostname or hostname:port."));
        return;
    }

    m_endpoint = QStringLiteral("%1:%2").arg(host).arg(port);
    m_password = password;
    m_host = host;
    m_port = port;
    m_webSocketPort = 0;
    m_secureWebSocket = false;
    m_secureWebSocketRetryAttempted = false;
    m_soundSocketConnected = false;
    m_waterfallSocketConnected = false;
    m_monitorMode = false;
    m_monitorQueueRequested = false;
    m_campAccepted = false;
    m_preflightReportedFull = false;
    m_soundAudioReady = false;
    m_soundAudioRateAcked = false;
    m_soundSampleRateCommandsSent = false;
    m_soundSampleRatePending = false;
    m_soundFrameSeen = false;
    m_loggedSoundFrameShape = false;
    m_loggedWaterfallFrameShape = false;
    m_soundSampleRateHz = 12000.0;
    m_soundAudioRateText.clear();
    m_soundResamplerRateHz = 0.0;
    m_soundResampler.reset();
    KiwiSdrProtocol::resetSoundAdpcmState(&m_soundAdpcmState);
    m_lastSoundFrameLayout = KiwiSdrProtocol::FrameLayout::Unknown;
    m_haveSoundAudioRate = false;
    m_haveSoundSampleRate = false;
    m_soundDiagWindowStartUtcMs = 0;
    m_lastSoundFrameUtcMs = 0;
    m_lastSoundKeepaliveSentUtcMs = 0;
    m_soundDiagFrames = 0;
    m_soundDiagBytes = 0;
    m_soundDiagDecodedSamples = 0;
    m_waterfallDiagWindowStartUtcMs = 0;
    m_lastWaterfallFrameUtcMs = 0;
    m_waterfallDiagFrames = 0;
    m_waterfallDiagBytes = 0;
    m_telemetry = {};
    resetCapabilityState();
    m_telemetryPending = false;
    m_lastSoundIdentityCallsign.clear();
    m_lastWaterfallIdentityCallsign.clear();
    m_lastDecodedSoundPcm.clear();
    m_lastWaterfallBins.clear();
    m_waterfallAutoScaleRows.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;
    m_waterfallServerAutoMinValid = false;
    m_waterfallServerAutoMaxValid = false;
    m_waterfallServerAutoZoom = 0;
    m_waterfallClientAutoRangeValid = false;
    m_waterfallClientAutoZoom = 0;
    m_waterfallAutoScalePending = true;
    m_waterfallRowPending = false;
    m_pendingWaterfallPanId.clear();
    m_pendingWaterfallBins.clear();
    m_pendingWaterfallLowMhz = 0.0;
    m_pendingWaterfallHighMhz = 0.0;
    m_pendingWaterfallTimecode = 0;
    m_lastWaterfallRowEmitUtcMs = 0;
    if (m_waterfallRowFlushTimer) {
        m_waterfallRowFlushTimer->stop();
    }
    m_waterfallServerCenterMhz = kDefaultWaterfallCenterMhz;
    m_waterfallServerBandwidthMhz = kDefaultWaterfallBandwidthMhz;
    m_waterfallZoomCap = kDefaultWaterfallZoomCap;
    m_waterfallFftBins = kDefaultWaterfallFftBins;
    m_waterfallRequestValid = false;
    m_waterfallRequestPanId.clear();
    m_waterfallRequestLowMhz = 0.0;
    m_waterfallRequestHighMhz = 0.0;
    m_waterfallAvailable = true;
    m_waterfallAvailabilityDetail.clear();
    m_waterfallRxChannel = -1;
    m_waterfallChannelCount = -1;
    m_userDisconnecting = true;
    cleanupSockets();
    m_userDisconnecting = false;
    const QString callsign = kiwiIdentityCallsign();
    setState(State::Connecting,
             callsign.isEmpty()
                 ? tr("Checking KiwiSDR access policy for %1.").arg(m_endpoint)
                 : tr("Checking KiwiSDR access policy for %1 as %2.")
                       .arg(m_endpoint, callsign));

#ifdef HAVE_WEBSOCKETS
    m_statusPreflightSecure = false;  // try http first, then https
    m_statusPreflightRedirectCount = 0;
    m_statusPreflightFirstHttpStatus = 0;
    m_statusPreflightFirstError.clear();
    startStatusPreflight(kiwiStatusUrl(m_host, m_port, m_statusPreflightSecure));
#else
    setState(State::Error, tr("Qt WebSockets support is required for KiwiSDR."));
#endif
}

#ifdef HAVE_WEBSOCKETS
void KiwiSdrClient::startStatusPreflight(const QUrl& url)
{
    if (!m_statusNetworkAccessManager) {
        m_statusNetworkAccessManager = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("AetherSDR"));
    request.setTransferTimeout(kStatusPreflightTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);

    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR status preflight"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << request.url().toString();
    m_statusReply = m_statusNetworkAccessManager->get(request);
    connect(m_statusReply, &QNetworkReply::finished, this,
            [this, reply = m_statusReply]() {
        handleStatusPreflightFinished(reply);
    });
}

void KiwiSdrClient::handleStatusPreflightFinished(QNetworkReply* reply)
{
    if (!reply || reply != m_statusReply) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    m_statusReply = nullptr;
    const QUrl url = reply->url();
    const QUrl requestUrl = reply->request().url();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorText = reply->errorString();
    const QUrl redirectUrl =
        reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QByteArray payload = ok ? reply->readAll() : QByteArray();
    reply->deleteLater();

    if (m_userDisconnecting || m_state != State::Connecting) {
        return;
    }

    if (httpStatus >= 300 && httpStatus < 400) {
        if (redirectUrl.isEmpty()) {
            setState(
                State::Error,
                tr("This KiwiSDR's status page returned unsupported HTTP %1 without a redirect target, so AetherSDR won't connect.")
                    .arg(httpStatus));
            cleanupSockets();
            return;
        }

        const QUrl nextUrl = requestUrl.resolved(redirectUrl);
        QString redirectDetail;
        if (m_statusPreflightRedirectCount >= kStatusPreflightMaxRedirects) {
            redirectDetail = QStringLiteral("too many redirects");
        } else {
            const bool allowed =
                KiwiSdrRedirectPolicy::isAllowedStatusRedirect(
                    requestUrl, nextUrl, &redirectDetail);
            if (allowed) {
                redirectDetail.clear();
            }
        }

        if (!redirectDetail.isEmpty()) {
            qCWarning(lcKiwiSdr).noquote()
                << "KiwiSDR status preflight rejected redirect"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "from=" << requestUrl.toString()
                << "to=" << nextUrl.toString()
                << "reason=" << redirectDetail;
            setState(
                State::Error,
                tr("This KiwiSDR's status page redirected outside its trusted KiwiSDR proxy host, so AetherSDR won't connect."));
            cleanupSockets();
            return;
        }

        ++m_statusPreflightRedirectCount;
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR status preflight redirect"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "from=" << requestUrl.toString()
            << "to=" << nextUrl.toString();
        startStatusPreflight(nextUrl);
        return;
    }

    if (ok) {
        const QString serverHeader =
            QString::fromLatin1(reply->rawHeader("Server"));
        KiwiSdrProtocol::ReceiverMetadata statusMetadata =
            KiwiSdrProtocol::parseStatusPayload(payload, serverHeader);

        const int extApiChannels = statusMetadata.hasExtApi
            ? statusMetadata.extApi
            : -1;
        const int users = statusMetadata.hasUsers ? statusMetadata.users : -1;
        const int usersMax =
            statusMetadata.hasUsersMax ? statusMetadata.usersMax : -1;
        const int preempt =
            statusMetadata.hasPreempt ? statusMetadata.preempt : 0;
        const bool hasUsers = statusMetadata.hasUsers;
        const bool hasUsersMax = statusMetadata.hasUsersMax;
        const bool statusReportsFull =
            hasUsers && hasUsersMax && usersMax > 0 && users >= usersMax
            && preempt <= 0;
        if (statusReportsFull) {
            statusMetadata.busy = true;
            statusMetadata.hasBusy = true;
            m_preflightReportedFull = true;
        }
        mergeReceiverMetadata(statusMetadata);

        if (statusMetadata.hasExtApi) {
            qCInfo(lcKiwiSdr).noquote()
                << "KiwiSDR status preflight"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "ext_api=" << extApiChannels;
            if (extApiChannels == 0) {
                setState(
                    State::Error,
                    tr("This KiwiSDR operator does not allow external API clients such as AetherSDR."));
                cleanupSockets();
                return;
            }
        } else {
            qCInfo(lcKiwiSdr).noquote()
                << "KiwiSDR status preflight"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "ext_api=unreported";
        }

        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR status preflight"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "users="
            << (hasUsers ? QString::number(users) : QStringLiteral("unreported"))
            << "users_max="
            << (hasUsersMax ? QString::number(usersMax) : QStringLiteral("unreported"))
            << "preempt=" << preempt;
        if (statusReportsFull) {
            qCInfo(lcKiwiSdr).noquote()
                << "KiwiSDR status preflight reports full receiver; "
                   "continuing to WebSocket admission for monitor/camping offer"
                << QStringLiteral("endpoint=%1").arg(logEndpoint());
        }

        const QString resolvedScheme = url.scheme().toLower();
        if (resolvedScheme == QStringLiteral("http")
            || resolvedScheme == QStringLiteral("https")) {
            const QString resolvedHost = url.host();
            const bool resolvedSecure = resolvedScheme == QStringLiteral("https");
            const int resolvedPort = url.port(resolvedSecure ? 443 : 80);
            if (!resolvedHost.isEmpty() && resolvedPort > 0 && resolvedPort <= 65535) {
                const quint16 currentSocketPort = m_webSocketPort > 0
                    ? m_webSocketPort
                    : (m_secureWebSocket ? 443 : m_port);
                const bool changed =
                    m_secureWebSocket != resolvedSecure
                    || currentSocketPort != static_cast<quint16>(resolvedPort)
                    || m_host.compare(resolvedHost, Qt::CaseInsensitive) != 0;
                if (changed) {
                    qCInfo(lcKiwiSdr).noquote()
                        << "KiwiSDR status preflight resolved transport"
                        << QStringLiteral("endpoint=%1").arg(logEndpoint())
                        << "status_url=" << url.toString()
                        << "websocket="
                        << QStringLiteral("%1://%2:%3")
                               .arg(resolvedSecure ? QStringLiteral("wss")
                                                   : QStringLiteral("ws"))
                               .arg(resolvedHost)
                               .arg(resolvedPort);
                }
                m_host = resolvedHost;
                m_port = static_cast<quint16>(resolvedPort);
                m_webSocketPort = static_cast<quint16>(resolvedPort);
                m_secureWebSocket = resolvedSecure;
                if (resolvedSecure) {
                    m_secureWebSocketRetryAttempted = true;
                }
            }
        }
    } else {
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR status preflight unavailable"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "url=" << url.toString()
            << "http_status=" << httpStatus
            << "error=" << errorText;

        // The http /status failed.  Many Kiwis are proxied / TLS-only, so retry
        // once over https before giving up.
        if (!m_statusPreflightSecure) {
            m_statusPreflightFirstHttpStatus = httpStatus;
            m_statusPreflightFirstError = errorText;
            m_statusPreflightSecure = true;
            m_statusPreflightRedirectCount = 0;
            startStatusPreflight(
                kiwiStatusUrl(m_host, m_port, m_statusPreflightSecure));
            return;
        }

        // Both http and https failed: we cannot read ext_api, so we cannot
        // confirm the operator permits external API clients.  Fail CLOSED —
        // honoring a possible ext_api=0 takes priority over connecting.  (A
        // server whose /status is unreachable is almost always down for the
        // WebSocket path too, so this rarely blocks a working receiver.)
        setState(
            State::Error,
            statusPreflightFailureMessage(m_statusPreflightFirstHttpStatus,
                                          m_statusPreflightFirstError,
                                          httpStatus,
                                          errorText));
        cleanupSockets();
        return;
    }

    const QString callsign = kiwiIdentityCallsign();
    setState(State::Connecting,
             callsign.isEmpty()
                 ? tr("Connecting to %1 without a radio callsign.").arg(m_endpoint)
                 : tr("Connecting to %1 as %2.").arg(m_endpoint, callsign));
    openWebSockets();
}

void KiwiSdrClient::openWebSockets()
{
    const QString scheme = m_secureWebSocket
        ? QStringLiteral("wss")
        : QStringLiteral("ws");
    const quint16 socketPort = m_webSocketPort > 0
        ? m_webSocketPort
        : (m_secureWebSocket ? 443 : m_port);
    // Clean black-box observation against KiwiSDR v1.842 showed the current
    // web client using /ws/kiwi/<session>/<stream>. Some servers still upgrade
    // /<session>/<stream> but never emit MSG or stream frames on that path.
    const quint64 sessionId =
        kWebSocketSessionIdBase
        + static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    const QString sessionIdText = QString::number(sessionId);
    const QString secureAwareBase = QStringLiteral("%1://%2:%3/ws/kiwi/%4")
        .arg(scheme)
        .arg(m_host)
        .arg(socketPort)
        .arg(sessionIdText);
    const QString soundUrl = secureAwareBase + QStringLiteral("/SND");
    const QString waterfallUrl = secureAwareBase + QStringLiteral("/W/F");
    const QString origin = webSocketOrigin(scheme, m_host, socketPort);
    resetProtocolTrace();
    traceConnectionInfo(scheme, socketPort, sessionIdText, origin,
                        soundUrl, waterfallUrl);

    m_soundSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_soundSocket, &QWebSocket::connected, this, [this]() {
        m_soundSocketConnected = true;
        sendSoundSetupCommands();
        m_keepaliveTimer->start();
        m_audioReadyTimer->start();
    });
    connect(m_soundSocket, &QWebSocket::textMessageReceived,
            this, [this](const QString& text) {
        handleTextMessage(StreamKind::Sound, text);
    });
    connect(m_soundSocket, &QWebSocket::binaryMessageReceived,
            this, [this](const QByteArray& frame) {
        handleBinaryMessage(StreamKind::Sound, frame);
    });
    connect(m_soundSocket, &QWebSocket::disconnected, this, [this]() {
        const int closeCode = m_soundSocket
            ? static_cast<int>(m_soundSocket->closeCode())
            : -1;
        const QString closeReason =
            m_soundSocket ? m_soundSocket->closeReason() : QString();
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR SND closed"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "code=" << closeCode
            << "reason=" << closeReason
            << "saw_snd="
            << (m_soundFrameSeen ? QStringLiteral("yes") : QStringLiteral("no"));
        traceClose(StreamKind::Sound, closeCode, closeReason);
        if (!m_userDisconnecting) {
            if (m_state == State::Connecting) {
                if (retryWithSecureWebSocket(m_soundSocketConnected)) {
                    return;
                }
                setState(State::Error,
                         tr("KiwiSDR sound connection closed during setup. %1")
                             .arg(identityDiagnosticText()));
                cleanupSockets();
            } else if (m_state == State::Camping) {
                const QString detail =
                    tr("KiwiSDR monitor audio connection closed.");
                setState(State::CampDisconnected, detail);
                cleanupSockets();
            } else if (m_state == State::Waiting
                       || m_state == State::Busy) {
                setState(State::Busy,
                         tr("KiwiSDR busy/waiting connection closed."));
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
                const QString error =
                    m_soundSocket ? m_soundSocket->errorString() : QString();
                handleSocketError(error.isEmpty()
                                      ? tr("KiwiSDR sound socket error.")
                                      : tr("KiwiSDR sound socket error: %1")
                                            .arg(error),
                                  m_soundSocketConnected);
            });
    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR SND URL"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << soundUrl;
    m_soundSocket->open(kiwiWebSocketRequest(soundUrl, origin));

    m_waterfallSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_waterfallSocket, &QWebSocket::connected, this, [this]() {
        m_waterfallSocketConnected = true;
        sendWaterfallSetupCommands();
    });
    connect(m_waterfallSocket, &QWebSocket::textMessageReceived,
            this, [this](const QString& text) {
        handleTextMessage(StreamKind::Waterfall, text);
    });
    connect(m_waterfallSocket, &QWebSocket::binaryMessageReceived,
            this, [this](const QByteArray& frame) {
        handleBinaryMessage(StreamKind::Waterfall, frame);
    });
    connect(m_waterfallSocket, &QWebSocket::disconnected, this, [this]() {
        const int closeCode = m_waterfallSocket
            ? static_cast<int>(m_waterfallSocket->closeCode())
            : -1;
        const QString closeReason =
            m_waterfallSocket ? m_waterfallSocket->closeReason() : QString();
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR W/F closed"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "code=" << closeCode
            << "reason=" << closeReason;
        traceClose(StreamKind::Waterfall, closeCode, closeReason);
        if (!m_userDisconnecting) {
            if (m_monitorMode
                || m_state == State::Waiting
                || m_state == State::Camping
                || m_state == State::Busy
                || m_state == State::CampDisconnected) {
                return;
            }
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
                const QString error = m_waterfallSocket
                    ? m_waterfallSocket->errorString()
                    : QString();
                handleSocketError(error.isEmpty()
                                      ? tr("KiwiSDR waterfall socket error.")
                                      : tr("KiwiSDR waterfall socket error: %1")
                                            .arg(error),
                                  m_waterfallSocketConnected);
            });
    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR W/F URL"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << waterfallUrl;
    m_waterfallSocket->open(kiwiWebSocketRequest(waterfallUrl, origin));
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
                                    int filterHighHz, const QString& panId,
                                    const QString& bandName)
{
    const QString normalizedBandName = bandName.trimmed();
    if (m_trackedSliceId == sliceId
        && qFuzzyCompare(m_trackedFrequencyMhz, frequencyMhz)
        && m_trackedMode == mode
        && m_trackedFilterLowHz == filterLowHz
        && m_trackedFilterHighHz == filterHighHz
        && m_trackedPanId == panId
        && m_trackedBandName == normalizedBandName) {
        return;
    }

    const bool bandChanged =
        (!m_trackedBandName.isEmpty() || !normalizedBandName.isEmpty())
        && m_trackedBandName != normalizedBandName;
    m_trackedSliceId = sliceId;
    m_trackedFrequencyMhz = frequencyMhz;
    m_trackedBandName = normalizedBandName;
    m_trackedMode = mode;
    m_trackedFilterLowHz = filterLowHz;
    m_trackedFilterHighHz = filterHighHz;
    m_trackedPanId = panId;
    if (bandChanged) {
        resetWaterfallAutoScaleHistory();
    }

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

void KiwiSdrClient::setWaterfallDisplayRange(int minDbm, int maxDbm,
                                             bool autoScale)
{
    const int clampedMin = std::clamp(minDbm,
                                      kWaterfallMinDbmLimit,
                                      kWaterfallMaxDbmLimit - 1);
    const int clampedMax = std::clamp(maxDbm,
                                      clampedMin + 1,
                                      kWaterfallMaxDbmLimit);
    if (m_waterfallManualMinDbm == clampedMin
        && m_waterfallManualMaxDbm == clampedMax
        && m_waterfallAutoScaleEnabled == autoScale) {
        return;
    }

    const bool autoEnabledChanged = m_waterfallAutoScaleEnabled != autoScale;
    m_waterfallManualMinDbm = clampedMin;
    m_waterfallManualMaxDbm = clampedMax;
    m_waterfallAutoScaleEnabled = autoScale;
    if (autoScale) {
        if (autoEnabledChanged || !m_waterfallClientAutoRangeValid) {
            m_waterfallAutoScalePending = true;
        }
    } else {
        m_waterfallAutoScalePending = false;
    }
    sendWaterfallDisplayAdjustmentsToServer();
    emitWaterfallDisplayRangeChanged();
}

void KiwiSdrClient::requestWaterfallAutoScale()
{
    m_waterfallAutoScaleEnabled = true;
    m_waterfallAutoScalePending = true;
    if (!m_waterfallAutoScaleRows.isEmpty()) {
        updateWaterfallAutoScale(true);
        if (m_waterfallAutoScalePending) {
            emitWaterfallDisplayRangeChanged(true);
        }
        return;
    }

    emitWaterfallDisplayRangeChanged(true);
}

void KiwiSdrClient::setWaterfallRateOverride(int rate)
{
    const int clamped = std::clamp(rate, 0, 4);
    if (m_waterfallRateOverride == clamped) {
        return;
    }

    m_waterfallRateOverride = clamped;
    sendWaterfallRateToServer();
}

void KiwiSdrClient::setAudioActive(bool active)
{
    const bool allowed = active && stateHasReceiveAudio(m_state);
    if (m_audioActive == allowed) {
        return;
    }

    m_audioActive = allowed;
    m_lastDecodedSoundPcm.clear();
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

bool KiwiSdrClient::diagnosticWaterfallCompressionRequested()
{
    return KiwiSdrProtocol::diagnosticCompressionFlagEnabled(
        qgetenv(kWaterfallCompressionEnv));
}

bool KiwiSdrClient::diagnosticSoundCompressionRequested()
{
    return KiwiSdrProtocol::diagnosticCompressionFlagEnabled(
        qgetenv(kSoundCompressionEnv));
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
    // Release the sound resampler on teardown, matching the other two teardown
    // sites (connectToEndpoint / stream-rate change). It was the one sound-decode
    // member cleanupSockets() left alive, so it lingered per retained
    // KiwiSdrClient until profile removal. It is rebuilt lazily on the next
    // connection once the rate is re-negotiated. Minor on its own (~0.2 MB per
    // receiver); the dominant #4199 growth is the cached waterfall history freed
    // in KiwiSdrManager::disconnectProfile().
    m_soundResamplerRateHz = 0.0;
    m_soundResampler.reset();
    KiwiSdrProtocol::resetSoundAdpcmState(&m_soundAdpcmState);
    m_lastSoundFrameLayout = KiwiSdrProtocol::FrameLayout::Unknown;
    m_soundAudioRateText.clear();
    m_haveSoundAudioRate = false;
    m_haveSoundSampleRate = false;
    m_soundDiagWindowStartUtcMs = 0;
    m_lastSoundFrameUtcMs = 0;
    m_lastSoundKeepaliveSentUtcMs = 0;
    m_soundDiagFrames = 0;
    m_soundDiagBytes = 0;
    m_soundDiagDecodedSamples = 0;
    m_waterfallDiagWindowStartUtcMs = 0;
    m_lastWaterfallFrameUtcMs = 0;
    m_waterfallDiagFrames = 0;
    m_waterfallDiagBytes = 0;
    m_lastWaterfallBins.clear();
    m_waterfallAutoScaleRows.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;
    m_waterfallRowPending = false;
    m_pendingWaterfallPanId.clear();
    m_pendingWaterfallBins.clear();
    m_pendingWaterfallLowMhz = 0.0;
    m_pendingWaterfallHighMhz = 0.0;
    m_pendingWaterfallTimecode = 0;
    m_lastWaterfallRowEmitUtcMs = 0;
    if (m_waterfallRowFlushTimer) {
        m_waterfallRowFlushTimer->stop();
    }
    m_waterfallAvailable = true;
    m_waterfallAvailabilityDetail.clear();
    m_waterfallRxChannel = -1;
    m_waterfallChannelCount = -1;
    m_waterfallCalibrationDb = 0;
    m_waterfallServerAutoMinDbm = 0.0f;
    m_waterfallServerAutoMaxDbm = 0.0f;
    m_waterfallServerAutoMinValid = false;
    m_waterfallServerAutoMaxValid = false;
    m_waterfallServerAutoZoom = 0;
    m_waterfallClientAutoMinDbm = 0.0f;
    m_waterfallClientAutoMaxDbm = 0.0f;
    m_waterfallClientAutoRangeValid = false;
    m_waterfallClientAutoZoom = 0;
    m_waterfallAutoScalePending = true;
    m_lastEmittedWaterfallRangeValid = false;
    m_lastEmittedWaterfallAutoRange = false;
    emit waterfallAvailabilityChanged(true, QString());

#ifdef HAVE_WEBSOCKETS
    if (m_statusReply) {
        m_statusReply->disconnect(this);
        m_statusReply->abort();
        m_statusReply->deleteLater();
        m_statusReply = nullptr;
    }

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
    sendSoundCommand(KiwiSdrProtocol::formatAuthCommand(m_password));
    sendSoundIdentityToServer();
    sendSoundCommand(KiwiSdrProtocol::formatSoundCompressionCommand(
        diagnosticSoundCompressionRequested()));
}

void KiwiSdrClient::sendSoundAudioRateAck()
{
    if (m_soundAudioRateAcked) {
        return;
    }

    m_soundAudioRateAcked = true;
    const QString inputRate = !m_soundAudioRateText.isEmpty()
        ? m_soundAudioRateText
        : QString::number(m_soundSampleRateHz, 'f', 0);
    sendSoundCommand(QStringLiteral("SET AR OK in=%1 out=24000")
        .arg(inputRate));
}

void KiwiSdrClient::sendSoundSampleRateCommands()
{
    if (m_soundSampleRateCommandsSent) {
        return;
    }

    if (!m_soundAudioRateAcked) {
        m_soundSampleRatePending = true;
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR SND setup waiting"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "missing=audio_rate";
        return;
    }

    m_soundSampleRatePending = false;
    m_soundSampleRateCommandsSent = true;
    sendSoundCommand(QStringLiteral("SERVER DE CLIENT AetherSDR SND"));
    const KiwiSdrReceiverControls c = normalizedControls(m_receiverControls);
    sendSoundCommand(KiwiSdrProtocol::formatSquelchCommand(
        c.squelchEnabled, c.squelchThresholdDb));
    sendSoundCommand(QStringLiteral("SET genattn=0"));
    sendSoundCommand(QStringLiteral("SET gen=0 mix=-1"));
    sendSoundCommand(KiwiSdrProtocol::formatAgcCommand(
        c.agcEnabled, c.agcHang, c.agcThresholdDb,
        c.agcGainDb, c.agcDecayMs));
    sendTrackedSliceToServer();
    sendKeepalive();
}

void KiwiSdrClient::sendWaterfallSetupCommands()
{
    sendWaterfallCommand(KiwiSdrProtocol::formatAuthCommand(m_password));
    sendWaterfallIdentityToServer();
    sendWaterfallCommand(QStringLiteral("SERVER DE CLIENT AetherSDR W/F"));
    sendWaterfallCommand(KiwiSdrProtocol::formatWaterfallCompressionCommand(
        diagnosticWaterfallCompressionRequested()));
    sendWaterfallCommand(QStringLiteral("SET send_dB=1"));
    sendWaterfallViewToServer();
    sendWaterfallDisplayAdjustmentsToServer();
    sendWaterfallCommand(QStringLiteral("SET interp=13"));
    sendWaterfallCommand(QStringLiteral("SET window_func=2"));
    sendWaterfallRateToServer();
}

void KiwiSdrClient::queueKiwiMonitor()
{
    if (m_monitorQueueRequested) {
        return;
    }

    m_monitorQueueRequested = true;
    sendSoundCommand(QStringLiteral("SET MON_QSET=1"));
}

void KiwiSdrClient::sendTrackedSliceToServer()
{
    if (receiverControlSuppressed()) {
        return;
    }
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
            << "KiwiSDR refusing invalid passband"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "mode=" << mode
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
    if (receiverControlSuppressed()) {
        return;
    }
#ifdef HAVE_WEBSOCKETS
    if (!m_soundSocket
        || m_soundSocket->state() != QAbstractSocket::ConnectedState
        || !m_soundSampleRateCommandsSent) {
        return;
    }
#endif
    const KiwiSdrReceiverControls c = normalizedControls(m_receiverControls);
    sendSoundCommand(KiwiSdrProtocol::formatSquelchCommand(
        c.squelchEnabled, c.squelchThresholdDb));
    sendSoundCommand(KiwiSdrProtocol::formatAgcCommand(
        c.agcEnabled, c.agcHang, c.agcThresholdDb,
        c.agcGainDb, c.agcDecayMs));
}

void KiwiSdrClient::sendWaterfallViewToServer()
{
    if (receiverControlSuppressed()) {
        m_waterfallRequestValid = false;
        return;
    }
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
    m_waterfallAutoScaleRows.clear();
    m_lastWaterfallPanId.clear();
    m_lastWaterfallLowMhz = 0.0;
    m_lastWaterfallHighMhz = 0.0;
    m_lastWaterfallRowValid = false;

    sendWaterfallCommand(QStringLiteral("SET zoom=%1 start=%2")
        .arg(zoom)
        .arg(start));
    sendWaterfallDisplayAdjustmentsToServer();
    emitWaterfallDisplayRangeChanged();
}

void KiwiSdrClient::sendWaterfallDisplayAdjustmentsToServer()
{
    if (receiverControlSuppressed()) {
        return;
    }
    const KiwiSdrProtocol::WaterfallDisplayRange range =
        currentWaterfallDisplayRange();
    if (!range.valid) {
        return;
    }
    sendWaterfallCommand(QStringLiteral("SET maxdb=%1 mindb=%2")
        .arg(static_cast<double>(range.maxDbm), 0, 'f', 0)
        .arg(static_cast<double>(range.minDbm), 0, 'f', 0));
}

KiwiSdrProtocol::WaterfallDisplayRange
KiwiSdrClient::adjustedAutoWaterfallDisplayRange(float minDbm,
                                                 float maxDbm,
                                                 int sourceZoom) const
{
    return clampedWaterfallDisplayRange(
        KiwiSdrProtocol::zoomAdjustedWaterfallDisplayRange(
            minDbm, maxDbm, sourceZoom, m_waterfallRequestZoom));
}

KiwiSdrProtocol::WaterfallDisplayRange
KiwiSdrClient::currentWaterfallDisplayRange() const
{
    if (!m_waterfallAutoScaleEnabled) {
        return clampedWaterfallDisplayRange(
            KiwiSdrProtocol::adjustedWaterfallDisplayRange(
                static_cast<float>(m_waterfallManualMinDbm),
                static_cast<float>(m_waterfallManualMaxDbm),
                0,
                0));
    }

    if (m_waterfallClientAutoRangeValid) {
        return adjustedAutoWaterfallDisplayRange(
            m_waterfallClientAutoMinDbm,
            m_waterfallClientAutoMaxDbm,
            m_waterfallClientAutoZoom);
    }

    if (m_waterfallServerAutoMinValid && m_waterfallServerAutoMaxValid) {
        return adjustedAutoWaterfallDisplayRange(
            m_waterfallServerAutoMinDbm,
            m_waterfallServerAutoMaxDbm,
            m_waterfallServerAutoZoom);
    }

    return clampedWaterfallDisplayRange(
        KiwiSdrProtocol::adjustedWaterfallDisplayRange(
            m_waterfallMinDbm
                - KiwiSdrProtocol::waterfallZoomCorrectionDb(
                    m_waterfallRequestZoom),
            m_waterfallMaxDbm,
            0,
            0));
}

void KiwiSdrClient::emitWaterfallDisplayRangeChanged(bool force)
{
    const bool autoRange =
        m_waterfallAutoScaleEnabled
        && (m_waterfallClientAutoRangeValid
            || (m_waterfallServerAutoMinValid && m_waterfallServerAutoMaxValid));
    const KiwiSdrProtocol::WaterfallDisplayRange range =
        currentWaterfallDisplayRange();
    if (!range.valid) {
        return;
    }

    if (!force && m_lastEmittedWaterfallRangeValid
        && m_lastEmittedWaterfallAutoRange == autoRange
        && std::abs(m_lastEmittedWaterfallMinDbm - range.minDbm) < 0.05f
        && std::abs(m_lastEmittedWaterfallMaxDbm - range.maxDbm) < 0.05f) {
        return;
    }

    m_lastEmittedWaterfallMinDbm = range.minDbm;
    m_lastEmittedWaterfallMaxDbm = range.maxDbm;
    m_lastEmittedWaterfallRangeValid = true;
    m_lastEmittedWaterfallAutoRange = autoRange;
    emit waterfallDisplayRangeChanged(range.minDbm, range.maxDbm, autoRange);
}

void KiwiSdrClient::recordWaterfallAutoScaleRow(const QVector<float>& binsDbm)
{
    if (binsDbm.size() != kDefaultWaterfallFftBins) {
        return;
    }

    m_waterfallAutoScaleRows.append(binsDbm);
    while (m_waterfallAutoScaleRows.size() > kWaterfallAutoHistoryRows) {
        m_waterfallAutoScaleRows.remove(0);
    }
}

void KiwiSdrClient::resetWaterfallAutoScaleHistory()
{
    m_waterfallAutoScaleRows.clear();
    m_waterfallClientAutoRangeValid = false;
    if (m_waterfallAutoScaleEnabled) {
        m_waterfallAutoScalePending = true;
    }
}

void KiwiSdrClient::updateWaterfallAutoScale(bool force)
{
    if (!m_waterfallAutoScaleEnabled) {
        return;
    }

    if (!force && !m_waterfallAutoScalePending) {
        return;
    }

    if (m_waterfallAutoScaleRows.isEmpty()) {
        return;
    }

    if (m_waterfallAutoScaleRows.size() < kWaterfallAutoMinRows) {
        return;
    }

    const KiwiSdrProtocol::WaterfallDisplayRange range =
        clampedWaterfallDisplayRange(
            KiwiSdrProtocol::autoWaterfallDisplayRangeFromRows(
                m_waterfallAutoScaleRows));
    if (!range.valid) {
        return;
    }

    if (!force
        && m_waterfallClientAutoRangeValid
        && std::abs(m_waterfallClientAutoMinDbm - range.minDbm)
            <= kWaterfallAutoReuseToleranceDb
        && std::abs(m_waterfallClientAutoMaxDbm - range.maxDbm)
            <= kWaterfallAutoReuseToleranceDb) {
        m_waterfallAutoScalePending = false;
        return;
    }

    if (!force
        && m_waterfallClientAutoRangeValid
        && std::abs(m_waterfallClientAutoMinDbm - range.minDbm) < 0.05f
        && std::abs(m_waterfallClientAutoMaxDbm - range.maxDbm) < 0.05f) {
        m_waterfallAutoScalePending = false;
        return;
    }

    m_waterfallClientAutoMinDbm = range.minDbm;
    m_waterfallClientAutoMaxDbm = range.maxDbm;
    m_waterfallClientAutoRangeValid = true;
    m_waterfallClientAutoZoom = m_waterfallRequestZoom;
    m_waterfallAutoScalePending = false;
    sendWaterfallDisplayAdjustmentsToServer();
    emitWaterfallDisplayRangeChanged(force);
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

bool KiwiSdrClient::isSupportedSoundFrame(
    const KiwiSdrProtocol::FrameObservation& observation) const
{
    return observation.stream == KiwiSdrProtocol::StreamMode::Sound
        && observation.supported;
}

QByteArray KiwiSdrClient::resampleSoundSamples(const QVector<float>& monoSamples)
{
    if (monoSamples.isEmpty()) {
        return {};
    }

    if (!m_soundResampler || std::abs(m_soundResamplerRateHz - m_soundSampleRateHz) > 1.0) {
        m_soundResampler = std::make_unique<Resampler>(
            m_soundSampleRateHz, 24000.0);
        m_soundResamplerRateHz = m_soundSampleRateHz;
    }
    return m_soundResampler->processMonoToStereo(monoSamples.constData(),
                                                 monoSamples.size());
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
    QVector<float> bins = KiwiSdrProtocol::decodeWaterfallFrame(
        frame, m_waterfallZoomCap).binsDbm;
    if (m_waterfallCalibrationDb != 0) {
        for (float& bin : bins) {
            bin = KiwiSdrProtocol::calibratedWaterfallLevel(
                bin, m_waterfallCalibrationDb);
        }
    }
    return bins;
}

void KiwiSdrClient::handleBinaryMessage(StreamKind stream,
                                        const QByteArray& frame)
{
    traceInboundBinary(stream, frame);
    if (frame.startsWith("MSG")) {
        handleMessage(stream, frame);
    } else if (frame.startsWith("SND")) {
        handleSoundFrame(frame);
    } else if (frame.startsWith("W/F")) {
        handleWaterfallFrame(frame);
    } else if (frame.startsWith("EXT")) {
        KiwiSdrProtocol::FrameObservation observation;
        observation.stream = KiwiSdrProtocol::StreamMode::Extension;
        observation.layout = KiwiSdrProtocol::FrameLayout::Extension;
        observation.frameBytes = frame.size();
        observation.supported = false;
        observation.unsupportedReason =
            QStringLiteral("Kiwi extensions are not supported");
        recordFrameObservation(observation);
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR EXT ignored"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    } else {
        const QString tag = QString::fromLatin1(frame.left(3));
        KiwiSdrProtocol::FrameObservation observation;
        observation.stream = KiwiSdrProtocol::StreamMode::Unknown;
        observation.layout = KiwiSdrProtocol::FrameLayout::UnknownBinary;
        observation.frameBytes = frame.size();
        observation.supported = false;
        observation.unsupportedReason =
            QStringLiteral("Unknown KiwiSDR binary tag: %1").arg(tag);
        recordFrameObservation(observation);
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR unknown binary tag=" << tag
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
}

void KiwiSdrClient::handleSoundFrame(const QByteArray& frame)
{
    m_soundFrameSeen = true;
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 deltaMs = m_lastSoundFrameUtcMs > 0
        ? nowUtcMs - m_lastSoundFrameUtcMs
        : 0;
    m_lastSoundFrameUtcMs = nowUtcMs;
    const bool logSoundFrameShape = !m_loggedSoundFrameShape;
    if (logSoundFrameShape) {
        m_loggedSoundFrameShape = true;
        qCDebug(lcKiwiSdrAudio).noquote()
            << "KiwiSDR SND frame"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
    const KiwiSdrProtocol::SoundFrameHeader header =
        KiwiSdrProtocol::parseSoundFrameHeader(frame);
    const KiwiSdrProtocol::FrameObservation soundObservation =
        KiwiSdrProtocol::classifySoundFrame(frame);
    recordFrameObservation(soundObservation);
    const int payloadOffset = header.valid ? soundPayloadOffset(frame) : 0;
    const qsizetype payloadBytes =
        std::max<qsizetype>(0, frame.size() - payloadOffset);
    if (!isSupportedSoundFrame(soundObservation)) {
        qCWarning(lcKiwiSdrAudio).noquote()
            << "KiwiSDR SND unsupported frame shape"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame)
            << "flags=0x"
            << (header.valid
                    ? QString::number(header.flags, 16).rightJustified(
                          2, QLatin1Char('0'))
                    : QStringLiteral("--"))
            << "compressed="
            << (header.valid && soundFrameCompressed(header.flags)
                    ? QStringLiteral("true")
                    : QStringLiteral("false"))
            << "payload_offset=" << payloadOffset
            << "payload_len=" << payloadBytes
            << "decoder=unsupported-shape"
            << "reason=" << soundObservation.unsupportedReason
            << "audio_rate=" << QString::number(m_soundSampleRateHz, 'f', 0)
            << "sequence=" << header.sequence
            << "receive_utc_ms=" << nowUtcMs
            << "delta_ms=" << deltaMs;
        if (header.valid && soundFrameCompressed(header.flags)) {
            KiwiSdrProtocol::invalidateSoundAdpcmState(&m_soundAdpcmState);
            m_lastSoundFrameLayout = KiwiSdrProtocol::FrameLayout::Unknown;
            m_lastDecodedSoundPcm.clear();
        }
        return;
    }
    const quint64 sequenceGaps = header.valid
        ? KiwiSdrProtocol::sequenceGapCount(m_telemetry.soundSequence,
                                            header.sequence)
        : 0;
    const bool compressedSound =
        soundObservation.layout == KiwiSdrProtocol::FrameLayout::SndCompressed;
    const bool restartFlag =
        header.valid && soundFrameRestart(header.flags);
    const bool layoutChanged =
        m_lastSoundFrameLayout != KiwiSdrProtocol::FrameLayout::Unknown
        && m_lastSoundFrameLayout != soundObservation.layout;
    if (compressedSound) {
        if (layoutChanged || restartFlag) {
            KiwiSdrProtocol::resetSoundAdpcmState(&m_soundAdpcmState);
            m_lastDecodedSoundPcm.clear();
            qCDebug(lcKiwiSdrAudio).noquote()
                << "KiwiSDR SND ADPCM decoder reset"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "layout_changed=" << (layoutChanged ? "true" : "false")
                << "sequence_gap=" << sequenceGaps
                << "restart_flag=" << (restartFlag ? "true" : "false")
                << "sequence=" << header.sequence;
        }
        if (sequenceGaps > 0 && !restartFlag) {
            KiwiSdrProtocol::invalidateSoundAdpcmState(&m_soundAdpcmState);
            m_lastDecodedSoundPcm.clear();
            qCDebug(lcKiwiSdrAudio).noquote()
                << "KiwiSDR SND ADPCM decoder state invalidated"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "sequence_gap=" << sequenceGaps
                << "sequence=" << header.sequence;
        }
    } else if (!compressedSound && layoutChanged) {
        KiwiSdrProtocol::resetSoundAdpcmState(&m_soundAdpcmState);
        m_lastDecodedSoundPcm.clear();
    }
    m_lastSoundFrameLayout = soundObservation.layout;
    updateSoundTelemetry(frame);
    KiwiSdrProtocol::MeterReading meterReading =
        KiwiSdrProtocol::extractMeterFromSndVerifiedLayout(
            frame, KiwiSdrProtocol::MeterContext{});
    if (meterReading.valid) {
        meterReading.squelchStateKnown = true;
        meterReading.squelched = header.squelched;
    }
    markSoundAudioReady();
    KiwiSdrProtocol::SoundFrameDecodeResult decodedSound;
    const bool decodeForAudio =
        m_audioActive || m_decodeAudioWhenInactive;
    const bool decodeForState = decodeForAudio || compressedSound;
    if (decodeForState) {
        if (compressedSound && !m_soundAdpcmState.valid && !restartFlag) {
            traceProtocolEvent(
                QStringLiteral("DROP SND compressed frame until ADPCM restart "
                               "seq=%1 gaps=%2")
                    .arg(header.sequence)
                    .arg(sequenceGaps));
            return;
        }
        decodedSound =
            KiwiSdrProtocol::decodeSoundFrame(frame, &m_soundAdpcmState);
        if (!decodedSound.observation.supported
            || decodedSound.monoSamples.isEmpty()) {
            if (compressedSound) {
                KiwiSdrProtocol::invalidateSoundAdpcmState(&m_soundAdpcmState);
                m_lastDecodedSoundPcm.clear();
            }
            return;
        }
    }
    ++m_soundDiagFrames;
    m_soundDiagBytes += static_cast<quint64>(frame.size());
    m_soundDiagDecodedSamples += static_cast<quint64>(
        decodedSound.monoSamples.size());
    if (m_soundDiagWindowStartUtcMs == 0) {
        m_soundDiagWindowStartUtcMs = nowUtcMs;
    }
    const qint64 diagElapsedMs = nowUtcMs - m_soundDiagWindowStartUtcMs;
    if (diagElapsedMs >= 1000) {
        const double elapsedSeconds =
            static_cast<double>(diagElapsedMs) / 1000.0;
        const qint64 keepaliveAgeMs = m_lastSoundKeepaliveSentUtcMs > 0
            ? nowUtcMs - m_lastSoundKeepaliveSentUtcMs
            : -1;
        qCDebug(lcKiwiSdrAudio).noquote()
            << "KiwiSDR SND rate"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "frames_per_sec="
            << QString::number(static_cast<double>(m_soundDiagFrames)
                               / elapsedSeconds, 'f', 1)
            << "bytes_per_sec="
            << QString::number(static_cast<double>(m_soundDiagBytes)
                               / elapsedSeconds, 'f', 0)
            << "decoded_samples_per_sec="
            << QString::number(static_cast<double>(m_soundDiagDecodedSamples)
                               / elapsedSeconds, 'f', 0)
            << "last_delta_ms=" << deltaMs
            << "audio_rate=" << QString::number(m_soundSampleRateHz, 'f', 0)
            << "keepalive_age_ms=" << keepaliveAgeMs;
        const qint64 msgAgeMs = m_lastInboundMsgUtcMs > 0
            ? nowUtcMs - m_lastInboundMsgUtcMs
            : -1;
        const qint64 outAgeMs = m_lastOutboundCommandUtcMs > 0
            ? nowUtcMs - m_lastOutboundCommandUtcMs
            : -1;
        traceProtocolEvent(
            QStringLiteral("RATE SND frames_per_sec=%1 bytes_per_sec=%2 "
                           "decoded_samples_per_sec=%3 "
                           "receive_queue_depth=n/a audio_queue_depth=n/a "
                           "dropped_frames=%4 last_keepalive_age_ms=%5 "
                           "last_inbound_msg_age_ms=%6 "
                           "last_outbound_command_age_ms=%7")
                .arg(QString::number(static_cast<double>(m_soundDiagFrames)
                                     / elapsedSeconds, 'f', 1))
                .arg(QString::number(static_cast<double>(m_soundDiagBytes)
                                     / elapsedSeconds, 'f', 0))
                .arg(QString::number(
                    static_cast<double>(m_soundDiagDecodedSamples)
                        / elapsedSeconds,
                    'f',
                    0))
                .arg(m_telemetry.soundSequenceGaps)
                .arg(keepaliveAgeMs)
                .arg(msgAgeMs)
                .arg(outAgeMs));
        m_soundDiagWindowStartUtcMs = nowUtcMs;
        m_soundDiagFrames = 0;
        m_soundDiagBytes = 0;
        m_soundDiagDecodedSamples = 0;
    }
    if (!m_audioActive && !m_decodeAudioWhenInactive) {
        return;
    }

    QByteArray pcm = resampleSoundSamples(decodedSound.monoSamples);
    if (!pcm.isEmpty()) {
        if (header.squelched) {
            pcm.fill(0);
        }
        if (logSoundFrameShape) {
            qCDebug(lcKiwiSdrAudio).noquote()
                << "KiwiSDR SND decode"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << "len=" << frame.size()
                << "flags=0x"
                << QString::number(header.flags, 16).rightJustified(
                       2, QLatin1Char('0'))
                << "compressed=" << (compressedSound ? "true" : "false")
                << "payload_offset=" << payloadOffset
                << "payload_len=" << payloadBytes
                << "decoder="
                << (compressedSound ? "adpcm16" : "pcm16")
                << "decoded_samples=" << decodedSound.monoSamples.size()
                << "audio_rate=" << QString::number(m_soundSampleRateHz, 'f', 0)
                << "sequence=" << header.sequence
                << "receive_utc_ms=" << nowUtcMs
                << "delta_ms=" << deltaMs;
        }
        const quint64 padFrames = std::min(sequenceGaps,
                                          kMaxSequenceGapPaddingFrames);
        if (!compressedSound && !m_lastDecodedSoundPcm.isEmpty()) {
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
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 deltaMs = m_lastWaterfallFrameUtcMs > 0
        ? nowUtcMs - m_lastWaterfallFrameUtcMs
        : 0;
    m_lastWaterfallFrameUtcMs = nowUtcMs;
    ++m_waterfallDiagFrames;
    m_waterfallDiagBytes += static_cast<quint64>(frame.size());
    if (m_waterfallDiagWindowStartUtcMs == 0) {
        m_waterfallDiagWindowStartUtcMs = nowUtcMs;
    }
    const qint64 diagElapsedMs = nowUtcMs - m_waterfallDiagWindowStartUtcMs;
    if (diagElapsedMs >= 1000) {
        const double elapsedSeconds =
            static_cast<double>(diagElapsedMs) / 1000.0;
        traceProtocolEvent(
            QStringLiteral("RATE W/F frames_per_sec=%1 bytes_per_sec=%2 "
                           "dropped_frames=%3 last_delta_ms=%4 "
                           "request_valid=%5 request_zoom=%6 request_start=%7 "
                           "request_low_mhz=%8 request_high_mhz=%9")
                .arg(QString::number(static_cast<double>(
                                         m_waterfallDiagFrames)
                                     / elapsedSeconds,
                                     'f',
                                     1))
                .arg(QString::number(static_cast<double>(
                                         m_waterfallDiagBytes)
                                     / elapsedSeconds,
                                     'f',
                                     0))
                .arg(m_telemetry.waterfallSequenceGaps)
                .arg(deltaMs)
                .arg(m_waterfallRequestValid ? QStringLiteral("true")
                                             : QStringLiteral("false"))
                .arg(m_waterfallRequestZoom)
                .arg(m_waterfallRequestStart)
                .arg(QString::number(m_waterfallRequestLowMhz, 'f', 6))
                .arg(QString::number(m_waterfallRequestHighMhz, 'f', 6)));
        m_waterfallDiagWindowStartUtcMs = nowUtcMs;
        m_waterfallDiagFrames = 0;
        m_waterfallDiagBytes = 0;
    }
    if (!m_loggedWaterfallFrameShape) {
        m_loggedWaterfallFrameShape = true;
        qCDebug(lcKiwiSdr).noquote()
            << "KiwiSDR W/F frame"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "len=" << frame.size()
            << "first=" << firstBytesHex(frame);
    }
    const KiwiSdrProtocol::WaterfallLineHeader header =
        KiwiSdrProtocol::parseWaterfallLineHeader(frame);
    recordFrameObservation(
        KiwiSdrProtocol::classifyWaterfallFrame(frame, m_waterfallZoomCap));
    const quint64 sequenceGaps = header.valid
        ? KiwiSdrProtocol::sequenceGapCount(m_telemetry.waterfallSequence,
                                            header.sequence)
        : 0;
    updateWaterfallTelemetry(frame);
    quint32 frameStart = 0;
    int frameZoom = 0;
    const bool hasExtendedHeader =
        parseWaterfallFrameHeader(frame, &frameStart, &frameZoom);
    const bool requestHasUsableRange =
        m_waterfallRequestValid
        && m_waterfallRequestLowMhz >= 0.0
        && m_waterfallRequestHighMhz > m_waterfallRequestLowMhz;
    if (!hasExtendedHeader && !requestHasUsableRange) {
        return;
    }

    const QVector<float> rawBins =
        KiwiSdrProtocol::decodeWaterfallFrame(frame,
                                              m_waterfallZoomCap).binsDbm;
    if (rawBins.isEmpty()) {
        return;
    }
    if (updateWaterfallFftBins(rawBins.size())) {
        return;
    }
    QVector<float> bins = rawBins;
    if (m_waterfallCalibrationDb != 0) {
        for (float& bin : bins) {
            bin = KiwiSdrProtocol::calibratedWaterfallLevel(
                bin, m_waterfallCalibrationDb);
        }
    }
    recordWaterfallAutoScaleRow(bins);
    updateWaterfallAutoScale();

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
    } else if (!requestHasUsableRange) {
        return;
    }
    const quint64 padRows = std::min(sequenceGaps,
                                     kMaxSequenceGapPaddingFrames);
    if (m_lastWaterfallRowValid
        && m_lastWaterfallPanId == panId
        && std::abs(m_lastWaterfallLowMhz - rowLowMhz) <= 1.0e-9
        && std::abs(m_lastWaterfallHighMhz - rowHighMhz) <= 1.0e-9) {
        for (quint64 i = 0; i < padRows; ++i) {
            queueWaterfallRow(panId, m_lastWaterfallBins,
                              m_lastWaterfallLowMhz,
                              m_lastWaterfallHighMhz,
                              0);
        }
    }
    queueWaterfallRow(panId, bins, rowLowMhz, rowHighMhz, 0);
    m_lastWaterfallBins = bins;
    m_lastWaterfallPanId = panId;
    m_lastWaterfallLowMhz = rowLowMhz;
    m_lastWaterfallHighMhz = rowHighMhz;
    m_lastWaterfallRowValid = true;
}

void KiwiSdrClient::handleMessage(StreamKind stream, const QByteArray& frame)
{
    const QString text = QString::fromLatin1(frame.constData(), frame.size());
    handleTextMessage(stream, text);
}

void KiwiSdrClient::handleTextMessage(StreamKind stream, const QString& text)
{
    traceInboundText(stream, text);
    const QVector<KiwiSdrProtocol::MsgToken> msgTokens =
        KiwiSdrProtocol::parseMsgTokens(text);

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
    auto messageValue = [&msgTokens](const QString& requestedKey) {
        for (const KiwiSdrProtocol::MsgToken& candidate : msgTokens) {
            if (candidate.hasValue && candidate.key == requestedKey) {
                return candidate.value;
            }
        }
        return QString();
    };
    auto downMessage = [](int downValue, const QString& reason) {
        if (downValue == 1) {
            return trKiwiSdrClient("This KiwiSDR is temporarily unavailable "
                                   "while a software update is in progress. Try "
                                   "again in a few minutes.");
        }
        if (downValue == 2) {
            return trKiwiSdrClient("This KiwiSDR is temporarily unavailable "
                                   "while a backup is in progress. Try again "
                                   "later.");
        }
        if (!reason.isEmpty()) {
            return trKiwiSdrClient("KiwiSDR endpoint is disabled: %1")
                .arg(reason);
        }
        return trKiwiSdrClient("KiwiSDR endpoint is disabled.");
    };
    updateWaterfallDisplayMetadata(stream, msgTokens);
    for (const KiwiSdrProtocol::MsgToken& token : msgTokens) {
        const QString& key = token.key;
        const QString& valueText = token.value;
        if (token.hasValue) {
            qCDebug(lcKiwiSdr).noquote()
                << "KiwiSDR MSG"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << key << "=" << abbreviatedMsgValue(valueText);
            traceProtocolEvent(QStringLiteral("PARSED %1 %2=%3")
                .arg(streamLabel(stream), key, abbreviatedMsgValue(valueText)));
        } else {
            qCDebug(lcKiwiSdr).noquote()
                << "KiwiSDR MSG"
                << QStringLiteral("endpoint=%1").arg(logEndpoint())
                << key;
            traceProtocolEvent(QStringLiteral("PARSED %1 %2")
                .arg(streamLabel(stream), key));
        }
        if (KiwiSdrProtocol::updateReceiverMetadataFromMsgToken(
                token, &m_telemetry.metadata)) {
            updateProtocolStateFromMetadata();
            emitTelemetryChanged();
        }
        if (updateCampStatusFromMetadata(token)) {
            return;
        }
        if (key == QStringLiteral("too_busy")) {
            bool busyOk = false;
            const int busyValue = valueText.toInt(&busyOk);
            // KiwiSDR sends too_busy on denial paths, not as a negative
            // status. A value of zero means the server configured zero
            // simultaneous non-Kiwi/API client channels.
            if (busyOk && busyValue == 0) {
                setState(State::Error,
                         tr("This KiwiSDR operator does not allow external API "
                            "clients such as AetherSDR."));
            } else {
                setState(State::Busy,
                         busyOk && busyValue > 0
                             ? tr("All %1 KiwiSDR receiver channels are busy. "
                                  "No waiting or camping slot was offered.")
                                   .arg(busyValue)
                             : tr("All KiwiSDR receiver channels are busy. "
                                  "No waiting or camping slot was offered."));
            }
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("reason_disabled")) {
            const QString reason =
                QUrl::fromPercentEncoding(valueText.toUtf8()).trimmed();
            bool downOk = false;
            const int downValue =
                messageValue(QStringLiteral("down")).toInt(&downOk);
            setState(State::Error,
                     downMessage(downOk ? downValue : 0, reason));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("down")) {
            bool downOk = false;
            const int downValue = valueText.toInt(&downOk);
            if (!downOk || downValue != 0) {
                const QString reason = QUrl::fromPercentEncoding(
                    messageValue(QStringLiteral("reason_disabled")).toUtf8())
                                           .trimmed();
                setState(State::Error,
                         downMessage(downOk ? downValue : 0, reason));
                cleanupSockets();
                return;
            }
            continue;
        }
        if (key == QStringLiteral("wb_only")) {
            setState(
                State::Error,
                tr("This KiwiSDR is configured for wideband use only and does "
                   "not accept normal receiver connections."));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("exclusive_use")) {
            setState(
                State::Error,
                tr("This KiwiSDR is locked for exclusive use by another "
                   "operation. Try again later or choose another receiver."));
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
        if (key == QStringLiteral("ip_limit")) {
            const KiwiSdrProtocol::IpLimitNotice notice =
                KiwiSdrProtocol::parseIpLimitNotice(valueText);
            const QString detail = notice.valid
                ? (notice.address.isEmpty()
                       ? tr("This KiwiSDR allows only %1 minutes per 24 hours "
                            "from each IP address. This IP address has reached "
                            "that limit. Try another receiver or wait for the "
                            "server's daily limit reset.")
                             .arg(notice.minutes)
                       : tr("This KiwiSDR allows only %1 minutes per 24 hours "
                            "from each IP address. The server reports this IP "
                            "address (%2) has reached that limit. Try another "
                            "receiver or wait for the server's daily limit "
                            "reset.")
                             .arg(notice.minutes)
                             .arg(notice.address))
                : tr("This KiwiSDR rejected the connection because its per-IP "
                     "usage limit was reached. Try another receiver or wait "
                     "for the server's daily limit reset.");
            setState(State::Error, detail);
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("inactivity_timeout")) {
            bool minutesOk = false;
            const int minutes = valueText.toInt(&minutesOk);
            setState(
                State::Error,
                minutesOk && minutes > 0
                    ? tr("This KiwiSDR disconnected the session after %1 "
                         "minutes without tuning or activity.")
                          .arg(minutes)
                    : tr("This KiwiSDR disconnected the session for inactivity."));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("password_timeout")) {
            setState(
                State::Error,
                tr("This KiwiSDR timed out waiting for password authentication."));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("kiwi_kick")) {
            const QString decoded =
                QUrl::fromPercentEncoding(valueText.toUtf8()).trimmed();
            const int comma = decoded.indexOf(QLatin1Char(','));
            const QString reason =
                comma >= 0 ? decoded.mid(comma + 1).trimmed() : QString();
            setState(State::Error,
                     reason.isEmpty()
                         ? tr("This KiwiSDR disconnected this client.")
                         : tr("This KiwiSDR disconnected this client: %1")
                               .arg(reason));
            cleanupSockets();
            return;
        }
        if (key == QStringLiteral("audio_init")) {
            // audio_init confirms setup progress, not usable decoded audio.
            // Wait for the first accepted SND frame so camping/silent sessions
            // do not look connected or stop the setup watchdog early.
            continue;
        }

        bool ok = false;
        const double value = valueText.toDouble(&ok);
        if (!ok) {
            continue;
        }
        if (key == QStringLiteral("badp")) {
            const int badp = static_cast<int>(value);
            const KiwiSdrProtocol::AuthMode authMode = badp == 0
                ? KiwiSdrProtocol::AuthMode::PublicPasswordless
                : (badp == 1
                       ? KiwiSdrProtocol::AuthMode::PasswordRequired
                       : KiwiSdrProtocol::AuthMode::Rejected);
            if (m_telemetry.protocol.authMode != authMode) {
                m_telemetry.protocol.authMode = authMode;
                emitTelemetryChanged();
            }
            if (badp != 0) {
                m_startupTrace.badpNonzeroSeen = true;
            }
            if (badp != 0) {
                setState(State::Error,
                         badpMessage(badp, identityDiagnosticText()));
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
        } else if (stream == StreamKind::Waterfall
                   && key == QStringLiteral("rx_chan")) {
            m_waterfallRxChannel = static_cast<int>(value);
            updateWaterfallAvailability();
        } else if (stream == StreamKind::Waterfall
                   && key == QStringLiteral("wf_chans")) {
            const int channelCount = static_cast<int>(value);
            if (channelCount >= 0) {
                m_waterfallChannelCount = channelCount;
                updateWaterfallAvailability();
            }
        } else if (stream == StreamKind::Waterfall
                   && key == QStringLiteral("wf_chans_real")) {
            const int channelCount = static_cast<int>(value);
            if (channelCount > 0) {
                m_waterfallChannelCount = channelCount;
                updateWaterfallAvailability();
            }
        } else if (key == QStringLiteral("audio_rate")) {
            m_haveSoundAudioRate = true;
            m_soundAudioRateText = valueText;
            setSoundSampleRate(value);
            sendSoundAudioRateAck();
            if (m_soundSampleRatePending) {
                sendSoundSampleRateCommands();
            }
        } else if (key == QStringLiteral("sample_rate")) {
            m_haveSoundSampleRate = true;
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

void KiwiSdrClient::updateWaterfallDisplayMetadata(
    StreamKind stream,
    const QVector<KiwiSdrProtocol::MsgToken>& msgTokens)
{
    if (stream != StreamKind::Waterfall) {
        return;
    }

    bool rangeChanged = false;
    for (const KiwiSdrProtocol::MsgToken& token : msgTokens) {
        if (!token.hasValue) {
            continue;
        }

        bool ok = false;
        const float value = token.value.trimmed().toFloat(&ok);
        if (!ok || !std::isfinite(value)) {
            continue;
        }

        if (token.key == QStringLiteral("wf_cal")) {
            const int calibrationDb = std::clamp(static_cast<int>(std::lround(value)),
                                                 -200,
                                                 200);
            if (m_waterfallCalibrationDb != calibrationDb) {
                m_waterfallCalibrationDb = calibrationDb;
                resetWaterfallAutoScaleHistory();
                if (m_waterfallAutoScaleEnabled) {
                    rangeChanged = true;
                }
            }
        } else if (token.key == QStringLiteral("mindb")) {
            const int sourceZoom = m_waterfallRequestZoom;
            if (!m_waterfallServerAutoMinValid
                || m_waterfallServerAutoZoom != sourceZoom
                || std::abs(m_waterfallServerAutoMinDbm - value) > 0.05f) {
                m_waterfallServerAutoMinDbm = value;
                m_waterfallServerAutoMinValid = true;
                m_waterfallServerAutoZoom = sourceZoom;
                rangeChanged = true;
            }
        } else if (token.key == QStringLiteral("maxdb")) {
            const int sourceZoom = m_waterfallRequestZoom;
            if (!m_waterfallServerAutoMaxValid
                || m_waterfallServerAutoZoom != sourceZoom
                || std::abs(m_waterfallServerAutoMaxDbm - value) > 0.05f) {
                m_waterfallServerAutoMaxDbm = value;
                m_waterfallServerAutoMaxValid = true;
                m_waterfallServerAutoZoom = sourceZoom;
                rangeChanged = true;
            }
        }
    }

    if (rangeChanged) {
        emitWaterfallDisplayRangeChanged();
    }
}

bool KiwiSdrClient::updateCampStatusFromMetadata(
    const KiwiSdrProtocol::MsgToken& token)
{
    const QString key = token.key.trimmed();
    if (key.isEmpty()) {
        return false;
    }

    const KiwiSdrProtocol::ReceiverMetadata& metadata = m_telemetry.metadata;
    auto setMonitorWaterfallUnavailable = [this, &metadata]() {
        QString detail;
        if (metadata.hasCampQueueReloadRecommended
            && metadata.campQueueReloadRecommended) {
            detail = tr("A KiwiSDR receiver slot is free. AetherSDR is "
                        "reconnecting for normal receiver control; the "
                        "temporary monitor session does not provide a "
                        "controllable waterfall.");
        } else if (metadata.hasCampQueuePosition
                   && metadata.hasCampQueueWaiters) {
            detail = tr("Waiting for a free KiwiSDR receiver slot "
                        "(position %1 of %2). AetherSDR will reconnect "
                        "automatically when the server reports one is "
                        "available. The monitor session does not provide a "
                        "controllable waterfall.")
                         .arg(metadata.campQueuePosition)
                         .arg(metadata.campQueueWaiters);
        } else if (metadata.campStatus == KiwiSdrProtocol::CampStatus::Accepted) {
            const QString channel = metadata.hasCampReceiverChannel
                ? tr("RX%1").arg(metadata.campReceiverChannel)
                : tr("another receiver");
            detail = tr("Monitoring KiwiSDR %1 while waiting. Monitor sessions "
                        "follow an existing receiver and do not provide a "
                        "controllable waterfall.")
                         .arg(channel);
        } else {
            detail = tr("All KiwiSDR receiver slots are busy. AetherSDR is "
                        "waiting for an available slot and will reconnect "
                        "automatically; the temporary monitor session does not "
                        "provide a controllable waterfall.");
        }
        if (!m_waterfallAvailable
            && m_waterfallAvailabilityDetail == detail) {
            return;
        }
        m_waterfallAvailable = false;
        m_waterfallAvailabilityDetail = detail;
        emit waterfallAvailabilityChanged(false, detail);
    };
    auto campChannelText = [&metadata]() {
        return metadata.hasCampReceiverChannel
            ? QStringLiteral("RX%1").arg(metadata.campReceiverChannel)
            : QStringLiteral("selected receiver channel");
    };

    if (key == QStringLiteral("monitor")) {
        m_monitorMode = true;
        setMonitorWaterfallUnavailable();
        setState(State::Waiting,
                 tr("All KiwiSDR receiver channels are busy. Waiting for a "
                    "free receiver slot; AetherSDR will reconnect "
                    "automatically when the server reports one is available. "
                    "Monitoring/camping does not allow normal tuning or mode "
                    "control."));
        queueKiwiMonitor();
        return false;
    }

    if (key == QStringLiteral("qpos")) {
        m_monitorMode = true;
        setMonitorWaterfallUnavailable();
        if (metadata.hasCampQueueReloadRecommended
            && metadata.campQueueReloadRecommended) {
            setState(State::Waiting,
                     tr("A KiwiSDR receiver slot is free. Reconnecting for "
                        "normal receiver control."));
            return false;
        }
        if (metadata.hasCampQueuePosition && metadata.hasCampQueueWaiters) {
            setState(State::Waiting,
                     tr("Waiting for a free KiwiSDR receiver slot "
                        "(position %1 of %2). AetherSDR will reconnect "
                        "automatically; normal tuning is disabled while "
                        "waiting.")
                         .arg(metadata.campQueuePosition)
                         .arg(metadata.campQueueWaiters));
            return false;
        }
        setState(State::Waiting,
                 tr("Waiting for a free KiwiSDR receiver slot. AetherSDR "
                    "will reconnect automatically; normal tuning is disabled "
                    "while waiting."));
        return false;
    }

    if (key == QStringLiteral("camp")) {
        m_monitorMode = true;
        setMonitorWaterfallUnavailable();
        if (metadata.campStatus == KiwiSdrProtocol::CampStatus::Accepted) {
            m_campAccepted = true;
            if (m_soundAudioReady) {
                setState(State::Camping,
                         tr("Monitoring KiwiSDR %1. This follows another "
                            "receiver channel, so normal tuning and mode "
                            "control are disabled.")
                             .arg(campChannelText()));
            } else {
                setState(State::Waiting,
                         tr("Camping accepted on KiwiSDR %1. Waiting for "
                            "audio before enabling monitoring.")
                             .arg(campChannelText()));
            }
            return false;
        }
        if (metadata.campStatus == KiwiSdrProtocol::CampStatus::Rejected) {
            m_campAccepted = false;
            setState(State::Busy,
                     tr("KiwiSDR rejected camping on %1 because too many "
                        "campers are already monitoring that channel.")
                         .arg(campChannelText()));
            return false;
        }
    }

    if (key == QStringLiteral("audio_camp")
        && metadata.campStatus == KiwiSdrProtocol::CampStatus::AudioStopped) {
        m_campAccepted = false;
        setState(State::CampDisconnected,
                 tr("KiwiSDR monitor audio stopped."));
        cleanupSockets();
        return true;
    }

    if (key == QStringLiteral("camp_disconnect")) {
        m_campAccepted = false;
        setState(State::CampDisconnected,
                 tr("The KiwiSDR receiver channel being monitored disconnected."));
        cleanupSockets();
        return true;
    }

    return false;
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

void KiwiSdrClient::updateWaterfallAvailability()
{
    bool available = true;
    QString detail;
    if (m_waterfallRxChannel >= 0 && m_waterfallChannelCount > 0
        && m_waterfallRxChannel >= m_waterfallChannelCount) {
        available = false;
        detail = tr("No KiwiSDR waterfall channel is available. Audio is connected on receiver slot %1, but this server only provides %2 waterfall channels.")
            .arg(m_waterfallRxChannel + 1)
            .arg(m_waterfallChannelCount);
    } else if (m_waterfallRxChannel >= 0 && m_waterfallChannelCount == 0) {
        available = false;
        detail = tr("No KiwiSDR waterfall channel is available for this receiver slot.");
    }

    if (m_waterfallAvailable == available
        && m_waterfallAvailabilityDetail == detail) {
        return;
    }

    m_waterfallAvailable = available;
    m_waterfallAvailabilityDetail = detail;
    if (!available) {
        qCWarning(lcKiwiSdr).noquote()
            << "KiwiSDR waterfall unavailable"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << "rx_chan=" << m_waterfallRxChannel
            << "wf_chans=" << m_waterfallChannelCount;
    }
    emit waterfallAvailabilityChanged(available, detail);
}

void KiwiSdrClient::resetCapabilityState()
{
    m_telemetry.protocol = KiwiSdrProtocol::defaultProtocolState();
    m_telemetry.protocol.authMode =
        KiwiSdrProtocol::AuthMode::PublicPasswordless;
    if (diagnosticSoundCompressionRequested()) {
        m_telemetry.protocol.sound.compressedRequested = true;
        m_telemetry.protocol.sound.uncompressedRequested = false;
    }
    if (diagnosticWaterfallCompressionRequested()) {
        m_telemetry.protocol.waterfall.compressedRequested = true;
        m_telemetry.protocol.waterfall.uncompressedRequested = false;
    }
}

void KiwiSdrClient::updateProtocolStateFromMetadata()
{
    KiwiSdrProtocol::ProtocolState& protocol = m_telemetry.protocol;
    const KiwiSdrProtocol::ReceiverMetadata& metadata = m_telemetry.metadata;
    if (!metadata.serverVersion.isEmpty()) {
        protocol.serverVersion = metadata.serverVersion;
    }
    if (!metadata.serverBuild.isEmpty()) {
        protocol.serverBuild = metadata.serverBuild;
    }
    if (metadata.hasExtApi) {
        protocol.extApi = metadata.extApi;
        protocol.apiPolicy = metadata.apiPolicy;
    }
}

void KiwiSdrClient::mergeReceiverMetadata(
    const KiwiSdrProtocol::ReceiverMetadata& metadata)
{
    if (KiwiSdrProtocol::mergeReceiverMetadata(&m_telemetry.metadata,
                                               metadata)) {
        updateProtocolStateFromMetadata();
        emitTelemetryChanged();
    }
}

void KiwiSdrClient::recordFrameObservation(
    const KiwiSdrProtocol::FrameObservation& observation)
{
    if (observation.layout == KiwiSdrProtocol::FrameLayout::Unknown) {
        return;
    }

    bool changed = false;
    KiwiSdrProtocol::StreamCapability* capability = nullptr;
    if (observation.stream == KiwiSdrProtocol::StreamMode::Sound) {
        capability = &m_telemetry.protocol.sound;
    } else if (observation.stream == KiwiSdrProtocol::StreamMode::Waterfall) {
        capability = &m_telemetry.protocol.waterfall;
    }

    if (capability) {
        if (!capability->observed) {
            capability->observed = true;
            changed = true;
        }
        if (capability->lastObservedLayout != observation.layout) {
            capability->lastObservedLayout = observation.layout;
            changed = true;
        }
        if (!capability->observedLayouts.contains(observation.layout)) {
            capability->observedLayouts.append(observation.layout);
            changed = true;
        }
        const bool directWaterfall =
            observation.layout
            == KiwiSdrProtocol::FrameLayout::WaterfallDirectBins;
        const bool plainSound =
            observation.layout == KiwiSdrProtocol::FrameLayout::SndPcm16
            || observation.layout
                == KiwiSdrProtocol::FrameLayout::SndObservedPcm16WithMeter;
        const bool compactOrCompressed =
            observation.layout == KiwiSdrProtocol::FrameLayout::SndCompressed
            || observation.layout
                == KiwiSdrProtocol::FrameLayout::WaterfallCompactEncoded;
        if (observation.supported
            && (directWaterfall || plainSound)
            && !capability->uncompressedObserved) {
            capability->uncompressedObserved = true;
            changed = true;
        }
        if (compactOrCompressed && !capability->compressedObserved) {
            capability->compressedObserved = true;
            changed = true;
        }
        if (!observation.supported
            && capability->unsupportedReason != observation.unsupportedReason) {
            capability->unsupportedReason = observation.unsupportedReason;
            changed = true;
        }
    }

    if (!observation.supported) {
        bool duplicate = false;
        for (const KiwiSdrProtocol::FrameObservation& existing :
             std::as_const(m_telemetry.protocol.unsupportedFrames)) {
            if (existing.stream == observation.stream
                && existing.layout == observation.layout
                && existing.unsupportedReason == observation.unsupportedReason) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            m_telemetry.protocol.unsupportedFrames.append(observation);
            while (m_telemetry.protocol.unsupportedFrames.size() > 16) {
                m_telemetry.protocol.unsupportedFrames.removeFirst();
            }
            changed = true;
        }
    }
    if (changed) {
        emitTelemetryChanged();
    }
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
        emit telemetryChanged(m_telemetry);
    });
}

void KiwiSdrClient::queueWaterfallRow(const QString& panId,
                                      const QVector<float>& binsDbm,
                                      double lowFreqMhz,
                                      double highFreqMhz,
                                      quint32 timecode)
{
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastWaterfallRowEmitUtcMs == 0
        || nowUtcMs - m_lastWaterfallRowEmitUtcMs >= kWaterfallGuiMinIntervalMs) {
        if (m_waterfallRowFlushTimer) {
            m_waterfallRowFlushTimer->stop();
        }
        m_waterfallRowPending = false;
        emitWaterfallRowNow(panId, binsDbm, lowFreqMhz, highFreqMhz, timecode);
        return;
    }

    m_waterfallRowPending = true;
    m_pendingWaterfallPanId = panId;
    m_pendingWaterfallBins = binsDbm;
    m_pendingWaterfallLowMhz = lowFreqMhz;
    m_pendingWaterfallHighMhz = highFreqMhz;
    m_pendingWaterfallTimecode = timecode;
    const int delayMs = std::max<qint64>(
        1,
        kWaterfallGuiMinIntervalMs - (nowUtcMs - m_lastWaterfallRowEmitUtcMs));
    if (m_waterfallRowFlushTimer && !m_waterfallRowFlushTimer->isActive()) {
        m_waterfallRowFlushTimer->start(delayMs);
    }
}

void KiwiSdrClient::flushPendingWaterfallRow()
{
    if (!m_waterfallRowPending) {
        return;
    }

    const QString panId = m_pendingWaterfallPanId;
    const QVector<float> binsDbm = m_pendingWaterfallBins;
    const double lowFreqMhz = m_pendingWaterfallLowMhz;
    const double highFreqMhz = m_pendingWaterfallHighMhz;
    const quint32 timecode = m_pendingWaterfallTimecode;
    m_waterfallRowPending = false;
    m_pendingWaterfallPanId.clear();
    m_pendingWaterfallBins.clear();
    m_pendingWaterfallLowMhz = 0.0;
    m_pendingWaterfallHighMhz = 0.0;
    m_pendingWaterfallTimecode = 0;
    emitWaterfallRowNow(panId, binsDbm, lowFreqMhz, highFreqMhz, timecode);
}

void KiwiSdrClient::emitWaterfallRowNow(const QString& panId,
                                        const QVector<float>& binsDbm,
                                        double lowFreqMhz,
                                        double highFreqMhz,
                                        quint32 timecode)
{
    m_lastWaterfallRowEmitUtcMs = QDateTime::currentMSecsSinceEpoch();
    emit waterfallRowReady(panId, binsDbm, lowFreqMhz, highFreqMhz, timecode);
}

void KiwiSdrClient::sendWaterfallRateToServer()
{
    if (receiverControlSuppressed()) {
        return;
    }
    if (m_waterfallRateOverride > 0) {
        sendWaterfallCommand(QStringLiteral("SET wf_speed=%1")
            .arg(m_waterfallRateOverride));
        return;
    }

    // Auto mode requests the fastest validated compact Kiwi speed. Public
    // endpoints advertise wf_fps before this command, but some do not emit
    // W/F rows until a speed is selected. Clean black-box observations showed
    // wf_speed=1 is slow, 2 is about 5 fps, 3 is about 13 fps, and 5+ can stop
    // rows entirely; keep auto at the highest accepted value.
    sendWaterfallCommand(QStringLiteral("SET wf_speed=4"));
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
    if (m_campAccepted) {
        const KiwiSdrProtocol::ReceiverMetadata& metadata =
            m_telemetry.metadata;
        const QString channel = metadata.hasCampReceiverChannel
            ? tr("RX%1").arg(metadata.campReceiverChannel)
            : tr("the selected receiver channel");
        setState(State::Camping,
                 tr("Monitoring KiwiSDR %1. This follows another receiver "
                    "channel, so normal tuning and mode control are disabled.")
                     .arg(channel));
        return;
    }

    setState(State::Connected, tr("Connected to %1.").arg(m_endpoint));
}

QString KiwiSdrClient::logEndpoint() const
{
    if (!m_endpoint.isEmpty()) {
        return m_endpoint;
    }
    if (!m_host.isEmpty() && m_port > 0) {
        return QStringLiteral("%1:%2").arg(m_host).arg(m_port);
    }
    if (!m_host.isEmpty()) {
        return m_host;
    }
    return QStringLiteral("<unset>");
}

QString KiwiSdrClient::setupTimeoutDetail() const
{
    // Preflight said the receiver was full and we proceeded to admission in
    // case a monitor/camp offer arrived. If none did (still not monitoring,
    // no camp accepted), this is the full-and-not-camp-capable case — surface
    // the precise capacity message the preflight fast-fail used to emit, rather
    // than a vague "no audio arrived" timeout.
    if (m_preflightReportedFull && !m_monitorMode
        && m_telemetry.metadata.campStatus
               == KiwiSdrProtocol::CampStatus::Unknown) {
        const KiwiSdrProtocol::ReceiverMetadata& md = m_telemetry.metadata;
        if (md.hasUsers && md.hasUsersMax) {
            return tr("This KiwiSDR endpoint is at capacity (%1/%2 users). "
                      "Try again later or choose another receiver.")
                .arg(md.users)
                .arg(md.usersMax);
        }
        return tr("This KiwiSDR endpoint is at capacity. Try again later or "
                  "choose another receiver.");
    }

    if (m_soundSampleRateCommandsSent) {
        return tr("KiwiSDR sound setup completed for %1, but no SND audio frames arrived.")
            .arg(logEndpoint());
    }

    QStringList missing;
    if (!m_haveSoundSampleRate) {
        missing.append(QStringLiteral("sample_rate"));
    }
    if (!m_haveSoundAudioRate) {
        missing.append(QStringLiteral("audio_rate"));
    }
    if (missing.isEmpty()) {
        return tr("No KiwiSDR audio channel became available from %1.")
            .arg(logEndpoint());
    }
    return tr("No KiwiSDR audio channel became available from %1 (missing %2).")
        .arg(logEndpoint(), missing.join(QStringLiteral(", ")));
}

QString KiwiSdrClient::streamLabel(StreamKind stream)
{
    switch (stream) {
    case StreamKind::Sound:
        return QStringLiteral("SND");
    case StreamKind::Waterfall:
        return QStringLiteral("W/F");
    }
    return QStringLiteral("?");
}

void KiwiSdrClient::resetProtocolTrace()
{
    m_protocolTraceStartUtcMs = QDateTime::currentMSecsSinceEpoch();
    m_protocolTraceTail.clear();
    m_lastOutboundCommand.clear();
    m_lastInboundMsg.clear();
    m_lastInboundFrameType.clear();
    m_lastOutboundCommandUtcMs = 0;
    m_lastInboundMsgUtcMs = 0;
    m_lastInboundFrameUtcMs = 0;
    m_startupTrace = {};
    m_protocolSendFailed = false;
}

qint64 KiwiSdrClient::protocolTraceElapsedMs() const
{
    if (m_protocolTraceStartUtcMs <= 0) {
        return 0;
    }
    return QDateTime::currentMSecsSinceEpoch() - m_protocolTraceStartUtcMs;
}

void KiwiSdrClient::traceProtocolEvent(const QString& event)
{
    const QString line = QStringLiteral("%1ms %2")
        .arg(protocolTraceElapsedMs(), 4, 10, QLatin1Char('0'))
        .arg(event);

    m_protocolTraceTail.append(line);
    while (m_protocolTraceTail.size() > 20) {
        m_protocolTraceTail.removeFirst();
    }
}

void KiwiSdrClient::traceConnectionInfo(const QString& scheme,
                                        quint16 socketPort,
                                        const QString& sessionId,
                                        const QString& origin,
                                        const QString& soundUrl,
                                        const QString& waterfallUrl)
{
    traceProtocolEvent(QStringLiteral("CONNECT scheme=%1 port=%2 origin=%3 "
                                      "user_agent=AetherSDR client_ip=unknown "
                                      "timestamp=%4 same_timestamp=yes "
                                      "path_style=/ws/kiwi/TIMESTAMP/STREAM")
        .arg(scheme)
        .arg(socketPort)
        .arg(origin)
        .arg(sessionId));
    traceProtocolEvent(QStringLiteral("URL SND %1").arg(soundUrl));
    traceProtocolEvent(QStringLiteral("URL W/F %1").arg(waterfallUrl));
}

void KiwiSdrClient::updateStartupTraceForOutbound(StreamKind stream,
                                                  const QString& command,
                                                  bool sent)
{
    if (!sent || stream != StreamKind::Sound) {
        return;
    }

    if (command.startsWith(QStringLiteral("SET auth "))) {
        m_startupTrace.authSent = true;
    } else if (command.startsWith(QStringLiteral("SET ident_user="))) {
        m_startupTrace.identUserSent = true;
    } else if (command.startsWith(QStringLiteral("SET browser="))) {
        m_startupTrace.browserSent = true;
    } else if (command.startsWith(QStringLiteral("SET compression="))) {
        m_startupTrace.compressionSent = true;
    } else if (command.startsWith(QStringLiteral("SET AR OK "))) {
        m_startupTrace.arOkSent = true;
        const QString expected = QStringLiteral("in=%1").arg(m_soundAudioRateText);
        m_startupTrace.arOkUsedActualAudioRate =
            m_haveSoundAudioRate
            && !m_soundAudioRateText.isEmpty()
            && command.contains(expected);
    } else if (command.startsWith(QStringLiteral("SERVER DE CLIENT "))) {
        m_startupTrace.serverDeClientSent = true;
    } else if (command.startsWith(QStringLiteral("SET squelch="))) {
        m_startupTrace.squelchSent = true;
    } else if (command == QStringLiteral("SET genattn=0")) {
        m_startupTrace.genattnSent = true;
    } else if (command.startsWith(QStringLiteral("SET gen="))) {
        m_startupTrace.genSent = true;
    } else if (command.startsWith(QStringLiteral("SET agc="))) {
        m_startupTrace.agcSent = true;
    } else if (command.startsWith(QStringLiteral("SET mod="))) {
        m_startupTrace.modSent = true;
    } else if (command == QStringLiteral("SET keepalive")) {
        m_startupTrace.keepaliveSentOnSound = true;
    }
}

void KiwiSdrClient::traceOutboundCommand(StreamKind stream,
                                         const QString& command,
                                         bool sent)
{
    const QString visible = redactedKiwiCommand(command);
    m_lastOutboundCommand =
        QStringLiteral("%1 %2").arg(streamLabel(stream), visible);
    m_lastOutboundCommandUtcMs = QDateTime::currentMSecsSinceEpoch();
    if (!sent) {
        m_protocolSendFailed = true;
    }
    updateStartupTraceForOutbound(stream, command, sent);
    traceProtocolEvent(QStringLiteral("OUT %1 %2%3")
        .arg(streamLabel(stream),
             visible,
             sent ? QString() : QStringLiteral(" send_failed=true")));
}

void KiwiSdrClient::traceInboundText(StreamKind stream, const QString& text)
{
    const QString visible = abbreviatedMsgValue(text);
    m_lastInboundMsg = QStringLiteral("%1 %2").arg(streamLabel(stream), visible);
    m_lastInboundMsgUtcMs = QDateTime::currentMSecsSinceEpoch();
    traceProtocolEvent(QStringLiteral("IN %1 %2")
        .arg(streamLabel(stream), visible));
}

void KiwiSdrClient::traceInboundBinary(StreamKind stream,
                                       const QByteArray& frame)
{
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    const QString label = streamLabel(stream);
    m_lastInboundFrameType =
        QStringLiteral("%1 binary len=%2").arg(label).arg(frame.size());
    m_lastInboundFrameUtcMs = nowUtcMs;

    if (stream != StreamKind::Sound || !frame.startsWith("SND")) {
        return;
    }

    const KiwiSdrProtocol::SoundFrameHeader header =
        KiwiSdrProtocol::parseSoundFrameHeader(frame);
    const int payloadOffset = header.valid ? soundPayloadOffset(frame) : 0;
    const int payloadBytes = static_cast<int>(
        std::max<qsizetype>(0, frame.size() - payloadOffset));
    const bool compressed = header.valid && soundFrameCompressed(header.flags);
    const KiwiSdrProtocol::FrameObservation observation =
        KiwiSdrProtocol::classifySoundFrame(frame);
    const qint64 deltaMs = m_lastSoundFrameUtcMs > 0
        ? nowUtcMs - m_lastSoundFrameUtcMs
        : 0;
    QString rawSmeter = QStringLiteral("n/a");
    if (frame.size() >= kObservedSoundHeaderBytes) {
        const auto* bytes = reinterpret_cast<const uchar*>(frame.constData());
        const quint16 raw =
            (static_cast<quint16>(bytes[8]) << 8)
            | static_cast<quint16>(bytes[9]);
        rawSmeter = QString::number(static_cast<qint16>(raw));
    }

    traceProtocolEvent(QStringLiteral("IN SND binary len=%1 first16=%2 "
                                      "flags=0x%3 compressed=%4 seq=%5 "
                                      "raw_smeter=%6 payload_offset=%7 "
                                      "payload_len=%8 decoder=%9 "
                                      "decoded_sample_count=%10 "
                                      "audio_rate=%11 expected_samples_per_sec=%12 "
                                      "receive_utc_ms=%13 delta_ms=%14")
        .arg(frame.size())
        .arg(firstBytesHex(frame))
        .arg(header.valid
                 ? QString::number(header.flags, 16)
                       .rightJustified(2, QLatin1Char('0'))
                 : QStringLiteral("--"))
        .arg(compressed ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(header.valid ? QString::number(header.sequence)
                          : QStringLiteral("n/a"))
        .arg(rawSmeter)
        .arg(payloadOffset)
        .arg(payloadBytes)
        .arg(observation.supported
                 ? (compressed ? QStringLiteral("adpcm16")
                               : QStringLiteral("pcm16"))
                 : QStringLiteral("unsupported-shape"))
        .arg(observation.supported
                 ? (compressed ? payloadBytes * 2 : payloadBytes / 2)
                 : 0)
        .arg(QString::number(m_soundSampleRateHz, 'f', 0))
        .arg(QString::number(m_soundSampleRateHz, 'f', 0))
        .arg(nowUtcMs)
        .arg(deltaMs));
}

void KiwiSdrClient::traceClose(StreamKind stream, int closeCode,
                               const QString& reason)
{
    auto boolText = [](bool value) {
        return value ? QStringLiteral("true") : QStringLiteral("false");
    };
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 keepaliveAgeMs = m_lastSoundKeepaliveSentUtcMs > 0
        ? nowUtcMs - m_lastSoundKeepaliveSentUtcMs
        : -1;
    const qint64 lastOutboundAgeMs = m_lastOutboundCommandUtcMs > 0
        ? nowUtcMs - m_lastOutboundCommandUtcMs
        : -1;
    const qint64 lastMsgAgeMs = m_lastInboundMsgUtcMs > 0
        ? nowUtcMs - m_lastInboundMsgUtcMs
        : -1;
    const qint64 lastFrameAgeMs = m_lastInboundFrameUtcMs > 0
        ? nowUtcMs - m_lastInboundFrameUtcMs
        : -1;
    const bool sndFramesContinuous =
        m_soundFrameSeen && m_telemetry.soundSequenceGaps == 0;
    const bool keepaliveTimerRunning =
        m_keepaliveTimer && m_keepaliveTimer->isActive();

    const QStringList closeFields{
        QStringLiteral("code=%1").arg(closeCode),
        QStringLiteral("reason=%1").arg(reason),
        QStringLiteral("connection_duration_ms=%1")
            .arg(protocolTraceElapsedMs()),
        QStringLiteral("last_outbound_command=\"%1\"")
            .arg(m_lastOutboundCommand),
        QStringLiteral("last_outbound_age_ms=%1").arg(lastOutboundAgeMs),
        QStringLiteral("last_inbound_msg=\"%1\"").arg(m_lastInboundMsg),
        QStringLiteral("last_inbound_msg_age_ms=%1").arg(lastMsgAgeMs),
        QStringLiteral("last_inbound_frame_type=\"%1\"")
            .arg(m_lastInboundFrameType),
        QStringLiteral("last_inbound_frame_age_ms=%1").arg(lastFrameAgeMs),
        QStringLiteral("last_keepalive_age_ms=%1").arg(keepaliveAgeMs),
        QStringLiteral("send_failed=%1").arg(boolText(m_protocolSendFailed)),
    };

    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR TRACE"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << QStringLiteral("%1ms CLOSE %2 %3")
               .arg(protocolTraceElapsedMs(), 4, 10, QLatin1Char('0'))
               .arg(streamLabel(stream),
                    closeFields.join(QLatin1Char(' ')));

    const QStringList startupFields{
        QStringLiteral("auth_sent=%1").arg(boolText(m_startupTrace.authSent)),
        QStringLiteral("auth_accepted_or_no_badp_seen=%1")
            .arg(boolText(!m_startupTrace.badpNonzeroSeen)),
        QStringLiteral("ident_user_sent=%1")
            .arg(boolText(m_startupTrace.identUserSent)),
        QStringLiteral("browser_sent=%1").arg(boolText(m_startupTrace.browserSent)),
        QStringLiteral("compression_sent=%1")
            .arg(boolText(m_startupTrace.compressionSent)),
        QStringLiteral("audio_rate_seen=%1").arg(boolText(m_haveSoundAudioRate)),
        QStringLiteral("ar_ok_sent=%1").arg(boolText(m_startupTrace.arOkSent)),
        QStringLiteral("ar_ok_used_actual_audio_rate=%1")
            .arg(boolText(m_startupTrace.arOkUsedActualAudioRate)),
        QStringLiteral("sample_rate_seen=%1")
            .arg(boolText(m_haveSoundSampleRate)),
        QStringLiteral("server_de_client_sent=%1")
            .arg(boolText(m_startupTrace.serverDeClientSent)),
        QStringLiteral("squelch_sent=%1")
            .arg(boolText(m_startupTrace.squelchSent)),
        QStringLiteral("genattn_sent=%1")
            .arg(boolText(m_startupTrace.genattnSent)),
        QStringLiteral("gen_sent=%1").arg(boolText(m_startupTrace.genSent)),
        QStringLiteral("agc_sent=%1").arg(boolText(m_startupTrace.agcSent)),
        QStringLiteral("mod_sent=%1").arg(boolText(m_startupTrace.modSent)),
        QStringLiteral("first_snd_frame_seen=%1").arg(boolText(m_soundFrameSeen)),
        QStringLiteral("snd_frames_continuous=%1")
            .arg(boolText(sndFramesContinuous)),
        QStringLiteral("keepalive_timer_running=%1")
            .arg(boolText(keepaliveTimerRunning)),
        QStringLiteral("keepalive_sent_on_snd_socket=%1")
            .arg(boolText(m_startupTrace.keepaliveSentOnSound)),
        QStringLiteral("websocket_ping_pong_ok_if_manual=not_checked"),
    };

    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR TRACE"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << QStringLiteral("startup_checklist %1")
               .arg(startupFields.join(QLatin1Char(' ')));

    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR TRACE"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << "last_20_protocol_events_begin";
    for (const QString& event : m_protocolTraceTail) {
        qCInfo(lcKiwiSdr).noquote()
            << "KiwiSDR TRACE"
            << QStringLiteral("endpoint=%1").arg(logEndpoint())
            << event;
    }
    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR TRACE"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << "last_20_protocol_events_end";
}

void KiwiSdrClient::sendKeepalive()
{
    if (m_soundSocketConnected) {
        m_lastSoundKeepaliveSentUtcMs = QDateTime::currentMSecsSinceEpoch();
    }
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
    m_webSocketPort = 443;
    m_soundSocketConnected = false;
    m_waterfallSocketConnected = false;
    m_monitorMode = false;
    m_monitorQueueRequested = false;
    m_campAccepted = false;
    // The wss retry does NOT re-run the HTTP status preflight, so clear the
    // "preflight reported full" flag too — otherwise a stale full result from
    // the ws attempt would make a wss-retry timeout emit a misleading capacity
    // message the secure attempt never actually verified.
    m_preflightReportedFull = false;
    m_soundAudioReady = false;
    m_soundAudioRateAcked = false;
    m_soundSampleRateCommandsSent = false;
    m_soundSampleRatePending = false;
    m_soundFrameSeen = false;
    m_loggedSoundFrameShape = false;
    m_loggedWaterfallFrameShape = false;
    m_haveSoundAudioRate = false;
    m_haveSoundSampleRate = false;
    m_soundAudioRateText.clear();
    m_soundDiagWindowStartUtcMs = 0;
    m_lastSoundFrameUtcMs = 0;
    m_lastSoundKeepaliveSentUtcMs = 0;
    m_soundDiagFrames = 0;
    m_soundDiagBytes = 0;
    m_soundDiagDecodedSamples = 0;
    m_waterfallDiagWindowStartUtcMs = 0;
    m_lastWaterfallFrameUtcMs = 0;
    m_waterfallDiagFrames = 0;
    m_waterfallDiagBytes = 0;
    m_soundResamplerRateHz = 0.0;
    m_soundResampler.reset();
    KiwiSdrProtocol::resetSoundAdpcmState(&m_soundAdpcmState);
    m_lastSoundFrameLayout = KiwiSdrProtocol::FrameLayout::Unknown;

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
    const bool connected =
        m_soundSocket && m_soundSocket->state() == QAbstractSocket::ConnectedState;
    if (!connected) {
        return;
    }

    const qint64 bytesWritten = m_soundSocket->sendTextMessage(command);
    traceOutboundCommand(StreamKind::Sound, command, bytesWritten >= 0);
    qCDebug(lcKiwiSdr).noquote()
        << "KiwiSDR SND ->"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << redactedKiwiCommand(command);
}

void KiwiSdrClient::sendWaterfallCommand(const QString& command)
{
    const bool connected =
        m_waterfallSocket
        && m_waterfallSocket->state() == QAbstractSocket::ConnectedState;
    if (!connected) {
        return;
    }

    const qint64 bytesWritten = m_waterfallSocket->sendTextMessage(command);
    traceOutboundCommand(StreamKind::Waterfall, command, bytesWritten >= 0);
    qCDebug(lcKiwiSdr).noquote()
        << "KiwiSDR W/F ->"
        << QStringLiteral("endpoint=%1").arg(logEndpoint())
        << redactedKiwiCommand(command);
}

void KiwiSdrClient::handleSocketError(const QString& detail,
                                      bool transportEstablished)
{
    if (!m_userDisconnecting) {
        if (retryWithSecureWebSocket(transportEstablished)) {
            return;
        }
        if (m_monitorMode
            && detail.contains(QStringLiteral("waterfall"),
                               Qt::CaseInsensitive)) {
            return;
        }
        if (m_state == State::Camping) {
            setState(State::CampDisconnected, detail);
            cleanupSockets();
            return;
        }
        if (m_state == State::Waiting || m_state == State::Busy) {
            setState(State::Busy, detail);
            cleanupSockets();
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
    case State::Busy:         return tr("Busy");
    case State::Waiting:      return tr("Waiting");
    case State::Camping:      return tr("Monitoring");
    case State::CampDisconnected: return tr("Camp disconnected");
    case State::Error:        return tr("Error");
    }
    return tr("Disconnected");
}

bool KiwiSdrClient::stateHasReceiveAudio(State state)
{
    return state == State::Connected || state == State::Camping;
}

bool KiwiSdrClient::stateAllowsReceiverControl(State state)
{
    return state == State::Connected;
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
    if (!stateHasReceiveAudio(m_state)) {
        setAudioActive(false);
    }

    emit stateChanged(m_state, detail);
}

} // namespace AetherSDR
