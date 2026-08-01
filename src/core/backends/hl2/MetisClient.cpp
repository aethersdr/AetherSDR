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

// Send a fixed-size wire buffer. Returns the bytes the socket accepted (<0 on
// failure), so callers can meter what actually went out rather than what they
// asked for.
template <std::size_t N>
qint64 sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
              const QHostAddress& host, quint16 port)
{
    return s.writeDatagram(reinterpret_cast<const char*>(buf.data()),
                           static_cast<qint64>(N), host, port);
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
    qRegisterMetaType<AetherSDR::hl2::MetisClient::LinkCounters>(
        "AetherSDR::hl2::MetisClient::LinkCounters");

    // Seed the C&C banks from the Params defaults rather than leaving them
    // zero-initialised until start().
    //
    // A default-constructed Cc is five zero bytes, which is not "no bank" -- it
    // is a WRITE OF ZERO TO REGISTER 0x00, the config register (48 kHz, one
    // receiver) with C0 = 0x00. Before start() the rotation was handing those
    // out, so any caller that built a packet without starting emitted banks that
    // looked like deliberate commands. Nothing shipped depended on it because
    // Hl2Backend always starts first, but it made the un-started object's output
    // meaningless, and it is what made hl2_tx_gate_test's "register address
    // intact" check depend on the rotation's phase rather than on its content.
    //
    // At least one frequency bank ALWAYS exists, so the rotation length is
    // never zero and buildNextControlPacket() has something real to send from
    // the first call.
    m_ccConfig = ccConfig(m_params.sampleRate, 1, m_params.ocFilterByte);
    m_ccGain = ccRxGain(m_params.lnaGainDb);
    m_ccRxFreq.assign(1, ccRxFreq(0, m_params.rxFrequencyHz));
    m_ccTxFreq = ccTxFreq(m_params.rxFrequencyHz);
    m_ccTxDrive = ccTxDrive(0, false);

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
    //
    // TWO PATHS ARM THIS: start() at connect, and setReceiverCount() when the
    // payload layout changes. The restart path is why the gate below is a RECENCY
    // test rather than "has a packet ever arrived".
    //
    // Neither m_linkUp nor m_haveRxSeq can answer for it. A restart deliberately
    // leaves m_linkUp true — clearing it would make the resumed stream re-emit
    // linkUp(), and Hl2Backend republishes its entire initial state on that, over
    // the operator's live panes. And packets the radio put on the wire BEFORE the
    // stop keep arriving for a round trip afterwards, so m_haveRxSeq goes true
    // from a straggler while the start is lost and the radio is silent. Both gates
    // read "the start landed" off evidence that predates the start.
    //
    // m_sinceLastEp6 has no such ambiguity and nothing to tune: a stream that is
    // running reads near zero, one that has stopped reads a full tick or more.
    m_startRetryTimer = new QTimer(this);
    m_startRetryTimer->setInterval(kStartRetryMs);
    connect(m_startRetryTimer, &QTimer::timeout, this, [this] {
        if (!m_running || !m_socket) {
            m_startRetryTimer->stop();
            return;
        }
        if (m_sinceLastEp6.isValid() && m_sinceLastEp6.elapsed() < kEp6FlowingWithinMs) {
            m_startRetryTimer->stop();
            return;   // EP6 is arriving; whichever start we sent got through
        }
        if (m_startAttempts >= kMaxStartAttempts) {
            m_startRetryTimer->stop();
            return;   // the connect watchdog reports the failure
        }
        ++m_startAttempts;
        countTx(sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port));
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

int MetisClient::effectiveNumRx(const Params& p)
{
    int n = p.numRx < 1 ? 1 : p.numRx;
    if (p.boardMaxRx > 0 && n > p.boardMaxRx)
        n = p.boardMaxRx;
    // ccRxFreq() only reaches RX1..RX7 (registers 0x02..0x08); RX8..RX12 live at
    // 0x12..0x16 and are not encoded. Clamping here rather than in the encoder
    // keeps the DEMUX in step with what we can actually tune: running 8 DDCs we
    // cannot retune would give the eighth panadapter a frozen NCO.
    if (n > kMaxTunableRx)
        n = kMaxTunableRx;
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
    // Every receiver starts on RX1's frequency; Hl2Backend moves the rest as it
    // brings each slice up. The BANK COUNT is fixed here and never re-derived
    // per packet, so the round robin and the EP6 demux cannot disagree about how
    // many receivers are running.
    m_ccRxFreq.clear();
    for (int rx = 0; rx < effectiveNumRx(); ++rx)
        m_ccRxFreq.push_back(ccRxFreq(rx, m_params.rxFrequencyHz));
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

    // Counters belong to the SESSION, so they are zeroed here rather than in the
    // constructor: a reconnect must not present the previous link's byte totals
    // as this one's.
    //
    // The port is knowable now; the ADDRESS is not. The bind above is a wildcard,
    // so localAddress() answers 0.0.0.0 — which names no interface to an operator
    // debugging a multi-homed host, where the Flex readout shows a real one in the
    // same field. Which interface actually reaches the radio is a routing decision
    // the kernel makes per datagram, and receiveDatagram() already asks for that
    // metadata, so onReadyRead() upgrades this from the first datagram that
    // carries a destination. "*" states the wildcard in the meantime rather than
    // dressing it up as an address.
    m_link = LinkCounters{};
    m_link.localEndpoint = QStringLiteral("*:%1").arg(m_socket->localPort());
    m_linkEndpointResolved = false;
    m_linkWindowClock.restart();
    m_sinceLastWakeup.invalidate();
    m_linkWindowWakeups = 0;
    m_linkWindowGapSumUs = 0;
    m_linkWindowGapMaxUs = 0;

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
    countTx(sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port));
    sendPrimingBurst(3);

    // NOT the RX sample rate: EP2 is the 48 kHz TX/audio stream (see the header).
    // kTxSamplesPerPacket, not the EP6 count — the EP2 packet is a fixed 126
    // samples whatever the receiver count is, so the pacing must not move when
    // receivers are added. (These were the same 126 while only one RX ran.)
    m_ep2IntervalUs = static_cast<qint64>(kTxSamplesPerPacket) * 1'000'000
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
        countTx(sendTo(*m_socket, metisStop(m_watchdogEnabled), m_host, m_port));
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
    setRxFrequencyHz(0, hz);
}

void MetisClient::setRxFrequencyHz(int rxIndex, std::uint32_t hz)
{
    if (rxIndex < 0 || rxIndex >= static_cast<int>(m_ccRxFreq.size()))
        return;   // not a running receiver -- see the header for why not clamped
    if (rxIndex == 0)
        m_params.rxFrequencyHz = hz;
    m_ccRxFreq[static_cast<std::size_t>(rxIndex)] = ccRxFreq(rxIndex, hz);
    // Send the new NCO value immediately rather than waiting for the rotation.
    // That matters more with several receivers than it did with one: the
    // rotation is now numRx + 2 slots long, so a tune that waited its turn would
    // lag by ~16 ms at four receivers instead of ~8 ms at one.
    //
    // This used to append a 0x39 filter-pipeline reset behind the frequency.
    // That WEDGED THE RADIO -- see requestPipelineReset() for the full story.
    m_oneShot.push_back(m_ccRxFreq[static_cast<std::size_t>(rxIndex)]);
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

void MetisClient::setReceiverCount(int count)
{
    const int before = effectiveNumRx();
    Params next = m_params;
    next.numRx = count;
    const int after = effectiveNumRx(next);
    if (after == before)
        return;                       // nothing to do; do NOT restart for a no-op

    // Preserve the frequency of every receiver that survives. They are the
    // operator's tuning, and a restart that silently returned them all to RX1's
    // frequency would look like the radio jumping bands on its own.
    std::vector<std::uint32_t> keptHz;
    keptHz.reserve(m_ccRxFreq.size());
    for (const Cc& bank : m_ccRxFreq) {
        keptHz.push_back((std::uint32_t(bank[1]) << 24) | (std::uint32_t(bank[2]) << 16)
                       | (std::uint32_t(bank[3]) << 8)  |  std::uint32_t(bank[4]));
    }

    m_params.numRx = count;

    if (!m_running || !m_socket) {
        // Not streaming: just restate the banks. There is no layout to race.
        m_ccConfig = ccConfig(m_params.sampleRate, after, m_params.ocFilterByte);
        m_ccRxFreq.assign(static_cast<std::size_t>(after),
                          ccRxFreq(0, m_params.rxFrequencyHz));
        for (int i = 0; i < after; ++i) {
            const std::uint32_t hz = (static_cast<std::size_t>(i) < keptHz.size())
                                         ? keptHz[static_cast<std::size_t>(i)]
                                         : m_params.rxFrequencyHz;
            m_ccRxFreq[static_cast<std::size_t>(i)] = ccRxFreq(i, hz);
        }
        return;
    }

    qInfo() << "MetisClient: receiver count" << before << "->" << after
                  << "— restarting the EP6 stream so the payload layout"
                     " changes on a hard edge";

    // STOP first. Past this point the radio sends nothing, so there is no packet
    // that could be decoded against the wrong layout.
    countTx(sendTo(*m_socket, metisStop(m_watchdogEnabled), m_host, m_port));

    m_ccConfig = ccConfig(m_params.sampleRate, after, m_params.ocFilterByte);
    m_ccRxFreq.clear();
    for (int i = 0; i < after; ++i) {
        const std::uint32_t hz = (static_cast<std::size_t>(i) < keptHz.size())
                                     ? keptHz[static_cast<std::size_t>(i)]
                                     : m_params.rxFrequencyHz;
        m_ccRxFreq.push_back(ccRxFreq(i, hz));
    }
    // Re-prime with the new config bank before starting, so the very first
    // packet the radio sends is already in the new layout.
    sendPrimingBurst(3);

    // DISCARD WHAT IS ALREADY IN THE SOCKET.
    //
    // The stop above tells the RADIO to stop. It says nothing about the packets the
    // radio had ALREADY put on the wire, which arrived while sendPrimingBurst sat
    // in its msleeps with no event loop running to drain them. Those packets are in
    // the OLD layout, and the comment above — "past this point the radio sends
    // nothing" — is true of the radio and not of this socket. Decoding them against
    // the new m_ccRxFreq misreads every round in them, and the payload carries no
    // receiver-count field, so nothing downstream could report it.
    //
    // This is best-effort by nature: it cannot catch a packet still in flight. That
    // is why the retry below tests how RECENTLY EP6 arrived rather than whether it
    // ever did — a straggler must not be mistaken for a reply to the start.
    int stalePackets = 0;
    while (m_socket->hasPendingDatagrams()) {
        m_socket->receiveDatagram();
        ++stalePackets;
    }
    if (stalePackets > 0)
        qInfo() << "MetisClient: discarded" << stalePackets
                << "EP6 packet(s) buffered in the old layout across the restart";

    // The decode buffers describe the OLD layout; drop them so the first packet
    // after the restart sizes them from the new m_ccRxFreq.
    m_blocks.clear();

    // Sequence tracking restarts with the stream. Without this the first packet
    // after the restart counts as a gap of tens of thousands of "dropped"
    // packets and the health panel reports a link fault that never happened.
    m_haveRxSeq = false;
    m_expectedRxSeq = 0;
    m_sinceLastEp6.restart();

    countTx(sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port));
    sendPrimingBurst(3);

    // ARM THE RETRY, exactly as start() does. This start datagram is as losable
    // as the one at connect, and without a retry a single dropped packet stopped
    // the stream for good: the radio sends nothing, nothing re-asks it to, and
    // kSilenceTimeoutMs (2000 ms) later the EP6 watchdog reports link loss. The
    // operator's session died from having clicked "Add Panadapter".
    //
    // The budget fits inside that window on purpose. From here the timer fires at
    // 300 ms intervals and m_startAttempts reaches kMaxStartAttempts (5) on the
    // tick at 1500 ms, so the last re-send goes out at 1200 ms — comfortably
    // before the 2000 ms silence timeout gives up. Raising kStartRetryMs or
    // kMaxStartAttempts far enough to push 4 * kStartRetryMs past
    // kSilenceTimeoutMs would make the watchdog fire while a retry was still
    // pending, and the retry would be dead code.
    //
    // NOT the connect watchdog: connectFailed() is a connect-time signal and the
    // link here is already up. A restart that never recovers surfaces as link
    // loss through onWatchdogTick(), which is the truthful description of it.
    m_startAttempts = 1;
    m_startRetryTimer->start(kStartRetryMs);
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
        // The rotation is every receiver's NCO, then gain, then ADC assignment:
        // numRx + 2 slots. Each receiver's NCO is RE-ASSERTED rather than sent
        // once, for the same reason the config bank is — a radio that resets or
        // reconnects mid-session must not be left with a stale NCO on the
        // receivers nobody happens to be tuning.
        //
        // At 381 EP2 packets/s and four receivers that refreshes each bank ~64
        // times a second, which is well inside anything the operator can see.
        // `slotCount`, not `slots`: Qt #defines `slots` to nothing.
        const std::size_t nFreq = m_ccRxFreq.size();
        const std::size_t slotCount = nFreq + 2;
        const std::size_t slot = m_roundRobin % slotCount;
        if (slot < nFreq)
            b = m_ccRxFreq[slot];
        else if (slot == nFreq)
            b = m_ccGain;
        else
            b = kCcAdc;
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
    countTx(sendTo(*m_socket, buildNextControlPacket(), m_host, m_port));
}

void MetisClient::countTx(qint64 bytesWritten) noexcept
{
    if (bytesWritten <= 0)
        return;
    m_link.txBytes += static_cast<quint64>(bytesWritten);
    ++m_link.txPackets;
}

void MetisClient::accountReceiveWakeup()
{
    // Gap from the previous wakeup. The FIRST wakeup of a session has nothing
    // to measure from, so it seeds the clock and contributes no sample —
    // otherwise the interval since start() (which includes the priming bursts
    // and the radio's own start latency) would land in the window as if it
    // were a delivery stall.
    if (m_sinceLastWakeup.isValid()) {
        const qint64 gapUs = m_sinceLastWakeup.nsecsElapsed() / 1000;
        ++m_linkWindowWakeups;
        m_linkWindowGapSumUs += gapUs;
        m_linkWindowGapMaxUs = std::max(m_linkWindowGapMaxUs, gapUs);
    }
    m_sinceLastWakeup.restart();
}

void MetisClient::publishLinkCountersIfDue()
{
    if (m_linkWindowClock.isValid() && m_linkWindowClock.elapsed() < kLinkPublishIntervalMs)
        return;

    // Round to the nearest millisecond rather than truncating: at 384 kHz the
    // mean gap is a few hundred microseconds, and truncation would publish a
    // flat 0 ms for every healthy link — indistinguishable from not measuring.
    auto usToMs = [](qint64 us) { return static_cast<int>((us + 500) / 1000); };
    m_link.meanGapMs = m_linkWindowWakeups > 0
                           ? usToMs(m_linkWindowGapSumUs / m_linkWindowWakeups)
                           : -1;
    m_link.maxGapMs = m_linkWindowWakeups > 0 ? usToMs(m_linkWindowGapMaxUs) : -1;

    emit linkCountersUpdated(m_link);

    // The gap figures describe the window that just closed, so the window is
    // reset here. The cumulative byte/packet totals are NOT — those are session
    // figures and the consumer diffs them itself.
    m_linkWindowClock.restart();
    m_linkWindowWakeups = 0;
    m_linkWindowGapSumUs = 0;
    m_linkWindowGapMaxUs = 0;
}

void MetisClient::onReadyRead()
{
    accountReceiveWakeup();

    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const auto bytes = asBytes(dg.data());
        // Counted before the EP6 test: these bytes crossed the wire and were
        // read off this socket whatever they turned out to be, and a receive
        // total that silently omits traffic is worse than one that includes a
        // stray discovery reply.
        m_link.rxBytes += static_cast<quint64>(dg.data().size());

        // The local address the kernel actually delivered on, which a wildcard
        // bind cannot tell us (see start()). Latched once: it is a routing fact
        // about this socket, and re-testing it per datagram would cost a string
        // compare on the hot path for an answer that does not change.
        if (!m_linkEndpointResolved) {
            const QHostAddress dest = dg.destinationAddress();
            if (!dest.isNull()) {
                m_link.localEndpoint = QStringLiteral("%1:%2")
                                           .arg(dest.toString())
                                           .arg(m_socket->localPort());
                m_linkEndpointResolved = true;
            }
        }

        const auto seq = ep6Seq(bytes);
        if (!seq)
            continue;   // not an EP6 packet (e.g. a stray discovery reply)
        ++m_link.rxPackets;

        // Restarted on every packet: this is the one piece of state that says how
        // recently the stream produced anything, which both the silence watchdog
        // and the start-retry read.
        m_sinceLastEp6.restart();
        if (!m_linkUp) {
            m_linkUp = true;
            if (m_connectWatchdog)
                m_connectWatchdog->stop();   // first EP6 — the link is alive
            // Only sound for the CONNECT path, where the socket is fresh and there
            // are no stragglers to mistake for a reply. A receiver-count restart
            // never reaches here (it leaves m_linkUp true on purpose) and disarms
            // its retry through the timer's own recency test instead.
            if (m_startRetryTimer)
                m_startRetryTimer->stop();
            emit linkUp();
        }
        if (m_haveRxSeq && *seq != m_expectedRxSeq) {
            const std::uint32_t gap = *seq - m_expectedRxSeq;   // unsigned wrap
            if (gap < 0x80000000u) {                            // forward gap = real loss
                m_drops += gap;
                m_link.drops = m_drops;
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

        // Decode ONCE against the receiver count we configured the radio with.
        // The wire carries no receiver-count field, so this number is the only
        // thing that makes the payload interpretable -- and it is the same
        // m_ccRxFreq.size() the round robin tunes, never a fresh derivation.
        const std::size_t numRx = m_ccRxFreq.empty() ? 1 : m_ccRxFreq.size();
        if (m_blocks.size() != numRx)
            m_blocks.resize(numRx);
        for (auto& b : m_blocks)
            b.clear();

        if (ep6SamplesMulti(bytes, m_blocks) > 0) {
            // RX1 goes out on both signals: iqBlockReady for the single-receiver
            // consumers, iqBlocksReady for the multi-receiver ones. Emitting the
            // first receiver twice is deliberate -- the alternative is every
            // existing consumer growing a receiver index it has no use for.
            emit iqBlockReady(m_blocks[0]);
            emit iqBlocksReady(m_blocks);
        }
    }

    // AFTER the drain, not before it. The gap sample belongs to the wakeup and is
    // taken at the top, but the byte and packet totals only become true once the
    // datagrams behind this wakeup have been counted — publishing first shipped a
    // snapshot that was one full drain out of date.
    publishLinkCountersIfDue();
}

}  // namespace AetherSDR::hl2
