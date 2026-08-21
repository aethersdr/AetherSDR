#include "core/backends/icom/IcomSession.h"

#include <QDateTime>

#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <algorithm>

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
// Re-send the CI-V data-stream open at the reference's cadence until the radio
// starts streaming. Matches kappanhang / SDR9700 startCivDataTimer(100).
constexpr int kCivOpenRetryMs = 100;
// ~5 s of asking. Long enough for a slow radio, short enough that a radio which
// will never answer says so rather than retrying silently forever.
constexpr int kCivOpenMaxAttempts = 50;

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
    m_tokenRequestId = params.tokenRequestId != 0
        ? params.tokenRequestId
        : static_cast<quint16>(QRandomGenerator::global()->bounded(1, 0x10000));
    m_lastAuthOkMs = 0;
    m_lastRenewalSeq = 0;
    m_lastRenewalResult = QStringLiteral("not sent");
    m_lastRenewalResponse = 0;
    m_renewAcceptedCount = 0;
    m_tokenReissuedCount = 0;
    m_renewRejectedCount = 0;
    m_ignoredAuthReplies = 0;
    m_ignoredControlPackets = 0;
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
    if (params.tokenRenewalMs <= 0 || params.tokenAckGraceMs <= 0
        || params.initialMaintenanceMs <= 0
        || params.tokenDeadMs <= params.tokenRenewalMs) {
        fail(QStringLiteral("invalid RS-BA1 lease timing configuration"));
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
    for (QTimer** t : {&m_tokenTimer, &m_txTimer, &m_civTimeout, &m_civOpenRetry}) {
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
    // Send it TWICE as TRACKED packets. buildAuth() leaves the outer sequence
    // at zero for IcomStream to stamp; sending it through sendRaw() bypassed
    // that stamp, so a live IC-7300MK2 discarded the deauth behind the already
    // advanced control-stream sequence. The next login succeeded but its first
    // token request was rejected with 0xffffffff until the old lease expired.
    // Two distinct tracked sequence numbers also avoid depending on a replay
    // request we cannot serve after closing the socket.
    if (m_control && m_control->isReady()) {
        const auto bye = buildAuth(m_control->localSessionId(),
                                   m_control->remoteSessionId(), m_innerSeq++, m_authId,
                                   AuthKind::Deauth);
        m_control->sendTracked(bye);
        m_control->sendTracked(bye);
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
    m_renewUnacked = false;
    m_initialMaintenancePending = false;
    m_renewRetries = 0;
    m_pendingRenewals.clear();
    m_haveRadioId = false;
    m_streamsRequested = false;
    m_streamGranted = false;
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

void IcomSession::sendRenewal(const QString& reason)
{
    if (!m_control || !m_control->isReady()) {
        return;
    }
    const quint16 seq = m_innerSeq++;
    m_pendingRenewals.insert(seq);
    m_lastRenewalSeq = seq;
    m_lastRenewalResult = QStringLiteral("pending");
    m_lastRenewalResponse = 0;
    m_renewUnacked = true;
    qCInfo(lcIcom).noquote() << "RS-BA1 token renewal sent: seq" << seq
                             << "reason" << reason
                             << "pending" << m_pendingRenewals.size();
    m_control->sendTracked(buildAuth(m_control->localSessionId(),
                                     m_control->remoteSessionId(), seq, m_authId,
                                     AuthKind::Renew));
}

bool IcomSession::isCurrentControlPacket(std::span<const std::uint8_t> packet) const
{
    if (!m_control || packet.size() < kHeaderSize) {
        return false;
    }
    const Header header = parseHeader(packet);
    return header.sentId == m_control->remoteSessionId()
        && header.rcvdId == m_control->localSessionId();
}

void IcomSession::onControlReady()
{
    qCInfo(lcIcom) << "control stream ready — sending login for user"
                   << m_params.username << "(password"
                   << (m_params.password.isEmpty() ? "EMPTY)" : "supplied)")
                   << "token request"
                   << QStringLiteral("0x%1").arg(m_tokenRequestId, 4, 16, QLatin1Char('0'));
    m_control->sendTracked(buildLogin(m_control->localSessionId(), m_control->remoteSessionId(),
                                      m_innerSeq++, m_tokenRequestId,
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
        sendRenewal(QStringLiteral("initial token request"));
        return;
    }

    case static_cast<qsizetype>(kLenToken): {
        const AuthReply reply = parseAuthReply(pkt);
        if (reply.result == AuthReplyResult::NotRenewal) {
            return;
        }
        const bool initialToken = !m_authOk && !m_streamsRequested && !m_streamGranted;
        const bool mayRotateInitialToken = initialToken
            && reply.result == AuthReplyResult::Nonzero;
        if (!isCurrentControlPacket(pkt)
            || (reply.authId != m_authId && !mayRotateInitialToken)
            || !m_pendingRenewals.remove(reply.innerSeq)) {
            ++m_ignoredAuthReplies;
            qCWarning(lcIcom).noquote()
                << "ignoring stale or duplicate RS-BA1 token reply: seq"
                << reply.innerSeq << "response"
                << QStringLiteral("0x%1").arg(reply.response, 8, 16, QLatin1Char('0'))
                << "pending" << m_pendingRenewals.size();
            return;
        }
        m_lastRenewalSeq = reply.innerSeq;
        m_lastRenewalResponse = reply.response;
        if (reply.result == AuthReplyResult::Nonzero && !initialToken) {
            ++m_renewRejectedCount;
            m_lastRenewalResult = QStringLiteral("rejected");
            qCWarning(lcIcom).noquote()
                << "RS-BA1 token renewal rejected: seq" << reply.innerSeq
                << "response"
                << QStringLiteral("0x%1").arg(reply.response, 8, 16, QLatin1Char('0'));
            fail(QStringLiteral("the radio rejected RS-BA1 session renewal %1 (response 0x%2)")
                     .arg(reply.innerSeq)
                     .arg(reply.response, 8, 16, QLatin1Char('0')));
            return;
        }

        if (reply.result == AuthReplyResult::Nonzero) {
            // wfview follows this exact reconnect path: the radio can answer
            // the first post-login token request with 0xffffffff while
            // returning the token that must be used for the stream request.
            // The same response on an established lease is a real rejection,
            // handled above. Context, not packet shape, is the discriminator.
            m_authId = reply.authId;
            ++m_tokenReissuedCount;
            m_lastRenewalResult = QStringLiteral("reissued");
            qCWarning(lcIcom) << "RS-BA1 token reissued during reconnect: seq"
                              << reply.innerSeq;
        } else {
            ++m_renewAcceptedCount;
            m_lastRenewalResult = QStringLiteral("accepted");
            qCInfo(lcIcom) << "RS-BA1 token renewal accepted: seq" << reply.innerSeq;
        }
        m_pendingRenewals.clear();
        m_authOk = true;
        m_lastAuthOkMs = QDateTime::currentMSecsSinceEpoch();
        m_renewUnacked = false;
        m_renewRetries = 0;
        // A live immediate reconnect received a media grant that went silent
        // around 45 s even though the ordinary 60 s renewal was later accepted.
        // Renew once at 30 s after initial authentication, then use the normal
        // reference cadence for the rest of the session.
        m_initialMaintenancePending = initialToken;
        if (!m_tokenTimer) {
            m_tokenTimer = new QTimer(this);
            connect(m_tokenTimer, &QTimer::timeout, this, &IcomSession::onTokenRenew);
            // This timer is both the 60 s renewal scheduler and the watchdog
            // for a missing acknowledgement.
            m_tokenTimer->start(m_params.tokenAckGraceMs);
        }
        requestStreamsIfReady();
        return;
    }

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
            // There are two distinguishable late-teardown shapes. A previous
            // session can carry its old header IDs, but the IC-7300MK2 also
            // sends 0x50/ffffffff on the NEW control association after that
            // association's stream grant. wfview applies the same lifecycle
            // discriminator: the sentinel is fatal before streamOpened and is
            // ignored afterwards. Keep the header check as stronger evidence,
            // then preserve the proven post-grant exception even if the radio
            // stamped the status with this session's IDs.
            if (!isCurrentControlPacket(pkt)) {
                ++m_ignoredControlPackets;
                qCWarning(lcIcom) << "ignoring auth-failure status for a stale control session";
                return;
            }
            if (m_streamGranted && m_authOk) {
                ++m_ignoredControlPackets;
                qCWarning(lcIcom) << "ignoring late auth-failure status after the current "
                                     "session's stream grant";
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
        if (!m_streamsRequested || !m_authOk
            || grant.localSid != m_control->localSessionId()
            || grant.remoteSid != m_control->remoteSessionId()) {
            ++m_ignoredControlPackets;
            qCWarning(lcIcom) << "ignoring stale RS-BA1 stream grant: requested="
                              << m_streamsRequested << "authenticated=" << m_authOk
                              << "headerMatches=" << isCurrentControlPacket(pkt);
            return;
        }
        m_deviceName = QString::fromStdString(grant.deviceName);
        m_authId = grant.authId;
        m_streamGranted = true;
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
    // THE RATE INVARIANT, CHECKED WHERE IT IS ACTUALLY DECIDED.
    //
    // IcomProtocol.h's static_assert reads kAudioRateHz, a compile-time
    // constant, while the rate that reaches the wire is this runtime field. So
    // that assert cannot catch the failure its own comment describes: setting
    // sampleRateHz to 16000 still yields 1920-byte frames, still 60 ms of audio
    // per frame, and still the silent zero-power transmit that cost a live
    // session to find. The build stays green the whole way.
    //
    // Clamped rather than refused: a wrong rate here is a transmitter that
    // keys and radiates nothing, which is far worse than ignoring a request
    // the packetiser cannot honour. When the low-bandwidth work lands it must
    // re-derive the 1364/556 split, and this is the guard that will make that
    // requirement impossible to skip.
    if (m_params.sampleRateHz != kAudioRateHz) {
        qCWarning(lcIcom) << "audio sample rate" << m_params.sampleRateHz
                          << "Hz cannot be honoured — the packet split is sized for"
                          << kAudioRateHz << "Hz; clamping. Changing the rate "
                             "requires re-deriving kAudioSplitLarge/Small.";
        m_params.sampleRateHz = kAudioRateHz;
    }

    m_serial->beginHandshake();
    m_audio->beginHandshake();
}

// The token ticker, and the renewal watchdog — one timer doing both.
//
// THE FAILURE THIS EXISTS FOR. The radio requires a renewal on the 60 s
// reference cadence and eventually stops media silently after the last accepted
// token: no disconnect packet, no error, and the UDP transport keeps running
// because idle keepalives are not token-gated. So `isConnected()` stays true,
// link statistics keep climbing, and CI-V is simply dead. It reads as a radio
// that stopped talking rather than as a session that expired.
//
// wfview and kappanhang both renew once per minute. A live IC-7300MK2 stopped
// serial and audio about 90 s after an established token, but an immediate
// reconnect grant stopped around 45 s even though its 60 s renewal was later
// accepted. We renew once at 30 s, then every 60 s, retry within 3 s, and fail
// at 80 s so the session cannot sit falsely green past the observed expiry.
void IcomSession::onTokenRenew()
{
    if (!m_control || !m_control->isReady())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastAuthOkMs <= 0)
        m_lastAuthOkMs = now;

    const qint64 sinceAck = now - m_lastAuthOkMs;

    // Nothing has been acknowledged for most of the contract. Report it while
    // it is still true — a session that dies here used to be discovered minutes
    // later as "the meters stopped".
    if (sinceAck > m_params.tokenDeadMs) {
        fail(QStringLiteral(
                 "the radio stopped renewing our session token (no acknowledgement "
                 "for %1 s). The link is up but the session has expired — this is "
                 "usually a lossy or power-saving WiFi path to the radio.")
                 .arg(sinceAck / 1000));
        return;
    }

    // A renewal is outstanding: resend it rather than waiting for the next
    // interval, because the next interval may be past the expiry.
    if (m_renewUnacked) {
        if (m_renewRetries < kTokenRenewMaxRetries) {
            ++m_renewRetries;
            qCWarning(lcIcom) << "token renewal unacknowledged after"
                              << m_params.tokenAckGraceMs << "ms — resend" << m_renewRetries
                              << "of" << kTokenRenewMaxRetries;
            sendRenewal(QStringLiteral("acknowledgement retry %1")
                            .arg(m_renewRetries));
            return;
        }
        // RELEASE THE LATCH. Retries are exhausted for THIS renewal, but the
        // token is alive until tokenDeadMs — so falling through to the cadence
        // below starts a fresh one with a fresh budget, which is effectively
        // continuous retry up to the dead-session deadline.
        //
        // Leaving it latched meant nothing was ever sent again: the cadence
        // branch could not run, and the session coasted into a guaranteed
        // teardown. A 10 s outage at t=20 s would lose the four resends and
        // then sit silent from t=30 s to the fail() at t=50 s — a full
        // disconnect for an outage that had been over for twenty seconds.
        m_renewUnacked = false;
        m_renewRetries = 0;
    }

    // A reconnect grant can have a shorter first window than an established
    // lease. Cover it once, then return to the wfview/kappanhang 60 s cadence.
    const int cadenceMs = m_initialMaintenancePending
        ? std::min(m_params.tokenRenewalMs, m_params.initialMaintenanceMs)
        : m_params.tokenRenewalMs;
    if (sinceAck < cadenceMs)
        return;
    m_renewRetries = 0;
    sendRenewal(QStringLiteral("scheduled"));
}

void IcomSession::onSerialReady()
{
    qCInfo(lcIcom) << "serial stream ready — opening the CI-V pipe";
    // Opening the CI-V pipe is a separate step from the stream handshake. A
    // serial stream that handshakes and never opens carries keepalives forever
    // and no commands.
    m_serial->sendTracked(buildSerialOpen(m_serial->localSessionId(),
                                          m_serial->remoteSessionId(), m_serialSendSeq++, true));

    // ⛔ ONE OPEN IS NOT ENOUGH. Observed on a live IC-9700 2026-08-05: the
    // radio accepts the open, reports the pipe ready, and then streams nothing
    // — not one CI-V frame in 45 s. Every consequence is downstream and silent:
    // no 0x19 0x00 reply, so the model never resolves; no model, so scope and
    // transmit stay disabled and no dBm range is published; no range, so the pan
    // auto-ranges into a runaway MainWindow rejects once a second. The operator
    // sees a blank frequency and a waterfall that keeps resetting.
    //
    // kappanhang and the SDR9700 reference both re-send the open on a 100 ms
    // timer until data flows (their startCivDataTimer), and Aether-gate does the
    // same driving THIS radio — 1356 frames in 45 s, 30.1 fps. An IC-705 that
    // happens to start on the first open would never expose this.
    m_civDataSeen = false;
    m_civOpenAttempts = 0;
    if (!m_civOpenRetry) {
        m_civOpenRetry = new QTimer(this);
        connect(m_civOpenRetry, &QTimer::timeout, this, [this]() {
            if (m_civDataSeen || !m_serial) {
                m_civOpenRetry->stop();
                return;
            }
            // Bounded. The reference retries indefinitely, but it is a headless
            // bridge; here an unbounded 10 Hz stream of opens at a radio that is
            // never going to answer is just noise that hides the real fault. Say
            // so once and stop — a silent forever-retry is how "it just does not
            // work" becomes unreportable.
            if (++m_civOpenAttempts > kCivOpenMaxAttempts) {
                m_civOpenRetry->stop();
                qCWarning(lcIcom)
                    << "CI-V stream never started after" << kCivOpenMaxAttempts
                    << "open attempts — the radio accepted the open and sent no data."
                    << "Check CI-V is enabled for the network port on the radio.";
                return;
            }
            m_serial->sendTracked(buildSerialOpen(m_serial->localSessionId(),
                                                  m_serial->remoteSessionId(),
                                                  m_serialSendSeq++, true));
        });
    }
    m_civOpenRetry->start(kCivOpenRetryMs);

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

    // First real CI-V payload: the stream is live, so stop asking it to open.
    // The reference stops its timer on exactly this condition rather than after
    // a fixed count, because a slow radio must not be abandoned early.
    if (!m_civDataSeen) {
        m_civDataSeen = true;
        if (m_civOpenRetry)
            m_civOpenRetry->stop();
        qCInfo(lcIcom) << "CI-V stream live — open-retry stopped";
    }

    for (const auto& raw : m_civ.feed(payload)) {
        auto frame = parseFrame(raw);
        if (!frame)
            continue;
        // Drop our OWN commands. CI-V is a bus protocol and the radio echoes
        // everything addressed to it straight back; treating those echoes as
        // radio state makes every command look confirmed the instant it is
        // sent, including the ones the radio goes on to reject with FA.
        //
        // `from` ALONE is the test, because a frame we sent is never radio
        // state — whoever it happened to be addressed to. This used to also
        // require `to` to be our own address or the 0x00 broadcast, which was
        // true of every frame we send right up until setCivAddress() made our
        // address movable mid-session: after a CI-V retarget the echoes of
        // frames still in flight at the OLD address match neither arm and get
        // handed up as though the radio had spoken.
        //
        // Today's traffic through that window is read-only, so those echoes
        // carry no data byte and are rejected downstream anyway — but
        // applyScopeStartup() sends 0x27 WITH a payload, so the class is not
        // structurally empty, and an invariant that merely happens to hold is
        // the thing this filter was written not to depend on.
        //
        // The broadcast case is what first made the `to` test insufficient: a
        // 0x19 0x00 sent to 0x00 — the address query that needs no prior
        // knowledge of who is out there — echoes back as to=0x00, from=0xE0.
        // Measured on an IC-9700 and an IC-705 on 2026-08-14: the echo always
        // arrives first, ahead of the real reply.
        //
        // A radio answering transceive to 0x00 is a real frame and still gets
        // through, because its `from` is the radio's address, not ours.
        if (frame->from == kControllerAddress)
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

QVariantMap IcomSession::leaseDiagnostics() const
{
    QVariantMap out;
    const qint64 ageMs = m_lastAuthOkMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_lastAuthOkMs : -1;
    out.insert(QStringLiteral("authenticated"), m_authOk);
    out.insert(QStringLiteral("connected"), m_connected);
    out.insert(QStringLiteral("streamsRequested"), m_streamsRequested);
    out.insert(QStringLiteral("streamGranted"), m_streamGranted);
    out.insert(QStringLiteral("nextInnerSequence"), m_innerSeq);
    out.insert(QStringLiteral("tokenRequestId"),
               QStringLiteral("0x%1").arg(m_tokenRequestId, 4, 16, QLatin1Char('0')));
    out.insert(QStringLiteral("lastRenewalSequence"), m_lastRenewalSeq);
    out.insert(QStringLiteral("lastRenewalResult"), m_lastRenewalResult);
    out.insert(QStringLiteral("lastRenewalResponse"),
               QStringLiteral("0x%1").arg(m_lastRenewalResponse, 8, 16, QLatin1Char('0')));
    out.insert(QStringLiteral("lastAcceptedAgeMs"), ageMs);
    out.insert(QStringLiteral("pendingRenewals"), m_pendingRenewals.size());
    out.insert(QStringLiteral("renewalRetries"), m_renewRetries);
    out.insert(QStringLiteral("acceptedRenewals"),
               QVariant::fromValue<qulonglong>(m_renewAcceptedCount));
    out.insert(QStringLiteral("reissuedTokens"),
               QVariant::fromValue<qulonglong>(m_tokenReissuedCount));
    out.insert(QStringLiteral("rejectedRenewals"),
               QVariant::fromValue<qulonglong>(m_renewRejectedCount));
    out.insert(QStringLiteral("ignoredAuthReplies"),
               QVariant::fromValue<qulonglong>(m_ignoredAuthReplies));
    out.insert(QStringLiteral("ignoredControlPackets"),
               QVariant::fromValue<qulonglong>(m_ignoredControlPackets));
    out.insert(QStringLiteral("renewalCadenceMs"), m_params.tokenRenewalMs);
    out.insert(QStringLiteral("initialMaintenanceMs"), m_params.initialMaintenanceMs);
    out.insert(QStringLiteral("initialMaintenancePending"), m_initialMaintenancePending);
    out.insert(QStringLiteral("ackGraceMs"), m_params.tokenAckGraceMs);
    out.insert(QStringLiteral("deadSessionMs"), m_params.tokenDeadMs);
    return out;
}

}  // namespace AetherSDR::icom
