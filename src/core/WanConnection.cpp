#include "WanConnection.h"
#include "AppSettings.h"
#include "LogManager.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSslCertificate>
#include <QSslConfiguration>

namespace AetherSDR {

static constexpr int HEARTBEAT_INTERVAL_MS = 10000; // 10s ping (same as FlexLib)

// Cap the line-assembly buffer.  A buggy or hostile SmartLink peer that
// dribbles bytes without ever sending '\n' would otherwise grow m_readBuffer
// unbounded until QByteArray refuses to allocate (process OOM).  CAT lines
// are tens of bytes; 16 MiB is wildly larger than any legitimate radio
// command or status burst.  Same pattern as RadioConnection (issue #2955)
// and GHSA-7w4w-wfqm-wh93 (M2, RigctlServer).
static constexpr int kMaxReadBuffer = 16 * 1024 * 1024;

namespace {

// TOFU cert-pin cache.  Stored as a JSON object in AppSettings under
// the key "SmartLinkCertFingerprintCache".  See GHSA-wfx7-w6p8-4jr2.
//
// Phase 1 shape (warn-only): { "<host>": "<sha256-hex>", ... }
// Phase 2 shape (#2951, enforcement): {
//     "<host>": { "fp": "<sha256-hex>", "pinnedAt": "<iso8601>" },
//     ...
// }
//
// Cache reader handles both shapes so upgrades don't lose existing
// pins; the writer always emits Phase 2 shape.
constexpr const char* kCertCacheKey = "SmartLinkCertFingerprintCache";

struct PinEntry {
    QString fingerprintHex;
    QString pinnedAtIso;   // empty for Phase 1 entries (no timestamp recorded)
};

QJsonObject loadCertCache()
{
    const QByteArray json = AppSettings::instance().value(kCertCacheKey).toByteArray();
    if (json.isEmpty()) return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

PinEntry pinEntryFromValue(const QJsonValue& v)
{
    PinEntry e{};
    if (v.isString()) {
        // Phase 1 string-only entry — no timestamp available.
        e.fingerprintHex = v.toString();
    } else if (v.isObject()) {
        const QJsonObject obj = v.toObject();
        e.fingerprintHex = obj.value(QStringLiteral("fp")).toString();
        e.pinnedAtIso    = obj.value(QStringLiteral("pinnedAt")).toString();
    }
    return e;
}

QString loadStoredFingerprint(const QString& host)
{
    return pinEntryFromValue(loadCertCache().value(host)).fingerprintHex;
}

void storeFingerprint(const QString& host, const QString& fingerprintHex)
{
    QJsonObject obj = loadCertCache();
    QJsonObject entry;
    entry[QStringLiteral("fp")] = fingerprintHex;
    entry[QStringLiteral("pinnedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    obj[host] = entry;
    AppSettings::instance().setValue(
        kCertCacheKey,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace

// File-scope helpers exposed for the Pinned Certs UI (#2951).
namespace WanCertCache {
QVector<PinnedCertInfo> listPinnedCerts()
{
    QVector<PinnedCertInfo> out;
    const QJsonObject obj = loadCertCache();
    out.reserve(obj.size());
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const PinEntry e = pinEntryFromValue(it.value());
        if (!e.fingerprintHex.isEmpty())
            out.append({it.key(), e.fingerprintHex, e.pinnedAtIso});
    }
    return out;
}

void forgetPinnedCert(const QString& host)
{
    QJsonObject obj = loadCertCache();
    obj.remove(host);
    AppSettings::instance().setValue(
        kCertCacheKey,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void forgetAllPinnedCerts()
{
    AppSettings::instance().remove(kCertCacheKey);
}
} // namespace WanCertCache

WanConnection::WanConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QSslSocket::connected,    this, &WanConnection::onTlsConnected);
    connect(&m_socket, &QSslSocket::disconnected, this, &WanConnection::onTlsDisconnected);
    connect(&m_socket, &QSslSocket::readyRead,    this, &WanConnection::onReadyRead);
    connect(&m_socket, &QSslSocket::sslErrors,    this, &WanConnection::onSslErrors);
    connect(&m_socket, &QAbstractSocket::errorOccurred,
            this, &WanConnection::onSocketError);

    m_heartbeat.setInterval(HEARTBEAT_INTERVAL_MS);
    connect(&m_heartbeat, &QTimer::timeout, this, &WanConnection::onHeartbeat);
}

WanConnection::~WanConnection()
{
    disconnectFromRadio();
}

// ─── Connection ──────────────────────────────────────────────────────────────

void WanConnection::connectToRadio(const QString& host, quint16 tlsPort,
                                    const QString& wanHandle)
{
    if (m_connected || m_socket.state() != QAbstractSocket::UnconnectedState) {
        qCWarning(lcSmartLink) << "WanConnection: already connected or still closing";
        return;
    }

    m_wanHandle = wanHandle;
    m_validated = false;
    m_handle    = 0;
    m_host      = host;
    m_expectedFingerprintHex = loadStoredFingerprint(host);

    qCDebug(lcSmartLink) << "WanConnection: TLS connecting to" << host << ":" << tlsPort
                         << (m_expectedFingerprintHex.isEmpty()
                                ? "(no prior cert pin)"
                                : "(prior cert pin loaded)");

    // Radio uses self-signed certificate — VerifyNone is still required to
    // complete the handshake.  The TOFU fingerprint check in onTlsConnected
    // is what catches MITM; see GHSA-wfx7-w6p8-4jr2.
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_socket.setSslConfiguration(sslConfig);

    m_socket.connectToHostEncrypted(host, tlsPort);
}

void WanConnection::disconnectFromRadio()
{
    m_heartbeat.stop();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        // Send disconnect marker (FlexLib sends 0x04)
        m_socket.write("\x04");
        m_socket.disconnectFromHost();
    }
    m_pendingCallbacks.clear();
    m_seqCounter = 1;
    m_handle     = 0;
    m_connected  = false;
    m_validated  = false;
    m_awaitingCertDecision   = false;
    m_presentedFingerprintHex.clear();
}

// ─── Command dispatch ────────────────────────────────────────────────────────

quint32 WanConnection::sendCommand(const QString& command, ResponseCallback callback)
{
    if (!m_connected) {
        qCWarning(lcSmartLink) << "WanConnection::sendCommand: not connected";
        return 0;
    }

    const quint32 seq = m_seqCounter.fetch_add(1);
    if (callback)
        m_pendingCallbacks.insert(seq, std::move(callback));

    const QByteArray data = CommandParser::buildCommand(seq, command);
    if (command.startsWith("ping")) {
        m_lastPingSeq = seq;
        m_pingStopwatch.restart();
    } else {
        qCDebug(lcSmartLink) << "WAN TX:" << data.trimmed();
    }
    m_socket.write(data);
    m_socket.flush();  // push SSL plaintext buffer to OS TCP buffer immediately (#rade-shutdown)
    return seq;
}

// ─── TLS Socket Callbacks ────────────────────────────────────────────────────

void WanConnection::onTlsConnected()
{
    qCDebug(lcSmartLink) << "WanConnection: TLS handshake complete";

    // TOFU cert-pin check (GHSA-wfx7-w6p8-4jr2 phase 2, enforced).
    const QSslCertificate cert = m_socket.peerCertificate();
    if (cert.isNull()) {
        qCWarning(lcSmartLink) << "WanConnection: peer presented no certificate; skipping TOFU check";
        sendWanValidate();
        return;
    }

    const QString fpHex = QString::fromLatin1(
        cert.digest(QCryptographicHash::Sha256).toHex());
    if (m_expectedFingerprintHex.isEmpty()) {
        // First-use TOFU pin — silent, matches Phase 1 behaviour.
        storeFingerprint(m_host, fpHex);
        m_expectedFingerprintHex = fpHex;
        qCInfo(lcSmartLink) << "WanConnection: pinned cert fingerprint for"
                            << m_host << "(first-use TOFU; sha256=" << fpHex << ")";
        sendWanValidate();
        return;
    }

    if (m_expectedFingerprintHex == fpHex) {
        qCDebug(lcSmartLink) << "WanConnection: cert fingerprint matches stored pin for" << m_host;
        sendWanValidate();
        return;
    }

    // Mismatch — Phase 2 holds the handshake. Do NOT send wan validate.
    // The UI will catch certFingerprintMismatch, show a modal, and call
    // either acceptPresentedCert() or rejectPresentedCert(). Until then
    // the radio sees an open TLS channel but no authenticated session.
    qCWarning(lcSmartLink)
        << "WanConnection: cert fingerprint MISMATCH for" << m_host
        << "— expected" << m_expectedFingerprintHex
        << "got" << fpHex
        << "— possible MITM, radio replaced, or firmware update."
        << "Phase 2: handshake paused pending operator decision. See GHSA-wfx7-w6p8-4jr2.";
    m_presentedFingerprintHex = fpHex;
    m_awaitingCertDecision = true;
    emit certFingerprintMismatch(m_host, m_expectedFingerprintHex, fpHex);
}

void WanConnection::sendWanValidate()
{
    // Send wan validate as a proper command (C<seq>|wan validate handle=...)
    // FlexLib uses SendCommand() for this, not raw write.
    const quint32 seq = m_seqCounter.fetch_add(1);
    const QByteArray data = CommandParser::buildCommand(
        seq, QString("wan validate handle=%1").arg(m_wanHandle));
    qCDebug(lcSmartLink) << "WAN TX: C" << seq << "|wan validate handle=***REDACTED***";
    m_socket.write(data);
    m_validated = true;
    m_awaitingCertDecision = false;
    m_presentedFingerprintHex.clear();
    // The radio will now send V<version>\n then H<handle>\n
    // just like a LAN connection. processLine() handles it.
}

void WanConnection::acceptPresentedCert()
{
    if (!m_awaitingCertDecision) {
        qCWarning(lcSmartLink) << "WanConnection::acceptPresentedCert: no pending decision; ignoring";
        return;
    }
    if (m_presentedFingerprintHex.isEmpty()) {
        qCWarning(lcSmartLink) << "WanConnection::acceptPresentedCert: no presented fingerprint cached; ignoring";
        m_awaitingCertDecision = false;
        return;
    }
    storeFingerprint(m_host, m_presentedFingerprintHex);
    m_expectedFingerprintHex = m_presentedFingerprintHex;
    qCInfo(lcSmartLink) << "WanConnection: operator accepted new cert pin for" << m_host
                        << "sha256=" << m_presentedFingerprintHex;
    sendWanValidate();
}

void WanConnection::rejectPresentedCert()
{
    if (!m_awaitingCertDecision) {
        qCWarning(lcSmartLink) << "WanConnection::rejectPresentedCert: no pending decision; ignoring";
        return;
    }
    qCWarning(lcSmartLink) << "WanConnection: operator rejected new cert pin for" << m_host
                           << "expected=" << m_expectedFingerprintHex
                           << "presented=" << m_presentedFingerprintHex
                           << "— tearing down WAN session.";
    m_awaitingCertDecision = false;
    m_presentedFingerprintHex.clear();
    emit errorOccurred(QStringLiteral("Certificate rejected by user"));
    disconnectFromRadio();
}

void WanConnection::onTlsDisconnected()
{
    qCDebug(lcSmartLink) << "WanConnection: TLS disconnected";
    m_heartbeat.stop();
    m_connected = false;
    emit disconnected();
}

void WanConnection::onSslErrors(const QList<QSslError>& errors)
{
    // Radio uses self-signed cert — ignore SSL errors
    qCDebug(lcSmartLink) << "WanConnection: ignoring SSL errors (radio self-signed cert)";
    for (const auto& err : errors)
        qCDebug(lcSmartLink) << "  " << err.errorString();
    m_socket.ignoreSslErrors();
}

void WanConnection::onSocketError(QAbstractSocket::SocketError /*error*/)
{
    const QString msg = m_socket.errorString();
    qCWarning(lcSmartLink) << "WanConnection: socket error:" << msg;
    emit errorOccurred(msg);
}

void WanConnection::onReadyRead()
{
    m_readBuffer.append(m_socket.readAll());
    if (m_readBuffer.size() > kMaxReadBuffer) {
        qCWarning(lcSmartLink) << "WanConnection: read buffer exceeded"
                               << kMaxReadBuffer << "bytes without newline — disconnecting";
        m_socket.disconnectFromHost();
        // disconnectFromHost() is async on TLS sockets; clear the buffer so a
        // stale onReadyRead() before the disconnect completes can't trip the
        // cap again.
        m_readBuffer.clear();
        return;
    }

    int newlinePos;
    while ((newlinePos = m_readBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_readBuffer.left(newlinePos)).trimmed();
        m_readBuffer.remove(0, newlinePos + 1);
        if (!line.isEmpty())
            processLine(line);
    }
}

void WanConnection::onHeartbeat()
{
    if (m_connected)
        sendCommand("ping");
}

// ─── Line processing (same protocol as RadioConnection) ──────────────────────

void WanConnection::processLine(const QString& line)
{
    // GHSA-wfx7-w6p8-4jr2 phase 2: while the operator is being prompted
    // about a cert mismatch, the TLS channel stays open but we must not
    // act on anything that arrives. A MITM on the path (the exact threat
    // we're defending against) could otherwise fabricate V<version>\n
    // and H<handle>\n bytes before the operator answers, causing this
    // client to fire connected() / start heartbeats on attacker-controlled
    // data. The radio never sees wan validate, but downstream code reacts
    // to connected() — so we extend the suppression symmetrically:
    // nothing gets parsed until the operator's decision lands.
    if (m_awaitingCertDecision) {
        qCDebug(lcSmartLink)
            << "WAN RX (suppressed during cert-pin decision):" << line;
        return;
    }

    // Suppress noisy messages
    const bool isGps = line.contains("|gps ");
    bool isPingReply = false;
    if (m_lastPingSeq && line.startsWith("R")) {
        isPingReply = line.startsWith(QString("R%1|").arg(m_lastPingSeq));
        if (isPingReply) {
            emit pingRttMeasured(static_cast<int>(m_pingStopwatch.elapsed()));
            m_lastPingSeq = 0;
        }
    }
    if (!isGps && !isPingReply)
        qCDebug(lcSmartLink) << "WAN RX:" << line;

    ParsedMessage msg = CommandParser::parseLine(line);
    emit messageReceived(msg);

    switch (msg.type) {
    case MessageType::Version:
        qCDebug(lcSmartLink) << "WanConnection: firmware version:" << msg.object;
        emit versionReceived(msg.object);
        break;

    case MessageType::Handle:
        m_handle = msg.handle;
        qCDebug(lcSmartLink) << "WanConnection: assigned handle 0x" << QString::number(m_handle, 16)
                 << "— WAN validated, starting heartbeat";
        m_connected = true;
        m_heartbeat.start();
        emit connected();
        break;

    case MessageType::Response: {
        auto it = m_pendingCallbacks.find(msg.sequence);
        if (it != m_pendingCallbacks.end()) {
            it.value()(msg.resultCode, msg.object);
            m_pendingCallbacks.erase(it);
        }
        break;
    }

    case MessageType::Status:
        emit statusReceived(msg.object, msg.kvs);
        break;

    default:
        break;
    }
}

} // namespace AetherSDR
