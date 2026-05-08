#include "PanadapterStream.h"
#include "LogManager.h"
#include "OpusCodec.h"
#include "PerfTelemetry.h"
#include "RadioConnection.h"

#include <QNetworkDatagram>
#include <QHostAddress>
#include <QStringList>
#include <QtEndian>
#include <QSet>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr QAbstractSocket::BindMode kLanVitaBindMode = QAbstractSocket::DontShareAddress;

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

bool PanadapterStream::start(RadioConnection* conn)
{
    if (isRunning()) stop();  // clean up previous session before rebinding (#561)

    m_audioPacketGapMs.store(0);
    m_audioPacketGapMaxMs.store(0);
    m_audioPacketJitterMs.store(0);
    m_audioPacketTimerStarted = false;
    m_previousAudioPacketGapMs = 0;
    m_audioPacketJitterEstimateMs = 0.0;

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

    m_audioPacketGapMs.store(0);
    m_audioPacketGapMaxMs.store(0);
    m_audioPacketJitterMs.store(0);
    m_audioPacketTimerStarted = false;
    m_previousAudioPacketGapMs = 0;
    m_audioPacketJitterEstimateMs = 0.0;
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

    m_audioPacketGapMs.store(0);
    m_audioPacketGapMaxMs.store(0);
    m_audioPacketJitterMs.store(0);
    m_audioPacketTimerStarted = false;
    m_previousAudioPacketGapMs = 0;
    m_audioPacketJitterEstimateMs = 0.0;

    // For WAN: bind to any port. The actual UDP registration happens later
    // in startWanUdpRegister() once the client handle is known.
    bool bound = m_socket->bind(QHostAddress::AnyIPv4, 0);
    if (!bound) {
        qCWarning(lcVita49) << "PanadapterStream: WAN — failed to bind UDP socket:"
                   << m_socket->errorString();
        return false;
    }

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
    m_audioPacketGapMs.store(0);
    m_audioPacketGapMaxMs.store(0);
    m_audioPacketJitterMs.store(0);
    m_audioPacketTimerStarted = false;
    m_previousAudioPacketGapMs = 0;
    m_audioPacketJitterEstimateMs = 0.0;
}

// ─── Datagram reception ───────────────────────────────────────────────────────

void PanadapterStream::registerPanStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownPanStreams.insert(streamId);
    qCDebug(lcVita49) << "PanadapterStream: registered pan stream 0x" + QString::number(streamId, 16);
}

void PanadapterStream::registerWfStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownWfStreams.insert(streamId);
    qCDebug(lcVita49) << "PanadapterStream: registered wf stream 0x" + QString::number(streamId, 16);
}

void PanadapterStream::unregisterPanStream(quint32 streamId)
{
    QMutexLocker lock(&m_streamMutex);
    m_knownPanStreams.remove(streamId);
    m_frames.remove(streamId);
    m_dbmRanges.remove(streamId);
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
    m_daxStreamIds.clear();
    m_iqStreamIds.clear();
    m_loggedDaxPacketStreams.clear();
    m_loggedIqPacketStreams.clear();
    qCDebug(lcVita49) << "PanadapterStream: cleared all registered streams";
}

void PanadapterStream::setDbmRange(quint32 streamId, float minDbm, float maxDbm)
{
    QMutexLocker lock(&m_streamMutex);
    m_dbmRanges[streamId] = {minDbm, maxDbm};
    qCDebug(lcVita49) << "PanadapterStream: dBm range for 0x" + QString::number(streamId, 16)
             << minDbm << "->" << maxDbm;
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
            }
        }
        stats.lastSeq = vitaSeq;

        if (cat == CatAudio) {
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

    // Route by PacketClassCode
    switch (pcc) {
    case PCC_IF_NARROW:
        decodeNarrowAudio(raw, data.size(), hasTrailer);
        return;
    case PCC_IF_NARROW_REDUCED:
        decodeReducedBwAudio(raw, data.size(), hasTrailer);
        return;
    case PCC_OPUS:
        decodeOpusAudio(raw, data.size(), hasTrailer);
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
    const int   count = frame.buf.size();
    QVector<float> bins(count);

    const float yPix = static_cast<float>(yPixVal);
    for (int i = 0; i < count; ++i)
        bins[i] = maxDbm - (static_cast<float>(frame.buf[i]) / (yPix - 1.0f)) * range;

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

    // FrameLowFreq and BinBandwidth may be VitaFrequency (Hz × 2^20) or plain Hz.
    // Try VitaFrequency first; if the result is unreasonable, try plain Hz.
    double lowFreqMhz  = static_cast<double>(frameLowRaw) / (1048576.0 * 1e6);
    double binBwMhz    = static_cast<double>(binBwRaw) / (1048576.0 * 1e6);
    if (lowFreqMhz < 0.001 || lowFreqMhz > 1000.0) {
        // Fallback: treat as plain Hz
        lowFreqMhz = static_cast<double>(frameLowRaw) / 1e6;
        binBwMhz   = static_cast<double>(binBwRaw) / 1e6;
    }
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
    // Start a new frame if timecode changed
    auto& wfFrame = m_wfFrames[streamId];
    if (timecode != wfFrame.timecode) {
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

void PanadapterStream::decodeNarrowAudio(const uchar* raw, int totalBytes, bool hasTrailer)
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

    emit audioDataReady(pcm);
}

void PanadapterStream::decodeReducedBwAudio(const uchar* raw, int totalBytes, bool hasTrailer)
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

void PanadapterStream::decodeOpusAudio(const uchar* raw, int totalBytes, bool hasTrailer)
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

    // Opus payload is raw bytes — no byte-swapping needed
    QByteArray opusFrame(reinterpret_cast<const char*>(raw + payloadStart), payloadBytes);
    QByteArray int16pcm = m_opusCodec->decode(opusFrame);
    if (int16pcm.isEmpty()) return;

    // Convert Opus int16 output to float32 stereo
    const int numSamples = int16pcm.size() / static_cast<int>(sizeof(qint16));
    const auto* src = reinterpret_cast<const qint16*>(int16pcm.constData());
    QByteArray pcm(numSamples * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < numSamples; ++i) {
        dst[i] = src[i] / 32768.0f;
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
    QMutexLocker lock(&m_streamMutex);
    m_daxStreamIds.remove(streamId);
    m_loggedDaxPacketStreams.remove(streamId);
    qCDebug(lcVita49) << "PanadapterStream: unregistered DAX stream" << Qt::hex << streamId;
}

QList<quint32> PanadapterStream::daxStreamIds() const
{
    QMutexLocker lock(&m_streamMutex);
    return m_daxStreamIds.keys();
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
