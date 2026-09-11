#pragma once

#include "core/backends/anan/P2Protocol.h"

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

// Owns the ANAN-G2 UDP wire (openHPSDR Ethernet Protocol 2): session setup
// (General + DDC-Specific + High Priority with run=1), the keepalive that
// keeps the radio in RUN state, and DDC0 IQ ingest into normalized blocks.
// Below the seam; the future AnanBackend owns one P2Client plus an AnanRxDsp.
//
// Lives on AnanBackend's dedicated I/O thread, not the GUI thread -- the
// same shape MetisClient uses for the Hermes-Lite 2, and for the same
// reason: this class is what keeps the radio's own watchdog fed, so it must
// not be at the mercy of a GUI stall.
//
// RX-ONLY: there is no PTT parameter anywhere in this class, because there
// is none in P2Protocol::buildHighPriority() -- the capability to key does
// not exist yet, not merely a guard that could be bypassed. TX is RFC §2.11
// Phase 3, a separate future addition.
//
// Does NOT redo discovery to IDENTIFY the radio -- by the time start() is
// called, a caller has already resolved a host to connect to via
// AnanDiscovery's broadcast sweep or a manual probe, the same division of
// responsibility MetisClient::start() uses.
//
// It DOES send its own Discovery packet as the first thing start() does,
// for an entirely different, ANAN-specific reason that has no Protocol-1
// analogue: Phase 1a measured every radio->PC stream (DDC0 IQ included)
// arriving at "the Source Port of the Host that initiated the Discovery
// Packet" (spec p.43, p.51, p.54), literally -- not at whatever port later
// sent the General/DDC-Specific/High-Priority session-setup packets. A
// caller's manual-connect probe (ConnectionPanel::probeAnan()) already sent
// one, but from ITS OWN throwaway socket, which is closed by the time this
// object opens a new one -- so without repeating the Discovery send here,
// on THIS socket, the radio has no reason to route DDC0 IQ anywhere this
// object is listening. Sending it is not WAITED for -- session setup
// proceeds immediately after -- but a reply that does arrive on this same
// socket (see discoveryInfoReceived()) is opportunistically parsed for the
// gateware version / DDC count / board id, since nothing else in this
// class's own session ever learns those otherwise.
class P2Client : public QObject {
    Q_OBJECT

public:
    explicit P2Client(QObject* parent = nullptr);
    ~P2Client() override;

    struct Params {
        QString host;
        // Valid rates: 48/96/192/384/768/1536 (spec p.24). Not validated here
        // -- P2Protocol::buildDdcSpecific() is the single place that would
        // reject or clamp one, and it does neither yet; an invalid value is
        // simply sent as-is and the radio's own reaction is the feedback.
        int ddc0RateKsps = 48;
        // Connect-time-only hardware options -- not operator controls an
        // engineering bench session changes mid-QSO, so unlike frequency
        // there is no live setter for any of these; a change takes a
        // reconnect. See buildDdcSpecific()/buildHighPriority()'s own
        // comments for the exact spec citations.
        bool ditherEnabled = true;
        bool randomEnabled = true;
        int ddc0AdcIndex = 0;          // 0 = ADC0, 1 = ADC1/RX2
        bool bypassAdc0Filters = true;
        bool bypassAdc1Filters = true;

        // Multi-DDC session. EMPTY (the default) means "single DDC0
        // session", built from ddc0RateKsps/ddc0AdcIndex above -- so every
        // existing caller keeps the exact bench-validated single-DDC
        // behaviour without naming this field at all.
        //
        // When non-empty this is authoritative and the two shorthand fields
        // are ignored: entry n configures DDC n. Capped at
        // P2Protocol's kMaxDdcs by the packet builders.
        //
        // Deliberately not merged into the shorthand fields: a caller that
        // sets BOTH would otherwise have two disagreeing sources of truth
        // for DDC0's rate, and silently picking one is exactly the kind of
        // thing that reads as a radio fault on the bench.
        std::vector<DdcConfig> activeDdcs;
    };

    // start()/stop() and setDdc0FrequencyHz() MUST execute on this object's
    // own thread: start() constructs the QUdpSocket, and a socket takes the
    // affinity of the thread that creates it. They are Q_INVOKABLE so a
    // future AnanBackend's I/O thread can marshal them the way Hl2Backend
    // does for MetisClient.
    // connectTimeoutMs overrides kConnectTimeoutMs's default -- see
    // AnanBackend::beginRateChange()'s own comment for why a rate-change
    // restart (start() called on a session the radio was JUST told to stop,
    // possibly after configure() held this thread for seconds of FFTW
    // planning) legitimately needs more grace than a first-ever connect: the
    // radio has real re-settling work to do that a fresh connect does not.
    Q_INVOKABLE bool start(const Params& params, int connectTimeoutMs = kConnectTimeoutMs);
    Q_INVOKABLE void stop();

    // Retune DDC0. No frequency is set by start() itself (see the class
    // comment on connectRadio()/setSliceFrequency() being separate seam
    // calls) -- a caller retunes immediately after a successful start if it
    // wants a specific frequency, the same connect-then-tune shape used
    // above this seam. Takes effect immediately (not on the next keepalive
    // tick) and is what the keepalive resends from then on.
    Q_INVOKABLE void setDdc0FrequencyHz(double hz);

    // Change one DDC's sample rate on a LIVE session -- no stop, no restart,
    // no reconnect. Returns false (sending nothing) if the session is not
    // running or ddcIndex is not one this session enabled. A matching rate
    // still sends: the packet is fire-and-forget UDP and the caller retries
    // across the settle window, so skipping an unchanged slot would drop
    // those retransmits.
    //
    // Supported by the radio, verified in p2app's own source rather than
    // assumed: IncomingDDCSpecific.c services DDC-Specific packets in a
    // continuous thread loop and, on any change, calls
    // WriteP2DDCRateRegister(), which is a direct
    // RegisterWrite(VADDRDDCRATES, ...) to the FPGA. Its companion
    // "something changed" hook, HandlerCheckDDCSettings(), is an EMPTY
    // function -- p2app writes the rate register and keeps streaming. There
    // is no teardown on the radio side to mirror.
    //
    // This answers the question AnanBackend::beginRateChange()'s own comment
    // left open ("whether the radio would accept a live rate change without
    // a session restart at all is a separate, unverified protocol
    // question"). The caller still has to rebuild ITS OWN WdspChannel, whose
    // input sample rate really did change -- that part is unavoidable and is
    // why AnanBackend builds the new channel in the background first.
    //
    // Resends the whole DDC-Specific packet, not a partial one: the packet
    // carries the enable bitmap and every DDC's row, so a partial resend
    // would disable the others.
    Q_INVOKABLE bool setDdcRateLive(int ddcIndex, int rateKsps);

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }

signals:
    void linkUp();       // first valid DDC0 frame seen
    void linkDown();     // stop() called, or a link failure once that exists
    // No DDC0 frame arrived within the connect-timeout window -- the radio
    // is off, unreachable, or the port never saw a genuine DDC0 frame (only
    // Mic Data / Status traffic, which onReadyRead() rejects and does not
    // count as a connection).
    void connectionError(const QString& reason);
    // DDC0's decoded IQ. Kept as its own signal because DDC0 is the one
    // receiver every session always has, and it is what linkUp()/the connect
    // timeout are keyed on. Emitted for ddcIndex 0 only; ddcIqReady() below
    // fires for the same block as well.
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    // Decoded IQ for ANY active DDC, demultiplexed by the datagram's sender
    // port (see P2Protocol::ddcIndexForSenderPort()). This is the general
    // form; a multi-receiver consumer routes on ddcIndex rather than
    // connecting per-DDC signals.
    void ddcIqReady(int ddcIndex, const std::vector<std::complex<float>>& block);
    void dropsUpdated(quint64 totalDrops);
    // This session's own Discovery reply -- the SAME radio start() already
    // sent a Discovery packet to, on this socket, per the class comment.
    // Emitted at most once per start(), whenever it happens to arrive
    // (before or after linkUp() -- no ordering relative to it). AnanBackend
    // uses this to report real capabilities instead of hardcoded ones; see
    // its capabilities() and the DiscoveryReply fields' own comments in
    // P2Protocol.h for what each means.
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 numDdc);

private slots:
    void onReadyRead();
    void onKeepaliveTick();
    void onConnectTimeout();

private:
    // p.8: "a Command & Control packet must be sent at least every second
    // (every 100 mS is recommended). Should a C&C packet not be received,
    // and the hardware is in the RUN state, then the hardware will switch
    // out of the RUN state into standby." Matches anan/spike/phase1a.py's
    // CC_KEEPALIVE_INTERVAL, the exact cadence already validated against
    // real hardware -- without it the radio's own watchdog (which
    // buildGeneral() leaves enabled on purpose) drops the session
    // mid-stream, which looks exactly like a radio anomaly and is not one.
    // That is precisely what the spike's first run did before this fix
    // existed.
    static constexpr int kKeepaliveMs = 100;
    // No packet ever arrived from a genuine DDC0 frame within this long of
    // start() -- mirrors MetisClient's kConnectTimeoutMs.
    static constexpr int kConnectTimeoutMs = 2000;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_keepaliveTimer = nullptr;
    QTimer* m_connectTimeoutTimer = nullptr;
    // Whatever start() was actually called with -- onConnectTimeout()'s
    // message reports this, not kConnectTimeoutMs, so the number an operator
    // sees always matches the window that was really used.
    int m_activeConnectTimeoutMs = kConnectTimeoutMs;
    // Guards discoveryInfoReceived() to at most once per start() -- a stray
    // reply to someone ELSE's broadcast discovery landing on this socket
    // (unlikely, but this parses unauthenticated UDP) must not re-fire it
    // repeatedly for the life of the session.
    bool m_discoveryInfoSent = false;

    QHostAddress m_host;
    std::uint32_t m_ddc0FreqWord = 0;   // last value sent; the keepalive resends this
    // Set once from Params at start(); every buildHighPriority() call for
    // the rest of this session (the keepalive tick included) carries these
    // same values, since there is no live setter for either (see Params'
    // own comment on why these are connect-time-only).
    bool m_bypassAdc0Filters = true;
    bool m_bypassAdc1Filters = true;

    // The session's resolved DDC list and the dither/random flags that went
    // with it, RETAINED (rather than consumed and dropped in start()) so
    // setDdcRateLive() can rebuild a complete DDC-Specific packet: that
    // packet carries every DDC's row plus the enable bitmap, so resending it
    // with only the changed rate known would disable every other DDC.
    std::vector<DdcConfig> m_activeDdcs;
    bool m_ditherEnabled = true;
    bool m_randomEnabled = true;

    bool m_running = false;
    bool m_linkUp = false;

    // Sequence-gap detection is PER DDC: every DDC runs its own independent
    // sequence counter on its own socket, so a single shared "expected next"
    // would report a gap on every alternating frame the moment a second DDC
    // started streaming -- a fault indication manufactured entirely by the
    // client. Indexed by ddcIndex; nullopt until that DDC's first frame.
    std::array<std::optional<std::uint32_t>, kMaxDdcs> m_expectedSeq{};
    // Drops stay a single session-wide counter (what dropsUpdated() has
    // always reported) -- an operator watching for a lossy link cares that
    // the session is dropping, not which receiver.
    quint64 m_drops = 0;

    // How many DDCs this session enabled, so onReadyRead() knows which
    // sender ports belong to it. Kept alongside m_activeDdcs (rather than
    // read from its size) because onReadyRead() consults it per datagram.
    int m_activeDdcCount = 1;

    // One shared phase word per active DDC. Every High Priority sender uses
    // this, so the keepalive (100 ms) can never contradict what start() sent:
    // a single-DDC overload on that path would pin DDC1..N-1 at word 0
    // regardless. Per-DDC tuning has no setter yet, so one word for all of
    // them is the honest encoding -- see start()'s own comment.
    [[nodiscard]] std::vector<std::uint32_t> sharedFreqWords() const
    {
        return std::vector<std::uint32_t>(
            static_cast<std::size_t>(m_activeDdcCount), m_ddc0FreqWord);
    }
    // Sender ports already warned about, so a port mismatch -- which by
    // nature affects every packet -- logs once per distinct port rather than
    // per datagram. Session-scoped; cleared with the rest of the state.
    QSet<quint16> m_warnedUnexpectedPorts;

    // Reused decode buffer, cleared and refilled per frame rather than
    // reallocated -- matches MetisClient's m_blocks for the same reason.
    // Shared across DDCs deliberately: onReadyRead() fully consumes each
    // block (the emit is a same-thread direct connection) before touching
    // the next datagram, so there is never more than one live at a time.
    std::vector<std::complex<float>> m_decodeScratch;
};

}  // namespace AetherSDR::anan
