#include "WanConnection.h"
#include <QPointer>
#include <QSignalBlocker>
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <utility>

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

// TOFU cert-pin cache: JSON in AppSettings "SmartLinkCertFingerprintCache"
// (GHSA-wfx7-w6p8-4jr2). The reader accepts both shapes; the writer emits phase 2.
//   phase 1: { "<host>": "<sha256-hex>" }
//   phase 2 (#2951): { "<host>": { "fp": "<sha256-hex>", "pinnedAt": "<iso8601>" } }
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
    m_commands.setWriter([this](const FlexCommandSession::Write& write) {
        const auto result = write.run([&] {
            if (!m_connected || write.epoch != m_sessionGeneration) {
                return FlexCommandSession::WriteResult::NotWritten;
            }
            const QByteArray data = CommandParser::buildCommand(write.sequence, write.command);
            if (write.command.startsWith(QStringLiteral("ping"))) {
                m_lastPingSeq = write.sequence;
                m_pingStopwatch.restart();
            } else {
                qCDebug(lcSmartLink) << "WAN TX:" << data.trimmed();
            }
            const qint64 written = m_socket.write(data);
            m_socket.flush();
            return written == data.size() ? FlexCommandSession::WriteResult::Written
                : written <= 0 ? FlexCommandSession::WriteResult::NotWritten
                               : FlexCommandSession::WriteResult::Unknown;
        });
        m_commands.writeFinished(write.epoch, write.sequence, result);
        return true;
    });
    connect(&m_socket, &QSslSocket::encrypted,    this, &WanConnection::onTlsConnected);
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
    m_commands.invalidate();
    m_heartbeat.stop();
    m_socket.blockSignals(true);
    m_socket.abort();
}

// ─── Connection ──────────────────────────────────────────────────────────────

void WanConnection::resetSessionState()
{
    ++m_sessionGeneration;
    // This object outlives every TLS session (owned by FlexWanSession), so
    // reset per session: otherwise a partial line from the dead link prefixes the
    // next session's first line and stale handle/sequence/callbacks answer into the
    // live one. Mirrors RadioConnection::resetSessionState() and FlexLib's
    // TcpCommandCommunication.Disconnect() (FlexLib_API_v4.1.5.39794
    // TcpCommandCommunication.cs:249). Retire replies only after transport state
    // is invalid, so a completion cannot write through the old session.
    m_readBuffer.clear();
    m_handle     = 0;
    m_connected  = false;
    m_validated  = false;
    m_commands.begin(m_sessionGeneration);
}

void WanConnection::connectToRadio(const QString& host, quint16 tlsPort,
                                    const QString& wanHandle)
{
    if (m_connected || m_socket.state() != QAbstractSocket::UnconnectedState) {
        qCWarning(lcSmartLink) << "WanConnection: already connected or still closing";
        return;
    }

    const QPointer<WanConnection> alive(this);
    const quint64 generation = m_sessionGeneration + 1;
    resetSessionState();
    if (!alive || m_sessionGeneration != generation) { return; }
    m_wanHandle = wanHandle;
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
    const QPointer<WanConnection> alive(this);
    const quint64 generation = m_sessionGeneration;
    m_commands.invalidate();
    m_heartbeat.stop();
    m_awaitingCertDecision = false;
    m_presentedFingerprintHex.clear();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        // Send disconnect marker (FlexLib sends 0x04)
        m_socket.write("\x04");
        m_socket.disconnectFromHost();
    }
    // A synchronous disconnected signal can retire this session and its
    // callback can start a replacement. Never reset that replacement here.
    if (alive && m_sessionGeneration == generation) { resetSessionState(); }
}

void WanConnection::abortConnection()
{
    const QSignalBlocker blocker(&m_socket);
    m_heartbeat.stop();
    m_socket.abort();
    m_awaitingCertDecision = false;
    m_presentedFingerprintHex.clear();
    resetSessionState();
    m_commands.retire(QStringLiteral("Connection closed."));
}

// ─── Command dispatch ────────────────────────────────────────────────────────

namespace {
FlexCommandSession::Completion wanReply(WanConnection::ResponseCallback callback)
{
    if (!callback) { return {}; }
    return [callback = std::move(callback)](const FlexCommandSession::Reply& reply) {
        callback(reply.kind == FlexCommandSession::ReplyKind::NotDispatched ? 0x50000063 : reply.code,
                 reply.body);
    };
}
}

quint32 WanConnection::sendSessionCommand(const QString& command, quint64 generation,
    const SessionCommandGuard& guard, ResponseCallback callback)
{
    if (!sessionWriteAdmitted(generation, guard)) { return 0; }
    FlexCommandSession::Options options;
    options.guard = guard;
    options.guarded = true;
    return m_commands.submit(command, options, wanReply(std::move(callback)));
}

quint32 WanConnection::sendAdministrationCommand(const QString& command, quint64 generation,
    const SessionCommandGuard& guard, ResponseCallback callback)
{
    return sendSessionCommand(command, generation, guard, std::move(callback));
}

quint32 WanConnection::sendCommand(const QString& command, ResponseCallback callback)
{
    if (!m_connected) { return 0; }
    return m_commands.submit(command, {}, wanReply(std::move(callback)));
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
    const quint32 seq = m_commands.allocateSequence();
    if (!seq) {
        qCWarning(lcSmartLink) << "WanConnection: command sequence exhausted before validation";
        disconnectFromRadio();
        return;
    }
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
    // Radio-initiated drop. This path previously cleared only m_connected, so a
    // truncated line, the dead handle and any in-flight callbacks all survived
    // into the next SmartLink session. (#5653 review)
    const QPointer<WanConnection> alive(this);
    const quint64 generation = m_sessionGeneration + 1;
    resetSessionState();
    if (alive && m_sessionGeneration == generation) { emit disconnected(); }
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
    // GPS coordinates are never useful in a support log. Compute this before
    // the certificate-decision early return so that path cannot bypass the
    // normal GPS payload suppression below.
    const bool isGps = line.contains("|gps ");

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
        if (isGps) {
            qCDebug(lcSmartLink)
                << "WAN RX (suppressed during cert-pin decision; GPS payload omitted)";
        } else {
            qCDebug(lcSmartLink)
                << "WAN RX (suppressed during cert-pin decision):" << line;
        }
        return;
    }

    // Suppress noisy messages
    bool isPingReply = false;
    if (m_lastPingSeq && line.startsWith("R")) {
        isPingReply = line.startsWith(QString("R%1|").arg(m_lastPingSeq));
        if (isPingReply) {
            emit pingRttMeasured(static_cast<int>(m_pingStopwatch.elapsed()));
            m_lastPingSeq = 0;
        }
    }
    if (!isGps && !isPingReply) {
        qCDebug(lcSmartLink) << "WAN RX:" << line;
    }

    ParsedMessage msg = CommandParser::parseLine(line);
    emit lineReceived(msg, m_lineEpoch);

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
        // Answer after the ingress has taken every line read ahead of this
        // reply: status goes over one queued hop, so a reply answered here
        // would overtake it. LAN replies take the same hop (commandResponse).
        QMetaObject::invokeMethod(this, [this, generation = m_sessionGeneration,
                                         sequence = msg.sequence, code = msg.resultCode,
                                         body = msg.object] {
            if (generation != m_sessionGeneration) { return; }
            m_commands.answer(generation, sequence, code, body);
        }, Qt::QueuedConnection);
        break;
    }

    default:
        break;
    }
}

} // namespace AetherSDR
