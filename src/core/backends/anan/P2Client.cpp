#include "core/backends/anan/P2Client.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include <span>

Q_LOGGING_CATEGORY(lcAnanP2, "aether.anan.p2")

namespace AetherSDR::anan {

namespace {

// View a QByteArray as a byte span for the protocol decoders. Same helper
// MetisClient.cpp defines locally for the same purpose.
std::span<const std::uint8_t> asBytes(const QByteArray& d) noexcept
{
    return {reinterpret_cast<const std::uint8_t*>(d.constData()),
           static_cast<std::size_t>(d.size())};
}

template <std::size_t N>
qint64 sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
             const QHostAddress& host, quint16 port)
{
    return s.writeDatagram(reinterpret_cast<const char*>(buf.data()),
                           static_cast<qint64>(N), host, port);
}

}  // namespace

P2Client::P2Client(QObject* parent) : QObject(parent)
{
    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(kKeepaliveMs);
    connect(m_keepaliveTimer, &QTimer::timeout, this, &P2Client::onKeepaliveTick);

    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &P2Client::onConnectTimeout);
}

P2Client::~P2Client()
{
    stop();
}

bool P2Client::start(const Params& params, int connectTimeoutMs)
{
    if (m_running)
        stop();

    m_host = QHostAddress(params.host);
    if (m_host.isNull())
        return false;   // not a parseable IPv4/IPv6 literal -- fail fast, no hostname lookup here

    m_ddc0FreqWord = 0;
    m_bypassAdc0Filters = params.bypassAdc0Filters;
    m_bypassAdc1Filters = params.bypassAdc1Filters;
    // Every DDC's sequence tracker, not just DDC0's -- a stale expectation
    // carried across a restart would report a phantom gap on the new
    // session's first frame.
    m_expectedSeq.fill(std::nullopt);
    m_activeDdcCount = 1;   // real value set once the DDC list is resolved below
    m_drops = 0;
    m_warnedUnexpectedPorts.clear();
    m_linkUp = false;
    m_discoveryInfoSent = false;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &P2Client::onReadyRead);

    m_running = true;

    // No artificial delay between these -- MetisClient::start() sends its
    // own startup sequence back-to-back too, and nothing in the spike's
    // proven session flow needed one either.
    //
    // Discovery FIRST, from this socket -- see the class comment for why:
    // it is the source port of THIS packet that decides where the radio
    // sends DDC0 IQ, not the source port of General/DDC-Specific/High-
    // Priority below. The reply (if any arrives here) is not parsed;
    // onReadyRead() already drops anything that isn't DDC0-shaped.
    sendTo(*m_socket, buildDiscovery(), m_host, kRadioPort);
    sendTo(*m_socket, buildGeneral(), m_host, kRadioPort);

    // Resolve the session's DDC list ONCE: either the caller's explicit
    // multi-DDC list, or a one-element list from the DDC0 shorthand. See
    // Params::activeDdcs for why these are not merged.
    std::vector<DdcConfig> ddcs = params.activeDdcs;
    if (ddcs.empty())
        ddcs.push_back(DdcConfig{params.ddc0RateKsps, params.ddc0AdcIndex});
    if (static_cast<int>(ddcs.size()) > kMaxDdcs)
        ddcs.resize(kMaxDdcs);
    m_activeDdcCount = static_cast<int>(ddcs.size());
    // Retained for setDdcRateLive() -- see its own comment for why the whole
    // list (not just the count) has to survive start().
    m_activeDdcs = ddcs;
    m_ditherEnabled = params.ditherEnabled;
    m_randomEnabled = params.randomEnabled;

    // Every DDC starts at the same frequency: m_ddc0FreqWord, which start()
    // zeroed above, so in practice baseband. Per-DDC tuning is a seam this
    // class does not expose yet -- setDdc0FrequencyHz() moves DDC0 only --
    // so sending one shared word is honest about what is actually
    // controllable rather than implying independent tuning that has no
    // setter behind it.
    //
    // The two other High Priority senders (setDdc0FrequencyHz() and
    // onKeepaliveTick()) send the SAME shared word for the same count, so
    // they cannot disagree with this packet. That matters because the
    // keepalive fires every 100 ms: a single-DDC overload there would pin
    // DDC1..N-1 at word 0 forever regardless of what this line sent, and the
    // two would diverge permanently the moment DDC0 was retuned.
    // (aethersdr-agent, #5547 review.)
    std::vector<std::uint32_t> freqWords(ddcs.size(), m_ddc0FreqWord);

    // Destination ports below are NOT interchangeable with kRadioPort -- see
    // kDdcSpecificPort/kHighPriorityPort's comment. p2app tells these two
    // packet types apart by which port they arrive on.
    sendTo(*m_socket,
          buildDdcSpecific(ddcs, /*numAdcs=*/2,
                           params.ditherEnabled, params.randomEnabled),
          m_host, kDdcSpecificPort);
    sendTo(*m_socket,
          buildHighPriority(true, freqWords, m_bypassAdc0Filters, m_bypassAdc1Filters),
          m_host, kHighPriorityPort);

    m_keepaliveTimer->start();
    m_activeConnectTimeoutMs = connectTimeoutMs;
    m_connectTimeoutTimer->start(connectTimeoutMs);
    return true;
}

void P2Client::stop()
{
    m_keepaliveTimer->stop();
    m_connectTimeoutTimer->stop();
    if (m_socket) {
        // The clean-stop packet, mirroring metisStop(): run=0 reaches the
        // radio before the socket that would carry any further keepalive
        // goes away.
        sendTo(*m_socket,
              buildHighPriority(false, 0, m_bypassAdc0Filters, m_bypassAdc1Filters),
              m_host, kHighPriorityPort);
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

void P2Client::setDdc0FrequencyHz(double hz)
{
    m_ddc0FreqWord = phaseWord(hz);
    if (m_running && m_socket)
        sendTo(*m_socket,
              buildHighPriority(true, sharedFreqWords(),
                                m_bypassAdc0Filters, m_bypassAdc1Filters),
              m_host, kHighPriorityPort);
}

bool P2Client::setDdcRateLive(int ddcIndex, int rateKsps)
{
    if (!m_running || !m_socket)
        return false;
    if (ddcIndex < 0 || ddcIndex >= static_cast<int>(m_activeDdcs.size()))
        return false;

    auto& slot = m_activeDdcs[static_cast<std::size_t>(ddcIndex)];
    const bool rateChanged = slot.rateKsps != rateKsps;
    slot.rateKsps = rateKsps;
    // Whole packet, every DDC's row -- see this function's declaration
    // comment. Same destination port start() used: p2app tells DDC-Specific
    // from High Priority by which port it arrives on, so this is not
    // interchangeable with kRadioPort. Sent even when rateChanged is false:
    // AnanBackend retries this call at 60/140 ms for UDP loss, and those
    // retries must not no-op after the first write (#5547).
    sendTo(*m_socket,
          buildDdcSpecific(m_activeDdcs, /*numAdcs=*/2,
                           m_ditherEnabled, m_randomEnabled),
          m_host, kDdcSpecificPort);

    // The stream's sample cadence changes underneath us from here, so the
    // sequence expectation for THIS DDC is no longer meaningful -- clear it
    // rather than let the next frame look like a gap and inflate the drop
    // counter for what is a deliberate, operator-initiated change. Retries
    // of the same rate leave the tracker alone: the cadence did not change
    // again.
    if (rateChanged) {
        m_expectedSeq[static_cast<std::size_t>(ddcIndex)].reset();
    }
    return true;
}

void P2Client::onKeepaliveTick()
{
    if (!m_running || !m_socket)
        return;
    // This IS the "any C&C packet" the watchdog needs (p.8) -- no need to
    // also replay General/DDC-Specific on this cadence, same as the spike.
    sendTo(*m_socket,
          buildHighPriority(true, sharedFreqWords(),
                            m_bypassAdc0Filters, m_bypassAdc1Filters),
          m_host, kHighPriorityPort);
}

void P2Client::onConnectTimeout()
{
    if (!m_linkUp)
        emit connectionError(QStringLiteral(
            "no DDC0 IQ from the radio within %1 ms of start").arg(m_activeConnectTimeoutMs));
}

void P2Client::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const auto frame = parseDdcFrame(asBytes(dg.data()));
        if (!frame) {
            // Not DDC0-shaped -- Mic Data or High Priority Status sharing
            // this port, exactly as measured in Phase 1a, OR the reply to
            // THIS session's own Discovery send (class comment). Try that
            // second, cheap parse before giving up on the datagram; neither
            // outcome is a drop or a connection attempt.
            if (!m_discoveryInfoSent) {
                if (const auto reply = parseDiscoveryReply(asBytes(dg.data()))) {
                    m_discoveryInfoSent = true;
                    emit discoveryInfoReceived(reply->boardId, reply->firmwareVer,
                                               reply->numDdc);
                }
            }
            continue;
        }

        // Which DDC sent this. The IQ packet carries no DDC index of its
        // own, so the sender port is the only discriminator -- see
        // ddcIndexForSenderPort()'s comment for how that is verified.
        const auto ddcIndex = ddcIndexForSenderPort(
            static_cast<std::uint16_t>(dg.senderPort()), m_activeDdcCount);
        if (!ddcIndex) {
            // DDC-shaped, but from a port this session did not enable --
            // another client's stream to this host, or a DDC left running by
            // a previous session. Dropping it is right: attributing it to a
            // DDC would corrupt that receiver's audio and its sequence
            // tracking, and counting it as a drop would blame this session
            // for someone else's traffic.
            //
            // "Not a drop" must not mean "not observable", though. If the
            // radio's source port ever differs from basePort + n -- other
            // firmware, a NAT or relay in the path, a future negotiated-port
            // session -- then EVERY datagram lands here and the operator sees
            // a dead receiver whose only diagnostic is onConnectTimeout()'s
            // "no DDC0 IQ from the radio", which reads as a radio fault. One
            // line naming the port turns that into a diagnosis. Logged once
            // per distinct port per session: this is in the hot receive path
            // and a mismatch is by nature every packet.
            if (!m_warnedUnexpectedPorts.contains(dg.senderPort())) {
                m_warnedUnexpectedPorts.insert(dg.senderPort());
                qCWarning(lcAnanP2).nospace()
                    << "ANAN: dropping DDC-shaped datagram from unexpected "
                       "sender port " << dg.senderPort() << " (expected "
                    << kDdc0DefaultPort << ".."
                    << (kDdc0DefaultPort + m_activeDdcCount - 1)
                    << " for " << m_activeDdcCount
                    << " active DDC(s)) -- not counted as a drop";
            }
            continue;
        }
        const std::size_t slot = static_cast<std::size_t>(*ddcIndex);

        // Per-DDC gap detection; see m_expectedSeq's own comment for why a
        // shared counter would manufacture drops once a second DDC streams.
        if (m_expectedSeq[slot] && frame->seq != *m_expectedSeq[slot]) {
            ++m_drops;
            emit dropsUpdated(m_drops);
        }
        m_expectedSeq[slot] = frame->seq + 1;

        // linkUp stays keyed on DDC0: it is the receiver every session has,
        // and the connect timeout's message says "no DDC0 IQ". A session
        // whose DDC1 streamed but whose DDC0 never did is a real failure,
        // not a connected session.
        if (!m_linkUp && *ddcIndex == 0) {
            m_linkUp = true;
            m_connectTimeoutTimer->stop();
            emit linkUp();
        }

        m_decodeScratch.clear();
        decodeIq(*frame, m_decodeScratch);
        emit ddcIqReady(*ddcIndex, m_decodeScratch);
        if (*ddcIndex == 0)
            emit ddc0IqReady(m_decodeScratch);
    }
}

}  // namespace AetherSDR::anan
