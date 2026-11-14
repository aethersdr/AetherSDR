#include "core/MicrohamTrioSession.h"

#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR {

using namespace MicrohamTrio;

namespace {

// The decision is re-run at least this often, so a bound that no frame
// announces (silence, an overdue reply) is still noticed.
inline constexpr qint64 kMaxTickMs = 1000;
inline constexpr int kNonceWords = 4;

} // namespace

MicrohamTrioSession::Entropy MicrohamTrioSession::systemEntropy()
{
    Entropy entropy;
    entropy.random32 = []() { return QRandomGenerator::system()->generate(); };
    return entropy;
}

MicrohamTrioSession::MicrohamTrioSession(MicrohamTrioTransport* transport,
                                         const MicrohamTrioClock* clock,
                                         Entropy entropy, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_clock(clock)
    , m_entropy(std::move(entropy))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &MicrohamTrioSession::tick);

    connect(m_transport, &MicrohamTrioTransport::opened,
            this, &MicrohamTrioSession::onOpened);
    connect(m_transport, &MicrohamTrioTransport::received,
            this, &MicrohamTrioSession::onReceived);
    connect(m_transport, &MicrohamTrioTransport::closedByPeer,
            this, &MicrohamTrioSession::onClosedByPeer);
    connect(m_transport, &MicrohamTrioTransport::failed,
            this, &MicrohamTrioSession::onTransportFailed);
}

MicrohamTrioSession::~MicrohamTrioSession() = default;

void MicrohamTrioSession::start(const Config& config)
{
    stop();
    m_config = config;
    m_running = true;
    schedule(1);
}

void MicrohamTrioSession::stop()
{
    const bool wasRunning = m_running;
    m_running = false;
    m_timer->stop();
    if (wasRunning) {
        if (m_phase == Phase::Open) {
            // An orderly end: status 1000, as a browser closes the page.
            m_transport->send(MicrohamTrio::Ws::encodeClientFrame(
                MicrohamTrio::Ws::Opcode::Close, QByteArray("\x03\xe8", 2), m_entropy.random32()));
        }
        dropConnection();
    }
    m_link = Link::NeverConnected;
    m_failure = Failure::None;
    m_lastConnectAtMs.reset();
    m_transientRetryAtMs.reset();
    m_consecutiveFailures = 0;
    m_transientFailures = 0;
    m_snapshot.reset();
    setStatus(Status::Idle);
}

void MicrohamTrioSession::setReleasedForBrowser(bool released)
{
    if (m_released == released) {
        return;
    }
    m_released = released;
    if (m_running) {
        schedule(1);
    }
}

State MicrohamTrioSession::state() const
{
    return buildState(m_clock->nowMs());
}

State MicrohamTrioSession::buildState(qint64 nowMs) const
{
    State state;
    state.releasedForBrowser = m_released;
    state.link = m_link;
    state.lastMessageAgeMs = m_lastMessageAtMs ? nowMs - *m_lastMessageAtMs : 0;
    if (m_lastKeepAliveAtMs) {
        state.lastKeepAliveAgeMs = nowMs - *m_lastKeepAliveAtMs;
    }
    if (m_unansweredKeepAliveAtMs) {
        state.unansweredKeepAliveAgeMs = nowMs - *m_unansweredKeepAliveAtMs;
    }
    if (m_lastConnectAtMs) {
        state.lastConnectAgeMs = nowMs - *m_lastConnectAtMs;
    }
    state.consecutiveFailures = m_consecutiveFailures;
    state.hasSnapshot = m_stateOnThisSession;
    return state;
}

void MicrohamTrioSession::tick()
{
    evaluate();
}

void MicrohamTrioSession::evaluate()
{
    if (!m_running) {
        return;
    }
    const qint64 now = m_clock->nowMs();
    const Policy& policy = m_config.policy;
    const bool connecting = m_phase != Phase::Idle && m_phase != Phase::Open;

    if (m_released) {
        // A connect under way is given up; a held session is closed by the
        // decision below.
        if (connecting) {
            dropConnection();
            m_link = Link::NeverConnected;
        }
    } else if (connecting) {
        const qint64 elapsed = now - m_lastConnectAtMs.value_or(now);
        if (elapsed >= m_config.connectTimeoutMs) {
            fail(Failure::ConnectTimeout);
        } else {
            schedule(m_config.connectTimeoutMs - elapsed);
        }
        return;
    } else if (m_transientRetryAtMs) {
        if (now >= *m_transientRetryAtMs) {
            m_transientRetryAtMs.reset();
            startAttempt(now);
        } else {
            schedule(*m_transientRetryAtMs - now);
        }
        return;
    }

    // The state document follows the 101 unasked; a session that never sends
    // one is as unusable as one that sends an error page.
    qint64 wait = kMaxTickMs;
    if (m_phase == Phase::Open && !m_stateOnThisSession) {
        const qint64 held = now - m_heldSinceMs.value_or(now);
        if (held >= m_config.firstStateTimeoutMs) {
            failTransient(Failure::NoStateDocument, now);
            return;
        }
        wait = std::min(wait, m_config.firstStateTimeoutMs - held);
    }

    const State state = buildState(now);
    const SessionDecision decision = decideSession(state, policy);
    if (m_phase == Phase::Open) {
        // Wake just past the silence bound rather than up to a tick late.
        wait = std::min(wait, policy.sessionDeadAfterMs - state.lastMessageAgeMs + 1);
        if (state.unansweredKeepAliveAgeMs) {
            wait = std::min(wait, policy.sessionDeadAfterMs - *state.unansweredKeepAliveAgeMs + 1);
        }
    }

    switch (decision.action) {
    case SessionAction::Connect:
        if (decision.reason == SessionReason::SessionDead) {
            m_failure = Failure::SessionSilent;
        }
        startAttempt(now);
        return;

    case SessionAction::Disconnect:
        dropConnection();
        m_link = Link::NeverConnected;
        setStatus(Status::Released);
        schedule(kMaxTickMs);
        return;

    case SessionAction::SendKeepAlive:
        sendText(encodeKeepAlive());
        m_lastKeepAliveAtMs = now;
        if (!m_unansweredKeepAliveAtMs) {
            m_unansweredKeepAliveAtMs = now;
        }
        setStatus(Status::Held);
        schedule(std::min(wait, policy.keepAliveIntervalMs));
        return;

    case SessionAction::Wait:
        break;
    }

    Status status = Status::Failed;
    if (m_released) {
        status = Status::Released;
    } else if (m_link == Link::Held) {
        status = Status::Held;
    } else if (m_link == Link::Evicted) {
        status = Status::Evicted;
    }
    setStatus(status);
    schedule(decision.waitMs > 0 ? std::min(wait, decision.waitMs) : wait);
}

void MicrohamTrioSession::startAttempt(qint64 nowMs)
{
    dropConnection();
    m_lastConnectAtMs = nowMs;
    if (MicrohamTrio::Ws::buildCookieRequest(m_config.host, m_config.port).isEmpty()) {
        fail(Failure::HostUnusable);
        return;
    }
    m_phase = Phase::CookieConnecting;
    setStatus(Status::Connecting);
    m_transport->open(m_config.host, m_config.port);
    schedule(m_config.connectTimeoutMs);
}

void MicrohamTrioSession::onOpened()
{
    if (m_phase == Phase::CookieConnecting) {
        m_phase = Phase::CookieReading;
        m_transport->send(MicrohamTrio::Ws::buildCookieRequest(m_config.host, m_config.port));
        return;
    }
    if (m_phase == Phase::UpgradeConnecting) {
        QByteArray nonce;
        for (int i = 0; i < kNonceWords; ++i) {
            const quint32 word = m_entropy.random32();
            nonce.append(static_cast<char>(word >> 24));
            nonce.append(static_cast<char>(word >> 16));
            nonce.append(static_cast<char>(word >> 8));
            nonce.append(static_cast<char>(word));
        }
        m_key = MicrohamTrio::Ws::makeKey(nonce);
        m_phase = Phase::UpgradeReading;
        m_transport->send(MicrohamTrio::Ws::buildUpgradeRequest(m_config.host, m_config.port,
                                                               m_key, m_wid));
    }
}

void MicrohamTrioSession::onReceived(const QByteArray& bytes)
{
    using namespace MicrohamTrio::Ws;

    if (m_phase == Phase::Open) {
        feedFrames(bytes);
        return;
    }

    if (m_phase == Phase::CookieReading) {
        if (m_cookieHeadDone) {
            m_cookieBodyBytes += bytes.size();
        } else {
            m_head.append(bytes);
            const HttpHead head = parseHead(m_head);
            if (!head.complete) {
                return;
            }
            const Cookie cookie = parseCookie(head);
            if (!head.valid || cookie.error != CookieError::None) {
                fail(Failure::CookieMissing);
                return;
            }
            m_cookieHeadDone = true;
            m_wid = cookie.wid;
            m_cookieBodyLength = contentLength(head);
            m_cookieBodyBytes = m_head.size() - head.bodyOffset;
            m_head.clear();
        }
        // The page is read to its end before the second connection opens, as
        // the recorded client did; without a length the peer's close ends it.
        if (m_cookieBodyLength && m_cookieBodyBytes >= *m_cookieBodyLength) {
            finishCookie();
        }
        return;
    }

    if (m_phase != Phase::UpgradeReading) {
        return;
    }
    m_head.append(bytes);
    const Upgrade upgrade = checkUpgradeResponse(m_head, m_key);
    switch (upgrade.result) {
    case UpgradeResult::Incomplete:
        return;
    case UpgradeResult::Accepted:
        enterHeld(m_clock->nowMs());
        feedFrames(upgrade.remainder);
        return;
    case UpgradeResult::Redirected:
        fail(Failure::UpgradeRedirected);
        return;
    case UpgradeResult::CookieRefused:
        fail(Failure::CookieRefused);
        return;
    case UpgradeResult::Refused:
        fail(Failure::UpgradeRefused);
        return;
    case UpgradeResult::Malformed:
    case UpgradeResult::NotWebSocket:
    case UpgradeResult::AcceptMissing:
    case UpgradeResult::AcceptMismatch:
        fail(Failure::HandshakeInvalid);
        return;
    }
}

void MicrohamTrioSession::finishCookie()
{
    m_transport->close();
    m_head.clear();
    m_phase = Phase::UpgradeConnecting;
    m_transport->open(m_config.host, m_config.port);
}

void MicrohamTrioSession::onClosedByPeer()
{
    switch (m_phase) {
    case Phase::Idle:
        return;
    case Phase::CookieReading:
        if (m_cookieHeadDone) {
            finishCookie();
            return;
        }
        fail(Failure::ClosedEarly);
        return;
    case Phase::CookieConnecting:
    case Phase::UpgradeConnecting:
    case Phase::UpgradeReading:
        fail(Failure::ClosedEarly);
        return;
    case Phase::Open:
        // The device serves one session: a close of a held one is another
        // client taking it.
        becomeEvicted();
        return;
    }
}

void MicrohamTrioSession::onTransportFailed(const QString& reason)
{
    Q_UNUSED(reason);
    if (m_phase == Phase::Idle) {
        return;
    }
    fail(m_phase == Phase::Open ? Failure::ConnectionLost : Failure::ConnectFailed);
}

void MicrohamTrioSession::enterHeld(qint64 nowMs)
{
    m_phase = Phase::Open;
    m_link = Link::Held;
    m_heldSinceMs = nowMs;
    m_lastMessageAtMs = nowMs;
    m_head.clear();
    setStatus(Status::Held);
    schedule(1);
}

void MicrohamTrioSession::feedFrames(const QByteArray& bytes)
{
    const QVector<MicrohamTrio::Ws::ServerFrame> frames = m_decoder.feed(bytes);
    for (const MicrohamTrio::Ws::ServerFrame& frame : frames) {
        if (m_phase != Phase::Open) {
            return;
        }
        handleFrame(frame);
    }
    if (m_phase == Phase::Open && m_decoder.error() != MicrohamTrio::Ws::DecodeError::None) {
        fail(Failure::ProtocolError);
    }
}

void MicrohamTrioSession::handleFrame(const MicrohamTrio::Ws::ServerFrame& frame)
{
    using MicrohamTrio::Ws::Opcode;

    const qint64 now = m_clock->nowMs();
    m_lastMessageAtMs = now;

    if (frame.opcode() == Opcode::Ping) {
        m_transport->send(MicrohamTrio::Ws::encodeClientFrame(Opcode::Pong, frame.payload,
                                                             m_entropy.random32()));
        return;
    }
    if (frame.opcode() == Opcode::Close) {
        m_transport->send(MicrohamTrio::Ws::encodeClientFrame(Opcode::Close, frame.payload,
                                                             m_entropy.random32()));
        becomeEvicted();
        return;
    }

    const Frame classified = classify(frame.firstByte, frame.payload);
    switch (classified.kind) {
    case FrameKind::ServerPong:
        return;
    case FrameKind::KeepAliveReply:
        m_unansweredKeepAliveAtMs.reset();
        return;
    case FrameKind::State:
        takeState(classified);
        return;
    case FrameKind::Unknown:
        break;
    }

    if (frame.opcode() != Opcode::Text || frame.payload.isEmpty()) {
        return;
    }
    // After a raced upgrade the device sends its HTML error page as the first
    // frame; that clears on a retry. Any other unreadable text is a schema
    // this client cannot act on.
    if (!m_stateOnThisSession && classified.state.error == ParseError::NotJson) {
        failTransient(Failure::ErrorPage, now);
        return;
    }
    fail(Failure::UnreadableState);
}

void MicrohamTrioSession::takeState(const Frame& frame)
{
    const bool first = !m_stateOnThisSession;
    m_stateOnThisSession = true;
    m_consecutiveFailures = 0;
    m_transientFailures = 0;
    m_failure = Failure::None;
    m_snapshot = frame.state.snapshot;
    emit snapshotChanged(*m_snapshot, first ? SnapshotSource::OnConnect : SnapshotSource::Push);
}

void MicrohamTrioSession::dropConnection()
{
    m_transport->close();
    m_phase = Phase::Idle;
    if (m_link == Link::Held) {
        m_link = Link::Failed;
    }
    m_head.clear();
    m_cookieHeadDone = false;
    m_cookieBodyBytes = 0;
    m_cookieBodyLength.reset();
    m_wid.clear();
    m_key.clear();
    m_decoder.reset();
    m_heldSinceMs.reset();
    m_lastMessageAtMs.reset();
    m_lastKeepAliveAtMs.reset();
    m_unansweredKeepAliveAtMs.reset();
    m_stateOnThisSession = false;
}

void MicrohamTrioSession::becomeEvicted()
{
    dropConnection();
    m_link = Link::Evicted;
    setStatus(Status::Evicted);
    schedule(1);
}

void MicrohamTrioSession::fail(Failure failure)
{
    dropConnection();
    m_link = Link::Failed;
    ++m_consecutiveFailures;
    m_transientFailures = 0;
    m_failure = failure;
    setStatus(Status::Failed);
    schedule(1);
}

void MicrohamTrioSession::failTransient(Failure failure, qint64 nowMs)
{
    if (m_transientFailures + 1 >= m_config.transientAttempts) {
        fail(failure);
        return;
    }
    ++m_transientFailures;
    dropConnection();
    m_link = Link::Failed;
    m_failure = failure;
    m_transientRetryAtMs = nowMs + m_config.transientRetryMs;
    setStatus(Status::Connecting);
    schedule(m_config.transientRetryMs);
}

void MicrohamTrioSession::sendText(const QByteArray& payload)
{
    m_transport->send(MicrohamTrio::Ws::encodeClientFrame(MicrohamTrio::Ws::Opcode::Text, payload,
                                                         m_entropy.random32()));
}

void MicrohamTrioSession::setStatus(Status status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit sessionStateChanged(status);
}

void MicrohamTrioSession::schedule(qint64 waitMs)
{
    const qint64 wait = std::clamp<qint64>(waitMs, 1, kMaxTickMs);
    m_nextTickAtMs = m_clock->nowMs() + wait;
    if (m_running) {
        m_timer->start(static_cast<int>(wait));
    }
}

} // namespace AetherSDR
