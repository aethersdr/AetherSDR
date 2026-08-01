#include "RadioConnection.h"
#include "LogManager.h"
#include "core/backends/sim/SimBackend.h"

#include <QEventLoop>
#include <QNetworkProxy>
#include <QTimer>

#ifdef Q_OS_LINUX
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(Q_OS_MACOS)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// MinGW headers may lack TCP_INFO_v0 / SIO_TCP_INFO (Windows 10 1703+)
#ifndef SIO_TCP_INFO
#define SIO_TCP_INFO _WSAIORW(IOC_VENDOR, 39)
typedef struct _TCP_INFO_v0 {
    ULONG    State;  // TCPSTATE enum — not used, just padding
    ULONG    Mss;
    ULONG64  ConnectionTimeMs;
    BOOLEAN  TimestampsEnabled;
    ULONG    RttUs;
    ULONG    MinRttUs;
    ULONG    BytesInFlight;
    ULONG    Cwnd;
    ULONG    SndWnd;
    ULONG    RcvWnd;
    ULONG    RcvBuf;
    ULONG64  BytesOut;
    ULONG64  BytesIn;
    ULONG    BytesReordered;
    ULONG    BytesRetrans;
    ULONG    FastRetrans;
    ULONG    DupAcksIn;
    ULONG    TimeoutEpisodes;
    UCHAR    SynRetrans;
} TCP_INFO_v0;
#endif
#endif

namespace AetherSDR {

static constexpr int HEARTBEAT_INTERVAL_MS = 30000;

// Cap the line-assembly buffer.  A buggy or hostile peer that dribbles bytes
// without ever sending '\n' would otherwise grow m_readBuffer unbounded until
// QByteArray refuses to allocate (process OOM).  CAT lines are tens of bytes;
// 16 MiB is wildly larger than any legitimate radio command or status burst.
// Mirrors the pattern from GHSA-7w4w-wfqm-wh93 (M2, RigctlServer) with a
// looser cap appropriate to the chattier status stream.
static constexpr int kMaxReadBuffer = 16 * 1024 * 1024;

static QString formatHexId(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 0, 16);
}

RadioConnection::RadioConnection(QObject* parent)
    : QObject(parent)
{
}

RadioConnection::~RadioConnection()
{
    disconnectFromRadio();
}

void RadioConnection::init()
{
    m_socket = new QTcpSocket(this);
    // LAN radio uses a proprietary CAT/VITA stream on port 4992; an OS or
    // application HTTP/SOCKS proxy can never tunnel it sensibly.  SmartSDR
    // dodges this because .NET's TcpClient ignores WinHTTP proxy settings,
    // while Qt's QTcpSocket honours QNetworkProxy::applicationProxy() by
    // default.  Force NoProxy so a system proxy doesn't block the connect.
    // (WAN/SmartLink paths intentionally keep default proxy handling.) (#3389)
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_heartbeat = new QTimer(this);

    connect(m_socket, &QTcpSocket::connected,
            this, &RadioConnection::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &RadioConnection::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &RadioConnection::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &RadioConnection::onSocketError);

    m_heartbeat->setInterval(HEARTBEAT_INTERVAL_MS);
    connect(m_heartbeat, &QTimer::timeout, this, &RadioConnection::onHeartbeat);
}

void RadioConnection::connectToRadio(const RadioInfo& info)
{
    if (isDemoTarget(info)) {
        // Demo radio: no socket — play the radio's part locally (RFC #4288).
        startSyntheticDemoConnect();
        return;
    }
    connectToHost(info.address, info.port,
                  info.bindSettings.mode,
                  info.bindSettings.bindAddress,
                  info.sessionBindAddress);
}

bool RadioConnection::isDemoTarget(const RadioInfo& info)
{
    return info.serial == SimBackend::demoSerial();
}

void RadioConnection::startSyntheticDemoConnect()
{
    m_syntheticDemo = true;
    setState(ConnectionState::Connecting);
    // Drive the connect sequence asynchronously (like a real socket connect),
    // so callers that expect connectToRadio() to return before `connected`
    // fires behave identically. Mirrors the real V-line then H-line order:
    // versionReceived, then a nonzero handle + connected().
    QTimer::singleShot(0, this, [this]() {
        if (!m_syntheticDemo) return;   // disconnected before we ran
        emit versionReceived(QStringLiteral("1.4.0.0"));
        m_handle = 0xDE30'0001u;        // stable, nonzero synthetic client handle
        setState(ConnectionState::Connected);
        if (m_heartbeat) m_heartbeat->start();

        // Radio-global status BEFORE connected(): it carries the nickname the
        // status bar reads, and MainWindow sets that label synchronously in its
        // connected() handler — so nickname() must already be populated by then
        // (there is no later nickname-change signal to refresh it). (RFC #4288)
        emitSyntheticStatus(QStringLiteral(
            "SDE300001|radio slices=1 panadapters=1 nickname=Demo "
            "callsign=DEMO model=AetherSDR-Demo"));

        emit connected();

        // Then the pan/waterfall/slice status that builds the panadapter — the
        // same lines a real Flex sends after `sub pan all`. Delayed slightly so
        // RadioModel's onConnected() handshake (which subscribes) has run first.
        // client_handle matches our handle so ownership is Claimed. Format/ids
        // per flex-sim/PROTOCOL.md. Kept short (50ms): the slice-list query is now
        // answered "0" (see writeCommand), sending RadioModel into a 500ms defer
        // waiting for this slice status — so it must land inside that window with
        // margin. 50ms lets connected()/onConnected() run first without racing the
        // defer. (RFC #4288 — the VFO=0 fix.)
        QTimer::singleShot(50, this, [this]() {
            if (!m_syntheticDemo) return;
            emitSyntheticStatus(QStringLiteral(
                // 8 kHz span — this MUST equal SimBackend's spectrum span
                // (kAudioSpanHz), because the demo's spectrum row IS the ±4 kHz
                // audio scene: AE stretches that row across the pan bandwidth, so
                // if the pan is wider than the data (the old 40 kHz vs 8 kHz), the
                // birdie renders at the wrong frequency and lands outside the RX
                // passband. Matching the two makes the on-screen birdie position
                // and the demodulated audio pitch agree. (RFC #4288 — birdie fix.)
                "SDE300001|display pan 0x40000000 client_handle=0xDE300001 "
                "waterfall=0x42000000 center=14.100 bandwidth=0.008 "
                "min_dbm=-140 max_dbm=-20 x_pixels=1024 y_pixels=700 fps=25 "
                "ant_list=ANT1"));
            // line_duration carries the 1..100 waterfall RATE, not milliseconds
            // (core/WaterfallRate.h). #4425 made it load-bearing: the renderer
            // interpolates the waterfall/3D scroll position over one row interval,
            // so a declared rate whose cadence disagrees with the real one makes
            // every animation cut off and restart part-way, which reads as the
            // noise floor rapidly jumping about (and, since the audio and the
            // spectrum come from the same NoiseMixer scene, is audible too).
            // The demo emits a row every 9th audio frame — 9 × 128/24000 s =
            // 48 ms — and asks for the top of the control so nothing re-gates it.
            // (RFC #4288.)
            emitSyntheticStatus(QStringLiteral(
                "SDE300001|display waterfall 0x42000000 client_handle=0xDE300001 "
                "panadapter=0x40000000 line_duration=%1 auto_black=1 "
                "black_level=15 color_gain=50")
                .arg(AetherSDR::SimBackend::kWaterfallRate));
            emitSyntheticStatus(QStringLiteral(
                "SDE300001|slice 0 client_handle=0xDE300001 pan=0x40000000 "
                "RF_frequency=14.100000 mode=USB filter_lo=100 filter_hi=2900 "
                "in_use=1 active=1"));
            // Oscillator/reference status. RadioModel parses "radio oscillator"
            // directly (backend-agnostic, not m_flexBackend-gated), driving the
            // status-bar Ref label; without it the demo shows "Ref: -- [Waiting]".
            // A demo has a perfect notional reference: TCXO, locked, present.
            // (RFC #4288 — the Ref [Waiting] fix.)
            emitSyntheticStatus(QStringLiteral(
                "SDE300001|radio oscillator state=tcxo setting=tcxo locked=1 "
                "tcxo_present=1 ext_present=0 gpsdo_present=0"));
        });
    });
}

void RadioConnection::injectFaultStatus(const QString& line)
{
    // Demo fault harness only — never touch a real socket connection.
    if (m_syntheticDemo)
        emitSyntheticStatus(line);
}

void RadioConnection::emitSyntheticStatus(const QString& line)
{
    // Parse the raw status line into (object, kvs) exactly as the real receive
    // path does, then emit statusReceived — so RadioModel sees an identical
    // payload to a live radio's. (RFC #4288 Stage 3)
    const ParsedMessage msg = CommandParser::parseLine(line);
    if (msg.type == MessageType::Status) {
        emit statusReceived(msg.object, msg.kvs);
    }
}

void RadioConnection::connectToHost(const QHostAddress& address,
                                    quint16 port,
                                    RadioBindMode bindMode,
                                    const QHostAddress& explicitBindAddr,
                                    const QHostAddress& sessionBindAddr)
{
    if (m_state.load() != ConnectionState::Disconnected) return;
    if (!m_socket) { qCWarning(lcConnection) << "RadioConnection: init() not called"; return; }

    m_bindMode = bindMode;
    m_explicitBindAddr = explicitBindAddr;
    m_sessionBindAddr = sessionBindAddr;
    m_radioAddr = address;
    m_localAddr = QHostAddress();
    m_localPort = 0;
    m_socket->abort();

    const QHostAddress preferredBindAddr =
        (bindMode == RadioBindMode::Explicit) ? explicitBindAddr : sessionBindAddr;
    const bool shouldBind =
        !preferredBindAddr.isNull() &&
        preferredBindAddr.protocol() == QAbstractSocket::IPv4Protocol;

    if (shouldBind) {
        if (!m_socket->bind(preferredBindAddr, 0)) {
            const QString msg = QStringLiteral("Failed to bind local TCP source address %1: %2")
                                    .arg(preferredBindAddr.toString(), m_socket->errorString());
            if (bindMode == RadioBindMode::Explicit) {
                qCWarning(lcConnection) << "RadioConnection:" << msg;
                m_socket->abort();
                m_socket->close();
                setState(ConnectionState::Error);
                emit errorOccurred(msg);
                return;
            }

            qCDebug(lcConnection) << "RadioConnection: auto bind to"
                                  << preferredBindAddr.toString()
                                  << "failed, falling back to OS routing:"
                                  << m_socket->errorString();
        } else {
            qCDebug(lcConnection) << "RadioConnection: bound local TCP source to"
                                  << preferredBindAddr.toString()
                                  << ":" << m_socket->localPort()
                                  << "(mode"
                                  << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                                  << ")";
        }
    } else {
        qCDebug(lcConnection) << "RadioConnection: no explicit local TCP bind, letting OS route"
                              << "(mode"
                              << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                              << ")";
    }

    qCDebug(lcConnection) << "RadioConnection: connecting to" << address.toString() << ":" << port;
    setState(ConnectionState::Connecting);
    m_socket->connectToHost(address, port);
}

void RadioConnection::disconnectFromRadio()
{
    if (m_heartbeat) m_heartbeat->stop();
    if (m_syntheticDemo) {
        // Demo teardown: no socket to close — just drop state and notify.
        m_syntheticDemo = false;
        m_handle = 0;
        setState(ConnectionState::Disconnected);
        emit disconnected();
        return;
    }
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        writeDisconnectMarker();
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(2000);
    }
    m_handle = 0;
}

void RadioConnection::gracefulDisconnect(quint32 handle,
                                         const QString& rxStreamId,
                                         quint32 streamRemoveSeq)
{
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState) {
        disconnectFromRadio();
        return;
    }

    if (m_heartbeat) m_heartbeat->stop();

    QElapsedTimer totalTimer;
    totalTimer.start();
    bool streamRemoveAcked = rxStreamId.isEmpty();
    qint64 streamRemoveMs = -1;

    // The radio needs to process teardown commands before TCP close begins.
    // Fire-and-forget C0 writes can leave the Flex session table stale (#2218).
    if (!rxStreamId.isEmpty() && streamRemoveSeq != 0) {
        QElapsedTimer stepTimer;
        stepTimer.start();
        streamRemoveAcked =
            sendCommandAndWait(streamRemoveSeq,
                               QString("stream remove 0x%1").arg(rxStreamId),
                               2000);
        streamRemoveMs = stepTimer.elapsed();
    }

    QElapsedTimer closeTimer;
    closeTimer.start();
    disconnectFromRadio();
    const qint64 closeMs = closeTimer.elapsed();

    qCInfo(lcConnection).nospace()
        << "RadioConnection: disconnect summary "
        << "handle=" << formatHexId(handle)
        << " streamId=" << (rxStreamId.isEmpty() ? QStringLiteral("<none>") : rxStreamId)
        << " streamSeq=" << streamRemoveSeq
        << " streamAck=" << (streamRemoveAcked ? "yes" : "no")
        << " streamMs=" << streamRemoveMs
        << " closeMs=" << closeMs
        << " totalMs=" << totalTimer.elapsed();
}

void RadioConnection::writeCommand(quint32 seq, const QString& command)
{
    if (m_syntheticDemo) {
        if (!isConnected()) return;
        // Keepalive: RadioModel pings the radio and force-disconnects after 5
        // unanswered pings. A ping is answered via pingRttMeasured (which resets
        // the miss counter), NOT a generic command response — so answer it that
        // way with a plausible tiny RTT. (RFC #4288)
        if (command.startsWith(QStringLiteral("ping"))) {
            emit pingRttMeasured(1);
            return;
        }
        // Slice tune: a real radio retunes and echoes the new RF_frequency in
        // slice status. Model that so demo tuning works — parse the freq, echo it
        // back (updates the VFO/display), and notify the audio side so the birdie
        // demodulates against the new VFO. Format: "slice tune <id> <mhz> [kv…]".
        if (command.startsWith(QStringLiteral("slice tune"))) {
            const QStringList parts = command.split(QLatin1Char(' '),
                                                    Qt::SkipEmptyParts);
            if (parts.size() >= 4) {
                bool ok = false;
                const double mhz = parts.at(3).toDouble(&ok);
                if (ok) {
                    // Async echo (see the mode/filter note below) — avoids any
                    // synchronous re-entrancy into SliceModel from writeCommand.
                    const QString line =
                        QStringLiteral("SDE300001|slice 0 client_handle=0xDE300001 "
                                       "RF_frequency=%1").arg(mhz, 0, 'f', 6);
                    QTimer::singleShot(0, this, [this, line] {
                        if (m_syntheticDemo) emitSyntheticStatus(line);
                    });
                    emit demoVfoChanged(mhz);
                }
            }
            emit commandResponse(seq, 0, QString());
            return;
        }
        // Slice set mode/filter: a real radio applies these and echoes them in
        // slice status; without the echo AE's SliceModel (radio-authoritative,
        // Principle II) never updates, so LSB "won't turn on" and filter drags do
        // nothing. Model the ones we support — echo them back, and tell the audio
        // side so the birdie demod follows the sideband. "slice set <id> k=v …".
        if (command.startsWith(QStringLiteral("slice set"))) {
            const QStringList parts = command.split(QLatin1Char(' '),
                                                    Qt::SkipEmptyParts);
            QStringList echo;
            for (const QString& tok : parts) {
                const int eq = tok.indexOf(QLatin1Char('='));
                if (eq <= 0) continue;
                const QString k = tok.left(eq);
                const QString v = tok.mid(eq + 1);
                if (k == QLatin1String("mode")) {
                    echo << QStringLiteral("mode=%1").arg(v);
                    emit demoModeChanged(v);
                } else if (k == QLatin1String("filter_lo")
                           || k == QLatin1String("filter_hi")) {
                    echo << QStringLiteral("%1=%2").arg(k, v);   // echo the filter
                } else if (k == QLatin1String("anf")) {
                    echo << QStringLiteral("anf=%1").arg(v);
                    emit demoAnfChanged(v == QLatin1String("1"));
                } else if (k == QLatin1String("nb")) {
                    echo << QStringLiteral("nb=%1").arg(v);
                    emit demoNbChanged(v == QLatin1String("1"));
                }
            }
            if (!echo.isEmpty()) {
                // Defer the echo to the next event-loop cycle. Emitting
                // statusReceived synchronously here re-enters SliceModel, which
                // can re-emit the mode intent → back into writeCommand → infinite
                // recursion → crash. A real radio's echo also arrives async (over
                // the socket), so this matches reality and breaks the loop.
                const QString line =
                    QStringLiteral("SDE300001|slice 0 client_handle=0xDE300001 %1")
                        .arg(echo.join(QLatin1Char(' ')));
                QTimer::singleShot(0, this, [this, line] {
                    if (m_syntheticDemo) emitSyntheticStatus(line);
                });
            }
            emit commandResponse(seq, 0, QString());
            return;
        }
        // Slice list query: a real radio answers with the space-separated ids of
        // its live slices. RadioModel's connect handshake queries this to decide
        // whether to adopt existing slices or create a default one. We MUST report
        // slice 0 here — otherwise RadioModel sees "(empty)", creates its own
        // default slice at freq 0 (the VFO=0 bug), and never adopts the synthetic
        // slice status pushed below. Reporting "0" sends RadioModel down the
        // "radio has slices, defer to let status arrive" path (#3212), which the
        // pushed slice status (RF_frequency=14.100000) then satisfies. (RFC #4288)
        if (command == QLatin1String("slice list")) {
            emit commandResponse(seq, 0, QStringLiteral("0"));
            return;
        }
        // Panadapter create: a real radio replies with the new pan's id in the
        // body (parsePanafallCreatePanId reads pan=/id=/bare-hex). If we returned
        // an empty body, RadioModel logs "returned empty pan_id" and the pan is
        // never claimed. We only reach here if RadioModel decided to create one;
        // hand back our synthetic pan id so it claims 0x40000000 cleanly. (#4288)
        if (command.startsWith(QLatin1String("display panafall create"))
            || command.startsWith(QLatin1String("panadapter create"))) {
            emit commandResponse(seq, 0, QStringLiteral("pan=0x40000000"));
            return;
        }
        // Stream create: a real radio replies with the new stream's hex id in the
        // body (parseCreateResponseStreamId). The demo has no real streams — its RX
        // audio and spectrum flow over the IRadioBackend seam, not a stream — but
        // RadioModel still issues "stream create type=remote_audio_rx …" on connect
        // and logs "returned unparseable body" on an empty reply. Hand back a
        // distinct synthetic stream id per type so the create parses cleanly and the
        // log stays quiet; nothing downstream depends on the id for the demo. (#4288)
        if (command.startsWith(QLatin1String("stream create"))) {
            quint32 streamId = 0x0B00'0001u;                       // default synthetic id
            if (command.contains(QLatin1String("remote_audio_rx"))) streamId = 0x0B00'0001u;
            else if (command.contains(QLatin1String("remote_audio_tx"))) streamId = 0x0B00'0002u;
            else if (command.contains(QLatin1String("netcw")))          streamId = 0x0B00'0003u;
            else if (command.contains(QLatin1String("dax_rx")))         streamId = 0x0B00'0004u;
            emit commandResponse(seq, 0,
                                 QStringLiteral("0x%1").arg(streamId, 8, 16, QLatin1Char('0')));
            return;
        }
        // Every other command: acknowledge OK (code 0) so the GUI-client
        // handshake (sub pan all, client gui, …) completes. The demo doesn't
        // model command effects; panadapter/slice state is pushed via synthetic
        // status lines, not command replies.
        emit commandResponse(seq, 0, QString());
        return;
    }
    if (!isConnected() || !m_socket) return;

    const QByteArray data = CommandParser::buildCommand(seq, command);
    if (command.startsWith("ping")) {
        m_lastPingSeq = seq;
        m_pingStopwatch.restart();  // fallback timer in case TCP_INFO unavailable
    } else {
        qCDebug(lcConnection) << "TX:" << data.trimmed();
    }
    m_socket->write(data);
    m_socket->flush();   // force immediate kernel send for keepalive reliability
}

void RadioConnection::onSocketConnected()
{
    qCDebug(lcConnection) << "RadioConnection: TCP connected";
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_radioAddr = m_socket->peerAddress();
    m_localAddr = m_socket->localAddress();
    m_localPort = m_socket->localPort();
    qCDebug(lcConnection) << "RadioConnection: local TCP endpoint"
                          << m_localAddr.toString() << ":" << m_localPort;
    qDebug() << "RTT method:" << (kernelRttMs() >= 0 ? "kernel TCP_INFO" : "QElapsedTimer fallback");
}

void RadioConnection::onSocketDisconnected()
{
    qCDebug(lcConnection) << "RadioConnection: TCP disconnected";
    if (m_heartbeat) m_heartbeat->stop();
    m_localAddr = QHostAddress();
    m_localPort = 0;
    setState(ConnectionState::Disconnected);
    emit disconnected();
}

void RadioConnection::onSocketError(QAbstractSocket::SocketError)
{
    if (!m_socket) return;
    const QString msg = m_socket->errorString();
    qCWarning(lcConnection) << "RadioConnection: socket error:" << msg;
    setState(ConnectionState::Error);
    emit errorOccurred(msg);
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        setState(ConnectionState::Disconnected);
}

void RadioConnection::onReadyRead()
{
    if (!m_socket) return;
    m_readBuffer.append(m_socket->readAll());
    if (m_readBuffer.size() > kMaxReadBuffer) {
        qCWarning(lcConnection) << "RadioConnection: read buffer exceeded"
                                << kMaxReadBuffer << "bytes without newline — disconnecting";
        m_socket->disconnectFromHost();
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

void RadioConnection::onHeartbeat()
{
}

void RadioConnection::processLine(const QString& line)
{
    // GPS coordinates are never useful in a support log. Drop the raw status
    // at the source; AsyncLogWriter also scrubs coordinate-shaped fields as a
    // defense against future logging paths.
    const bool isGps = line.contains("|gps ");
    bool isPingReply = false;
    if (m_lastPingSeq && line.startsWith("R")) {
        isPingReply = line.startsWith(QString("R%1|").arg(m_lastPingSeq));
        if (isPingReply) {
            int rtt = kernelRttMs();
            if (rtt < 0)
                rtt = static_cast<int>(m_pingStopwatch.elapsed());  // fallback
            emit pingRttMeasured(rtt);
            m_lastPingSeq = 0;
        }
    }
    if (!isGps && !isPingReply) {
        qCDebug(lcConnection) << "RX:" << line;
    }

    ParsedMessage msg = CommandParser::parseLine(line);
    emit messageReceived(msg);

    switch (msg.type) {
    case MessageType::Version:
        emit versionReceived(msg.object);
        break;
    case MessageType::Handle:
        m_handle = msg.handle;
        qCDebug(lcConnection) << "RadioConnection: assigned handle" << QString::number(m_handle, 16);
        setState(ConnectionState::Connected);
        if (m_heartbeat) m_heartbeat->start();
        emit connected();
        break;
    case MessageType::Response:
        emit commandResponse(msg.sequence, msg.resultCode, msg.object);
        break;
    case MessageType::Status:
        emit statusReceived(msg.object, msg.kvs);
        break;
    default:
        break;
    }
}

// ─── Kernel-level TCP RTT ────────────────────────────────────────────────────
// Read the smoothed RTT maintained by the kernel's TCP congestion control.
// This is measured from TCP ACK round-trips at the kernel level, completely
// independent of Qt event loop buffering or application-layer timing.

int RadioConnection::kernelRttMs() const
{
    if (!m_socket) return -1;
    const auto fd = m_socket->socketDescriptor();
    if (fd == -1) return -1;

#ifdef Q_OS_LINUX
    struct tcp_info info{};
    socklen_t len = sizeof(info);
    if (getsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_INFO, &info, &len) == 0)
        return static_cast<int>(info.tcpi_rtt / 1000);  // µs → ms
#elif defined(Q_OS_MACOS)
    struct tcp_connection_info info{};
    socklen_t len = sizeof(info);
    if (getsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) == 0)
        return static_cast<int>(info.tcpi_srtt);  // already ms on macOS
#elif defined(Q_OS_WIN)
    TCP_INFO_v0 info{};
    DWORD infoLen = sizeof(info);
    DWORD version = 0;
    if (WSAIoctl(static_cast<SOCKET>(fd), SIO_TCP_INFO, &version, sizeof(version),
                 &info, sizeof(info), &infoLen, nullptr, nullptr) == 0)
        return static_cast<int>(info.RttUs / 1000);  // µs → ms
#endif

    return -1;  // unsupported platform or call failed
}

void RadioConnection::setState(ConnectionState s)
{
    m_state.store(s);
    emit stateChanged(s);
}

bool RadioConnection::sendCommandAndWait(quint32 seq, const QString& command, int timeoutMs)
{
    if (seq == 0 || !m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return false;

    bool received = false;
    int responseCode = 0;
    QString responseBody;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    const QMetaObject::Connection responseConn =
        connect(this, &RadioConnection::commandResponse, &loop,
                [&](quint32 responseSeq, int code, const QString& body) {
            if (responseSeq != seq)
                return;
            received = true;
            responseCode = code;
            responseBody = body;
            loop.quit();
        });

    const QMetaObject::Connection disconnectedConn =
        connect(m_socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);

    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    writeCommand(seq, command);
    timer.start(timeoutMs);
    loop.exec();

    disconnect(responseConn);
    disconnect(disconnectedConn);

    if (!received) {
        qCWarning(lcConnection) << "RadioConnection: timed out waiting for teardown response"
                                << "seq" << seq << "command" << command;
        return false;
    }

    qCDebug(lcConnection) << "RadioConnection: teardown response"
                          << "seq" << seq
                          << "code" << Qt::hex << responseCode
                          << "body" << responseBody;
    return true;
}

void RadioConnection::writeDisconnectMarker()
{
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return;

    m_socket->write(QByteArray(1, '\x04'));
    m_socket->flush();
    m_socket->waitForBytesWritten(250);
}

} // namespace AetherSDR
