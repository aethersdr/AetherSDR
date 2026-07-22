#include "PanadapterStream.h"
#include "AppSettings.h"
#include "AudioEngine.h"
#include "LogManager.h"
#include "NetworkSettings.h"
#include "OpusCodec.h"
#include "PerfTelemetry.h"
#include "RadioConnection.h"
#include "VitaTileFrequency.h"

#include <QNetworkDatagram>
#include <QHostAddress>
#include <QStringList>
#include <QtEndian>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr QAbstractSocket::BindMode kLanVitaBindMode = QAbstractSocket::DontShareAddress;
constexpr float kMinSpectrumDbm = -180.0f;
constexpr int kAudioSampleRate = AudioEngine::DEFAULT_SAMPLE_RATE;
constexpr int kOpusFramesPerPacket = 240;

QHostAddress chooseLanBindAddress(RadioConnection* conn,
                                  QString* chosenReason,
                                  QHostAddress* chosenAddress,
                                  RadioBindMode* bindMode)
{
    if (chosenReason)
        chosenReason->clear();
    if (chosenAddress)
        *chosenAddress = QHostAddress();
    if (bindMode)
        *bindMode = conn ? conn->bindMode() : RadioBindMode::Auto;

    const QHostAddress explicitAddr = conn ? conn->explicitLocalBindAddress() : QHostAddress();
    const QHostAddress sessionAddr = conn ? conn->sessionLocalBindAddress() : QHostAddress();
    const QHostAddress tcpAddr = conn ? conn->localAddress() : QHostAddress();

    QHostAddress selectedAddress;
    QString selectedReason;
    if (!explicitAddr.isNull() && explicitAddr.protocol() == QAbstractSocket::IPv4Protocol) {
        selectedAddress = explicitAddr;
        selectedReason = QStringLiteral("explicit");
    } else if (!sessionAddr.isNull() && sessionAddr.protocol() == QAbstractSocket::IPv4Protocol) {
        selectedAddress = sessionAddr;
        selectedReason = QStringLiteral("probe-session");
    } else if (!tcpAddr.isNull() && tcpAddr.protocol() == QAbstractSocket::IPv4Protocol) {
        selectedAddress = tcpAddr;
        selectedReason = QStringLiteral("tcp-local");
    }

    if (chosenReason)
        *chosenReason = selectedReason;
    if (chosenAddress)
        *chosenAddress = selectedAddress;

    return selectedAddress.isNull() ? QHostAddress(QHostAddress::AnyIPv4) : selectedAddress;
}

} // namespace

// ─── VITA-49 header layout (28 bytes, big-endian) ─────────────────────────────
// Word 0 (bytes  0- 3): Packet header (type=3 ExtData, flags, count, size)
// Word 1 (bytes  4- 7): Stream ID
// Word 2 (bytes  8-11): Class ID OUI
// Word 3 (bytes 12-15): Class ID — InformationClassCode[15:0] | PacketClassCode[15:0]
// Word 4 (bytes 16-19): Integer timestamp
// Word 5 (bytes 20-23): Fractional timestamp (upper)
// Word 6 (bytes 24-27): Fractional timestamp (lower)
// Byte 28+            : Payload
//
// All FLEX radio streams use ExtDataWithStream (type 3), including audio.
// Audio is identified by PacketClassCode (lower 16 bits of word 3):
//   0x03E3 — SL_VITA_IF_NARROW_CLASS        — float32 stereo, big-endian
//   0x0123 — SL_VITA_IF_NARROW_REDUCED_BW   — int16 mono, big-endian
//   0x8005 — SL_VITA_OPUS_CLASS             — Opus compressed (not yet handled)
//
// Panadapter FFT: PCC = 0x8003 (SL_VITA_FFT_CLASS)
// Waterfall tile: PCC = 0x8004 (SL_VITA_WATERFALL_CLASS)

PanadapterStream::PanadapterStream(QObject* parent)
    : QObject(parent)
{
    // Socket and timer connections are deferred to init() which runs
    // on the network thread after moveToThread(). (#561)
}

void PanadapterStream::init()
{
    if (!m_socket) {
        m_socket = new QUdpSocket(this);
    }
    if (!m_wanRegisterTimer) {
        m_wanRegisterTimer = new QTimer(this);
    }
    if (!m_wanPingTimer) {
        m_wanPingTimer = new QTimer(this);
    }
    if (!m_routedPrimeTimer) {
        m_routedPrimeTimer = new QTimer(this);
    }

    // Pick up the persisted PLC toggle. Default true — strictly better in
    // the failure mode and a no-op when no packets are lost. (#2731)
    m_plcEnabled.store(
        AppSettings::instance().value("AudioPacketLossConcealment", "True")
            .toString() == "True");

    // Seed the receive-buffer request from the persisted operator setting; each
    // bind path then applies it via applyReceiveBufferSize(). (#3810)
    m_desiredRcvBufBytes = NetworkSettings::vitaReceiveBufferBytes();

    connect(m_socket, &QUdpSocket::readyRead,
            this, &PanadapterStream::onDatagramReady);

    // WAN UDP register: send "client udp_register handle=0x<handle>" every 50ms
    // until first VITA-49 packet arrives (confirms radio knows our address).
    connect(m_wanRegisterTimer, &QTimer::timeout, this, [this] {
        if (m_radioAddress.isNull() || m_wanClientHandle == 0) return;
        const QString hex = QString::number(m_wanClientHandle, 16).toUpper();
        const QByteArray cmd = QStringLiteral("client udp_register handle=0x%1").arg(hex).toUtf8();
        const qint64 sent = m_socket->writeDatagram(cmd, m_radioAddress, m_radioPort);
        if (sent > 0)
            m_totalTxBytes.fetch_add(sent);
    });

    // WAN ping keepalive: send "client ping handle=0x<handle>" every 5s
    // to maintain NAT pinhole after registration is confirmed.
    connect(m_wanPingTimer, &QTimer::timeout, this, [this] {
        if (m_radioAddress.isNull() || m_wanClientHandle == 0) return;
        const QString hex = QString::number(m_wanClientHandle, 16).toUpper();
        const QByteArray cmd = QStringLiteral("client ping handle=0x%1").arg(hex).toUtf8();
        m_socket->writeDatagram(cmd, m_radioAddress, m_radioPort);
        m_totalTxBytes.fetch_add(cmd.size());
        qCDebug(lcVita49) << "PanadapterStream: WAN ping keepalive"
                 << "(totalRx:" << m_totalRxBytes.load() << "bytes)";
    });

    connect(m_routedPrimeTimer, &QTimer::timeout, this, [this] {
        if (m_isWanMode || m_hasReceivedPacket || m_radioAddress.isNull())
            return;
        if (!m_routedPrimeElapsed.isValid() || m_routedPrimeElapsed.elapsed() >= 2000) {
            m_routedPrimeTimer->stop();
            return;
        }
        const QByteArray reg(1, '\x00');
        const qint64 sent = m_socket->writeDatagram(reg, m_radioAddress, 4992);
        if (sent > 0)
            m_totalTxBytes.fetch_add(sent);
    });
}

bool PanadapterStream::isRunning() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

void PanadapterStream::applyReceiveBufferSize()
{
    if (!m_socket)
        return;
    // The VITA-49 streams (panadapter FFT + waterfall tiles + audio + meters)
    // burst well above the OS default receive buffer (~208 KB on Linux). A burst
    // — or a brief worker-thread drain stall while the host is loaded — then
    // overflows the kernel buffer, and the kernel silently drops the excess
    // datagrams. Those drops surface as VITA-49 sequence gaps, which the network
    // monitor reads as packet loss and the adaptive throttle reacts to by capping
    // the radio's pan FPS — a visible "network stats dropped" event. Request a
    // generous SO_RCVBUF so normal bursts are absorbed. The kernel caps the grant
    // at net.core.rmem_max; we log the granted size so an undersized rmem_max is
    // visible in the logs rather than silently limiting us. The requested size
    // is operator-adjustable (Radio Setup → Advanced); default 4 MiB. (#3810)
    const int requested = m_desiredRcvBufBytes;
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, requested);
    const int granted =
        m_socket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
    m_grantedRcvBufBytes.store(granted);
    qCInfo(lcVita49).noquote()
        << "PanadapterStream: VITA UDP receive buffer"
        << QStringLiteral("requested=%1").arg(requested)
        << QStringLiteral("granted=%1").arg(granted)
        << (granted < requested
                ? QStringLiteral("(capped by net.core.rmem_max — raise it for more headroom)")
                : QString());
    emit receiveBufferApplied(requested, granted);
}

void PanadapterStream::setReceiveBufferSizeBytes(int bytes)
{
    m_desiredRcvBufBytes = bytes;
    // Re-apply live if we already have a bound socket; otherwise the next bind
    // picks it up via applyReceiveBufferSize().
    if (m_socket && m_socket->state() == QAbstractSocket::BoundState)
        applyReceiveBufferSize();
}

bool PanadapterStream::start(RadioConnection* conn)
{
    if (isRunning()) stop();  // clean up previous session before rebinding (#561)

    resetAudioStreamStats();

    m_isWanMode = false;
    m_hasReceivedPacket = false;

    QHostAddress chosenAddress;
    QString chosenReason;
    RadioBindMode bindMode = RadioBindMode::Auto;
    const QHostAddress bindAddr = chooseLanBindAddress(conn, &chosenReason, &chosenAddress, &bindMode);
    const QString bindReason = chosenReason.isEmpty() ? QStringLiteral("auto") : chosenReason;

    const bool bound = m_socket->bind(bindAddr, 0, kLanVitaBindMode);
    if (bound)
        qCInfo(lcVita49).noquote()
            << "PanadapterStream: LAN VITA UDP bind"
            << QStringLiteral("addr=%1").arg(bindAddr.toString())
            << QStringLiteral("port=%1").arg(m_socket->localPort())
            << QStringLiteral("flags=DontShareAddress")
            << QStringLiteral("reason=%1").arg(bindReason);
    if (!bound) {
        qCWarning(lcVita49) << "PanadapterStream: failed to bind UDP socket:"
                            << m_socket->errorString()
                            << "mode=" << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                            << "chosen=" << (chosenAddress.isNull() ? QStringLiteral("<none>")
                                                                    : chosenAddress.toString());
        return false;
    }

    applyReceiveBufferSize();   // #3810 — absorb VITA-49 bursts so they don't drop
    m_localAddress = m_socket->localAddress();
    m_localPort = m_socket->localPort();
    qCDebug(lcVita49) << "PanadapterStream: local UDP endpoint"
                      << m_localAddress.toString() << ":" << m_localPort;

    // Send a one-byte UDP registration datagram to the radio's VITA-49 port.
    // The radio learns our IP:port from the source address of this datagram.
    // This is required on firmware v1.4.0.0 where the TCP "client udpport"
    // command may return 0x50001000 ("command not supported").
    const QHostAddress radioAddr = conn->radioAddress();
    if (!radioAddr.isNull()) {
        const QByteArray reg(1, '\x00');
        const qint64 sent = m_socket->writeDatagram(reg, radioAddr, 4992);
        if (sent == 1) {
            m_totalTxBytes.fetch_add(sent);
            qCDebug(lcVita49) << "PanadapterStream: sent UDP registration to"
                              << radioAddr.toString() << ":4992";
            m_routedPrimeElapsed.restart();
            m_routedPrimeTimer->start(250);
        } else {
            qCWarning(lcVita49) << "PanadapterStream: UDP registration send failed:"
                                << m_socket->errorString();
        }
    } else {
        qCWarning(lcVita49) << "PanadapterStream: radio address unknown — skipping UDP registration";
    }

    // Store radio address for sendToRadio (DAX TX path)
    m_radioAddress = radioAddr;
    m_radioPort = 4991;

    m_conn = conn;
    return true;
}

bool PanadapterStream::rebindToEphemeralPort(RadioConnection* conn)
{
    if (!m_socket) {
        qCWarning(lcVita49) << "PanadapterStream: cannot rebind UDP socket before init";
        return false;
    }

    if (m_routedPrimeTimer)
        m_routedPrimeTimer->stop();

    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->close();

    resetAudioStreamStats();
    m_isWanMode = false;
    m_hasReceivedPacket = false;

    QHostAddress chosenAddress;
    QString chosenReason;
    RadioBindMode bindMode = RadioBindMode::Auto;
    const QHostAddress bindAddr = chooseLanBindAddress(conn, &chosenReason, &chosenAddress, &bindMode);
    const QString bindReason = chosenReason.isEmpty() ? QStringLiteral("auto") : chosenReason;

    const bool bound = m_socket->bind(bindAddr, 0, kLanVitaBindMode);
    if (!bound) {
        qCWarning(lcVita49) << "PanadapterStream: failed to rebind UDP socket after port collision:"
                            << m_socket->errorString()
                            << "mode=" << (bindMode == RadioBindMode::Explicit ? "Explicit" : "Auto")
                            << "chosen=" << (chosenAddress.isNull() ? QStringLiteral("<none>")
                                                                    : chosenAddress.toString());
        return false;
    }

    applyReceiveBufferSize();   // #3810 — absorb VITA-49 bursts so they don't drop
    m_localAddress = m_socket->localAddress();
    m_localPort = m_socket->localPort();
    m_radioAddress = conn ? conn->radioAddress() : QHostAddress();
    m_radioPort = 4991;
    m_conn = conn;

    qCWarning(lcVita49).noquote()
        << "PanadapterStream: LAN VITA UDP bind"
        << QStringLiteral("addr=%1").arg(bindAddr.toString())
        << QStringLiteral("port=%1").arg(m_localPort)
        << QStringLiteral("flags=DontShareAddress")
        << QStringLiteral("reason=%1").arg(bindReason);

    if (!m_radioAddress.isNull()) {
        const QByteArray reg(1, '\x00');
        const qint64 sent = m_socket->writeDatagram(reg, m_radioAddress, 4992);
        if (sent == 1) {
            m_totalTxBytes.fetch_add(sent);
            qCDebug(lcVita49) << "PanadapterStream: sent UDP registration to"
                              << m_radioAddress.toString() << ":4992";
            m_routedPrimeElapsed.restart();
            m_routedPrimeTimer->start(250);
        } else {
            qCWarning(lcVita49) << "PanadapterStream: UDP registration send failed after rebind:"
                                << m_socket->errorString();
        }
    } else {
        qCWarning(lcVita49) << "PanadapterStream: radio address unknown - skipping UDP registration after rebind";
    }

    return true;
}

bool PanadapterStream::startWan(const QHostAddress& radioAddr, quint16 radioUdpPort)
{
    if (isRunning()) stop();  // clean up previous session before rebinding (#561)

    resetAudioStreamStats();

    // For WAN: bind to any port. The actual UDP registration happens later
    // in startWanUdpRegister() once the client handle is known.
    bool bound = m_socket->bind(QHostAddress::AnyIPv4, 0);
    if (!bound) {
        qCWarning(lcVita49) << "PanadapterStream: WAN — failed to bind UDP socket:"
                   << m_socket->errorString();
        return false;
    }

    applyReceiveBufferSize();   // #3810 — absorb VITA-49 bursts so they don't drop
    m_localPort = m_socket->localPort();
    m_localAddress = m_socket->localAddress();
    m_radioAddress = radioAddr;
    m_radioPort = radioUdpPort;
    m_isWanMode = true;
    m_wanRegistered = false;
    m_hasReceivedPacket = false;

    qCDebug(lcVita49) << "PanadapterStream: WAN — bound to UDP port" << m_localPort
             << "radio=" << radioAddr.toString() << ":" << radioUdpPort;

    m_conn = nullptr;  // no RadioConnection in WAN mode
    return true;
}

void PanadapterStream::startWanUdpRegister(quint32 clientHandle)
{
    m_wanClientHandle = clientHandle;
    m_wanRegistered = false;

    const QString hex = QString::number(clientHandle, 16).toUpper();
    qCDebug(lcVita49) << "PanadapterStream: WAN — starting UDP registration,"
             << "handle=0x" + hex
             << "sending to" << m_radioAddress.toString() << ":" << m_radioPort;

    // Send first registration immediately, then every 50ms until confirmed
    const QByteArray cmd = QStringLiteral("client udp_register handle=0x%1").arg(hex).toUtf8();
    const qint64 sent = m_socket->writeDatagram(cmd, m_radioAddress, m_radioPort);
    if (sent > 0)
        m_totalTxBytes.fetch_add(sent);
    m_wanRegisterTimer->start(50);
}

void PanadapterStream::stop()
{
    if (m_wanRegisterTimer) {
        m_wanRegisterTimer->stop();
    }
    if (m_wanPingTimer) {
        m_wanPingTimer->stop();
    }
    if (m_routedPrimeTimer) {
        m_routedPrimeTimer->stop();
    }
    m_isWanMode = false;
    m_wanRegistered = false;
    m_wanClientHandle = 0;
    m_hasReceivedPacket = false;
    if (m_socket) {
        m_socket->close();
    }
    m_radioAddress = QHostAddress();
    m_radioPort = 0;
    m_localAddress = QHostAddress();
    m_localPort = 0;
    resetAudioStreamStats();
}

// ─── Datagram reception ───────────────────────────────────────────────────────

void PanadapterStream::registerPanStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownPanStreams.insert(streamId);
    m_everRegisteredPanStreams.insert(streamId);   // arms leak detection (#3856)
    m_orphanStreams.remove(streamId);              // reclaim drops stale orphan
    qCDebug(lcVita49) << "PanadapterStream: registered pan stream 0x" + QString::number(streamId, 16);
}

void PanadapterStream::registerWfStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownWfStreams.insert(streamId);
    m_everRegisteredWfStreams.insert(streamId);    // arms leak detection (#3856)
    m_orphanStreams.remove(streamId);              // reclaim drops stale orphan
    qCDebug(lcVita49) << "PanadapterStream: registered wf stream 0x" + QString::number(streamId, 16);
}

void PanadapterStream::unregisterPanStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownPanStreams.remove(streamId);
    m_frames.remove(streamId);
    m_dbmRanges.remove(streamId);
    m_pendingDbmRanges.remove(streamId);
}

void PanadapterStream::unregisterWfStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownWfStreams.remove(streamId);
    m_wfFrames.remove(streamId);
}

void PanadapterStream::clearRegisteredStreams()
{
    QMutexLocker lock(&m_streamMutex);
    m_knownPanStreams.clear();
    m_knownWfStreams.clear();
    m_frames.clear();
    m_wfFrames.clear();
    m_dbmRanges.clear();
    m_pendingDbmRanges.clear();
    m_daxStreamIds.clear();
    m_iqStreamIds.clear();
    m_loggedDaxPacketStreams.clear();
    m_loggedIqPacketStreams.clear();
    // Drop PLC state alongside the rest of the per-stream tables — this is
    // the disconnect-time reset hook, so anything keyed by VITA-49 stream
    // id can be cleared safely (#2738).
    m_audioPlc.clear();
    m_orphanStreams.clear();
    m_everRegisteredPanStreams.clear();   // new session — re-arm from scratch (#3856)
    m_everRegisteredWfStreams.clear();
    resetAudioStreamStats();
    qCDebug(lcVita49) << "PanadapterStream: cleared all registered streams";
}

QVector<PanadapterStream::OrphanStream> PanadapterStream::orphanStreams() const
{
    QMutexLocker lock(&m_streamMutex);
    const qint64 now = m_orphanClock.isValid() ? m_orphanClock.elapsed() : 0;
    QVector<OrphanStream> out;
    out.reserve(m_orphanStreams.size());
    for (auto it = m_orphanStreams.cbegin(); it != m_orphanStreams.cend(); ++it)
        out.push_back(OrphanStream{it.key(), it->waterfall, it->packets,
                                   now - it->lastSeenMs});
    return out;
}

QVector<quint32> PanadapterStream::registeredPanStreams() const
{
    QMutexLocker lock(&m_streamMutex);
    return QVector<quint32>(m_knownPanStreams.cbegin(), m_knownPanStreams.cend());
}

QVector<quint32> PanadapterStream::registeredWfStreams() const
{
    QMutexLocker lock(&m_streamMutex);
    return QVector<quint32>(m_knownWfStreams.cbegin(), m_knownWfStreams.cend());
}

void PanadapterStream::resetOrphanStreams()
{
    QMutexLocker lock(&m_streamMutex);
    m_orphanStreams.clear();
}

void PanadapterStream::setDbmRange(quint32 streamId, float minDbm, float maxDbm, bool waitForEcho)
{
    minDbm = std::max(minDbm, kMinSpectrumDbm);
    maxDbm = std::max(maxDbm, minDbm + 10.0f);

    QMutexLocker lock(&m_streamMutex);
    if (waitForEcho) {
        m_pendingDbmRanges[streamId] = {minDbm, maxDbm};
        m_dbmRanges[streamId] = {minDbm, maxDbm};
        qCDebug(lcVita49) << "PanadapterStream: pending dBm range for 0x" + QString::number(streamId, 16)
                 << minDbm << "->" << maxDbm;
        return;
    } else {
        const auto pendingIt = m_pendingDbmRanges.constFind(streamId);
        if (pendingIt != m_pendingDbmRanges.constEnd()) {
            const bool matchesPending = std::abs(minDbm - pendingIt->first) < 0.01f
                && std::abs(maxDbm - pendingIt->second) < 0.01f;
            if (!matchesPending) {
                qCDebug(lcVita49) << "PanadapterStream: ignored stale dBm range for 0x"
                         + QString::number(streamId, 16)
                         << minDbm << "->" << maxDbm;
                return;
            }
            m_pendingDbmRanges.remove(streamId);
        }
    }
    m_dbmRanges[streamId] = {minDbm, maxDbm};
    qCDebug(lcVita49) << "PanadapterStream: dBm range for 0x" + QString::number(streamId, 16)
             << minDbm << "->" << maxDbm;
}

bool PanadapterStream::cancelPendingDbmRange(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    const bool removed = m_pendingDbmRanges.remove(streamId) > 0;
    if (removed) {
        qCDebug(lcVita49) << "PanadapterStream: cancelled pending dBm range for 0x"
                         + QString::number(streamId, 16);
    }
    return removed;
}

void PanadapterStream::setYPixels(quint32 streamId, int yPixels)
{
    QMutexLocker lock(&m_streamMutex);
    m_yPixels[streamId] = yPixels;
}


void PanadapterStream::onDatagramReady()
{
    auto& perf = PerfTelemetry::instance();
    const bool perfEnabled = perf.enabled();
    const qint64 batchStartNs = perfEnabled ? PerfTelemetry::nowNs() : 0;
    int batchDatagrams = 0;
    qint64 batchBytes = 0;

    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        if (!dg.isNull()) {
            const QByteArray payload = dg.data();
            const int payloadBytes = payload.size();
            batchDatagrams++;
            batchBytes += payloadBytes;
            if (!m_hasReceivedPacket) {
                m_hasReceivedPacket = true;
                if (m_routedPrimeTimer) {
                    m_routedPrimeTimer->stop();
                }
                const qint64 afterMs = m_routedPrimeElapsed.isValid()
                    ? m_routedPrimeElapsed.elapsed()
                    : -1;
                qCInfo(lcVita49).noquote()
                    << "PanadapterStream: first UDP packet received"
                    << QStringLiteral("after_ms=%1").arg(afterMs)
                    << QStringLiteral("local_port=%1").arg(m_localPort)
                    << QStringLiteral("from=%1:%2").arg(dg.senderAddress().toString()).arg(dg.senderPort());
            }
            // On first VITA-49 packet in WAN mode: registration confirmed.
            // Stop the rapid udp_register timer and switch to ping keepalive.
            if (m_isWanMode && !m_wanRegistered) {
                qCDebug(lcVita49) << "PanadapterStream: WAN — first VITA-49 packet received!"
                         << payloadBytes << "bytes from"
                         << dg.senderAddress().toString() << ":" << dg.senderPort()
                         << "— UDP registration confirmed";
                m_wanRegistered = true;
                if (m_wanRegisterTimer) {
                    m_wanRegisterTimer->stop();
                }
                if (m_wanPingTimer) {
                    m_wanPingTimer->start(5000);
                }
            }
            m_totalRxBytes.fetch_add(payloadBytes);
            processDatagram(payload);
        }
    }

    if (perfEnabled && batchDatagrams > 0) {
        perf.recordUdpBatch(batchDatagrams, batchBytes,
                            static_cast<double>(PerfTelemetry::nowNs() - batchStartNs) / 1000000.0);
    }
}

void PanadapterStream::processDatagram(const QByteArray& data)
{
    if (data.size() < VITA49_HEADER_BYTES) return;

    const auto* raw = reinterpret_cast<const uchar*>(data.constData());

    const quint32 word0    = qFromBigEndian<quint32>(raw);
    const quint32 streamId = qFromBigEndian<quint32>(raw + 4);
    const bool    hasTrailer = (word0 & 0x04000000u) != 0;

    // PacketClassCode is in the lower 16 bits of word 3 (bytes 12-15).
    const quint16 pcc = static_cast<quint16>(qFromBigEndian<quint32>(raw + 12) & 0xFFFFu);
    const int vitaSeq = (word0 >> 16) & 0x0F;

    // Log the first occurrence of each unique stream ID.
    static QSet<quint32> seenIds;
    if (!seenIds.contains(streamId)) {
        seenIds.insert(streamId);
        qCDebug(lcVita49) << "PanadapterStream: new stream" << data.size()
                 << "bytes, word0=0x" + QString::number(word0, 16)
                 << "streamId=0x" + QString::number(streamId, 16)
                 << "pcc=0x" + QString::number(pcc, 16)
                 << "trailer=" << hasTrailer;
    }

    // Check if this stream ID is a DAX stream — route separately
    // Lock for stream ID lookups (written from main thread) (#502)
    int daxChannel = -1, iqChannel = -1;
    bool isPan = false, isWf = false;
    bool logFirstDaxPacket = false;
    bool logFirstIqPacket = false;
    {
        QMutexLocker lock(&m_streamMutex);
        if (m_daxStreamIds.contains(streamId))
            daxChannel = m_daxStreamIds[streamId];
        if (m_iqStreamIds.contains(streamId))
            iqChannel = m_iqStreamIds[streamId];
        if (daxChannel >= 0 && !m_loggedDaxPacketStreams.contains(streamId)) {
            m_loggedDaxPacketStreams.insert(streamId);
            logFirstDaxPacket = true;
        }
        if (iqChannel >= 0 && !m_loggedIqPacketStreams.contains(streamId)) {
            m_loggedIqPacketStreams.insert(streamId);
            logFirstIqPacket = true;
        }
        isPan = m_knownPanStreams.isEmpty() || m_knownPanStreams.contains(streamId);
        isWf  = m_knownWfStreams.isEmpty() || m_knownWfStreams.contains(streamId);

        // #3856 Layer A leak detector: a display packet for a stream we ONCE
        // registered but no longer own is a stream the radio is still sending
        // after we let it go — e.g. a waterfall left alive by a panafall close
        // that omitted "display panafall remove". Keying off "ever-registered
        // AND not-now-registered" keeps the leak detectable even after the live
        // set empties (`pan close all`), and never flags a freshly-created
        // stream still in its registration-lag window. DAX/IQ are excluded.
        if (daxChannel < 0 && iqChannel < 0) {
            const bool wfOrphan  = pcc == PCC_WATERFALL
                                   && m_everRegisteredWfStreams.contains(streamId)
                                   && !m_knownWfStreams.contains(streamId);
            const bool fftOrphan = pcc == PCC_FFT
                                   && m_everRegisteredPanStreams.contains(streamId)
                                   && !m_knownPanStreams.contains(streamId);
            if (wfOrphan || fftOrphan) {
                if (!m_orphanClock.isValid())
                    m_orphanClock.start();
                auto it = m_orphanStreams.find(streamId);
                if (it == m_orphanStreams.end()) {
                    // At capacity, evict the least-recently-seen entry rather
                    // than dropping the new one: a live leak keeps updating its
                    // lastSeenMs so it's never the stalest, while a long-quiet
                    // (already-stopped) orphan is the right one to discard. This
                    // keeps an actively-streaming leak from being lost behind 32
                    // transient entries. (#3856 review)
                    if (m_orphanStreams.size() >= kMaxOrphanStreams) {
                        auto stalest = m_orphanStreams.begin();
                        for (auto e = m_orphanStreams.begin(); e != m_orphanStreams.end(); ++e)
                            if (e->lastSeenMs < stalest->lastSeenMs)
                                stalest = e;
                        m_orphanStreams.erase(stalest);
                    }
                    it = m_orphanStreams.insert(streamId, OrphanRec{});
                }
                it->waterfall  = wfOrphan;
                it->packets   += 1;
                it->lastSeenMs = m_orphanClock.elapsed();
            }
        }
    }

    if (logFirstDaxPacket || logFirstIqPacket) {
        const QStringList fields = {
            QStringLiteral("kind=%1").arg(logFirstDaxPacket ? QStringLiteral("rx") : QStringLiteral("iq")),
            QStringLiteral("stream=0x%1").arg(QString::number(streamId, 16)),
            QStringLiteral("channel=%1").arg(logFirstDaxPacket ? daxChannel : iqChannel),
            QStringLiteral("pcc=0x%1").arg(QString::number(pcc, 16).rightJustified(4, QLatin1Char('0'))),
            QStringLiteral("bytes=%1").arg(data.size()),
            QStringLiteral("seq=%1").arg(vitaSeq),
            QStringLiteral("trailer=%1").arg(hasTrailer ? 1 : 0)
        };
        qCDebug(lcDax).noquote()
            << "PanadapterStream: first DAX packet" << fields.join(QLatin1Char(' '));
        qCDebug(lcCat).noquote()
            << "PanadapterStream: first DAX packet" << fields.join(QLatin1Char(' '));
    }

    // Determine category for per-stream-type stats (#455)
    StreamCategory cat = CatCount;  // CatCount = uncategorized
    if (daxChannel >= 0 || iqChannel >= 0)
        cat = CatDAX;
    else if (pcc == PCC_IF_NARROW || pcc == PCC_IF_NARROW_REDUCED || pcc == PCC_OPUS)
        cat = CatAudio;
    else if (pcc == PCC_FFT && isPan)
        cat = CatFFT;
    else if (pcc == PCC_WATERFALL && isWf)
        cat = CatWaterfall;
    else if (pcc == PCC_METER)
        cat = CatMeter;

    // Per-category byte/packet/sequence tracking.
    // Only track owned/routed streams — skip uncategorized packets. (#455)
    bool sequenceError = false;
    int  audioMissedThisPacket = 0;
    if (cat != CatCount) {
        QMutexLocker statsLock(&m_statsMutex);
        m_catStats[cat].bytes += data.size();
        m_catStats[cat].packets++;
        auto& stats = m_streamStats[streamId];
        stats.totalCount++;
        if (stats.lastSeq >= 0) {
            const int expected = (stats.lastSeq + 1) & 0x0F;
            if (vitaSeq != expected) {
                sequenceError = true;
                stats.errorCount++;
                m_catStats[cat].errors++;
                if (cat == CatAudio) {
                    // 4-bit modular distance, minus the one packet we just got. (#2731)
                    audioMissedThisPacket =
                        ((vitaSeq - stats.lastSeq - 1) & 0x0F);
                }
            }
        }
        stats.lastSeq = vitaSeq;

        if (cat == CatAudio) {
            const int payloadBytes = data.size() - VITA49_HEADER_BYTES - (hasTrailer ? 4 : 0);
            recordAudioStreamPacketLocked(streamId, pcc, payloadBytes, sequenceError);
            if (m_audioPacketTimerStarted) {
                const int gapMs = static_cast<int>(m_audioPacketTimer.restart());
                m_audioPacketGapMs.store(gapMs);
                m_audioPacketGapMaxMs.store(std::max(m_audioPacketGapMaxMs.load(), gapMs));
                if (m_previousAudioPacketGapMs > 0) {
                    const double variation = std::abs(gapMs - m_previousAudioPacketGapMs);
                    m_audioPacketJitterEstimateMs +=
                        (variation - m_audioPacketJitterEstimateMs) / 8.0;
                    m_audioPacketJitterMs.store(
                        static_cast<int>(std::lround(m_audioPacketJitterEstimateMs)));
                }
                m_previousAudioPacketGapMs = gapMs;
            } else {
                m_audioPacketTimer.start();
                m_audioPacketTimerStarted = true;
            }
        }
    }
    if ((cat == CatFFT || cat == CatWaterfall) && PerfTelemetry::instance().enabled()) {
        PerfTelemetry::instance().recordStreamPacket(
            cat == CatWaterfall ? PerfTelemetry::FrameKind::Waterfall
                                : PerfTelemetry::FrameKind::Panadapter,
            sequenceError);
    }

    if (daxChannel >= 0) {
        int channel = daxChannel;
        QByteArray pcm;
        if (pcc == PCC_IF_NARROW) {
            // Float32 stereo big-endian from radio → native float32 stereo
            const int payloadStart = VITA49_HEADER_BYTES;
            const int payloadBytes = data.size() - payloadStart - (hasTrailer ? 4 : 0);
            if (payloadBytes < 4) return;
            const int numFloats = payloadBytes / 4;
            const uchar* src = raw + payloadStart;
            pcm.resize(numFloats * static_cast<int>(sizeof(float)));
            auto* dst = reinterpret_cast<float*>(pcm.data());
            for (int i = 0; i < numFloats; ++i) {
                const quint32 u = qFromBigEndian<quint32>(src + i * 4);
                std::memcpy(&dst[i], &u, 4);
            }
        } else if (pcc == PCC_IF_NARROW_REDUCED) {
            const int payloadStart = VITA49_HEADER_BYTES;
            const int payloadBytes = data.size() - payloadStart - (hasTrailer ? 4 : 0);
            if (payloadBytes < 2) return;
            const int monoSamples = payloadBytes / 2;
            const uchar* src = raw + payloadStart;
            pcm.resize(monoSamples * 2 * static_cast<int>(sizeof(float)));
            auto* dst = reinterpret_cast<float*>(pcm.data());
            for (int i = 0; i < monoSamples; ++i) {
                const float s = qFromBigEndian<qint16>(src + i * 2) / 32768.0f;
                dst[i * 2]     = s;
                dst[i * 2 + 1] = s;
            }
        } else {
            return;
        }
        emit daxAudioReady(channel, pcm);
        return;
    }

    // Check if this stream ID is a DAX IQ stream — route to worker thread
    if (iqChannel >= 0) {
        static constexpr quint16 PCC_IQ_24K  = 0x02E3;
        static constexpr quint16 PCC_IQ_48K  = 0x02E4;
        static constexpr quint16 PCC_IQ_96K  = 0x02E5;
        static constexpr quint16 PCC_IQ_192K = 0x02E6;
        int rate = 0;
        switch (pcc) {
        case PCC_IQ_24K:  rate = 24000;  break;
        case PCC_IQ_48K:  rate = 48000;  break;
        case PCC_IQ_96K:  rate = 96000;  break;
        case PCC_IQ_192K: rate = 192000; break;
        default: return;
        }
        int channel = iqChannel;
        const int payloadStart = VITA49_HEADER_BYTES;
        const int payloadBytes = data.size() - payloadStart - (hasTrailer ? 4 : 0);
        if (payloadBytes < 8) return;
        QByteArray payload(reinterpret_cast<const char*>(raw + payloadStart), payloadBytes);
        emit iqDataReady(channel, payload, rate);
        return;
    }

    // Accumulate PLC pending-missed count before dispatching the audio
    // decoder, so it can prepend concealment to the new packet's PCM. (#2731)
    if (audioMissedThisPacket > 0 && m_plcEnabled.load()) {
        auto& plc = m_audioPlc[streamId];
        plc.pendingMissed =
            std::min(plc.pendingMissed + audioMissedThisPacket, kMaxConcealPackets);
    }

    // Route by PacketClassCode
    switch (pcc) {
    case PCC_IF_NARROW:
        decodeNarrowAudio(raw, data.size(), hasTrailer, streamId);
        return;
    case PCC_IF_NARROW_REDUCED:
        decodeReducedBwAudio(raw, data.size(), hasTrailer, streamId);
        return;
    case PCC_OPUS:
        decodeOpusAudio(raw, data.size(), hasTrailer, streamId);
        return;
    case PCC_FFT:
        if (!isPan) return;
        decodeFFT(raw, data.size(), hasTrailer, streamId);
        return;
    case PCC_WATERFALL:
        if (!isWf) return;
        decodeWaterfallTile(raw, data.size(), hasTrailer, streamId);
        return;
    case PCC_METER:
        decodeMeterData(raw, data.size(), hasTrailer);
        return;
    default:
        break;
    }
}

// ─── FFT decode ──────────────────────────────────────────────────────────────

void PanadapterStream::decodeFFT(const uchar* raw, int totalBytes, bool hasTrailer, quint32 streamId)
{
    static constexpr int FFT_SUBHEADER_BYTES = 12;
    if (totalBytes < VITA49_HEADER_BYTES + FFT_SUBHEADER_BYTES) return;

    const uchar* sub = raw + VITA49_HEADER_BYTES;
    const quint16 startBin   = qFromBigEndian<quint16>(sub + 0);
    const quint16 numBins    = qFromBigEndian<quint16>(sub + 2);
    const quint16 binSize    = qFromBigEndian<quint16>(sub + 4);
    const quint16 totalBins  = qFromBigEndian<quint16>(sub + 6);
    const quint32 frameIndex = qFromBigEndian<quint32>(sub + 8);

    if (numBins == 0 || binSize == 0 || totalBins == 0) return;

    const int binDataOffset = VITA49_HEADER_BYTES + FFT_SUBHEADER_BYTES;
    int binDataBytes = numBins * binSize;
    const int available = totalBytes - binDataOffset - (hasTrailer ? 4 : 0);
    if (available < binDataBytes) {
        binDataBytes = available;
        if (binDataBytes <= 0) return;
    }

    const uchar* binData = raw + binDataOffset;

    // Per-stream frame assembly
    auto& frame = m_frames[streamId];
    if (frameIndex != frame.frameIndex) {
        if (frame.totalBins > 0 && !frame.isComplete())
            PerfTelemetry::instance().recordFrameRestart(PerfTelemetry::FrameKind::Panadapter);
        frame.reset(frameIndex, totalBins);
    }

    if (startBin + numBins > static_cast<quint16>(frame.buf.size()))
        return;

    for (quint16 i = 0; i < numBins; ++i)
        frame.buf[startBin + i] = qFromBigEndian<quint16>(binData + i * 2);

    frame.binsReceived += numBins;

    if (!frame.isComplete()) return;

    // Convert to dBm using per-stream range
    QPair<float,float> dbmRange;
    int yPixVal;
    {
        QMutexLocker lock(&m_streamMutex);
        dbmRange = m_dbmRanges.value(streamId, {-130.0f, -40.0f});
        yPixVal = m_yPixels.value(streamId, 700);
    }
    auto [minDbm, maxDbm] = dbmRange;
    const float range = maxDbm - minDbm;
    if (range <= 0.0f) return;

    const int   count = frame.buf.size();
    QVector<float> bins(count);

    int effectiveYPixels = std::max(yPixVal, 2);
    int rawMax = 0;
    int overRangeCount = 0;
    for (const quint16 rawBin : frame.buf) {
        const int rawValue = static_cast<int>(rawBin);
        rawMax = std::max(rawMax, rawValue);
        if (rawValue >= effectiveYPixels) {
            ++overRangeCount;
        }
    }
    // A stale/tiny y_pixels status makes normal noise bins clamp to one flat
    // floor while strong signals still poke through. If the frame itself shows
    // that the radio is encoding against a taller pixel space, preserve the
    // trace and let the normal dimension re-push/echo path catch up.
    if (overRangeCount > std::max(8, count / 8)) {
        effectiveYPixels = std::max(effectiveYPixels, rawMax + 1);
    }

    const float yPix = static_cast<float>(effectiveYPixels);

    for (int i = 0; i < count; ++i) {
        const float pixel = std::clamp(
            static_cast<float>(frame.buf[i]), 0.0f, yPix - 1.0f);
        const float dbm = maxDbm - (pixel / (yPix - 1.0f)) * range;
        bins[i] = std::clamp(dbm, minDbm, maxDbm);
    }

    const qint64 emittedNs = PerfTelemetry::instance().enabled() ? PerfTelemetry::nowNs() : 0;
    emit spectrumReady(streamId, bins, emittedNs);
}

// ─── Waterfall tile decode ───────────────────────────────────────────────────
//
// Tile sub-header (36 bytes, big-endian, at byte 28):
//   int64  FrameLowFreq      (Hz × 1e6 — i.e. VitaFrequency)
//   int64  BinBandwidth      (Hz × 1e6)
//   uint32 LineDurationMS
//   uint16 Width             (bins per row)
//   uint16 Height            (rows in this tile)
//   uint32 Timecode          (frame index for reassembly)
//   uint32 AutoBlackLevel
//   uint16 TotalBinsInFrame  (total bins if fragmented across packets)
//   uint16 FirstBinIndex
//
// Payload: Width × Height uint16 values (big-endian).
// Conversion: treat as signed int16, divide by 128.  Typical noise floor
// ~96-106, signal peaks ~110-115.  Colour-mapped in SpectrumWidget.

void PanadapterStream::decodeWaterfallTile(const uchar* raw, int totalBytes, bool hasTrailer, quint32 streamId)
{
    static constexpr int TILE_SUBHEADER_BYTES = 36;
    if (totalBytes < VITA49_HEADER_BYTES + TILE_SUBHEADER_BYTES) return;

    const uchar* sub = raw + VITA49_HEADER_BYTES;

    // Extract frequency range from tile sub-header.
    // FrameLowFreq and BinBandwidth are int64 "VitaFrequency" (Hz × 2^20).
    const qint64 frameLowRaw  = qFromBigEndian<qint64>(sub + 0);
    const qint64 binBwRaw     = qFromBigEndian<qint64>(sub + 8);
    const quint16 tileWidth       = qFromBigEndian<quint16>(sub + 20);
    const quint16 tileHeight      = qFromBigEndian<quint16>(sub + 22);
    const quint32 timecode        = qFromBigEndian<quint32>(sub + 24);
    const quint32 autoBlack       = qFromBigEndian<quint32>(sub + 28);
    const quint16 totalBinsInFrame = qFromBigEndian<quint16>(sub + 32);
    const quint16 firstBinIndex   = qFromBigEndian<quint16>(sub + 34);

    if (tileWidth == 0 || tileHeight == 0) return;

    // FrameLowFreq and BinBandwidth arrive as either VitaFrequency (Hz × 2^20)
    // or plain Hz; disambiguate on the raw integer magnitude so there is no
    // upper frequency ceiling. The previous "divide then reject results above
    // 1000 MHz" heuristic blacked out the waterfall for every transverter above
    // 1 GHz (#3449, #1843, #1928, #2835). See VitaTileFrequency.h.
    const auto tileFreq = AetherSDR::Vita::decodeTileFrequencyMhz(frameLowRaw, binBwRaw);
    const double lowFreqMhz = tileFreq.lowMhz;
    const double binBwMhz   = tileFreq.binBwMhz;
    const double highFreqMhz = lowFreqMhz + binBwMhz * tileWidth;

    const int payloadOffset = VITA49_HEADER_BYTES + TILE_SUBHEADER_BYTES;
    const int payloadBytes  = totalBytes - payloadOffset - (hasTrailer ? 4 : 0);
    if (payloadBytes < tileWidth * 2) return;  // need at least one row of bins

    static bool loggedOnce = false;
    if (!loggedOnce) {
        qCDebug(lcVita49) << "WaterfallTile: width=" << tileWidth << "height=" << tileHeight
                 << "totalBinsInFrame=" << totalBinsInFrame
                 << "firstBinIndex=" << firstBinIndex
                 << "timecode=" << timecode
                 << "lowFreqMhz=" << lowFreqMhz
                 << "binBwMhz=" << binBwMhz
                 << "highFreqMhz=" << highFreqMhz
                 << "fullFrameMhz=" << (lowFreqMhz + binBwMhz * totalBinsInFrame)
                 << "autoBlack=" << autoBlack;
        loggedOnce = true;
    }

    // ── Waterfall frame assembly ─────────────────────────────────────────
    // Start a new frame if timecode changed OR if totalBinsInFrame changed.
    // Without the totalBins check, a spoofed packet that reuses a timecode
    // with an inflated totalBinsInFrame leaves wfFrame.buf undersized
    // relative to the bounds calculation below, and the inner write loop
    // then writes past the buffer's end with attacker-chosen bytes.
    // See GHSA-7gvg-x594-pprq.
    auto& wfFrame = m_wfFrames[streamId];
    if (timecode != wfFrame.timecode
        || totalBinsInFrame != wfFrame.totalBins) {
        if (wfFrame.totalBins > 0 && !wfFrame.isComplete())
            PerfTelemetry::instance().recordFrameRestart(PerfTelemetry::FrameKind::Waterfall);
        wfFrame.reset(timecode, totalBinsInFrame, lowFreqMhz, binBwMhz, autoBlack);
    }

    // Copy this fragment's bins into the assembly buffer.
    // Only process the first row (height is typically 1).
    const uchar* tilePayload = raw + payloadOffset;
    const int binsToRead = qMin(static_cast<int>(tileWidth),
                                static_cast<int>(totalBinsInFrame) - static_cast<int>(firstBinIndex));
    if (binsToRead <= 0) return;

    // Defense in depth: even with the reset condition above, validate that
    // the upper write index fits the buffer.  Guards against any future
    // refactor that breaks the wfFrame.buf.size() <-> wfFrame.totalBins
    // invariant.  GHSA-7gvg-x594-pprq.
    if (firstBinIndex + binsToRead > wfFrame.buf.size()) return;

    for (int i = 0; i < binsToRead; ++i) {
        const auto raw16 = static_cast<qint16>(qFromBigEndian<quint16>(tilePayload + i * 2));
        wfFrame.buf[firstBinIndex + i] = static_cast<float>(raw16) / 128.0f;
    }
    wfFrame.binsReceived += binsToRead;

    // Only emit when the full frame is assembled
    if (!wfFrame.isComplete()) return;

    const double frameHighMhz = wfFrame.lowFreqMhz + wfFrame.binBwMhz * wfFrame.totalBins;
    emit waterfallAutoBlackLevel(streamId, wfFrame.autoBlack);
    const qint64 emittedNs = PerfTelemetry::instance().enabled() ? PerfTelemetry::nowNs() : 0;
    emit waterfallRowReady(streamId, wfFrame.buf, wfFrame.lowFreqMhz, frameHighMhz,
                           wfFrame.timecode, emittedNs);
}

// ─── Audio decode ─────────────────────────────────────────────────────────────

void PanadapterStream::setPacketLossConcealment(bool on)
{
    m_plcEnabled.store(on);
    if (!on) {
        // Drop any queued concealment so a future re-enable doesn't dump
        // a stale tail into the next emitted packet.  Safe because the
        // only caller (RadioSetupDialog) routes this through
        // QMetaObject::invokeMethod(..., Qt::QueuedConnection), so the
        // map-clear runs on the same network worker thread that owns
        // m_audioPlc — no cross-thread access on the map. (#2731)
        m_audioPlc.clear();
    }
}

void PanadapterStream::decodeNarrowAudio(const uchar* raw, int totalBytes, bool hasTrailer, quint32 streamId)
{
    // One-time: log the RX audio VITA-49 header for comparison with our TX packets
    static bool rxHeaderLogged = false;
    if (!rxHeaderLogged && totalBytes >= 28) {
        rxHeaderLogged = true;
        const quint32* w = reinterpret_cast<const quint32*>(raw);
        qCDebug(lcVita49) << "VITA-49 RX audio header (host-order):"
                 << QString("w0=%1 w1=%2 w2=%3 w3=%4 w4=%5 w5=%6 w6=%7")
                    .arg(qFromBigEndian(w[0]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[1]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[2]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[3]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[4]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[5]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[6]), 8, 16, QChar('0'))
                 << "totalBytes=" << totalBytes;
    }

    // Payload: big-endian float32 stereo interleaved (L, R, L, R, ...).
    // Byte-swap to native float32 and emit directly — no int16 conversion.
    const int payloadStart = VITA49_HEADER_BYTES;
    const int payloadBytes = totalBytes - payloadStart - (hasTrailer ? 4 : 0);
    if (payloadBytes < 4) return;

    const int numFloats = payloadBytes / 4;
    const uchar* src = raw + payloadStart;

    QByteArray pcm(numFloats * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    for (int i = 0; i < numFloats; ++i) {
        const quint32 u = qFromBigEndian<quint32>(src + i * 4);
        std::memcpy(&dst[i], &u, 4);
    }

    auto& plc = m_audioPlc[streamId];
    pcm = applyConcealmentFade(std::move(pcm), plc, m_plcEnabled.load());
    emit audioDataReady(pcm);
}

void PanadapterStream::decodeReducedBwAudio(const uchar* raw, int totalBytes, bool hasTrailer, quint32 streamId)
{
    // One-time: log the reduced-BW RX audio VITA-49 header
    static bool rxReducedLogged = false;
    if (!rxReducedLogged && totalBytes >= 28) {
        rxReducedLogged = true;
        const quint32* w = reinterpret_cast<const quint32*>(raw);
        qCDebug(lcVita49) << "VITA-49 RX reduced-BW audio header (host-order):"
                 << QString("w0=%1 w1=%2 w2=%3 w3=%4 w4=%5 w5=%6 w6=%7")
                    .arg(qFromBigEndian(w[0]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[1]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[2]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[3]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[4]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[5]), 8, 16, QChar('0'))
                    .arg(qFromBigEndian(w[6]), 8, 16, QChar('0'))
                 << "totalBytes=" << totalBytes;
    }

    // Payload: big-endian int16 mono. Convert to float32 stereo.
    const int payloadStart = VITA49_HEADER_BYTES;
    const int payloadBytes = totalBytes - payloadStart - (hasTrailer ? 4 : 0);
    if (payloadBytes < 2) return;

    const int monoSamples = payloadBytes / 2;
    const uchar* src = raw + payloadStart;

    QByteArray pcm(monoSamples * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    for (int i = 0; i < monoSamples; ++i) {
        const float s = qFromBigEndian<qint16>(src + i * 2) / 32768.0f;
        dst[i * 2]     = s;  // L
        dst[i * 2 + 1] = s;  // R
    }

    auto& plc = m_audioPlc[streamId];
    pcm = applyConcealmentFade(std::move(pcm), plc, m_plcEnabled.load());
    emit audioDataReady(pcm);
}

// ─── Meter data decode ───────────────────────────────────────────────────────
//
// VITA-49 meter packet (PCC 0x8002): payload is N × 4-byte pairs:
//   uint16 meter_id  (big-endian)
//   int16  raw_value (big-endian)
//
// Raw values are converted by MeterModel based on the meter's unit type.
// Reference: FlexLib VitaMeterPacket.cs

void PanadapterStream::decodeOpusAudio(const uchar* raw, int totalBytes, bool hasTrailer, quint32 streamId)
{
    const int payloadStart = VITA49_HEADER_BYTES;
    const int payloadBytes = totalBytes - payloadStart - (hasTrailer ? 4 : 0);
    if (payloadBytes <= 0) return;

    // Lazy-init Opus decoder on first packet
    if (!m_opusCodec) {
        m_opusCodec = new OpusCodec();
        if (!m_opusCodec->isValid()) {
            qCWarning(lcVita49) << "PanadapterStream: Opus codec init failed";
            delete m_opusCodec;
            m_opusCodec = nullptr;
            return;
        }
        qCDebug(lcVita49) << "PanadapterStream: Opus decoder initialized";
    }

    // For each missed packet, synthesize a concealment frame using libopus
    // native PLC before decoding the received frame. Materially better
    // than raw silence for 1–2 dropped frames because the decoder fades
    // from its own internal state. (#2731)
    auto& plc = m_audioPlc[streamId];
    QByteArray int16Concealed;
    if (m_plcEnabled.load() && plc.pendingMissed > 0) {
        const int n = std::min(plc.pendingMissed, kMaxConcealPackets);
        for (int i = 0; i < n; ++i) {
            QByteArray frame = m_opusCodec->concealLost();
            if (frame.isEmpty()) break;
            int16Concealed.append(frame);
        }
        plc.pendingMissed = 0;
    }

    // Opus payload is raw bytes — no byte-swapping needed
    QByteArray opusFrame(reinterpret_cast<const char*>(raw + payloadStart), payloadBytes);
    QByteArray int16pcm = m_opusCodec->decode(opusFrame);
    if (int16pcm.isEmpty()) return;

    QByteArray combined = int16Concealed;
    combined.append(int16pcm);

    // Convert combined int16 PCM to float32 stereo
    const int numSamples = combined.size() / static_cast<int>(sizeof(qint16));
    const auto* src = reinterpret_cast<const qint16*>(combined.constData());
    QByteArray pcm(numSamples * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < numSamples; ++i) {
        dst[i] = src[i] / 32768.0f;
    }
    // Track frame count so a follow-on loss on the uncompressed path (very
    // unusual for a single stream, but keeps state consistent) uses the
    // right fill size. Opus is always stereo at FRAME_SIZE; record frames.
    if (numSamples >= 2) {
        plc.lastFrames = numSamples / 2;
        plc.tailL = dst[numSamples - 2];
        plc.tailR = dst[numSamples - 1];
    }
    emit audioDataReady(pcm);
}

void PanadapterStream::decodeMeterData(const uchar* raw, int totalBytes, bool hasTrailer)
{
    const int payloadStart = VITA49_HEADER_BYTES;
    const int payloadBytes = totalBytes - payloadStart - (hasTrailer ? 4 : 0);
    if (payloadBytes < 4) return;

    const int numMeters = payloadBytes / 4;
    const uchar* payload = raw + payloadStart;

    QVector<quint16> ids(numMeters);
    QVector<qint16>  vals(numMeters);

    for (int i = 0; i < numMeters; ++i) {
        ids[i]  = qFromBigEndian<quint16>(payload + i * 4);
        vals[i] = qFromBigEndian<qint16>(payload + i * 4 + 2);
    }

    emit meterDataReady(ids, vals);
}

int PanadapterStream::audioPayloadFrames(quint16 pcc, int payloadBytes)
{
    if (payloadBytes <= 0) {
        return 0;
    }

    switch (pcc) {
    case PCC_IF_NARROW:
        return payloadBytes / (2 * static_cast<int>(sizeof(float)));
    case PCC_IF_NARROW_REDUCED:
        return payloadBytes / static_cast<int>(sizeof(qint16));
    case PCC_OPUS:
        return kOpusFramesPerPacket;
    default:
        return 0;
    }
}

void PanadapterStream::resetAudioStreamStats()
{
    QMutexLocker statsLock(&m_statsMutex);
    m_audioStreamStats.clear();
    m_audioStreamStatsTimer.restart();
    m_audioPacketGapMs.store(0);
    m_audioPacketGapMaxMs.store(0);
    m_audioPacketJitterMs.store(0);
    m_audioPacketTimerStarted = false;
    m_previousAudioPacketGapMs = 0;
    m_audioPacketJitterEstimateMs = 0.0;
}

void PanadapterStream::resetAudioStreamDiagnostics()
{
    resetAudioStreamStats();
}

void PanadapterStream::recordAudioStreamPacketLocked(quint32 streamId,
                                                     quint16 pcc,
                                                     int payloadBytes,
                                                     bool sequenceError)
{
    const int frames = audioPayloadFrames(pcc, payloadBytes);
    if (frames <= 0) {
        return;
    }

    if (!m_audioStreamStatsTimer.isValid()) {
        m_audioStreamStatsTimer.start();
    }

    const qint64 nowMs = m_audioStreamStatsTimer.elapsed();
    AudioStreamTracker& tracker = m_audioStreamStats[streamId];
    tracker.packetClassCode = pcc;
    ++tracker.packets;
    tracker.frames += frames;
    if (sequenceError) {
        ++tracker.sequenceErrors;
    }

    tracker.expectedPacketMs = frames * 1000.0 / kAudioSampleRate;
    if (tracker.lastArrivalMs >= 0) {
        const int gapMs = static_cast<int>(std::max<qint64>(0, nowMs - tracker.lastArrivalMs));
        tracker.lastGapMs = gapMs;
        tracker.maxGapMs = std::max(tracker.maxGapMs, gapMs);

        const double lateThresholdMs =
            std::max(tracker.expectedPacketMs * 2.0, tracker.expectedPacketMs + 5.0);
        if (gapMs > lateThresholdMs) {
            ++tracker.latePackets;
        }
    }
    tracker.lastArrivalMs = nowMs;

    if (tracker.windowStartMs < 0) {
        tracker.windowStartMs = nowMs;
    }
    tracker.windowFrames += frames;

    const qint64 windowMs = std::max<qint64>(1, nowMs - tracker.windowStartMs);
    if (windowMs >= 250) {
        tracker.feedRateHz = tracker.windowFrames * 1000.0 / windowMs;
        const double mediaMs = tracker.windowFrames * 1000.0 / kAudioSampleRate;
        tracker.deficitMs = windowMs - mediaMs;
    }
    if (windowMs >= 1000) {
        tracker.windowStartMs = nowMs;
        tracker.windowFrames = 0;
    }
}

int PanadapterStream::packetErrorCount() const
{
    QMutexLocker lock(&m_statsMutex);
    int total = 0;
    for (auto it = m_streamStats.constBegin(); it != m_streamStats.constEnd(); ++it)
        total += it->errorCount;
    return total;
}

int PanadapterStream::packetTotalCount() const
{
    QMutexLocker lock(&m_statsMutex);
    int total = 0;
    for (auto it = m_streamStats.constBegin(); it != m_streamStats.constEnd(); ++it)
        total += it->totalCount;
    return total;
}

PanadapterStream::CategoryStats PanadapterStream::categoryStats(StreamCategory cat) const
{
    if (cat < 0 || cat >= CatCount) {
        return {};
    }
    QMutexLocker lock(&m_statsMutex);
    return m_catStats[cat];
}

QVector<PanadapterStream::AudioStreamDiagnostics> PanadapterStream::audioStreamDiagnostics() const
{
    QMutexLocker lock(&m_statsMutex);
    const qint64 nowMs = m_audioStreamStatsTimer.isValid()
        ? m_audioStreamStatsTimer.elapsed()
        : 0;

    QVector<AudioStreamDiagnostics> snapshot;
    snapshot.reserve(m_audioStreamStats.size());
    for (auto it = m_audioStreamStats.constBegin(); it != m_audioStreamStats.constEnd(); ++it) {
        const AudioStreamTracker& tracker = it.value();
        AudioStreamDiagnostics diag;
        diag.streamId = it.key();
        diag.packetClassCode = tracker.packetClassCode;
        diag.packets = tracker.packets;
        diag.frames = tracker.frames;
        diag.sequenceErrors = tracker.sequenceErrors;
        diag.latePackets = tracker.latePackets;
        diag.lastGapMs = tracker.lastGapMs;
        diag.maxGapMs = tracker.maxGapMs;
        diag.expectedPacketMs = tracker.expectedPacketMs;
        diag.feedRateHz = tracker.feedRateHz;
        diag.deficitMs = tracker.deficitMs;
        if (tracker.windowStartMs >= 0 && tracker.windowFrames > 0) {
            const qint64 windowMs = std::max<qint64>(1, nowMs - tracker.windowStartMs);
            if (windowMs >= 250) {
                diag.feedRateHz = tracker.windowFrames * 1000.0 / windowMs;
                const double mediaMs = tracker.windowFrames * 1000.0 / kAudioSampleRate;
                diag.deficitMs = windowMs - mediaMs;
            }
        }
        diag.lastPacketAgeMs = tracker.lastArrivalMs >= 0
            ? std::max<qint64>(0, nowMs - tracker.lastArrivalMs)
            : 0;
        snapshot.push_back(diag);
    }

    std::sort(snapshot.begin(), snapshot.end(),
              [](const AudioStreamDiagnostics& a, const AudioStreamDiagnostics& b) {
        return a.streamId < b.streamId;
    });
    return snapshot;
}

void PanadapterStream::registerDaxStream(quint32 streamId, int channel)
{
    QMutexLocker lock(&m_streamMutex);
    // Enforce one active stream per channel. A stale stream from a previous
    // session or a duplicate subscription created by another code path would
    // cause daxAudioReady to fire twice per audio period — doubling perceived
    // speed. Remove any prior mapping for this channel before inserting.
    for (auto it = m_daxStreamIds.begin(); it != m_daxStreamIds.end(); ) {
        if (it.value() == channel && it.key() != streamId) {
            qCDebug(lcVita49) << "PanadapterStream: evicting stale DAX stream"
                              << Qt::hex << it.key() << "from channel" << channel;
            it = m_daxStreamIds.erase(it);
        } else {
            ++it;
        }
    }
    m_daxStreamIds[streamId] = channel;
    m_loggedDaxPacketStreams.remove(streamId);
    // Ownership table (#3305): the create we requested has materialized (or a
    // leftover stream from a previous arm was adopted). If nobody holds the
    // channel, start the grace clock — an unheld stream is an orphan-in-waiting.
    {
        auto& st = m_daxChannelStates[channel];
        st.streamId = streamId;
        st.createPending = false;
        st.generation = ++m_daxGenCounter;
        if (st.holders == 0)
            scheduleDaxRemovalLocked(channel);
    }
    qCDebug(lcVita49) << "PanadapterStream: registered DAX stream" << Qt::hex << streamId << "-> channel" << channel;
}

quint32 PanadapterStream::daxStreamIdForChannel(int channel) const
{
    QMutexLocker lock(&m_streamMutex);
    for (auto it = m_daxStreamIds.constBegin(); it != m_daxStreamIds.constEnd(); ++it) {
        if (it.value() == channel)
            return it.key();
    }
    return 0;
}

void PanadapterStream::unregisterDaxStream(quint32 streamId)
{
    int channel = 0;
    {
        QMutexLocker lock(&m_streamMutex);
        m_daxStreamIds.remove(streamId);
        m_loggedDaxPacketStreams.remove(streamId);
        // DAX audio streams use the PLC path too; drop the per-stream PLC
        // entry so it doesn't outlive the stream itself (#2738).
        m_audioPlc.remove(streamId);
        // Ownership table (#3305): if the radio tore the stream down while
        // consumers still hold the channel (profile load / slice teardown),
        // schedule a re-create. Our own removals erase the entry first, so
        // this only fires for radio-initiated teardown.
        for (auto it = m_daxChannelStates.begin(); it != m_daxChannelStates.end(); ++it) {
            if (it->streamId == streamId) {
                channel = it.key();
                it->streamId = 0;
                it->generation = ++m_daxGenCounter;
                if (it->holders != 0)
                    scheduleDaxRecreateLocked(it.key());
                else
                    m_daxChannelStates.erase(it);
                break;
            }
        }
        qCDebug(lcVita49) << "PanadapterStream: unregistered DAX stream" << Qt::hex << streamId;
    }
    if (channel)
        emit daxStreamUnregistered(channel, streamId);
}

QList<quint32> PanadapterStream::daxStreamIds() const
{
    QMutexLocker lock(&m_streamMutex);
    return m_daxStreamIds.keys();
}

// ---- Centralized DAX RX channel ownership (#3305) ----

const char* PanadapterStream::daxConsumerName(DaxConsumer who)
{
    switch (who) {
    case DaxConsumer::Bridge: return "bridge";
    case DaxConsumer::Tci:    return "tci";
    case DaxConsumer::Rade:   return "rade";
    }
    return "?";
}

static inline quint8 daxHolderBit(PanadapterStream::DaxConsumer who)
{
    return quint8(1u << quint8(who));
}

quint32 PanadapterStream::acquireDaxChannel(int channel, DaxConsumer who)
{
    if (channel < 1 || channel > 4) return 0;
    bool needCreate = false;
    quint32 streamId = 0;
    {
        QMutexLocker lock(&m_streamMutex);
        auto& st = m_daxChannelStates[channel];
        const quint8 bit = daxHolderBit(who);
        const bool alreadyHeld = st.holders & bit;
        st.holders |= bit;
        // Any acquire invalidates a pending deferred removal.
        st.generation = ++m_daxGenCounter;
        if (st.streamId == 0 && !st.createPending) {
            st.createPending = true;
            needCreate = true;
        }
        streamId = st.streamId;
        if (!alreadyHeld) {
            qCInfo(lcVita49) << "PanadapterStream: DAX ch" << channel
                             << "acquired by" << daxConsumerName(who)
                             << "holders=0x" + QString::number(st.holders, 16)
                             << (needCreate ? "(creating stream)" : "");
        }
    }
    if (needCreate)
        emit daxStreamCreateNeeded(channel);
    return streamId;
}

void PanadapterStream::releaseDaxChannel(int channel, DaxConsumer who)
{
    if (channel < 1 || channel > 4) return;
    QMutexLocker lock(&m_streamMutex);
    auto it = m_daxChannelStates.find(channel);
    if (it == m_daxChannelStates.end()) return;
    const quint8 bit = daxHolderBit(who);
    if (!(it->holders & bit)) return;
    it->holders &= ~bit;
    it->generation = ++m_daxGenCounter;
    qCInfo(lcVita49) << "PanadapterStream: DAX ch" << channel
                     << "released by" << daxConsumerName(who)
                     << "holders=0x" + QString::number(it->holders, 16);
    if (it->holders == 0) {
        if (it->streamId != 0) {
            scheduleDaxRemovalLocked(channel);
        } else if (!it->createPending) {
            m_daxChannelStates.erase(it);
        }
        // else: a create is in flight for a channel nobody wants anymore.
        // Keep the entry so registerDaxStream() finds it when the status
        // lands, sees holders==0, and schedules ONE deterministic removal —
        // erasing here would make the registration re-insert a fresh entry
        // and bounce through create→remove churn (review #4017 item 3).
    }
}

void PanadapterStream::notifyDaxCreateFailed(int channel)
{
    if (channel < 1 || channel > 4) return;
    bool retryArmed = false;
    {
        QMutexLocker lock(&m_streamMutex);
        auto it = m_daxChannelStates.find(channel);
        if (it == m_daxChannelStates.end() || it->streamId != 0) return;
        it->createPending = false;
        it->generation = ++m_daxGenCounter;
        if (it->holders == 0) {
            m_daxChannelStates.erase(it);
        } else {
            // Still wanted: retry on a gentle cadence. Each cycle re-enters
            // this method on failure, so a persistent condition (DAX slots
            // exhausted on the radio) costs one command per kDaxCreateRetryMs
            // — and heals the moment a slot frees or the connection is up.
            const quint32 gen = it->generation;
            QTimer::singleShot(kDaxCreateRetryMs, this, [this, channel, gen]() {
                bool needCreate = false;
                {
                    QMutexLocker lock(&m_streamMutex);
                    auto it = m_daxChannelStates.find(channel);
                    if (it == m_daxChannelStates.end()) return;
                    if (it->generation != gen) return;
                    if (it->holders == 0 || it->streamId != 0 || it->createPending) return;
                    it->createPending = true;
                    it->generation = ++m_daxGenCounter;
                    needCreate = true;
                }
                if (needCreate) {
                    qCInfo(lcVita49) << "PanadapterStream: retrying DAX ch" << channel
                                     << "stream create after failure (#3305)";
                    emit daxStreamCreateNeeded(channel);
                }
            });
            retryArmed = true;
        }
    }
    qCWarning(lcVita49) << "PanadapterStream: DAX ch" << channel
                        << "stream create failed/dropped —"
                        << (retryArmed ? "retry armed" : "channel unheld, entry dropped");
}

void PanadapterStream::releaseAllDaxChannels(DaxConsumer who)
{
    for (int ch = 1; ch <= 4; ++ch)
        releaseDaxChannel(ch, who);
}

bool PanadapterStream::daxChannelHeldBy(int channel, DaxConsumer who) const
{
    QMutexLocker lock(&m_streamMutex);
    auto it = m_daxChannelStates.constFind(channel);
    return it != m_daxChannelStates.constEnd() && (it->holders & daxHolderBit(who));
}

QVector<PanadapterStream::DaxChannelSnapshot> PanadapterStream::daxChannelSnapshot() const
{
    QMutexLocker lock(&m_streamMutex);
    QVector<DaxChannelSnapshot> out;
    out.reserve(m_daxChannelStates.size());
    for (auto it = m_daxChannelStates.constBegin(); it != m_daxChannelStates.constEnd(); ++it) {
        DaxChannelSnapshot s;
        s.channel = it.key();
        s.streamId = it->streamId;
        s.createPending = it->createPending;
        for (DaxConsumer who : {DaxConsumer::Bridge, DaxConsumer::Tci, DaxConsumer::Rade}) {
            if (it->holders & daxHolderBit(who))
                s.holders << QString::fromLatin1(daxConsumerName(who));
        }
        out.append(s);
    }
    std::sort(out.begin(), out.end(),
              [](const DaxChannelSnapshot& a, const DaxChannelSnapshot& b) {
        return a.channel < b.channel;
    });
    return out;
}

void PanadapterStream::resetDaxChannelsForDisconnect()
{
    QMutexLocker lock(&m_streamMutex);
    // Radio reaps every stream of a disconnected client itself
    // (state-machines.md §4.2) — no removal commands, just forget. Bump every
    // generation via the counter so in-flight deferred lambdas expire.
    ++m_daxGenCounter;
    m_daxChannelStates.clear();
    qCDebug(lcVita49) << "PanadapterStream: DAX channel ownership reset for disconnect";
}

// m_streamMutex held. Last holder left: remove the radio-side stream after a
// grace window. The window absorbs the radio's transient unbind/rebind
// dax=0/dax=<ch> status pairs (#3626) — a re-acquire inside the window bumps
// the generation and the removal quietly expires.
void PanadapterStream::scheduleDaxRemovalLocked(int channel)
{
    auto it = m_daxChannelStates.find(channel);
    if (it == m_daxChannelStates.end()) return;
    const quint32 gen = it->generation;
    QTimer::singleShot(kDaxRemovalGraceMs, this, [this, channel, gen]() {
        quint32 removeId = 0;
        {
            QMutexLocker lock(&m_streamMutex);
            auto it = m_daxChannelStates.find(channel);
            if (it == m_daxChannelStates.end()) return;
            if (it->generation != gen) return;      // state changed — expired
            if (it->holders != 0) return;           // re-acquired
            removeId = it->streamId;
            m_daxChannelStates.erase(it);
        }
        if (removeId) {
            qCInfo(lcVita49) << "PanadapterStream: DAX ch" << channel
                             << "unheld past grace — removing stream"
                             << Qt::hex << removeId << "(#3305)";
            unregisterDaxStream(removeId);   // entry already erased: no recreate
            emit daxStreamUnregistered(channel, removeId);
            emit daxStreamRemoveNeeded(removeId, channel);
        }
    });
}

// m_streamMutex held. The radio destroyed a stream we still hold (profile
// load / slice teardown replaces dax_rx streams without a TCI/bridge
// disconnect — the #3476 "switched profile, never came back" failure).
// Re-create after a short backoff so a genuine teardown storm can't turn
// into a create storm.
void PanadapterStream::scheduleDaxRecreateLocked(int channel)
{
    auto it = m_daxChannelStates.find(channel);
    if (it == m_daxChannelStates.end()) return;
    const quint32 gen = it->generation;
    QTimer::singleShot(kDaxRecreateDelayMs, this, [this, channel, gen]() {
        bool needCreate = false;
        {
            QMutexLocker lock(&m_streamMutex);
            auto it = m_daxChannelStates.find(channel);
            if (it == m_daxChannelStates.end()) return;
            if (it->generation != gen) return;
            if (it->holders == 0 || it->streamId != 0 || it->createPending) return;
            it->createPending = true;
            it->generation = ++m_daxGenCounter;  // keep the mutation⇒generation-bump invariant
            needCreate = true;
        }
        if (needCreate) {
            qCInfo(lcVita49) << "PanadapterStream: DAX ch" << channel
                             << "still held after radio-side removal — re-creating (#3476)";
            emit daxStreamCreateNeeded(channel);
        }
    });
}

void PanadapterStream::registerIqStream(quint32 streamId, int channel)
{
    QMutexLocker lock(&m_streamMutex);
    m_iqStreamIds[streamId] = channel;
    m_loggedIqPacketStreams.remove(streamId);
    qCDebug(lcVita49) << "PanadapterStream: registered IQ stream" << Qt::hex << streamId << "-> channel" << channel;
}

void PanadapterStream::unregisterIqStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_iqStreamIds.remove(streamId);
    m_loggedIqPacketStreams.remove(streamId);
    qCDebug(lcVita49) << "PanadapterStream: unregistered IQ stream" << Qt::hex << streamId;
}

void PanadapterStream::sendToRadio(const QByteArray& packet)
{
    if (m_radioAddress.isNull() || m_radioPort == 0) {
        static int dropCount = 0;
        if (++dropCount <= 5)
            qCWarning(lcVita49) << "PanadapterStream::sendToRadio: no dest! addr="
                       << m_radioAddress.toString() << "port=" << m_radioPort;
        return;
    }
    if (!m_socket) {
        return;
    }
    const qint64 sent = m_socket->writeDatagram(packet, m_radioAddress, m_radioPort);
    if (sent > 0) m_totalTxBytes.fetch_add(sent);
    static int txCount = 0;
    ++txCount;
    if (txCount <= 5 || txCount % 1000 == 0) {
        qCDebug(lcVita49) << "PanadapterStream::sendToRadio #" << txCount
                 << "bytes=" << packet.size()
                 << "sent=" << sent
                 << "to=" << m_radioAddress.toString() << ":" << m_radioPort
                 << "localPort=" << m_socket->localPort();
    }
    if (sent < 0) {
        static int errCount = 0;
        if (++errCount <= 10)
            qCWarning(lcVita49) << "PanadapterStream::sendToRadio ERROR:" << m_socket->errorString();
    }
}

} // namespace AetherSDR
