#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/Hl2EmergencyStop.h"
#include "core/LogManager.h"   // lcHl2 — commanded-NCO breadcrumbs

#include <QElapsedTimer>
#include <QThread>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtGlobal>

#include <QDebug>
#include <QLoggingCategory>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

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

// Starvation lines log on the shared lcHl2Tx ("aether.hl2.tx", high-rate, off by
// default), which also carries the radio's DSIQ FIFO lines. Reported here, not
// from Hl2Backend's telemetry handler, because the EP2 pacer increments these
// counters on the I/O thread.

namespace {

// A family switch destroys the transport without power-cycling the radio.
// Keep unresolved clock recovery until a complete OFF sequence is sent.
struct Cl1RecoveryRegistry {
    std::mutex mutex;
    QSet<QString> serials;
};

Cl1RecoveryRegistry& cl1RecoveryRegistry()
{
    static Cl1RecoveryRegistry registry;
    return registry;
}

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
    // Started once, here, and never restarted. controlNowMs() differences it
    // across an outstanding request, so a restart mid-request would move the
    // wall-clock floor underneath the deadline that is counting against it.
    m_controlClock.start();

    // Telemetry crosses from the I/O thread to the GUI thread as a queued
    // signal argument; without registration Qt drops the emission with only a
    // warning, and the meters would simply never move.
    qRegisterMetaType<AetherSDR::hl2::Hl2Telemetry>("AetherSDR::hl2::Hl2Telemetry");
    qRegisterMetaType<AetherSDR::hl2::MetisClient::LinkCounters>(
        "AetherSDR::hl2::MetisClient::LinkCounters");
    qRegisterMetaType<AetherSDR::hl2::Ep4Stats>("AetherSDR::hl2::Ep4Stats");
    // The on-demand frame's payload. Registered explicitly rather than relying
    // on moc's automatic registration, because this one crosses the I/O thread
    // to the GUI thread QUEUED, and a queued emit of an unregistered type is a
    // runtime warning and a dropped signal rather than a compile error.
    qRegisterMetaType<QList<float>>("QList<float>");

    // The duty-cycle gate's two timers, built here rather than in start() for
    // the same reason the EP2 pacer is: this object outlives a connect, and a
    // timer created per session is a timer leaked per reconnect. Neither is
    // STARTED until setBandscopeEnabled() asks for one, so a session that never
    // touches the bandscope — which is every session by default, and every
    // session on every other radio family, none of which construct a
    // MetisClient at all — pays for two stopped QTimers and nothing else.
    m_bandscopeTimer = new QTimer(this);
    m_bandscopeTimer->setInterval(kBandscopeSampleMs);
    m_bandscopeTimer->setTimerType(Qt::CoarseTimer);
    connect(m_bandscopeTimer, &QTimer::timeout, this, &MetisClient::onBandscopeTick);
    m_bsSamples.reserve(static_cast<std::size_t>(kEp4BlockSamples));
    m_bandscopeGuard = new QTimer(this);
    m_bandscopeGuard->setSingleShot(true);
    connect(m_bandscopeGuard, &QTimer::timeout, this, &MetisClient::onBandscopeGuardTimeout);

    // Seed the C&C banks from Params defaults: a zero Cc is a write of zero to the
    // config register, not "no bank". At least one frequency bank always exists, so
    // the rotation is never empty.
    m_ccConfig = ccConfig(m_params.sampleRate, 1, m_params.ocFilterByte,
                          m_params.ditherBit, m_params.randomBit);
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

    // setReceiverCount()'s restart steps. Single-shot and precise: the spacing
    // is a floor re-checked on every timeout (advanceReceiverCountRestart), so
    // precision here only keeps the whole restart close to its 40 ms.
    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    m_restartTimer->setTimerType(Qt::PreciseTimer);
    connect(m_restartTimer, &QTimer::timeout, this,
            &MetisClient::advanceReceiverCountRestart);

    // metis-start is one UDP datagram and can be lost: re-send until EP6 flows or the
    // budget is spent. Armed by start() and setReceiverCount(). The test is EP6
    // recency, not m_linkUp (kept true across a restart) or m_haveRxSeq (set by
    // stragglers sent before the stop).
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
        // NOT metisStart(): that is 0x01, and a retry landing while the gate
        // holds a cycle would drop wide_spectrum mid-capture. The cycle could
        // then only end in a guard timeout, charging bandscopeTimeouts for
        // something this client did and logging a line that blames the radio.
        // Identical to metisStart() whenever the gate is idle -- both are
        // 0x01 -- so this is a strict superset. (PR #5650 review round 3;
        // a THIRD benign cause of bandscopeTimeouts, and the only one of the
        // three that is ours.)
        countTx(sendTo(*m_socket,
                       metisRunCommand(m_bsState != BandscopeState::Idle,
                                       m_watchdogEnabled),
                       m_host, m_port));
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
    m_ccConfig = ccConfig(m_params.sampleRate, effectiveNumRx(), m_params.ocFilterByte,
                          m_params.ditherBit, m_params.randomBit);
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
    // The bandscope's own state. ep4_seq_no restarts with the stream, and the
    // radio comes up with wide_spectrum clear because metisStart() sends 0x01
    // — so carrying a stale `enabled` across a connect would report a stream
    // that is not running.
    m_haveEp4Seq = false;
    m_expectedEp4Seq = 0;
    m_ep4Drops = 0;
    m_ep4Rewinds = 0;
    // The gate's counters belong to the session for the same reason the wire
    // ones do. Left standing they survive a reconnect while every sibling is
    // zeroed here and m_link is replaced below, so resetBandscopeMirrors()
    // shows 0 and the FIRST block of the new session publishes the PREVIOUS
    // session's total plus one — a row that jumps instead of counting.
    // (PR #5650 review, K5PTB.)
    m_bsBlocks = 0;
    m_bsTimeouts = 0;
    m_bsConsecutiveTimeouts = 0;
    // Speaker audio belongs to a session: a block queued before the link went
    // down describes a moment that has passed, and playing it into the new
    // session's codec is a quarter-second of the previous connection.
    m_speakerAudio.clear();
    // The ATU request cannot survive a connect either. The radio boots with the
    // bit clear, and re-asserting a tune nobody asked for would start one.
    m_atuTune = false;
    // A CONNECT is where the gate's standing intent is cleared, and one of only
    // two places: a receiver-count restart CARRIES it (Params::bandscope), a
    // stop ends the session it belonged to. Cleared after `m_params = params`
    // above, so a caller cannot open a session with the sensor already
    // running — it is a diagnostic an operator asks for, per session.
    resetBandscopeGate();
    failPendingBandscopeFrame(QStringLiteral("the session restarted"));
    m_params.bandscope = false;
    m_linkUp = false;
    // Same rule, and for the same reason as the counters above: this object
    // outlives a connect, so a window left half-accumulated by the previous
    // session would be published as the first window of the next one with a
    // denominator that belongs to neither.
    m_adcWindowSamples = 0;
    m_adcWindowOverload = 0;
    m_telemetry.adcSamples = 0;
    m_telemetry.adcOverloadSamples = 0;
    m_telemetry.adcWindowMs = 0;
    // This object OUTLIVES a connect: Hl2Backend builds it in its constructor
    // and deletes it in its destructor, so without this the dedupe would carry
    // a frequency across a disconnect and suppress the first push of the next
    // session. The IO board may have been power-cycled in between, and nothing
    // in the protocol can be asked what it currently holds -- the same reason
    // the band filter is re-primed at every connect rather than trusted.
    m_ioBoardTxFreqSent = false;
    m_ioBoardTxFreqHz = 0;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    m_socket->setReadBufferSize(1 << 21);   // absorb the continuous EP6 torrent
    connect(m_socket, &QUdpSocket::readyRead, this, &MetisClient::onReadyRead);

    // Counters belong to the session. The bind is a wildcard, so the local address
    // reads "*" until onReadyRead() resolves it from a datagram's destination.
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
    armStartRetry();

    // HL2 boots on its crystal. Reapply requested CL1 after priming, or send
    // OFF only for this serial's unresolved switch earlier in this process.
    // Ordinary connects and unknown serials must not touch the clock bus.
    if (m_params.cl1RefClock || cl1RecoveryPending()) {
        queueCl1Sequence(m_params.cl1RefClock);
    }
    return true;
}

void MetisClient::sendPrimingBurst(int countPerBank)
{
    sendPrimingBank(countPerBank);
    QThread::msleep(kPrimingBankSpacingMs);
    sendPrimingBank(countPerBank);
    QThread::msleep(kPrimingBankSpacingMs);
}

void MetisClient::sendPrimingBank(int countPerBank)
{
    for (int i = 0; i < countPerBank; ++i)
        sendControlPacket();
}

qint64 MetisClient::sendCommandDatagram(const std::array<std::uint8_t, 64>& cmd)
{
    if (m_commandSinkForTest)
        return m_commandSinkForTest(cmd);
    if (!m_socket)
        return -1;
    return sendTo(*m_socket, cmd, m_host, m_port);
}

void MetisClient::armStartRetry()
{
    // Seed 1: every caller has just sent a run byte, so the timer re-sends on
    // attempts 2..kMaxStartAttempts, i.e. kStartResendsAfterArm more datagrams.
    m_startAttempts = 1;
    if (m_startRetryTimer)
        m_startRetryTimer->start(kStartRetryMs);
}

void MetisClient::onEp2PacerTick()
{
    if (!m_running || (!m_socket && !m_packetSinkForTest) || m_ep2IntervalUs <= 0)
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
    // Inside a receiver-count restart's stop-to-start window the silence is
    // ours, and stage 1's run command (bit 0 set) would be a metis-start ahead
    // of the priming. The window's spacing is a floor with no ceiling, so this
    // is a gate, not an argument from kSilenceTimeoutMs.
    if (restartAwaitingStart())
        return;
    if (m_sinceLastEp6.isValid() && m_sinceLastEp6.elapsed() > kSilenceTimeoutMs) {
        const qint64 silentMs = m_sinceLastEp6.elapsed();
        // Stage 1: re-send the run command before declaring link loss. If the gateware
        // halted because EP2 stopped (dsopenhpsdr1.v's watchdog drops `run`), resuming
        // EP2 does not restart it; only a run command does. A duplicate start is safe:
        // RUNSTOP latches `run_next = eth_data[0]` with no flush or reset, but it also
        // re-latches wide_spectrum (bit 1) and watchdog_disable (bit 7), so the gate is
        // reset first (resetBandscopeGate() sends nothing) and this sends
        // wideSpectrum=false. No metis-stop or priming burst: the layout is unchanged and
        // the burst msleeps on the EP2 thread. m_linkUp stays true so a recovery does not
        // republish state; m_params.bandscope and a pending frame request survive.
        if (!m_silenceRecoveryArmed && m_socket && m_startRetryTimer) {
            m_silenceRecoveryArmed = true;
            ++m_link.silenceRecoveryAttempts;
            // Publish in this turn. linkCountersUpdated otherwise leaves only
            // from onReadyRead, after a datagram, and this path runs because
            // none is arriving. Without the emit, stage 2's linkDown is followed
            // by start(), which replaces m_link, and the failed attempt never
            // reaches the health rows. The snapshot may still carry the previous
            // window's gap figures; the counters are the reason it goes out now.
            emit linkCountersUpdated(m_link);
            qInfo() << "MetisClient: no EP6 for" << silentMs
                    << "ms — re-sending the run command before declaring link loss"
                       " (this one, then up to" << kStartResendsAfterArm
                    << "re-sends at" << kStartRetryMs << "ms)";
            // Sequence tracking is left alone, unlike setReceiverCount() (whose metisStop
            // makes the gateware zero ep6_seq_no on ~run). No path a duplicate start takes
            // resets the sequence: if the radio never halted the gap is real loss, and if it
            // restarts at zero the backward-gap guard (`gap < 0x80000000u`) declines to score
            // it. hl2_receiver_count_restart_test covers both.
            resetBandscopeGate();
            countTx(sendTo(*m_socket,
                           metisRunCommand(/*wideSpectrum=*/false, m_watchdogEnabled),
                           m_host, m_port));
            armStartRetry();
            return;
        }
        // Stage 2. The retry disarms once EP6 flows, so an active timer means the budget
        // is still running; waiting adds at most kMaxStartAttempts * kStartRetryMs
        // (1500 ms) before link loss, against a 5000 ms full teardown.
        if (m_startRetryTimer && m_startRetryTimer->isActive())
            return;
        // A C&C request caught in a silence fails at ~3.5 s rather than ~2 s: its own
        // deadline counts EP6 frames and cannot expire meanwhile, and failing it at stage
        // 1 would turn a successful recovery into a caller-visible failure.

        // Socket still open but the radio went quiet — surface it as link loss
        // rather than sitting in a permanently "connected" state.
        m_linkUp = false;
        m_silenceRecoveryArmed = false;
        // And drop the outstanding request with the link, exactly as stop()
        // does. Hl2ControlRequest::reset()'s own doc says "for a link that went
        // down", and this is that; leaving the machine Awaiting here made the
        // asymmetry a lie and, when the silence is a real metis-stop/restart,
        // reported TimedOut for a request the radio may well have applied
        // before it went away.
        dropControlRequest();
        // End the gate's intent as stop() does; this path does not call stop(), and if
        // EP6 resumes before RadioModel reconnects, linkUp() re-fires with no start().
        // Lower wide_spectrum on the wire first: with no metis-stop here, a bit left set
        // is never lowered and EP4 streams ungated (~3.3 Mbit/s). Usually a no-op, since
        // stage 1 already stopped the gate; covers the no-socket/no-retry-timer route.
        if (m_bsState != BandscopeState::Idle)
            bandscopeDisarm(/*expectTrailing=*/m_bsState != BandscopeState::Arming);
        resetBandscopeGate();
        // resetBandscopeGate() is noexcept and cannot emit, so the answer owed
        // to an outstanding requestBandscopeFrame() is still owed here, as it
        // is at stop() and at setReceiverCount(). Without this the window that
        // asked reads "Waiting for a frame" for the rest of the session.
        failPendingBandscopeFrame(QStringLiteral("the link went down"));
        m_params.bandscope = false;
        emit linkDown();
    }
}

void MetisClient::dropControlRequest()
{
    // Publish first: a verdict that had already settled is real and was earned
    // before the link went, so it is not thrown away with it.
    publishControlVerdict();
    // Then tell a caller that is still waiting. reset() alone would leave it
    // waiting for a signal that can no longer be emitted — silence is the one
    // outcome this seam is not allowed to produce. `refused` is false: the
    // radio did not refuse, it stopped being reachable, which is the same
    // no-answer the deadline reports.
    const auto st = m_ccRequest.state();
    if (st == Hl2ControlRequest::State::Queued
        || st == Hl2ControlRequest::State::Awaiting) {
        emit controlRequestFailed(m_ccRequest.outstanding().addr, /*refused=*/false);
    }
    m_ccRequest.reset();
    // A bank already built but not yet confirmed has nowhere to land now.
    m_requestOnBuiltPacket = false;
}

void MetisClient::stop()
{
    // A receiver-count restart in flight ends here: its remaining steps would
    // put a metis-start on the wire after the stop below.
    cancelReceiverCountRestart();
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
    // Any silence recovery in flight ends with the session. The COUNTERS do
    // not: they live on m_link, which start() replaces and stop() leaves
    // standing, because a stop is not a reason to forget that a recovery
    // happened.
    m_silenceRecoveryArmed = false;
    // metisStop() is 0x00, which clears wide_spectrum as well as run. The gate
    // has nothing left to sample and its intent ends with the session.
    resetBandscopeGate();
    failPendingBandscopeFrame(QStringLiteral("the radio stopped streaming"));
    m_params.bandscope = false;
    // An interrupted write must not finish in the next session. Three kinds of
    // bank: the two below, and the CL1 VersaClock sequence (dropQueuedCl1Banks()):
    //   - the IO board's five-bank I2C write, which a later session would
    //     complete against a board that may have been power-cycled since;
    //   - the 0x09 drive bank. It carries the PA enable and the ATU tune
    //     request, and the next start()'s priming bursts would put a leftover
    //     one on the wire before Hl2Backend's drive-0 -- the PA biased on, or a
    //     tune started, by a session that has already ended (#4579). start()
    //     clears m_atuTune for the same reason.
    // Preserve unrelated one-shot setup: the RX and TX NCO banks assert nothing
    // on the transmit side, and the next session restates both anyway.
    std::erase_if(m_oneShot, [](const Cc& bank) {
        const bool ioBoardWrite = bank[0] == kC0I2c2 && bank[1] == kI2cCookieWrite
                               && bank[2] == (kI2cStopAtEnd | kIoBoardI2cAddr);
        return ioBoardWrite || bank[0] == kC0TxDrive;
    });
    m_ioBoardTxFreqSent = false;
    // Same rule, higher stakes: a half-sent VersaClock sequence finishing in
    // the NEXT session would configure the part from the middle of a table
    // whose earlier writes never happened — that is not a wrong band relay,
    // it is a converter with no usable clock.
    //
    // "start() re-queues the whole sequence, so nothing is lost" is what this
    // comment used to say, and for the OFF table it was NOT TRUE: the record
    // that the radio might still be on CL1 was cleared when the table was
    // queued, so start() had nothing left to act on and the radio stayed on the
    // external reference (#5923 review). The record is now released only on a
    // confirmed send, and dropQueuedCl1Banks() abandons the countdown along with
    // the banks — so the claim holds in both directions and an interrupted
    // recovery resumes on the next connect.
    dropQueuedCl1Banks();
    // Whatever was still queued for the speaker describes a session that has
    // ended, and on a radio with no codec the same bytes would be EADDR writes.
    m_speakerAudio.clear();
    // The radio's response register does not survive a metis-stop, and neither
    // does the quarantine's reason for existing: nothing the next stream
    // delivers can be a reply to a request from this one.
    dropControlRequest();
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
    // The COMMANDED value, which is the only place it can be observed. Nothing
    // reads an NCO register back, and above this seam the frequency is still in
    // the true-RF domain — so without this line a frequency calibration, a
    // transverter offset or an off-by-one in the bank index is invisible to
    // everything except a spectrum analyser on the antenna port.
    qCDebug(lcHl2) << "HL2: RX" << rxIndex << "NCO <-" << hz << "Hz (commanded)";
    m_ccRxFreq[static_cast<std::size_t>(rxIndex)] = ccRxFreq(rxIndex, hz);
    // Send the new NCO value immediately rather than waiting for the rotation.
    // That matters more with several receivers than it did with one: the
    // rotation is now numRx + 2 slots long, so a tune that waited its turn would
    // lag by ~16 ms at four receivers instead of ~8 ms at one.
    //
    // This used to append a 0x39 filter-pipeline reset behind the frequency.
    // That WEDGED THE RADIO -- see requestPipelineReset() for the full story.
    //
    // Not while stopped: see queueOneShotIfRunning(). The bank above is still
    // recorded, and the rotation carries m_ccRxFreq from the first frame of a
    // session -- start() rebuilds it from Params, which is authoritative there.
    queueOneShotIfRunning(m_ccRxFreq[static_cast<std::size_t>(rxIndex)]);
}

void MetisClient::queueOneShotIfRunning(const Cc& bank)
{
    // NOTHING IS QUEUED WHILE STOPPED. That is all this guard establishes.
    // m_oneShot is not cleared by start(), so a bank queued while stopped would
    // go out in the priming burst of whatever session starts next -- ahead of
    // the backend's own start-up state, and for the drive bank ahead of the
    // drive-0 Hl2Backend asserts once start() returns. Guarding the push rather
    // than clearing the queue leaves stop()'s "preserve unrelated one-shot
    // setup" meaning what it says (#4579).
    //
    // It says nothing about a bank queued WHILE RUNNING and not yet drained
    // when the session ends. stop() decides those: it drops the IO-board write,
    // the drive bank and the CL1 sequence, and keeps the RX and TX NCO banks,
    // which do cross into the next session's first frames. See stop().
    //
    // The setting itself is not dropped: every caller records it before this
    // runs. What reaches the next session is decided by that session's own
    // start-up -- the RX NCOs by start()'s Params and the rotation, the drive by
    // the zero Hl2Backend writes after start(), the TX NCO by pushInitialState()
    // on linkUp -- and not by a bank left over from before it began.
    if (!m_running) {
        return;
    }
    m_oneShot.push_back(bank);
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
    m_ccConfig = ccConfig(rate, effectiveNumRx(), m_params.ocFilterByte,
                          m_params.ditherBit, m_params.randomBit);
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

    if (!m_running || !hasCommandTransport()) {
        // Not streaming: just restate the banks. There is no layout to race.
        m_ccConfig = ccConfig(m_params.sampleRate, after, m_params.ocFilterByte,
                              m_params.ditherBit, m_params.randomBit);
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
    countTx(sendCommandDatagram(metisStop(m_watchdogEnabled)));

    m_ccConfig = ccConfig(m_params.sampleRate, after, m_params.ocFilterByte,
                          m_params.ditherBit, m_params.randomBit);
    m_ccRxFreq.clear();
    for (int i = 0; i < after; ++i) {
        const std::uint32_t hz = (static_cast<std::size_t>(i) < keptHz.size())
                                     ? keptHz[static_cast<std::size_t>(i)]
                                     : m_params.rxFrequencyHz;
        m_ccRxFreq.push_back(ccRxFreq(i, hz));
    }
    // Between stop and start the steps are spaced by m_restartTimer, not
    // msleep, so this thread keeps pacing EP2 (now carrying the new config
    // bank) and reading EP6 (discarded while restartAwaitingStart()). No run
    // byte may reach the wire before the Start step: bit 0 would start the
    // radio unprimed. So the gate, the start retry and the watchdog hold here.
    cancelReceiverCountRestart();   // a newer restart supersedes an older one
    if (m_startRetryTimer)
        m_startRetryTimer->stop();
    resetBandscopeGate();
    m_sinceLastEp6.restart();
    m_silenceRecoveryArmed = false;
    m_restartStalePackets = 0;

    // Re-prime with the new config bank before starting, so the very first
    // packet the radio sends is already in the new layout.
    sendPrimingBank(kPrimingFramesPerBank);
    m_restartStep = RestartStep::PrimeBeforeStart;
    scheduleReceiverCountRestartStep();
}

void MetisClient::scheduleReceiverCountRestartStep()
{
    m_restartStepClock.restart();
    if (m_restartTimer)
        m_restartTimer->start(kPrimingBankSpacingMs);
}

void MetisClient::cancelReceiverCountRestart() noexcept
{
    if (m_restartTimer)
        m_restartTimer->stop();
    m_restartStep = RestartStep::Idle;
}

void MetisClient::advanceReceiverCountRestart()
{
    if (m_restartStep == RestartStep::Idle)
        return;
    if (!m_running || !hasCommandTransport()) {
        cancelReceiverCountRestart();
        return;
    }
    // The spacing is a floor: a timer may fire early and elapsed() truncates to
    // whole milliseconds, so a step runs only once kPrimingBankSpacingMs have
    // passed; otherwise the timer is re-armed for the remainder.
    const qint64 remainingMs = kPrimingBankSpacingMs - m_restartStepClock.elapsed();
    if (remainingMs > 0) {
        if (m_restartTimer)
            m_restartTimer->start(static_cast<int>(remainingMs));
        return;
    }

    switch (m_restartStep) {
    case RestartStep::Idle:
        return;

    case RestartStep::PrimeBeforeStart:
        sendPrimingBank(kPrimingFramesPerBank);
        m_restartStep = RestartStep::Start;
        scheduleReceiverCountRestartStep();
        return;

    case RestartStep::Start: {
        // Discard what is still in the socket: packets sent before the stop are in
        // the old layout and would be misread against the new m_ccRxFreq,
        // undetectably. handleDatagram() dropped those read since the stop. Best
        // effort (in-flight packets still arrive), hence the retry's recency test.
        while (m_socket && m_socket->hasPendingDatagrams()) {
            m_socket->receiveDatagram();
            ++m_restartStalePackets;
        }
        if (m_restartStalePackets > 0)
            qInfo() << "MetisClient: discarded" << m_restartStalePackets
                    << "datagram(s) received between the restart's stop and start"
                       " (old-layout EP6, plus any EP4 or stray reply)";

        // The decode buffers describe the OLD layout; drop them so the first packet
        // after the restart sizes them from the new m_ccRxFreq.
        m_blocks.clear();

        // Sequence tracking restarts with the stream. Without this the first packet
        // after the restart counts as a gap of tens of thousands of "dropped"
        // packets and the health panel reports a link fault that never happened.
        m_haveRxSeq = false;
        m_expectedRxSeq = 0;
        // Same for the bandscope, and for the same reason: the restart puts 0x00 and
        // then 0x01 on the wire, RUNSTOP clears wide_spectrum along with run, and
        // ep4_seq_no restarts from zero.
        m_haveEp4Seq = false;
        m_expectedEp4Seq = 0;
        // The gate's in-flight cycle does not survive the restart: the run byte
        // went to 0x00 and back. Its intent does, in m_params.bandscope, re-applied
        // at Finish. Reset again here, not only at the stop: a gate enabled or a
        // frame requested inside the window changed state without reaching the wire.
        resetBandscopeGate();
        // The stream this request would have been answered from is being torn down
        // and rebuilt. A frame taken across that boundary would be half of each.
        failPendingBandscopeFrame(QStringLiteral("the receiver count changed"));
        m_sinceLastEp6.restart();
        // AND END ANY SILENCE RECOVERY IN FLIGHT, or this path steals its result.
        // A restart sends its own stop + start + priming burst, so the EP6 that
        // comes back afterwards is THIS path's doing. Left armed, handleDatagram
        // would credit it to the watchdog's run command and record a completion the
        // recovery did not earn -- which is exactly the number that has to stay
        // honest, because "attempts without completions" is the whole diagnostic.
        // The reverse case needs nothing: the watchdog cannot arm a recovery while
        // this is running, because m_sinceLastEp6 has just been restarted.
        m_silenceRecoveryArmed = false;

        // Leave the window BEFORE the start goes out: from here on a datagram in
        // the socket is a reply to it.
        m_restartStep = RestartStep::PrimeAfterStart;
        countTx(sendCommandDatagram(metisStart(m_watchdogEnabled)));
        sendPrimingBank(kPrimingFramesPerBank);
        scheduleReceiverCountRestartStep();
        return;
    }

    case RestartStep::PrimeAfterStart:
        sendPrimingBank(kPrimingFramesPerBank);
        m_restartStep = RestartStep::Finish;
        scheduleReceiverCountRestartStep();
        return;

    case RestartStep::Finish:
        m_restartStep = RestartStep::Idle;
        // Arm the retry as start() does: this start datagram can be lost too. The
        // budget expires inside kSilenceTimeoutMs (static_assert in the header), so
        // the watchdog cannot fire with a retry pending. A restart that never
        // recovers surfaces as link loss via onWatchdogTick(), not connectFailed().
        armStartRetry();

        // RE-ESTABLISH THE GATE ACROSS THE RESTART. This is the path the guard
        // timer's second term exists for: the run byte has just gone 0x00 -> 0x01,
        // so the gateware's bs_cnt is re-arming from scratch and the first EP4
        // datagram is 129 EP6 packets away — 0.339 s at 48 kHz, 0.042 s at 384 kHz.
        // A guard fixed at 10 block intervals (105 ms) would abandon every cycle at
        // 48 kHz and none at 384. See bandscopeGuardMs().
        if (m_params.bandscope)
            applyBandscopeGate();
        return;
    }
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
    m_ccConfig = ccConfig(m_params.sampleRate, effectiveNumRx(), oc,
                          m_params.ditherBit, m_params.randomBit);
    // Nothing queued: live m_ccConfig rides bank A of every EP2 frame, so the relays
    // follow on the next frame (~2.6 ms). A one-shot copy would land in bank B, which
    // the radio applies after A, as a stale snapshot (#4579).
}

void MetisClient::setDitherRandomBits(bool dither, bool random)
{
    if (dither == m_params.ditherBit && random == m_params.randomBit)
        return;
    m_params.ditherBit = dither;
    m_params.randomBit = random;
    m_ccConfig = ccConfig(m_params.sampleRate, effectiveNumRx(),
                          m_params.ocFilterByte, dither, random);
    // NOTHING IS QUEUED, for the reason setBandFilter() spells out at length:
    // live m_ccConfig rides bank A of every EP2 frame, so this is on the wire
    // within ~2.6 ms at 48 kHz, and a queued COPY would be a snapshot that
    // could re-assert a stale sample rate behind a later change.
}

void MetisClient::setLocalCodec(bool present)
{
    if (present == m_params.hasCodec)
        return;
    m_params.hasCodec = present;
    if (!present) {
        // DROPPED, not left to drain. With no codec declared the audio slot is
        // EADDR again, so every queued sample would be an extended-address
        // write rather than sound — see ep2WriteTxAudio().
        m_speakerAudio.clear();
    }
}

void MetisClient::submitSpeakerAudio(const QByteArray& interleavedInt16)
{
    if (!m_params.hasCodec)
        return;                       // the slot is EADDR on this radio
    const auto n = static_cast<std::size_t>(interleavedInt16.size()) / sizeof(std::int16_t);
    if (n == 0)
        return;
    const char* raw = interleavedInt16.constData();
    for (std::size_t i = 0; i < n; ++i) {
        std::int16_t v = 0;
        std::memcpy(&v, raw + i * sizeof(std::int16_t), sizeof(v));
        m_speakerAudio.push_back(v);
    }
    // Drop the oldest: after a consumer stall, keeping the newest returns the speaker
    // to real time. The radio-clocked producer and wall-clocked consumer drift; if
    // the producer is faster the queue pins at the cap (a quarter second of latency)
    // and nothing corrects that yet. Parity is structural: the front is always a left
    // sample, and the push, the cap and the consumer's `& ~1` all move even counts.
    static_assert(kSpeakerAudioCapSamples % 2 == 0,
                  "the cap counts samples, so it must hold whole L/R frames");
    while (m_speakerAudio.size() > kSpeakerAudioCapSamples)
        m_speakerAudio.pop_front();
}

void MetisClient::clearSpeakerAudio()
{
    m_speakerAudio.clear();
}

void MetisClient::setCl1RefClock(bool externalRef)
{
    m_params.cl1RefClock = externalRef;
    if (!m_running) {
        return;        // start() re-sends the sequence; see its call site
    }
    queueCl1Sequence(externalRef);
}

bool MetisClient::cl1RecoveryPendingFor(const QString& serial) noexcept
{
    if (serial.isEmpty()) {
        return false;
    }
    Cl1RecoveryRegistry& registry = cl1RecoveryRegistry();
    const std::lock_guard lock(registry.mutex);
    return registry.serials.contains(serial);
}

bool MetisClient::isCl1Bank(const Cc& bank) noexcept
{
    return bank[0] == kC0I2c1 && bank[1] == kI2cCookieWrite
        && bank[2] == (kI2cStopAtEnd | kVersaClockI2cAddr);
}

void MetisClient::queueCl1Sequence(bool externalRef)
{
    // Replace rather than append. Two sequences in the queue would apply the
    // older one LAST — an operator who toggled the setting twice would end on
    // the state they toggled away from.
    dropQueuedCl1Banks();
    // Remember ON before any writes can leave the host. Interrupted sequences
    // may already have moved the clock; only all 24 completed OFF writes release
    // this serial's recovery record, including across transport recreation.
    if (externalRef) {
        if (m_params.radioSerial.isEmpty()) {
            // Nothing to key the record on, so nothing could recover it. Worth a
            // line in the log rather than a silent gap: the radio is going onto
            // an external reference that no later connect will offer to undo.
            qCWarning(lcHl2) << "HL2: switching CL1 on for a radio with no serial —"
                             << "this session cannot offer to switch it back";
        } else {
            Cl1RecoveryRegistry& registry = cl1RecoveryRegistry();
            const std::lock_guard lock(registry.mutex);
            registry.serials.insert(m_params.radioSerial);
        }
    }
    m_cl1SequenceRadio = m_params.radioSerial;
    m_cl1SequenceIsOff = !externalRef;
    m_cl1BanksUnsent = static_cast<int>(kVersaClockCl1Banks);
    for (const Cc& bank : versaClockCl1Banks(externalRef)) {
        m_oneShot.push_back(bank);
    }
    qCInfo(lcHl2) << "HL2: CL1 reference clock ->"
                  << (externalRef ? "external 10 MHz" : "onboard crystal")
                  << "— queued" << kVersaClockCl1Banks << "VersaClock writes";
}

void MetisClient::dropQueuedCl1Banks()
{
    std::erase_if(m_oneShot, isCl1Bank);
    // Discarded writes cannot complete an OFF sequence or release recovery.
    m_cl1BanksUnsent = 0;
    m_cl1BankOnBuiltPacket = false;
    m_cl1SequenceRadio.clear();
    m_cl1SequenceIsOff = false;
}

void MetisClient::setAtuTuneRequest(bool request)
{
    // Refused outright while stopped, not merely left unsent: start() clears
    // m_atuTune because "re-asserting a tune nobody asked for would start one",
    // and a request with no session behind it is one nobody asked for yet.
    // Logged, because nothing answers the caller and a tune has no readback:
    // without the line a dropped request leaves no trace in a support bundle.
    if (!m_running) {
        qCDebug(lcHl2) << "HL2: ATU tune request refused — no session is running";
        return;
    }
    if (request == m_atuTune)
        return;
    m_atuTune = request;
    // 0x09 carries the drive level and the PA enable alongside this bit, so the
    // whole bank is restated. Reading the level back out of m_ccTxDrive keeps
    // the PA-enable rule in setTxDriveLevel()'s hands — there is exactly one
    // place that decides whether the amplifier is on, and this is not it.
    m_ccTxDrive = ccTxDrive(m_ccTxDrive[1], m_ccTxDrive[1] > 0, m_atuTune);
    queueOneShotIfRunning(m_ccTxDrive);
}

void MetisClient::setIoBoardTxFrequencyHz(quint64 hz)
{
    // Refuse disconnected requests even if a caller missed the backend guard.
    // stop() also discards any unfinished board write from a running session.
    if (!m_running)
        return;
    if (m_ioBoardTxFreqSent && hz == m_ioBoardTxFreqHz)
        return;                       // already queued in this session
    m_ioBoardTxFreqSent = true;
    m_ioBoardTxFreqHz = hz;
    // INFO, not debug, and for the same reason the band filter is: there is no
    // readback. The board never answers -- we deliberately do not set RQST --
    // so this line is the only record of what an amplifier was told to switch
    // to, and a support log captured after a mis-keying has to already have it.
    qCInfo(lcHl2).nospace()
        << "HL2 IO board: TX frequency -> "
        << QString::number(static_cast<double>(hz) / 1.0e6, 'f', 6)
        << " MHz (I2C2 chip 0x1D, 5 banks)";
    // Order is the batch's, not ours -- see ccIoBoardTxFrequency(). Appending
    // in sequence is the whole contract: the deque preserves it, and the last
    // bank is the one that makes the board latch.
    for (const Cc& bank : ccIoBoardTxFrequency(hz))
        m_oneShot.push_back(bank);
}

void MetisClient::requestPipelineReset()
{
    // Deliberately a no-op. 0x39 resets at pan-drag rate (~30/s) wedge the HL2: the
    // stream halts and the board needs a power cycle. The cause is either the rate or
    // the zeros written to 0x39's [27:24] watchdog and [11:8] master-enable fields
    // (assumed "no action", unverified against the RTL). Before re-enabling: verify
    // those fields in the RTL, rate-limit to large jumps, and test a sustained drag.
}

bool MetisClient::requestRegister(int addr, quint32 data, bool subsystemRead)
{
    // Allow-list (fails closed; add an entry with its reason). Rule: re-asserted or
    // excluded. 0x0a and 0x0e are re-asserted by buildNextControlPacket's round
    // robin, so a bad write self-corrects within one rotation (~16 ms at 4 RX), and
    // neither emits RF. Excluded:
    //   0x01  TX1 NCO: sent once per tune, never refreshed.
    //   0x3d  I2C2: the IO board's amplifiers, relays, transverters.
    //   0x3b  a generic AD9866 SPI write (cookie 8'h06), not a read: control.v's
    //         RESP_READ returns no AD9866 data. It can hit TX gain registers that the
    //         0x09 handler's shadow register would not re-assert.
    //   0x09  TX drive, PA enable, ATU.
    //   0x39  sync/reset; carries the watchdog and master enables.
    // Adding 0x09/0x39 needs a transmitEnabled() gate; 0x3b needs its cookie and
    // target register named at the call site.
    static constexpr int kRequestableAddresses[] = {
        kC0AdcGain >> 1,            // 0x0a  AD9866 RX LNA gain (ccRxGain)
        kC0AdcAssignOrTxGain >> 1,  // 0x0e  ADC assign / TX LNA gain (ccAdcAssign)
    };
    if (std::find(std::begin(kRequestableAddresses), std::end(kRequestableAddresses), addr)
        == std::end(kRequestableAddresses))
        return false;

    // No allow-listed address replies with a READ value: on this gateware only
    // the I2C buses do (control.v RESP_READ, `cmd_resp_data_i2c`), and neither
    // is on the list. A caller asking for Echo::SubsystemRead here is asking to
    // throw away the echo and match on six bits of address alone — which is the
    // pairing the quarantine narrows but cannot rule out. Refuse rather than
    // silently downgrade the match.
    if (subsystemRead)
        return false;

    // Response slots live inside the EP6 frame, and the EP6 emit path is gated
    // on `run`. Before the stream is up there is no clock on which a reply
    // could arrive, so arming here would guarantee a timeout and blame the
    // radio for it.
    if (!m_running || !m_linkUp)
        return false;

    Hl2ControlRequest::Request r;
    r.addr = addr;
    r.data = data;
    // Unconditional, because the refusal above has already established that
    // subsystemRead is false. Every reachable address replies with an echo, so
    // every request spends it.
    r.echo = Hl2ControlRequest::Echo::Exact;
    return m_ccRequest.arm(r);
}

void MetisClient::publishControlVerdict()
{
    const auto reply = m_ccRequest.takeReply();
    if (!reply)
        return;
    if (reply->outcome == Hl2ControlRequest::Outcome::Answered) {
        emit controlReplyReady(reply->addr, reply->data);
    } else {
        emit controlRequestFailed(
            reply->addr, reply->outcome == Hl2ControlRequest::Outcome::Refused);
    }
}

qint64 MetisClient::controlNowMs() const noexcept
{
    return m_controlClock.isValid() ? m_controlClock.elapsed() : 0;
}

void MetisClient::tickControlRequest(qint64 nowMs)
{
    // ONE CALL PER EP6 FRAME, not per packet: the gateware opens exactly one
    // response slot per 512-byte frame (usopenhpsdr1.v, SYNC_RESP), so this is
    // the same clock the radio answers on. The wall clock rides alongside it
    // because a frame is not a fixed amount of time — see Hl2ControlRequest's
    // class note for the rate table.
    m_ccRequest.onEp6Frame(nowMs);
    publishControlVerdict();
}

void MetisClient::ingestControlResponse(const Ep6Response& resp)
{
    m_ccRequest.onResponse(resp);
    publishControlVerdict();
}

void MetisClient::setMox(bool keyed, const TxCoordinator::Operation& operation)
{
    setMoxImpl(keyed, operation, false);
}

void MetisClient::setCwMox(bool keyed, const TxCoordinator::Operation& operation)
{
    setMoxImpl(keyed, operation, true);
}

void MetisClient::setMoxImpl(bool keyed, const TxCoordinator::Operation& operation, bool cwBreakIn)
{
    if (!TxCoordinator::Command{operation, keyed}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    const bool was = m_mox;
    if (keyed && !m_txAllowed) {
        // Fail SAFE and stay refused. Not an error return: a caller that could
        // retry past a refusal is exactly what this gate exists to prevent.
        keyed = false;
    }
    m_mox = keyed;
    m_moxOperation = keyed
        ? (cwBreakIn ? operation.heldCwKeying() : operation.heldKeying()) : operation;
    // main 2c693d1c: the bandscope transmit interlock runs on real key edges
    // only. The operation fence above is set first and unconditionally — a
    // repeat key from a NEW producer must re-anchor the fence even though the
    // wire state did not change.
    if (m_mox == was)
        return;

    // THE BANDSCOPE'S TRANSMIT INTERLOCK, on the edges of the one member that
    // is the final authority for keying on the wire (buildNextControlPacket
    // reads exactly this, ANDed with the gate). The HL2 receives while it
    // transmits and hears its own PA at enormous strength, so a block taken
    // under MOX is a picture of us, at a level with no relation to the band.
    if (m_mox) {
        // Abandon whatever cycle was in flight rather than let it finish: the
        // packets still to come would be transmit-contaminated, and half a
        // clean block merged with half a keyed one is worse than no block.
        // expectTrailing matches onBandscopeGuardTimeout's rule rather than
        // passing an unconditional true: a cycle still ARMING has seen no EP4
        // packet, so there is nothing in the WIDE states to flush, and a flag
        // set here would swallow the first packet of the next cycle instead.
        // Keying up inside the arming window is exactly when that happens.
        if (m_bsState != BandscopeState::Idle)
            bandscopeDisarm(/*expectTrailing=*/m_bsState != BandscopeState::Arming);
        // A one-shot caller has no period tick to resume this capture. Answer
        // before leaving its guard stopped and its request latched forever.
        failPendingBandscopeFrame(QStringLiteral("the radio started transmitting"));
    } else {
        // Start the post-unkey hold-off. d83's measured transient runs
        // 178-285 ms past the falling edge; kBandscopeUnkeyHoldoffMs clears it.
        m_sinceUnkey.restart();
    }
}

void MetisClient::setTxFrequencyHz(std::uint32_t hz)
{
    // Logged for the same reason as the RX banks above, and it matters more
    // here: the transmit oscillator is written by exactly one caller and never
    // echoed anywhere, so "did transmit follow" has had no answer short of
    // keying up and listening.
    qCDebug(lcHl2) << "HL2: TX NCO <-" << hz << "Hz (commanded)";
    m_ccTxFreq = ccTxFreq(hz);
    queueOneShotIfRunning(m_ccTxFreq);
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
    // The ATU request is re-asserted, not re-decided: it shares this register,
    // so rebuilding the bank without it would clear a tune in progress.
    m_ccTxDrive = ccTxDrive(level, level > 0, m_atuTune);
    queueOneShotIfRunning(m_ccTxDrive);
}

void MetisClient::setCwKeyDown(bool down, const TxCoordinator::Operation& operation)
{
    if (!TxCoordinator::Command{operation, down}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    // Refuse the carrier at the same final wire authority that refuses MOX.
    // Do not even latch a pending down edge: opening the gate later must never
    // turn an earlier refused request into RF.
    if (down && !m_txAllowed) {
        return;
    }
    if (!down && !m_cwMode) {
        return;
    }
    if (down && !m_cwMode) {
        // CW owns the IQ stream until PTT drops. Voice already queued behind a
        // manual MOX must not leak into the spaces between elements. Abandoned,
        // not dropped -- uncounted for the same reason as clearCwKeying().
        m_txIq.clear();
        m_cwEnvelope = 0.0;
        // ...but a starvation in progress ended at this instant, not at the
        // unkey that eventually follows.
        reportTxUnderflowRun();
    }
    m_cwMode = true;
    m_cwKeyDown = down;
    // The engine suppresses a global up while another CW contributor holds
    // down. Retain that compatible set after checking this original command.
    m_cwOperation = down ? operation.heldCwKeying() : operation;
}

void MetisClient::clearCwKeying()
{
    m_cwMode = false;
    m_cwKeyDown = false;
    m_cwEnvelope = 0.0;
    // Audio captured while CW owned the stream is stale; dropping it keeps a CW exit
    // under held PTT from transmitting old mic audio. Not counted as overflow (that
    // counts carry-loss, not a deliberate abandon), but the underflow run ends here.
    m_txIq.clear();
    reportTxUnderflowRun();
}

void MetisClient::queueTxIq(std::span<const std::complex<float>> iq, const TxCoordinator::Context& context)
{
    if (!context.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    if (!m_txIqContext.sameContext(context)) {
        // NOT COUNTED AS OVERFLOW: a context change means the queued samples
        // belong to a transmission that is no longer the current one, so this
        // is an abandon of stale audio rather than a FIFO dropping audio it was
        // asked to carry -- see clearCwKeying() for the same distinction.
        m_txIq.clear();
        m_txIqContext = context;
    }
    for (const auto& s : iq)
        m_txIq.push_back(s);
    // Drop the OLDEST on overflow: stale transmit audio is worse than a gap.
    // COUNTED, because the drop is a discontinuity in the middle of an
    // envelope and nothing else on the wire or in the telemetry records that
    // it happened -- see txOverflowSamples().
    while (m_txIq.size() > kTxQueueMax) {
        m_txIq.pop_front();
        ++m_txOverflowSamples;
    }
}

void MetisClient::setTxTestTone(double offsetHz, double amplitude, const TxCoordinator::Operation& operation)
{
    if (!TxCoordinator::Command{operation, amplitude > 0.0}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    m_toneOperation = operation;
    m_toneHz = offsetHz;
    m_toneAmp = amplitude < 0.0 ? 0.0 : (amplitude > 1.0 ? 1.0 : amplitude);
    if (m_toneAmp == 0.0)
        m_tonePhase = 0.0;
}

void MetisClient::flushTxIq()
{
    m_txIq.clear();
    // A flush is how an over ends, so it is also where a starvation that ran
    // to the end of that over stops. Without this the run would sit unreported
    // until the NEXT transmission happened to emit a full packet, and would
    // then be attributed to it.
    reportTxUnderflowRun();
}

void MetisClient::reportTxUnderflowRun()
{
    m_txUnderflowRunQuietPackets = 0;
    if (m_txUnderflowRunPackets == 0)
        return;
    qCDebug(lcHl2Tx) << "HL2 tx fifo: HOST queue starved for"
                     << m_txUnderflowRunPackets << "EP2 packet(s),"
                     << m_txUnderflowRunSamples
                     << "sample(s) of substituted silence on the air"
                     << "(totals SINCE PROCESS START, not since this link came"
                     << "up and not per radio: underflow" << m_txUnderflowPackets
                     << "packets /" << m_txUnderflowSamples << "samples, overflow"
                     << m_txOverflowSamples << "samples)."
                     << "This is the CLIENT's queue, not the radio's DSIQ FIFO:"
                     << "the EP2 frame went out full size on schedule, so the"
                     << "gateware's fill and pacingFault readings are unaffected"
                     << "by it and cannot be used to rule it out.";
    m_txUnderflowRunPackets = 0;
    m_txUnderflowRunSamples = 0;
}

std::array<std::uint8_t, kUsbPacketSize> MetisClient::buildNextControlPacket()
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_moxOperation.permitsDispatch(now)) {
        m_mox = false;
    }
    if (!m_cwOperation.permitsDispatch(now)) {
        m_cwMode = false;
        m_cwKeyDown = false;
        m_cwEnvelope = 0.0;
    }
    if (!m_toneOperation.permitsDispatch(now)) {
        m_toneAmp = 0.0;
        m_tonePhase = 0.0;
    }
    if (!m_txIqContext.permitsDispatch(TxCoordinator::monotonicMs())) {
        m_txIq.clear();
    }
    static const Cc kCcAdc = ccAdcAssign();
    // Cleared here rather than only on confirmation: a packet built and then
    // thrown away (a test, or a caller that inspects the bytes) must not leave
    // a stale claim that some later packet carried the request.
    m_requestOnBuiltPacket = false;
    m_cl1BankOnBuiltPacket = false;
    Cc b;
    if (!m_oneShot.empty()) {
        b = m_oneShot.front();
        m_oneShot.pop_front();
        // Claimed, not yet counted: onControlPacketSent() decides whether this
        // bank actually reached the wire. Same shape as m_requestOnBuiltPacket
        // above and for the same reason — a packet built and then discarded must
        // not retire one of the twenty-four writes.
        m_cl1BankOnBuiltPacket = isCl1Bank(b);
    } else if (const auto rqst = m_ccRequest.wireBank()) {
        // AFTER the one-shots, ahead of the round robin. After, because a
        // one-shot is a write the operator asked for and a read-back that
        // overtook it would return the value from before the change. Ahead of
        // the rotation, because the rotation never ends and the request would
        // otherwise never go out.
        //
        // wireBank() is non-empty only in Queued, so this fires exactly once
        // per armed request — the RQST bit never reaches a bank the round robin
        // re-asserts, which would re-request three times a rotation for ever.
        b = *rqst;
        // Not onRequestSent() here: the deadline starts when onControlPacketSent() sees
        // the socket accept the bank, so a failed send leaves the request Queued for the
        // next EP2 frame instead of timing out unseen.
        m_requestOnBuiltPacket = true;
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
    if (keyed && m_cwMode) {
        // EVERY EXIT FROM THE QUEUED-IQ PATH REPORTS, and taking CW mid-over is
        // one of them. Without this a starvation still running when the
        // operator switches to CW sits until the NEXT full queued packet or the
        // unkey, and is then timestamped there instead of where it happened.
        reportTxUnderflowRun();
        // piHPSDR's local/PC keyer likewise transmits host-generated IQ under
        // MOX. Five milliseconds is long enough to suppress key clicks while
        // staying short against a 40 ms dit at 30 WPM. A raised cosine has zero
        // slope at both ends, unlike a linear edge.
        constexpr double kCwCarrierAmplitude = 0.5;
        constexpr int kCwRampSamples = 5 * kEp2AudioRateHz / 1000;
        constexpr double kRampStep = 1.0 / static_cast<double>(kCwRampSamples);
        constexpr double kPi = 3.14159265358979323846;
        std::vector<std::complex<float>> block(kTxSamplesPerPacket);
        for (std::complex<float>& sample : block) {
            if (m_cwKeyDown) {
                m_cwEnvelope = std::min(1.0, m_cwEnvelope + kRampStep);
            } else {
                m_cwEnvelope = std::max(0.0, m_cwEnvelope - kRampStep);
            }
            const double shaped = 0.5 - 0.5 * std::cos(kPi * m_cwEnvelope);
            sample = {static_cast<float>(kCwCarrierAmplitude * shaped), 0.0f};
        }
        ep2WriteTxIq(pkt, block);
    } else if (keyed && m_toneAmp > 0.0) {
        // Same reason as the CW arm above: TUNE pressed while keyed takes the
        // stream away from the queue, so any run in progress ends HERE.
        reportTxUnderflowRun();
        // EP2 is clocked at a fixed 48 kHz regardless of the RX sample rate.
        std::vector<std::complex<float>> block(kTxSamplesPerPacket);
        const double dphi = 2.0 * 3.14159265358979323846 * m_toneHz / kEp2AudioRateHz;
        for (int n = 0; n < kTxSamplesPerPacket; ++n) {
            // Negative sine: the HPSDR wire has the opposite handedness to the
            // standard analytic convention, so this is the conjugate. Voice
            // reaches the same wire convention (TXA by its signed passband, the
            // phasing build by conjugating); a tone at a non-zero offset must
            // match it or land on the wrong side of the carrier. At TUNE's zero
            // offset handedness has no effect.
            block[static_cast<std::size_t>(n)] = {
                static_cast<float>(m_toneAmp * std::cos(m_tonePhase)),
                static_cast<float>(-m_toneAmp * std::sin(m_tonePhase))};
            m_tonePhase += dphi;
        }
        // Keep the accumulator bounded without introducing a phase step.
        while (m_tonePhase > 2.0 * 3.14159265358979323846)
            m_tonePhase -= 2.0 * 3.14159265358979323846;
        ep2WriteTxIq(pkt, block);
    } else if (keyed) {
        // Just `keyed`: the arms above take CW and positive tone amplitude, and a NaN
        // m_toneAmp (setTxTestTone's clamp passes NaN) fails both `> 0.0` and `<= 0.0`,
        // so a narrower condition would send uncounted silence. The queued-IQ path is the
        // only one that can starve; an empty queue is counted here as a full underflow.
        std::vector<std::complex<float>> block;
        const std::size_t n = std::min<std::size_t>(kTxSamplesPerPacket, m_txIq.size());
        block.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            block.push_back(m_txIq.front());
            m_txIq.pop_front();
        }
        if (n < static_cast<std::size_t>(kTxSamplesPerPacket)) {
            // Count the SILENCE, not the samples: the figure that matters is
            // how much of this envelope the radio was handed as zeros.
            const auto shortBy = static_cast<std::uint64_t>(kTxSamplesPerPacket) - n;
            ++m_txUnderflowPackets;
            m_txUnderflowSamples += shortBy;
            ++m_txUnderflowRunPackets;
            m_txUnderflowRunSamples += shortBy;
            // Still starving: whatever gap was accumulating was not a recovery.
            m_txUnderflowRunQuietPackets = 0;
        } else if (m_txUnderflowRunPackets > 0) {
            // A full packet does NOT end the run on its own -- see
            // kUnderflowRunQuietPackets. It only starts the clock on the gap
            // that will.
            if (++m_txUnderflowRunQuietPackets >= kUnderflowRunQuietPackets)
                reportTxUnderflowRun();
        }
        if (n > 0)
            ep2WriteTxIq(pkt, block);   // a short block leaves the rest as silence
        // n == 0 needs no write at all: ep2Packet already zero-filled the
        // payload, which is the same bytes ep2WriteTxIq would produce for an
        // empty span. Skipping it keeps the wire identical to before this
        // change -- the counters observe, they do not alter.
    } else {
        // UNKEYED. Silence here is the design, not a fault, so nothing is
        // counted -- but the key having gone up is also the end of any
        // starvation that was still running when it did, and the pacer reaches
        // this branch within one EP2 interval of the unkey. That makes this
        // the backstop for every way an over can end, including the paths that
        // clear m_txIq without going through flushTxIq().
        reportTxUnderflowRun();
    }

    // Audio slot. Outside the IQ branches because speaker audio plays on receive.
    // Gated on a declared codec, never on queued audio: on a bare HL2 these four
    // bytes are EADDR, so a sample there is a register command.
    if (m_params.hasCodec && !m_speakerAudio.empty()) {
        // Whole stereo frames only. An odd count would swap the channels of
        // everything after it, and the swap would carry into the next packet.
        const std::size_t want = static_cast<std::size_t>(kTxSamplesPerPacket) * 2;
        const std::size_t take = std::min(want, m_speakerAudio.size()) & ~std::size_t{1};
        if (take > 0) {
            std::vector<std::int16_t> block(m_speakerAudio.begin(),
                                            m_speakerAudio.begin()
                                                + static_cast<std::ptrdiff_t>(take));
            m_speakerAudio.erase(m_speakerAudio.begin(),
                                 m_speakerAudio.begin()
                                     + static_cast<std::ptrdiff_t>(take));
            ep2WriteTxAudio(pkt, block);
        }
    }
    return pkt;
}

void MetisClient::onControlPacketSent(qint64 bytesWritten, qint64 nowMs) noexcept
{
    // Consumed either way: the claim belongs to the packet just built, and a
    // send that failed must not leave it standing for the next one.
    const bool carriedRequest = m_requestOnBuiltPacket;
    m_requestOnBuiltPacket = false;
    if (carriedRequest && bytesWritten > 0)
        m_ccRequest.onRequestSent(nowMs);
    // Only a complete OFF sequence releases recovery. A session interrupted
    // earlier leaves the record for the next transport to resend the whole table.
    const bool carriedCl1 = m_cl1BankOnBuiltPacket;
    m_cl1BankOnBuiltPacket = false;
    if (carriedCl1 && bytesWritten > 0 && m_cl1BanksUnsent > 0) {
        if (--m_cl1BanksUnsent == 0 && m_cl1SequenceIsOff) {
            Cl1RecoveryRegistry& registry = cl1RecoveryRegistry();
            const std::lock_guard lock(registry.mutex);
            registry.serials.remove(m_cl1SequenceRadio);
            qCInfo(lcHl2) << "HL2: CL1 off sequence complete —"
                          << m_cl1SequenceRadio << "is back on its crystal";
        }
    }
}

void MetisClient::sendControlPacket()
{
    if (!m_socket && !m_packetSinkForTest) {
        return;
    }
    // Sub-frame 0 always carries the config bank (sample rate + receiver count)
    // so the DDC configuration is re-asserted on every frame; sub-frame 1
    // alternates the remaining banks. Matches the reference client, which pairs a
    // constant config bank with an alternating frequency bank.
    // The ADC-assignment bank joins the alternation: a conforming openHPSDR
    // device leaves every receiver unassigned (and therefore emits all-zero IQ)
    // until it has seen it. Re-asserting it rather than sending it once keeps a
    // device that reconnects or resets mid-session from silently going quiet.
    TxCoordinator::Dispatch audioDispatch;
    // Count the writer through sendTo(), including CW/TUNE packets which have
    // no queued PCM. Cancellation cannot retract an already-entered write.
    TxCoordinator::Dispatch keyDispatch = m_moxOperation.beginDispatch(
        TxCoordinator::monotonicMs(), m_mox);
    if (m_mox && !keyDispatch) {
        m_mox = false;
    }
    if (!m_txIq.empty()) {
        audioDispatch = m_txIqContext.beginDispatch(TxCoordinator::monotonicMs());
        if (!audioDispatch) {
            m_txIq.clear();
        }
    }
    const auto packet = buildNextControlPacket();
    const qint64 written = m_packetSinkForTest ? m_packetSinkForTest(packet)
                                              : sendTo(*m_socket, packet, m_host, m_port);
    countTx(written);
    // AFTER the write, and taking its return value: this is the seam where a
    // RQST bank stops being something we intend to send and becomes something
    // the radio has been given a chance to answer.
    onControlPacketSent(written, controlNowMs());
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
    // Taken here, at the one place the counters leave this thread, so no path
    // that clears the intent — stop(), the silence watchdog, the give-up in
    // onBandscopeGuardTimeout — can forget to update it.
    m_link.bandscopeEnabled = m_params.bandscope;

    emit linkCountersUpdated(m_link);

    // The gap figures describe the window that just closed, so the window is
    // reset here. The cumulative byte/packet totals are NOT — those are session
    // figures and the consumer diffs them itself.
    m_linkWindowClock.restart();
    m_linkWindowWakeups = 0;
    m_linkWindowGapSumUs = 0;
    m_linkWindowGapMaxUs = 0;
}

// One datagram off this socket, whatever endpoint it came from.
//
// Split out of onReadyRead's drain loop so the ingest path can be exercised
// WITHOUT a socket: MetisClientTestAccess::feedDatagram hands it recorded bytes
// directly, which is how the bandscope's sequence accounting is proved against
// a real radio's arrivals in a test that binds nothing. The drain loop keeps
// everything that genuinely needs the QNetworkDatagram — the destination
// address it latches the local endpoint from.
void MetisClient::handleDatagram(std::span<const std::uint8_t> bytes)
{
    // Between a receiver-count restart's stop and its start, everything that
    // arrives was sent in the old payload layout. Discard it undecoded and
    // uncounted, as the drain at the restart's Start step does.
    if (restartAwaitingStart()) {
        ++m_restartStalePackets;
        return;
    }

    // Counted before the EP6 test: these bytes crossed the wire and were
    // read off this socket whatever they turned out to be, and a receive
    // total that silently omits traffic is worse than one that includes a
    // stray discovery reply.
    m_link.rxBytes += static_cast<quint64>(bytes.size());

    const auto seq = ep6Seq(bytes);
    if (!seq) {
        // Not EP6. The bandscope (EP4) shares this address and port (usopenhpsdr1.v's
        // WIDE1 and UDP1 both use run_destination_port). Tested second so the EP6 hot
        // path is unchanged.
        if (const auto bsSeq = ep4Seq(bytes))
            handleEp4(*bsSeq, bytes);
        return;     // not an EP6 packet (e.g. a stray discovery reply)
    }
    ++m_link.rxPackets;

    // Restarted on every packet: this is the one piece of state that says how
    // recently the stream produced anything, which both the silence watchdog
    // and the start-retry read.
    m_sinceLastEp6.restart();
    if (m_silenceRecoveryArmed) {
        // The stream is back and m_linkUp never dropped, so nothing downstream
        // saw this happen. The counters are the only record that it did, which
        // is the point: a recovery that is invisible to the operator must not
        // also be invisible to whoever asks later why the audio had a hole in
        // it. They ride LinkCounters. Hl2Backend mirrors linkCountersUpdated
        // onto the health rows; the support bundle does not carry those rows.
        // The attempt itself was published when it was armed — onReadyRead is
        // not what delivers a recovery that never gets another datagram.
        m_silenceRecoveryArmed = false;
        ++m_link.silenceRecoveriesCompleted;
        qInfo() << "MetisClient: EP6 resumed after a silence recovery ("
                << m_link.silenceRecoveriesCompleted << "of"
                << m_link.silenceRecoveryAttempts
                << "attempts recovered) — no teardown was needed";
        // GIVE THE BANDSCOPE BACK. Stage 1 stopped the gate so that its run
        // command was the only one on the wire; the operator's intent survived
        // in m_params.bandscope, and this is where it is honoured again. Same
        // call setReceiverCount() makes after its own restart, and for the same
        // reason -- the run byte has been through a known state and the gate
        // has to re-arm from scratch. A no-op when the bandscope was never on.
        applyBandscopeGate();
    }
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
        // Loss accounting excludes rewinds and duplicates, but their samples
        // are still delivered below. Every discontinuity must invalidate the
        // partial FFT before that delivery. Zero means no forward loss.
        emit rxSequenceGap(gap < 0x80000000u ? gap : 0);
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
            if (resp->ack) {
                // An ACK's raddr is a COMMAND address and its data is our
                // own echo. Feeding that to Hl2Telemetry would invent a
                // firmware version and a FIFO depth out of bytes we sent;
                // Hl2Telemetry::apply refuses it too, belt and braces.
                ingestControlResponse(*resp);
            } else {
                const bool wasRadioPtt = m_telemetry.ptt;
                m_telemetry.apply(*resp);
                if (m_telemetry.ptt != wasRadioPtt)
                    onRadioPttEdge(m_telemetry.ptt);
                telemetryChanged = true;
                // Read back from apply() rather than re-decoding DATA[24], so the layout has one
                // decoder; apply() writes adcOverload only for response address 0. Non-ACK only:
                // an ACK's raddr is a command address.
                if (resp->raddr == 0x00 && m_telemetry.adcOverload) {
                    ++m_adcWindowSamples;
                    if (*m_telemetry.adcOverload)
                        ++m_adcWindowOverload;
                }
            }
        }
        // One response slot per FRAME, so the RQST deadline advances here
        // and not once per packet. AFTER the parse, so a reply that lands
        // on the deadline frame is an answer and not a timeout, and ticked
        // even when the frame carries no parseable C&C — a frame that
        // arrived is a slot that passed.
        tickControlRequest(controlNowMs());
    }
    // Coalesce to ~10 Hz: telemetry free-runs continuously, so a frame
    // skipped by the throttle is superseded within the interval and the
    // meters never miss a settled value. (#4449 review)
    if (telemetryChanged
        && (!m_telemetryEmitClock.isValid()
            || m_telemetryEmitClock.elapsed() >= kTelemetryMinIntervalMs)) {
        // Read the window length BEFORE restarting: this is the
        // denominator's denominator, and a consumer that assumed 100 ms
        // would be wrong on the first window after a start and on any
        // window the I/O thread was late for.
        m_telemetry.adcWindowMs = m_telemetryEmitClock.isValid()
            ? static_cast<int>(m_telemetryEmitClock.elapsed()) : 0;
        m_telemetryEmitClock.restart();
        m_telemetry.adcSamples = m_adcWindowSamples;
        m_telemetry.adcOverloadSamples = m_adcWindowOverload;
        m_adcWindowSamples = 0;
        m_adcWindowOverload = 0;
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

void MetisClient::onReadyRead()
{
    accountReceiveWakeup();

    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
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

        handleDatagram(asBytes(dg.data()));
    }

    // AFTER the drain, not before it. The gap sample belongs to the wakeup and is
    // taken at the top, but the byte and packet totals only become true once the
    // datagrams behind this wakeup have been counted — publishing first shipped a
    // snapshot that was one full drain out of date.
    publishLinkCountersIfDue();
}

void MetisClient::handleEp4(std::uint32_t seq, std::span<const std::uint8_t> bytes)
{
    ++m_link.ep4Packets;

    // Hoisted out of the branch below because the GATE needs it too, not just
    // the counters: a loss of a multiple of kEp4PacketsPerBlock leaves
    // `seq % 4` unchanged, so the phase test alone cannot see it.
    std::uint32_t stepDrops = 0;
    if (m_haveEp4Seq) {
        // ep4SeqStep (MetisProtocol) scores a backward jump as a reset, not a loss. The
        // gateware forces ep4_seq_no's low two bits to zero while the capture FIFO fills,
        // so every stream starts 0, 1, 2, 0; a forward gap there reads as ~1M lost.
        const Ep4SeqStep step = ep4SeqStep(m_expectedEp4Seq, seq);
        stepDrops = step.drops;
        m_ep4Drops += step.drops;
        if (step.rewind)
            ++m_ep4Rewinds;
    }
    // Masked, so the counter's 20-bit wrap (about 46 minutes at the measured
    // packet rate) is an ordinary step of one and not a gap of a million.
    m_expectedEp4Seq = (seq + 1) & (kEp4SeqModulus - 1);
    m_haveEp4Seq = true;

    m_link.ep4Drops = m_ep4Drops;
    m_link.ep4Rewinds = m_ep4Rewinds;

    // The counters above describe the WIRE and are kept for every datagram
    // whatever the gate is doing. Whether this packet becomes part of a READING
    // is a separate question, and the gate answers it.
    bandscopeOnPacket(seq, stepDrops, bytes);
}

// ---------------------------------------------------------------------------
// The duty-cycle gate
// ---------------------------------------------------------------------------

void MetisClient::bandscopeOnPacket(std::uint32_t seq, std::uint32_t drops,
                                    std::span<const std::uint8_t> bytes)
{
    // The trailing packet first and unconditionally: one datagram arrives 24-61 us
    // after every disable (usopenhpsdr1.v's WIDE1..WIDE4 cannot be interrupted) and
    // belongs to the block already emitted. Not a drop: ep4_seq_no is continuous.
    if (m_bsTrailingPending) {
        m_bsTrailingPending = false;
        return;
    }

    // The phase the gateware puts on a block boundary. §1.4 of the protocol
    // study: `seq % 4 == 0` marks the first packet of a 2048-sample block.
    const int phase = static_cast<int>(seq % static_cast<std::uint32_t>(kEp4PacketsPerBlock));

    switch (m_bsState) {
    case BandscopeState::Idle:
        return;

    case BandscopeState::Arming:
        // Wait for phase 0: a mid-stream re-enable does not re-align ep4_seq_no (only
        // `~run` zeroes it), so the first packet may be the tail of a block from seconds
        // earlier. Sequence continuity is not temporal continuity.
        if (phase != 0)
            return;
        m_bsState = BandscopeState::Flushing;
        m_bsPhase = 1;      // this packet is the stale block's first
        return;

    case BandscopeState::Flushing:
        // The block already in the 2048-word capture FIFO when wide_spectrum
        // went up. Its samples predate the enable by an unknown amount, so it
        // is consumed and thrown away — accumulating nothing is the point of
        // this state.
        // `drops` and not only the phase: PHASE IS MODULO FOUR, so a loss of
        // exactly four — or eight, or twelve — leaves it intact and this test
        // alone would accept a packet from a later capture as the next one.
        if (drops != 0 || phase != m_bsPhase) {
            // A packet lost mid-flush breaks the phase. Go back to waiting for
            // a boundary rather than guessing; the guard bounds how long that
            // can go on.
            m_bsState = BandscopeState::Arming;
            return;
        }
        if (++m_bsPhase == kEp4PacketsPerBlock) {
            m_bsState = BandscopeState::Capturing;
            m_bsPhase = 0;
            m_bsBlock = Ep4Stats{};
            // Latched HERE and nowhere else: a request that arrives mid-block
            // must not produce a record made of the packets that happened to
            // be left, and a vector that is short by a packet is exactly the
            // half-record the phase check above exists to refuse.
            m_bsCaptureSamples = m_bsFrameRequested;
            m_bsSamples.clear();
        }
        return;

    case BandscopeState::Capturing:
        // AGAIN `drops`, and here it is the whole guarantee. seq % 4 survives
        // a loss of any multiple of four, so the phase test on its own emitted
        // a block built from packets 4, 5, 10 and 11 — two hardware captures
        // ~10 ms apart with a hole between them — published as one contiguous
        // 2048-sample record, while ep4Drops had already counted the four that
        // went missing. (PR #5650 review round 3; hl2_ep4_gate_test section 15.)
        if (drops != 0 || phase != m_bsPhase) {
            // Four CONSECUTIVE in-phase packets or none. A gap here would make
            // the 2048 samples span two hardware blocks with a hole between
            // them, and Ep4Stats has no way to say so.
            m_bsState = BandscopeState::Arming;
            return;
        }
        if (const auto s = ep4Stats(bytes))
            m_bsBlock.merge(*s);
        // Only for a cycle somebody asked a picture of. ep4Samples decodes 512
        // codes; the gated sampler that feeds the headroom rows needs none of
        // them and must not pay for them.
        if (m_bsCaptureSamples)
            ep4Samples(bytes, m_bsSamples);
        if (++m_bsPhase < kEp4PacketsPerBlock)
            return;

        // A COMPLETE BLOCK: 2048 contiguous converter samples.
        ++m_bsBlocks;
        // The RUN of timeouts ends here, not the total: m_bsTimeouts is the
        // session's history and is what the health row reports.
        m_bsConsecutiveTimeouts = 0;
        m_link.bandscopeBlocks = m_bsBlocks;
        emit bandscopeBlockReady(m_bsBlock);
        const bool deliverFrame = m_bsFrameRequested && m_bsCaptureSamples
                                  && m_bsSamples.size()
                                         == static_cast<std::size_t>(kEp4BlockSamples);
        const bool retryFrame = m_bsFrameRequested && !deliverFrame;
        // Down again immediately. The duty cycle is the whole point: one block
        // per kBandscopeSampleMs is TWELVE datagrams a second against 381 —
        // 3 discarded in Arming, 4 flushed, 4 kept, 1 trailing. See the count's
        // derivation at setBandscopeEnabled's header in MetisClient.h.
        bandscopeDisarm(/*expectTrailing=*/true);
        if (deliverFrame) {
            m_bsFrameRequested = false;
            m_bsCaptureSamples = false;
            emit bandscopeFrameReady(
                QList<float>(m_bsSamples.begin(), m_bsSamples.end()));
        } else if (retryFrame) {
            // The request landed after this cycle had already started
            // capturing, so its samples were never decoded. One more cycle
            // serves it, and only one: m_bsCaptureSamples is latched from
            // m_bsFrameRequested at Capturing entry, which is now necessarily
            // true. bandscopeArm() can still refuse on the transmit interlock,
            // and a refusal leaves the state Idle — which is the request's
            // answer, not a wedge.
            bandscopeArm();
            if (m_bsState == BandscopeState::Idle) {
                m_bsFrameRequested = false;
                emit bandscopeFrameFailed(
                    QStringLiteral("the radio is transmitting"));
            }
        }
        return;
    }
}

bool MetisClient::bandscopeInterlocked() const noexcept
{
    if (m_mox)
        return true;
    // THE RADIO'S OWN KEYING, which m_mox never sees. m_mox is the final
    // authority for keying THIS CLIENT initiated; it is not the final
    // authority for whether the PA is on the air. ptt_resp is
    // `cw_on | ext_ptt` (see Hl2Response::ptt), so a PTT jack, a foot switch
    // or the radio's internal keyer transmits without m_mox ever becoming
    // true -- and a block taken then is still a picture of us at a level with
    // no relation to the band. The bit is already decoded on this object and
    // already published as the "PTT (radio)" health row; it just was not
    // consulted here. (PR #5650 review round 3.)
    if (m_telemetry.ptt)
        return true;
    // Invalid until the first unkey of the session, which is the right default:
    // a session that has never transmitted has no transient to wait out.
    return m_sinceUnkey.isValid() && m_sinceUnkey.elapsed() < kBandscopeUnkeyHoldoffMs;
}

// The radio keyed or unkeyed itself. Same two jobs setMox does on its own
// edges, and for the same reasons -- abandon a cycle that would be
// transmit-contaminated, and start the post-unkey hold-off -- but driven by
// ptt_resp rather than by anything this client asked for.
void MetisClient::onRadioPttEdge(bool keyed)
{
    if (keyed) {
        if (m_bsState != BandscopeState::Idle)
            bandscopeDisarm(/*expectTrailing=*/m_bsState != BandscopeState::Arming);
        failPendingBandscopeFrame(QStringLiteral("the radio started transmitting"));
        return;
    }
    // d83's measured post-unkey transient runs 178-285 ms past the falling
    // edge whoever caused it, so the hold-off is the same one setMox starts.
    // Not conditioned on m_mox: if the host is still keyed the interlock holds
    // on m_mox anyway, and this timer only starts mattering once it drops.
    m_sinceUnkey.restart();
}

void MetisClient::sendBandscopeRunByte(bool wideSpectrum)
{
    // Composed and recorded before the socket test so socket-free tests
    // (hl2_ep4_gate_test) can assert the gate's arguments; metisRunCommand()'s bits
    // are pinned in hl2_metis_protocol_test.
    const auto cmd = metisRunCommand(wideSpectrum, m_watchdogEnabled);
    // Every run byte has bit 0 set. Inside a receiver-count restart's
    // stop-to-start window that is a metis-start ahead of the priming, so it
    // is held back, and not recorded: the record means "what went on the
    // wire". The restart's Start step resets the gate, Finish re-applies it.
    if (restartAwaitingStart())
        return;
    m_lastBandscopeRunByte = cmd[3];
    if (!hasCommandTransport())
        return;
    // The run byte is a bit field and `run` must STAY set: this goes out while
    // already streaming, where re-asserting bit 0 is a no-op in the gateware's
    // RUNSTOP decode but clearing it would stop the IQ the operator is
    // listening to. The composition lives in metisRunCommand() rather than here
    // precisely because of that: expressed inline it was unreachable by any
    // socket-free test, and the only assertion possible was one that re-derived
    // the expression and agreed with itself.
    countTx(sendCommandDatagram(cmd));
}

int MetisClient::bandscopeGuardIntervalMs() const noexcept
{
    return bandscopeGuardMs(sampleRateHz(m_params.sampleRate), effectiveNumRx());
}

void MetisClient::bandscopeArm()
{
    // Refused silently, and only on this tick: the operator's standing intent
    // is untouched, so the sensor resumes on its own once the transmission and
    // its transient are over. Nothing here is an error to report.
    if (bandscopeInterlocked())
        return;

    m_bsState = BandscopeState::Arming;
    m_bsPhase = 0;
    m_bsBlock = Ep4Stats{};
    sendBandscopeRunByte(true);
    // Recomputed per cycle rather than cached, because both of its inputs — the
    // sample rate and the receiver count — can change between cycles, and a
    // guard sized for the previous rate is exactly the failure it exists to
    // prevent.
    if (m_bandscopeGuard)
        m_bandscopeGuard->start(bandscopeGuardIntervalMs());
}

void MetisClient::bandscopeDisarm(bool expectTrailing)
{
    if (m_bandscopeGuard)
        m_bandscopeGuard->stop();
    m_bsState = BandscopeState::Idle;
    m_bsPhase = 0;
    sendBandscopeRunByte(false);
    // Only when packets were actually flowing. A cycle abandoned before the
    // first EP4 arrived has no packet in the WIDE states to flush out, and a
    // flag set then would swallow the first packet of the NEXT cycle instead.
    m_bsTrailingPending = expectTrailing;
}

void MetisClient::onBandscopeTick()
{
    if (!m_running || !m_params.bandscope)
        return;
    // A cycle still in flight when the next period arrives means the guard is
    // about to fire; do not start a second one on top of it.
    if (m_bsState != BandscopeState::Idle)
        return;
    bandscopeArm();
}

void MetisClient::onBandscopeGuardTimeout()
{
    if (m_bsState == BandscopeState::Idle)
        return;
    ++m_bsTimeouts;
    ++m_bsConsecutiveTimeouts;
    m_link.bandscopeTimeouts = m_bsTimeouts;
    // A cycle that never saw a packet has no trailing packet coming. One that
    // reached Flushing or Capturing does.
    bandscopeDisarm(/*expectTrailing=*/m_bsState != BandscopeState::Arming);

    // GIVE UP ON A RADIO THAT DOES NOT HAVE THIS ENDPOINT, and only on that
    // radio. The condition is deliberately narrow: a long run of timeouts AND
    // not one EP4 datagram in the whole session. A lossy link, a busy gateware
    // or a receiver-count change all produce timeouts with packets flowing, and
    // none of them should stop a sensor the operator asked for. Gateware that
    // does not implement endpoint 0x04 produces timeouts with the packet count
    // still at zero, forever -- one re-arm, two run-byte datagrams and one log
    // line every second for the life of the session, with nothing giving up.
    // (PR #5650 review round 3.)
    if (m_bsConsecutiveTimeouts >= kMaxConsecutiveBandscopeTimeouts
        && m_link.ep4Packets == 0) {
        qCWarning(lcHl2)
            << "HL2: no EP4 datagram in" << m_bsConsecutiveTimeouts
            << "bandscope cycles — this gateware does not answer endpoint 0x04."
            << "Stopping the gate; re-enable it to try again.";
        m_params.bandscope = false;
        applyBandscopeGate();   // takes the !bandscope branch: stops both timers
        // AND THE REQUESTER IS STILL OWED AN ANSWER. applyBandscopeGate()
        // reaches resetBandscopeGate(), which is noexcept and deliberately
        // leaves the request flag alone; giving up on the endpoint is the one
        // abandoning path that never retries, so a frame request left pending
        // here would never be answered by anything.
        failPendingBandscopeFrame(
            QStringLiteral("this gateware does not answer endpoint 0x04"));
        return;
    }
    qCWarning(lcHl2) << "HL2: bandscope block did not complete within"
                     << bandscopeGuardIntervalMs() << "ms; abandoning this cycle";
    failPendingBandscopeFrame(
        QStringLiteral("no bandscope block arrived within %1 ms")
            .arg(bandscopeGuardIntervalMs()));}

void MetisClient::resetBandscopeGate() noexcept
{
    if (m_bandscopeTimer)
        m_bandscopeTimer->stop();
    if (m_bandscopeGuard)
        m_bandscopeGuard->stop();
    m_bsState = BandscopeState::Idle;
    m_bsPhase = 0;
    m_bsBlock = Ep4Stats{};
    // The request flag itself is NOT cleared here — this is noexcept and cannot
    // emit, so the answer is owed by failPendingBandscopeFrame() at each of the
    // callers below. Only the per-cycle latch and its buffer go.
    m_bsCaptureSamples = false;
    m_bsSamples.clear();
    // The stream this belonged to is gone; nothing is still in the WIDE states.
    m_bsTrailingPending = false;
}

void MetisClient::applyBandscopeGate()
{
    if (!m_running || !m_params.bandscope) {
        resetBandscopeGate();
        return;
    }
    if (m_bandscopeTimer)
        m_bandscopeTimer->start(kBandscopeSampleMs);
    // Take the first sample NOW rather than a second from now. This is also the
    // path taken straight after setReceiverCount()'s restart, where the run
    // byte has just been through 0x00 — the case bandscopeGuardMs()'s
    // EP6-packet term exists for.
    if (m_bsState == BandscopeState::Idle)
        bandscopeArm();
}

void MetisClient::failPendingBandscopeFrame(const QString& reason)
{
    if (!m_bsFrameRequested)
        return;
    m_bsFrameRequested = false;
    m_bsCaptureSamples = false;
    emit bandscopeFrameFailed(reason);
}

void MetisClient::requestBandscopeFrame()
{
    if (!m_running) {
        // Same reason setBandscopeEnabled() refuses: the run byte means nothing
        // to a radio that was never started. Answered rather than dropped, so a
        // caller waiting on a reply is not left waiting for the session.
        emit bandscopeFrameFailed(QStringLiteral("the radio is not streaming"));
        return;
    }
    if (m_bsFrameRequested)
        return;   // an answer is already on its way to whoever asked first
    if (bandscopeInterlocked()) {
        // The HL2 receives while it transmits and hears its own PA at enormous
        // strength, so a block taken now is a picture of us. Refused with a
        // reason, unlike the gated sampler's silent skip: that one resumes by
        // itself on the next tick, and a request has no next tick.
        emit bandscopeFrameFailed(QStringLiteral("the radio is transmitting"));
        return;
    }
    m_bsFrameRequested = true;
    // A cycle already in flight is left alone and will serve this request on
    // the retry the completion path takes; arming a second one on top of it
    // would put two wide_spectrum edges on the wire for one picture.
    if (m_bsState == BandscopeState::Idle)
        bandscopeArm();
}

void MetisClient::setBandscopeEnabled(bool on)
{
    if (!m_running) {
        // The run byte means nothing to a radio that was never started, and
        // start() brings wide_spectrum up clear — so a request latched here
        // would report a sensor that nothing had enabled.
        return;
    }
    if (on == m_params.bandscope)
        return;
    m_params.bandscope = on;
    // The flag bandscopeDisarm sets here does NOT survive the next line:
    // applyBandscopeGate() takes the !m_params.bandscope branch into
    // resetBandscopeGate(), which clears it. That is harmless only because the
    // trailing packet then lands in `case Idle` and is dropped there anyway —
    // so do not read this as the flag doing work on this path. Left as the
    // sibling calls spell it rather than passed false, because the argument
    // states the condition truthfully and it is the reset, not the caller,
    // that makes it moot.
    if (!on)
        bandscopeDisarm(/*expectTrailing=*/m_bsState != BandscopeState::Idle);
    applyBandscopeGate();
    if (!on) {
        failPendingBandscopeFrame(QStringLiteral("the bandscope sampler was disabled"));
    }
    qCInfo(lcHl2) << "HL2: wideband bandscope gate (EP4)"
                  << (on ? "running" : "stopped") << "— one block per"
                  << kBandscopeSampleMs << "ms";
}

}  // namespace AetherSDR::hl2
