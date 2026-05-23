#include "WanConnection.h"
#include "AppSettings.h"
#include "LogManager.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSslCertificate>
#include <QSslConfiguration>

namespace AetherSDR {

static constexpr int HEARTBEAT_INTERVAL_MS = 10000; // 10s ping (same as FlexLib)

namespace {

// TOFU cert-pin cache.  Stored as a JSON object in AppSettings under the
// key "SmartLinkCertFingerprintCache": { "<host>": "<sha256-hex>", … }.
// See GHSA-wfx7-w6p8-4jr2 — WAN TLS still uses VerifyNone (radio is
// self-signed), but we silently capture each host's cert fingerprint on
// first connect and log a warning if it changes.  Phase 1 is warn-only;
// no user prompt, no enforcement.
constexpr const char* kCertCacheKey = "SmartLinkCertFingerprintCache";

QJsonObject loadCertCache()
{
    const QByteArray json = AppSettings::instance().value(kCertCacheKey).toByteArray();
    if (json.isEmpty()) return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

QString loadStoredFingerprint(const QString& host)
{
    return loadCertCache().value(host).toString();
}

void storeFingerprint(const QString& host, const QString& fingerprintHex)
{
    QJsonObject obj = loadCertCache();
    obj[host] = fingerprintHex;
    AppSettings::instance().setValue(
        kCertCacheKey,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace

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
    return seq;
}

// ─── TLS Socket Callbacks ────────────────────────────────────────────────────

void WanConnection::onTlsConnected()
{
    qCDebug(lcSmartLink) << "WanConnection: TLS handshake complete";

    // TOFU cert-pin check (GHSA-wfx7-w6p8-4jr2 phase 1, warn-only).
    const QSslCertificate cert = m_socket.peerCertificate();
    if (!cert.isNull()) {
        const QString fpHex = QString::fromLatin1(
            cert.digest(QCryptographicHash::Sha256).toHex());
        if (m_expectedFingerprintHex.isEmpty()) {
            storeFingerprint(m_host, fpHex);
            m_expectedFingerprintHex = fpHex;
            qCInfo(lcSmartLink) << "WanConnection: pinned cert fingerprint for"
                                << m_host << "(first-use TOFU; sha256=" << fpHex << ")";
        } else if (m_expectedFingerprintHex != fpHex) {
            qCWarning(lcSmartLink)
                << "WanConnection: cert fingerprint MISMATCH for" << m_host
                << "— expected" << m_expectedFingerprintHex
                << "got" << fpHex
                << "— possible MITM, radio replaced, or firmware update."
                << "Phase 1 is warn-only; connection proceeds. See GHSA-wfx7-w6p8-4jr2.";
        } else {
            qCDebug(lcSmartLink) << "WanConnection: cert fingerprint matches stored pin for" << m_host;
        }
    } else {
        qCWarning(lcSmartLink) << "WanConnection: peer presented no certificate; skipping TOFU check";
    }

    // Send wan validate as a proper command (C<seq>|wan validate handle=...)
    // FlexLib uses SendCommand() for this, not raw write.
    const quint32 seq = m_seqCounter.fetch_add(1);
    const QByteArray data = CommandParser::buildCommand(seq, QString("wan validate handle=%1").arg(m_wanHandle));
    qCDebug(lcSmartLink) << "WAN TX: C" << seq << "|wan validate handle=***REDACTED***";
    m_socket.write(data);
    m_validated = true;

    // The radio will now send V<version>\n then H<handle>\n
    // just like a LAN connection. processLine() handles it.
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
