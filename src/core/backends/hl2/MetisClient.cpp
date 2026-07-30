#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QElapsedTimer>
#include <QThread>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtGlobal>

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <span>

#ifdef Q_OS_WIN
// winsock2.h pulls in windows.h, whose min/max function-like macros otherwise
// clobber std::min/std::max at their use sites (MSVC error C2589).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR::hl2 {

namespace {

// View a QByteArray as a byte span for the protocol decoders.
std::span<const std::uint8_t> asBytes(const QByteArray& d) noexcept
{
    return {reinterpret_cast<const std::uint8_t*>(d.constData()), static_cast<std::size_t>(d.size())};
}

// Send a fixed-size wire buffer.
template <std::size_t N>
void sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
            const QHostAddress& host, quint16 port)
{
    s.writeDatagram(reinterpret_cast<const char*>(buf.data()), static_cast<qint64>(N), host, port);
}

// QUdpSocket does not enable SO_BROADCAST on its own; set it on the native
// descriptor so discovery datagrams reach the subnet broadcast address.
void enableBroadcast(QUdpSocket& s) noexcept
{
    const qintptr fd = s.socketDescriptor();
    if (fd < 0)
        return;
    const int on = 1;
#ifdef Q_OS_WIN
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
#else
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
}

}  // namespace

MetisClient::MetisClient(QObject* parent) : QObject(parent) {
    // Telemetry crosses from the I/O thread to the GUI thread as a queued
    // signal argument; without registration Qt drops the emission with only a
    // warning, and the meters would simply never move.
    qRegisterMetaType<AetherSDR::hl2::Hl2Telemetry>("AetherSDR::hl2::Hl2Telemetry");

    // Paces EP2 from a wall clock (see kEp2PacerTickMs) so C&C keeps flowing
    // even if the EP6 receive path stalls.
    m_ep2Timer = new QTimer(this);
    m_ep2Timer->setInterval(kEp2PacerTickMs);
    m_ep2Timer->setTimerType(Qt::PreciseTimer);
    connect(m_ep2Timer, &QTimer::timeout, this, &MetisClient::onEp2PacerTick);

    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setInterval(kWatchdogTickMs);
    connect(m_watchdogTimer, &QTimer::timeout, this, &MetisClient::onWatchdogTick);

    // metis-start is a single UDP datagram and can simply be lost. Re-send it
    // until EP6 flows or the attempt budget is spent; C&C keeps flowing from the
    // pacer meanwhile, so a retry only needs to repeat the start itself.
    m_startRetryTimer = new QTimer(this);
    m_startRetryTimer->setInterval(kStartRetryMs);
    connect(m_startRetryTimer, &QTimer::timeout, this, [this] {
        if (m_linkUp || !m_running || !m_socket) {
            m_startRetryTimer->stop();
            return;
        }
        if (m_startAttempts >= kMaxStartAttempts) {
            m_startRetryTimer->stop();
            return;   // the connect watchdog reports the failure
        }
        ++m_startAttempts;
        sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port);
    });

    m_connectWatchdog = new QTimer(this);
    m_connectWatchdog->setSingleShot(true);
    connect(m_connectWatchdog, &QTimer::timeout, this, [this] {
        if (!m_linkUp)
            emit connectFailed(QStringLiteral(
                "no IQ stream from the radio within %1 ms of start")
                    .arg(kConnectTimeoutMs));
    });
}

MetisClient::~MetisClient()
{
    stop();
}

QList<MetisClient::Discovered> MetisClient::discover(int timeoutMs, const QHostAddress& broadcast,
                                                     quint16 port)
{
    QList<Discovered> found;
    QUdpSocket sock;
    if (!sock.bind(QHostAddress::AnyIPv4, 0))
        return found;
    enableBroadcast(sock);

    const auto req = discoveryRequest();
    sock.writeDatagram(reinterpret_cast<const char*>(req.data()), static_cast<qint64>(req.size()),
                       broadcast, port);

    QList<QByteArray> seenMacs;   // dedup by MAC
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
        if (!sock.waitForReadyRead(remaining))
            continue;
        while (sock.hasPendingDatagrams()) {
            const QNetworkDatagram dg = sock.receiveDatagram();
            const auto reply = parseDiscoveryReply(asBytes(dg.data()));
            if (!reply)
                continue;
            const QByteArray mac(reinterpret_cast<const char*>(reply->mac.data()),
                                 static_cast<qsizetype>(reply->mac.size()));
            if (seenMacs.contains(mac))
                continue;
            seenMacs.append(mac);
            found.append(Discovered{*reply, dg.senderAddress()});
        }
    }
    return found;
}

int MetisClient::effectiveNumRx() const
{
    int n = m_params.numRx < 1 ? 1 : m_params.numRx;
    if (m_params.boardMaxRx > 0 && n > m_params.boardMaxRx)
        n = m_params.boardMaxRx;
    return n;
}

bool MetisClient::start(const Params& params)
{
    if (m_running)
        stop();

    m_params = params;
    m_host = params.host;
    m_port = params.port;
    m_ccConfig = ccConfig(m_params.sampleRate, effectiveNumRx(), m_params.ocFilterByte);
    m_ccGain = ccRxGain(m_params.lnaGainDb);
    m_ccFreq = ccRx1Freq(m_params.rxFrequencyHz);
    m_txSeq = 0;
    m_roundRobin = 0;
    m_haveRxSeq = false;
    m_drops = 0;
    m_linkUp = false;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    m_socket->setReadBufferSize(1 << 21);   // absorb the continuous EP6 torrent
    connect(m_socket, &QUdpSocket::readyRead, this, &MetisClient::onReadyRead);

    // Order matters. Prime with real C&C frames FIRST so the DDC latches the
    // sample rate, NCO and receiver count, then start the stream, then prime
    // again so nothing is lost to the start transition. Starting before any C&C
    // has landed makes the firmware stream ADC-idle samples (Q pinned to zero).
    m_running = true;

    // Arm the signal-handler stop BEFORE the first start datagram goes out.
    //
    // Ordering matters and it is one-sided: armed-but-not-streaming costs a
    // stray 64-byte datagram to a radio that is not listening for it, while
    // streaming-but-not-armed is a radio that has to be power-cycled. Arm
    // early, on the pessimistic side.
    armEmergencyStop(m_socket->socketDescriptor(), m_host, m_port,
                     metisStop(m_watchdogEnabled));

    sendPrimingBurst(3);
    sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port);
    sendPrimingBurst(3);

    // NOT the RX sample rate: EP2 is the 48 kHz TX/audio stream (see the header).
    m_ep2IntervalUs = static_cast<qint64>(kSamplesPerPacket) * 1'000'000
                    / kEp2AudioRateHz;
    m_ep2Sent = 0;
    m_ep2Clock.restart();
    m_ep2Timer->start();
    m_sinceLastEp6.restart();
    m_watchdogTimer->start();
    m_connectWatchdog->start(kConnectTimeoutMs);
    m_startAttempts = 1;
    m_startRetryTimer->start(kStartRetryMs);
    return true;
}

void MetisClient::sendPrimingBurst(int countPerBank)
{
    for (int i = 0; i < countPerBank; ++i)
        sendControlPacket();
    QThread::msleep(10);
    for (int i = 0; i < countPerBank; ++i)
        sendControlPacket();
    QThread::msleep(10);
}

void MetisClient::onEp2PacerTick()
{
    if (!m_running || !m_socket || m_ep2IntervalUs <= 0)
        return;
    // Catch-up: emit however many frames the wall clock says are due, capped so
    // a long stall cannot produce an unbounded burst.
    const qint64 elapsedUs = m_ep2Clock.nsecsElapsed() / 1000;
    const quint64 due = static_cast<quint64>(elapsedUs / m_ep2IntervalUs);
    int burst = 0;
    while (m_ep2Sent < due && burst < kEp2MaxBurstPerTick) {
        sendControlPacket();
        ++m_ep2Sent;
        ++burst;
    }
}

void MetisClient::onWatchdogTick()
{
    if (!m_running || !m_linkUp)
        return;
    if (m_sinceLastEp6.isValid() && m_sinceLastEp6.elapsed() > kSilenceTimeoutMs) {
        // Socket still open but the radio went quiet — surface it as link loss
        // rather than sitting in a permanently "connected" state.
        m_linkUp = false;
        emit linkDown();
    }
}

void MetisClient::stop()
{
    if (m_startRetryTimer) m_startRetryTimer->stop();
    if (m_ep2Timer)        m_ep2Timer->stop();
    if (m_watchdogTimer)   m_watchdogTimer->stop();
    if (m_connectWatchdog) m_connectWatchdog->stop();
    if (m_socket) {
        sendTo(*m_socket, metisStop(m_watchdogEnabled), m_host, m_port);
        // Disarm only AFTER the normal stop has gone out, and before the
        // descriptor is closed. Disarming earlier would leave a window where a
        // signal arriving mid-teardown released nothing; later would leave a
        // closed — or worse, recycled — descriptor armed.
        disarmEmergencyStop();
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_running = false;
    if (m_linkUp) {
        m_linkUp = false;
        emit linkDown();
    }
}

void MetisClient::setRxFrequencyHz(std::uint32_t hz)
{
    m_params.rxFrequencyHz = hz;
    m_ccFreq = ccRx1Freq(hz);
    // Send the new NCO value immediately rather than waiting for the rotation.
    //
    // This used to append a 0x39 filter-pipeline reset behind the frequency.
    // That WEDGED THE RADIO -- see requestPipelineReset() for the full story.
    m_oneShot.push_back(m_ccFreq);
}

void MetisClient::setSampleRate(SampleRate rate)
{
    m_params.sampleRate = rate;
    // Was hardcoded to 1: changing sample rate silently reset the receiver
    // count, so any multi-receiver configuration would have collapsed to a
    // single receiver the first time the operator changed bandwidth.
    //
    // The filter byte is here for the SAME reason. Everything that shares this
    // register has to be carried through every rebuild of it; anything a
    // rebuild re-defaults gets silently dropped the next time an unrelated
    // control changes — a zoom would have released the band relays.
    m_ccConfig = ccConfig(rate, effectiveNumRx(), m_params.ocFilterByte);
}

void MetisClient::setLnaGainDb(int db)
{
    m_params.lnaGainDb = db;
    m_ccGain = ccRxGain(db);
}

void MetisClient::setBandFilter(int ocFilterByte)
{
    const std::uint8_t oc = static_cast<std::uint8_t>(ocFilterByte & 0x7F);
    if (oc == m_params.ocFilterByte)
        return;                       // relays already where they belong
    m_params.ocFilterByte = oc;
    m_ccConfig = ccConfig(m_params.sampleRate, effectiveNumRx(), oc);
    // Ahead of the rotation: a band change moves the NCO and the filter in the
    // same gesture, and waiting for the round robin would leave the relays on
    // the old band for up to three EP2 frames.
    m_oneShot.push_back(m_ccConfig);
}

void MetisClient::requestPipelineReset()
{
    // DELIBERATELY A NO-OP. Do not re-enable without reading this.
    //
    // This queued a 0x39 filter-pipeline reset after every NCO move. A pan drag
    // issues a centre command every 33 ms (SpectrumWidget kPanDragCommandMs),
    // and Hl2Backend::setPanCenter forwards every one, so dragging fired ~30
    // resets per SECOND at the gateware. The board halted its stream and then
    // stopped answering discovery -- alive at the network layer, but requiring
    // a physical power cycle. Exactly the wedge MetisProtocol.h warns about.
    //
    // It was validated at 7 resets spaced ~2 s apart. The drag path, which is
    // three orders of magnitude more aggressive, was never exercised.
    //
    // TWO causes are plausible and were never separated:
    //   1. The reset rate itself -- resetting the decimation pipeline over and
    //      over while it is streaming.
    //   2. The other fields in 0x39. We wrote ZEROS to [27:24] (watchdog
    //      enable/disable) and [11:8] (master enable/disable) on the ASSUMPTION
    //      that 0 means "no action", because the documented act encodings
    //      (0x8/0x9) have bit 3 set. That was reasoning from the pattern, NOT
    //      verified against the Hermes-Lite 2 gateware RTL.
    //
    // Before bringing this back: verify (2) against the RTL, then rate-limit to
    // genuine large jumps -- never per drag frame -- and test a sustained drag
    // on hardware, not just discrete tunes.
}

void MetisClient::setMox(bool keyed)
{
    if (keyed && !m_txAllowed) {
        // Fail SAFE and stay refused. Not an error return: a caller that could
        // retry past a refusal is exactly what this gate exists to prevent.
        m_mox = false;
        return;
    }
    m_mox = keyed;
}

void MetisClient::setTxFrequencyHz(std::uint32_t hz)
{
    m_ccTxFreq = ccTxFreq(hz);
    m_oneShot.push_back(m_ccTxFreq);
}

void MetisClient::setTxDriveLevel(int level)
{
    // The PA follows the drive level: a non-zero drive means the operator wants
    // output, and on this board that requires the onboard amplifier. Drive 0
    // leaves it disabled, so the safe default state stays safe.
    //
    // Hard safety: a closed transmit gate forces drive 0 / PA off on the wire,
    // whatever level a caller requests. This is the last authority before the
    // C&C bytes are sent, so even a mis-gated caller cannot bias the PA on in a
    // transmit-blocked session. (#4449 review — complements the Hl2Backend guard)
    if (!m_txAllowed)
        level = 0;
    m_ccTxDrive = ccTxDrive(level, level > 0);
    m_oneShot.push_back(m_ccTxDrive);
}

void MetisClient::queueTxIq(std::span<const std::complex<float>> iq)
{
    for (const auto& s : iq)
        m_txIq.push_back(s);
    // Drop the OLDEST on overflow: stale transmit audio is worse than a gap.
    while (m_txIq.size() > kTxQueueMax)
        m_txIq.pop_front();
}

void MetisClient::setTxTestTone(double offsetHz, double amplitude)
{
    m_toneHz = offsetHz;
    m_toneAmp = amplitude < 0.0 ? 0.0 : (amplitude > 1.0 ? 1.0 : amplitude);
    if (m_toneAmp == 0.0)
        m_tonePhase = 0.0;
}

void MetisClient::flushTxIq()
{
    m_txIq.clear();
}

std::array<std::uint8_t, kUsbPacketSize> MetisClient::buildNextControlPacket()
{
    static const Cc kCcAdc = ccAdcAssign();
    Cc b;
    if (!m_oneShot.empty()) {
        b = m_oneShot.front();
        m_oneShot.pop_front();
    } else {
        const Cc* alt[3] = {&m_ccFreq, &m_ccGain, &kCcAdc};
        b = *alt[m_roundRobin % 3];
        ++m_roundRobin;
    }
    // MOX rides in C0 bit 0 of EVERY frame, so BOTH sub-frames carry it -- the
    // radio keys off whichever bank is in flight. m_mox can only be true if the
    // gate allowed it (see setMox), so this is the single place keying reaches
    // the wire and it cannot be set behind the gate's back.
    const bool keyed = m_mox && m_txAllowed;
    auto pkt = ep2Packet(m_txSeq++, withMox(m_ccConfig, keyed), withMox(b, keyed));

    // Only put samples on the wire while actually keyed. Unkeyed frames carry
    // transmit silence, which is what ep2Packet's zero fill already gives us --
    // and which also keeps EADDR zero (see ep2WriteTxIq).
    if (keyed && m_toneAmp > 0.0) {
        // EP2 is clocked at a fixed 48 kHz regardless of the RX sample rate.
        std::vector<std::complex<float>> block(kTxSamplesPerPacket);
        const double dphi = 2.0 * 3.14159265358979323846 * m_toneHz / kEp2AudioRateHz;
        for (int n = 0; n < kTxSamplesPerPacket; ++n) {
            // Negative sine: the HPSDR wire has the opposite handedness to the
            // standard analytic convention, so this is the conjugate — the same
            // correction Hl2TxDsp applies. ONE convention for both transmit
            // paths, or a tone at a non-zero offset would land on the opposite
            // side of the carrier from voice. (At the zero offset TUNE uses,
            // handedness has no effect either way.)
            block[static_cast<std::size_t>(n)] = {
                static_cast<float>(m_toneAmp * std::cos(m_tonePhase)),
                static_cast<float>(-m_toneAmp * std::sin(m_tonePhase))};
            m_tonePhase += dphi;
        }
        // Keep the accumulator bounded without introducing a phase step.
        while (m_tonePhase > 2.0 * 3.14159265358979323846)
            m_tonePhase -= 2.0 * 3.14159265358979323846;
        ep2WriteTxIq(pkt, block);
    } else if (keyed && !m_txIq.empty()) {
        std::vector<std::complex<float>> block;
        const std::size_t n = std::min<std::size_t>(kTxSamplesPerPacket, m_txIq.size());
        block.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            block.push_back(m_txIq.front());
            m_txIq.pop_front();
        }
        ep2WriteTxIq(pkt, block);   // a short block leaves the rest as silence
    }
    return pkt;
}

void MetisClient::sendControlPacket()
{
    if (!m_socket)
        return;
    // Sub-frame 0 always carries the config bank (sample rate + receiver count)
    // so the DDC configuration is re-asserted on every frame; sub-frame 1
    // alternates the remaining banks. Matches the reference client, which pairs a
    // constant config bank with an alternating frequency bank.
    // The ADC-assignment bank joins the alternation: a conforming openHPSDR
    // device leaves every receiver unassigned (and therefore emits all-zero IQ)
    // until it has seen it. Re-asserting it rather than sending it once keeps a
    // device that reconnects or resets mid-session from silently going quiet.
    sendTo(*m_socket, buildNextControlPacket(), m_host, m_port);
}

void MetisClient::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const auto bytes = asBytes(dg.data());

        const auto seq = ep6Seq(bytes);
        if (!seq)
            continue;   // not an EP6 packet (e.g. a stray discovery reply)

        m_sinceLastEp6.restart();
        if (!m_linkUp) {
            m_linkUp = true;
            if (m_connectWatchdog)
                m_connectWatchdog->stop();   // first EP6 — the link is alive
            if (m_startRetryTimer)
                m_startRetryTimer->stop();
            emit linkUp();
        }
        if (m_haveRxSeq && *seq != m_expectedRxSeq) {
            const std::uint32_t gap = *seq - m_expectedRxSeq;   // unsigned wrap
            if (gap < 0x80000000u) {                            // forward gap = real loss
                m_drops += gap;
                emit dropsUpdated(m_drops);
            }
        }
        m_expectedRxSeq = *seq + 1;
        m_haveRxSeq = true;

        // Telemetry rides in the C&C bytes of each EP6 frame. The radio
        // free-runs through the classic response addresses, so this arrives
        // continuously without us ever issuing a RQST -- which is the cadence
        // the oracle asks for anyway (§5: saturating with requests starves the
        // classic responses that carry exactly this).
        const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
        bool telemetryChanged = false;
        for (const std::size_t fs : frameStarts) {
            if (bytes.size() < fs + 8)
                break;
            if (const auto resp = parseEp6Response(bytes.data() + fs)) {
                m_telemetry.apply(*resp);
                telemetryChanged = true;
            }
        }
        // Coalesce to ~10 Hz: telemetry free-runs continuously, so a frame
        // skipped by the throttle is superseded within the interval and the
        // meters never miss a settled value. (#4449 review)
        if (telemetryChanged
            && (!m_telemetryEmitClock.isValid()
                || m_telemetryEmitClock.elapsed() >= kTelemetryMinIntervalMs)) {
            m_telemetryEmitClock.restart();
            emit telemetryUpdated(m_telemetry);
        }

        m_block.clear();
        if (ep6Samples(bytes, m_block) > 0)
            emit iqBlockReady(m_block);

    }
}

}  // namespace AetherSDR::hl2
