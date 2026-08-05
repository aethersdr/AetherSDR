#include "core/backends/icom/IcomSession.h"

#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

namespace AetherSDR::icom {

// The RS-BA1 handshake is a multi-step chain with no error packet for most of
// its failure modes — a radio that refuses at any step simply stops answering.
// Without these lines the whole sequence is a black box that either works or
// silently does not, which is exactly what happened on first contact with real
// hardware. Off by default; enable with QT_LOGGING_RULES="aether.icom*=true".
Q_LOGGING_CATEGORY(lcIcom, "aether.icom.session")

namespace {

// How often the transmit packetiser is drained. A 20 ms frame is produced every
// 20 ms, so pumping at 10 ms keeps latency below one frame without spinning.
constexpr int kTxPumpMs = 10;

// A partial CI-V frame older than this is abandoned. Without it, one truncated
// frame swallows every subsequent byte and the radio appears to stop answering
// while the link is demonstrably fine.
constexpr int kCivFrameTimeoutMs = 100;

std::span<const std::uint8_t> asSpan(const QByteArray& b)
{
    return {reinterpret_cast<const std::uint8_t*>(b.constData()),
            static_cast<std::size_t>(b.size())};
}

}  // namespace

IcomSession::IcomSession(QObject* parent) : QObject(parent) {}

IcomSession::~IcomSession() { stop(); }

bool IcomSession::start(const Params& params)
{
    stop();
    m_params = params;
    m_tx = TxPacketizer(params.codec);
    m_rx = RxAssembler(params.codec);

    if (!codecSupported(params.codec)) {
        // Refusing here rather than at decode time is deliberate: a codec we
        // cannot decode, fed through the LPCM path, is full-scale noise into
        // the operator's headphones.
        fail(QStringLiteral("audio codec %1 is not supported by this client")
                 .arg(static_cast<int>(params.codec)));
        return false;
    }

    // Bind the media sockets FIRST, without handshaking. The control stream's
    // request has to announce their local ports, and it is sent before either
    // may start — so we have to know the ports by then.
    m_serial = new IcomStream(this);
    m_audio  = new IcomStream(this);
    IcomStream::Config serialCfg{params.host, params.serialPort, 0, IcomStream::Role::Serial};
    IcomStream::Config audioCfg{params.host, params.audioPort, 0, IcomStream::Role::Audio};
    if (!m_serial->bindOnly(serialCfg) || !m_audio->bindOnly(audioCfg)) {
        fail(QStringLiteral("cannot bind local UDP sockets for the CI-V and audio streams"));
        return false;
    }

    connect(m_serial, &IcomStream::ready, this, &IcomSession::onSerialReady);
    connect(m_serial, &IcomStream::payloadReady, this, &IcomSession::onSerialPayload);
    connect(m_serial, &IcomStream::failed, this, &IcomSession::fail);
    connect(m_audio, &IcomStream::ready, this, &IcomSession::onAudioReady);
    connect(m_audio, &IcomStream::payloadReady, this, &IcomSession::onAudioPayload);
    connect(m_audio, &IcomStream::failed, this, &IcomSession::fail);
    // LOSS IS CONCEALED, not merely counted.
    //
    // This used to forward the count and stop there, and nothing downstream
    // connected to audioLost — so when the reorder buffer gave up and skipped
    // forward, the decoded stream simply got shorter by that many packets and
    // the receive timeline WALKED. RxAssembler::concealLoss() existed for
    // exactly this and was reachable only from its own unit test, which read
    // as implemented while doing nothing.
    //
    // Emitting the fill through the same audioReady path keeps the timeline
    // continuous; the signal still goes out so a consumer can count the gaps.
    connect(m_audio, &IcomStream::packetsLost, this, [this](int packets) {
        emit audioLost(packets);
        if (packets <= 0)
            return;
        auto fill = m_rx.concealLoss(packets, m_params.sampleRateHz);
        if (!fill.empty())
            emit audioReady(fill);
    });

    m_control = new IcomStream(this);
    connect(m_control, &IcomStream::ready, this, &IcomSession::onControlReady);
    connect(m_control, &IcomStream::payloadReady, this, &IcomSession::onControlPayload);
    connect(m_control, &IcomStream::failed, this, &IcomSession::fail);

    IcomStream::Config controlCfg{params.host, params.controlPort, 0, IcomStream::Role::Control};
    return m_control->start(controlCfg);
}

void IcomSession::stop()
{
    for (QTimer** t : {&m_tokenTimer, &m_txTimer, &m_civTimeout}) {
        if (*t) {
            (*t)->stop();
            (*t)->deleteLater();
            *t = nullptr;
        }
    }
    // DEAUTHENTICATE BEFORE TEARING DOWN.
    //
    // A per-stream disconnect (type 0x05) closes the STREAMS; it does not end
    // the SESSION. The radio goes on holding the authenticated session until it
    // times out, and the next login is refused — which is exactly the
    // "auth failed on reconnect" the operator hit, and why a retry a minute
    // later works. kappanhang sends auth 0x01 here for the same reason.
    //
    // Sent TWICE like every other control packet: this is the one packet whose
    // loss the radio cannot ask us to retransmit, because we stop listening
    // immediately afterwards.
    if (m_control && m_control->isReady()) {
        const auto bye = buildAuth(m_control->localSessionId(),
                                   m_control->remoteSessionId(), m_innerSeq++, m_authId,
                                   AuthKind::Deauth);
        m_control->sendRaw(bye);
        m_control->sendRaw(bye);
        // FLUSH, don't sleep.
        //
        // This was QThread::msleep(150), which was wrong twice over. It froze
        // the GUI thread for 150 ms on every disconnect — and on every
        // reconnect too, since connectRadio() calls disconnectRadio() first.
        // Worse, it could not do what it was for: sendRaw() only writes into
        // QAbstractSocket's buffer, and blocking the event loop is precisely
        // what stops Qt draining it. The deauth sat queued locally for the
        // whole wait and then left in the same flush as the disconnect packets
        // it had been carefully ordered ahead of.
        //
        // Flushing puts it on the wire immediately, which is what the wait was
        // trying to buy.
        m_control->flush();
    }

    // Control FIRST now. It owns the session the other two hang off, so the
    // deauth above must be the last thing the radio hears from us — tearing the
    // media streams down first would put two more disconnects after it.
    for (IcomStream** s : {&m_control, &m_serial, &m_audio}) {
        if (*s) {
            (*s)->stop();
            (*s)->deleteLater();
            *s = nullptr;
        }
    }
    m_authOk = false;
    m_haveRadioId = false;
    m_streamsRequested = false;
    m_connected = false;
    m_innerSeq = 0;
    m_serialSendSeq = 0;
    m_audioSendSeq = 1;
    m_civ.reset();
    m_tx.flush();
}

void IcomSession::fail(const QString& reason)
{
    if (m_failing)
        return;   // a teardown can make several streams fail at once
    if (!m_connected && reason.isEmpty())
        return;
    m_failing = true;
    qCWarning(lcIcom) << "session failed:" << reason;
    const bool was = m_connected;
    m_connected = false;
    if (was || !reason.isEmpty())
        emit disconnected(reason);

    // ACTUALLY TEAR DOWN. Emitting disconnected() and leaving the streams
    // running left three UDP sockets open to the radio after a failed connect
    // — and because an Icom serves ONE client, that held the radio's session
    // slot so every later attempt timed out with "no answer". The operator sees
    // a radio that worked once and then refuses to talk to anything.
    //
    // Deferred to the event loop because fail() is usually reached FROM a
    // stream's own signal, and stop() deletes those streams.
    QTimer::singleShot(0, this, [this] {
        stop();
        m_failing = false;
    });
}

void IcomSession::onControlReady()
{
    qCInfo(lcIcom) << "control stream ready — sending login for user"
                   << m_params.username << "(password"
                   << (m_params.password.isEmpty() ? "EMPTY)" : "supplied)");
    m_control->sendTracked(buildLogin(m_control->localSessionId(), m_control->remoteSessionId(),
                                      m_innerSeq++, 0x0000,
                                      m_params.username.toStdString(),
                                      m_params.password.toStdString()));
}

void IcomSession::onControlPayload(const QByteArray& packet)
{
    const auto pkt = asSpan(packet);

    // Dispatch on LENGTH, not on type: every one of these rides packet type
    // 0x00, and the length is the only thing that distinguishes them.
    switch (packet.size()) {
    case static_cast<qsizetype>(kLenLoginReply): {
        qCInfo(lcIcom) << "got login reply";
        AuthId id{};
        switch (parseLoginReply(pkt, id)) {
        case LoginResult::BadCredentials:
            fail(QStringLiteral("the radio rejected the username or password"));
            return;
        case LoginResult::NotALoginReply:
            return;
        case LoginResult::Ok:
            break;
        }
        m_authId = id;
        qCInfo(lcIcom) << "login accepted — sending first auth + token request";
        // Two auths, in this order. The first establishes the session and the
        // second requests the token that actually gates the media streams;
        // sending only one leaves a session that looks logged in and never
        // opens audio.
        m_control->sendTracked(buildAuth(m_control->localSessionId(),
                                         m_control->remoteSessionId(), m_innerSeq++, m_authId,
                                         AuthKind::First));
        m_control->sendTracked(buildAuth(m_control->localSessionId(),
                                         m_control->remoteSessionId(), m_innerSeq++, m_authId,
                                         AuthKind::Renew));
        return;
    }

    case static_cast<qsizetype>(kLenToken):
        qCInfo(lcIcom) << "got token/auth reply, accepted =" << isAuthAccepted(pkt)
                       << "requestreply =" << pkt[0x14] << "requesttype =" << pkt[0x15];
        if (isAuthAccepted(pkt)) {
            m_authOk = true;
            if (!m_tokenTimer) {
                // Renew comfortably inside the 60 s contract. Missing it stops
                // the media streams with NO disconnect packet and no error —
                // the operator sees audio simply stop.
                m_tokenTimer = new QTimer(this);
                connect(m_tokenTimer, &QTimer::timeout, this, &IcomSession::onTokenRenew);
                m_tokenTimer->start(kTokenRenewEarlyMs);
            }
            requestStreamsIfReady();
        }
        return;

    case static_cast<qsizetype>(kLenCapabilities):
        qCInfo(lcIcom) << "got capabilities packet";
        if (parseCapabilities(pkt, m_radioId)) {
            m_haveRadioId = true;
            m_radioName = QString::fromStdString(parseCapabilitiesName(pkt));
            requestStreamsIfReady();
        }
        return;

    case static_cast<qsizetype>(kLenStatus):
        switch (parseStatus(pkt)) {
        case StatusKind::AuthFailed:
            // ONLY FATAL BEFORE THE STREAMS ARE GRANTED.
            //
            // On a RECONNECT the radio sends this after the new session is
            // fully established — login accepted, capabilities read, streams
            // granted, token accepted, both media streams handshaking — and
            // then one 0x50 carrying the failure sentinel. It is reporting the
            // teardown of the PREVIOUS session, not a failure of this one.
            //
            // Treating it as fatal killed a working session every time, which
            // is what the "auth error on reconnect" actually was. kappanhang
            // hits the same packet and distinguishes the two cases by whether
            // its streams had opened; this is that test, applied to the grant.
            if (m_streamsRequested && m_authOk) {
                qCWarning(lcIcom) << "status reports an auth failure AFTER the streams were "
                                     "granted — treating as the previous session's teardown, "
                                     "not this one's";
                return;
            }
            fail(QStringLiteral("authentication failed — check the user name and password, "
                                "or power-cycle the radio"));
            return;
        case StatusKind::Disconnected:
            fail(QStringLiteral("the radio dropped the session"));
            return;
        default:
            return;
        }

    case static_cast<qsizetype>(kLenConnInfo): {
        if (m_connected)
            return;
        const StreamGrant grant = parseStreamGrant(pkt);
        qCInfo(lcIcom) << "got stream-request reply, granted =" << grant.granted
                       << "byte0x60 =" << static_cast<quint8>(packet.at(0x60));
        if (!grant.granted)
            return;
        m_deviceName = QString::fromStdString(grant.deviceName);
        // The grant may carry DIFFERENT session ids and a new auth id. Adopting
        // them is what keeps the 60 s renewals working; caching the login's
        // values instead authenticates correctly exactly once.
        m_authId = grant.authId;
        openMediaStreams();
        return;
    }

    default:
        // THE diagnostic that matters most on first contact: a control payload
        // whose length we do not recognise means the radio answered with
        // something this client does not model, and silently ignoring it is
        // indistinguishable from the radio saying nothing at all.
        qCWarning(lcIcom) << "UNHANDLED control payload, length" << packet.size()
                          << "first bytes" << packet.left(24).toHex(' ');
        return;
    }
}

void IcomSession::requestStreamsIfReady()
{
    if (m_streamsRequested || !m_authOk || !m_haveRadioId) {
        qCDebug(lcIcom) << "stream request not yet possible: requested="
                        << m_streamsRequested << "authOk=" << m_authOk
                        << "haveRadioId=" << m_haveRadioId;
        return;
    }
    qCInfo(lcIcom) << "requesting serial + audio streams; radio name =" << m_radioName
                   << "civ port =" << m_serial->localPort()
                   << "audio port =" << m_audio->localPort();
    m_streamsRequested = true;

    StreamRequest req;
    req.localSid  = m_control->localSessionId();
    req.remoteSid = m_control->remoteSessionId();
    req.innerSeq  = m_innerSeq++;
    req.authId    = m_authId;
    req.radioId   = m_radioId;
    req.radioName = m_radioName.toStdString();
    req.username  = m_params.username.toStdString();
    req.rxCodec   = m_params.codec;
    req.txCodec   = m_params.codec;
    req.sampleRateHz = m_params.sampleRateHz;
    // The ports we ACTUALLY bound, not the defaults. This is why the media
    // sockets are bound before the handshake runs.
    req.civLocalPort   = m_serial->localPort();
    req.audioLocalPort = m_audio->localPort();
    req.txBufferMs = m_params.txBufferMs;
    req.enableTx   = m_params.enableTx;

    m_control->sendTracked(buildStreamRequest(req));
}

void IcomSession::openMediaStreams()
{
    m_serial->beginHandshake();
    m_audio->beginHandshake();
}

void IcomSession::onTokenRenew()
{
    if (!m_control || !m_control->isReady())
        return;
    m_control->sendTracked(buildAuth(m_control->localSessionId(), m_control->remoteSessionId(),
                                     m_innerSeq++, m_authId, AuthKind::Renew));
}

void IcomSession::onSerialReady()
{
    qCInfo(lcIcom) << "serial stream ready — opening the CI-V pipe";
    // Opening the CI-V pipe is a separate step from the stream handshake. A
    // serial stream that handshakes and never opens carries keepalives forever
    // and no commands.
    m_serial->sendTracked(buildSerialOpen(m_serial->localSessionId(),
                                          m_serial->remoteSessionId(), m_serialSendSeq++, true));

    if (!m_civTimeout) {
        m_civTimeout = new QTimer(this);
        connect(m_civTimeout, &QTimer::timeout, this, &IcomSession::onCivFrameTimeout);
        m_civTimeout->start(kCivFrameTimeoutMs);
    }

    if (!m_connected) {
        m_connected = true;
        emit connected(m_deviceName.isEmpty() ? m_radioName : m_deviceName);
    }
}

void IcomSession::onSerialPayload(const QByteArray& packet)
{
    const auto payload = serialPayload(asSpan(packet));
    if (payload.empty())
        return;   // a keepalive idle, or the serial open/close echo

    for (const auto& raw : m_civ.feed(payload)) {
        auto frame = parseFrame(raw);
        if (!frame)
            continue;
        // Drop our OWN commands. CI-V is a bus protocol and the radio echoes
        // everything addressed to it straight back; treating those echoes as
        // radio state makes every command look confirmed the instant it is
        // sent, including the ones the radio goes on to reject with FA.
        if (frame->to == m_params.civAddress && frame->from == kControllerAddress)
            continue;
        emit civFrameReady(*frame);
    }
}

void IcomSession::onCivFrameTimeout()
{
    if (m_civ.framePending())
        m_civ.timeout();
}

void IcomSession::onAudioReady()
{
    qCInfo(lcIcom) << "audio stream ready";
    if (!m_txTimer && m_params.enableTx) {
        m_txTimer = new QTimer(this);
        connect(m_txTimer, &QTimer::timeout, this, &IcomSession::onTxPump);
        m_txTimer->start(kTxPumpMs);
    }
}

void IcomSession::onAudioPayload(const QByteArray& packet)
{
    const auto payload = audioPayload(asSpan(packet));
    if (payload.empty())
        return;
    auto samples = m_rx.accept(payload);
    if (!samples.empty())
        emit audioReady(samples);
}

void IcomSession::onTxPump()
{
    if (!m_audio || !m_audio->isReady())
        return;
    // Drain every frame that is ready, not just one: a host audio callback can
    // deliver several frames' worth in one block, and pacing them out one per
    // 10 ms tick would fall permanently behind.
    for (auto chunks = m_tx.takeFrame(); !chunks.empty(); chunks = m_tx.takeFrame()) {
        for (const auto& c : chunks) {
            m_audio->sendTracked(buildAudio(m_audio->localSessionId(),
                                            m_audio->remoteSessionId(), 0, m_audioSendSeq++,
                                            c.bytes));
        }
    }
}

void IcomSession::sendCiv(std::span<const std::uint8_t> frame)
{
    if (!m_serial || !m_serial->isReady())
        return;
    m_serial->sendTracked(buildSerialData(m_serial->localSessionId(),
                                          m_serial->remoteSessionId(), 0, m_serialSendSeq++,
                                          frame));
}

void IcomSession::sendAudio(std::span<const float> mono)
{
    if (!m_params.enableTx)
        return;
    m_tx.submit(mono);
}

void IcomSession::flushTxAudio() { m_tx.flush(); }

IcomSession::Stats IcomSession::stats() const
{
    Stats s;
    if (m_control) s.control = m_control->counters();
    if (m_serial)  s.serial  = m_serial->counters();
    if (m_audio)   s.audio   = m_audio->counters();
    return s;
}

}  // namespace AetherSDR::icom
