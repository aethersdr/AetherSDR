#include "AetherPDTestClient.h"

#include "core/AetherPDTransportPolicy.h"
#include "core/TailnetShimClient.h"

#include <QByteArray>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <utility>

namespace AetherSDR::txlin {
namespace {
constexpr qint64 kFreshMs = 500;
constexpr qint64 kPollMs = 250;
constexpr qint64 kReplyBytes = 512;
}

AetherPDTestClient::AetherPDTestClient(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}
AetherPDTestClient::AetherPDTestClient(QNetworkAccessManager& network, Clock clock, QObject* parent)
    : QObject(parent), m_network(&network), m_now(std::move(clock))
{
    initialize();
}
void AetherPDTestClient::initialize()
{
    m_ownedNetwork.setParent(this);
    m_clock.start();
    if (!m_now) { m_now = [this] { return m_clock.elapsed(); }; }
    m_timer.setParent(this);
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, &AetherPDTestClient::poll);
    connect(m_network, &QObject::destroyed, this, [this] {
        m_network = nullptr;
        if (m_used && m_state != State::Ended && m_state != State::Failed) {
            fail(QStringLiteral("Test helper transport was destroyed"));
        }
    });
}
AetherPDTestClient::~AetherPDTestClient()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    m_timer.stop();
    abandon();
}
bool AetherPDTestClient::setSession(const QHostAddress& address, const QString& token, std::function<bool()> current)
{
    if (QThread::currentThread() != thread() || m_used) { return false; }
    m_address = address;
    m_token = token;
    m_current = std::move(current);
    return true;
}
bool AetherPDTestClient::setChangedCallback(Changed changed)
{
    if (QThread::currentThread() != thread() || m_used) { return false; }
    m_changed = std::move(changed);
    return true;
}
bool AetherPDTestClient::current() const
{
    return m_network && m_network->thread() == thread() && m_current && m_current()
        && aetherpd::lanAddress(m_address) && aetherpd::validAdminToken(m_token);
}
std::optional<qint64> AetherPDTestClient::now()
{
    const qint64 value = m_now();
    if (value < m_lastNow || value < 0 || value > std::numeric_limits<qint64>::max() - 15001) { return std::nullopt; }
    m_lastNow = value;
    return value;
}
void AetherPDTestClient::abandon()
{
    ++m_generation;
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        if (reply) { reply->deleteLater(); }
    }
}
void AetherPDTestClient::notify()
{
    const Changed callback = m_changed;
    if (callback) { callback(); }
}
void AetherPDTestClient::fail(const QString& reason)
{
    m_state = State::Failed;
    m_error = reason;
    m_timer.stop();
    abandon();
    notify();
}
bool AetherPDTestClient::begin(int cleanupAfterMs)
{
    if (QThread::currentThread() != thread() || m_used || !current() || !aetherpd::testBeginBody(cleanupAfterMs) || !now()) { return false; }
    m_used = true;
    m_duration = cleanupAfterMs;
    m_state = State::Starting;
    m_timer.start();
    send(aetherpd::TestOperation::Begin);
    return true;
}
bool AetherPDTestClient::requestStop()
{
    if (QThread::currentThread() != thread() || !m_used || m_state == State::Ended) { return false; }
    if (m_stopSent) { return m_state == State::Retiring; }
    m_stopSent = true;
    m_state = State::Retiring; // Withdraw availability before canceling any reply.
    abandon();
    const std::optional<qint64> stamp = now();
    if (m_testId.isEmpty() || !m_network || m_network->thread() != thread() || !stamp) {
        fail(QStringLiteral("Test helper cleanup could not be confirmed"));
        return false;
    }
    m_stopAt = *stamp;
    m_timer.start();
    send(aetherpd::TestOperation::Stop);
    return true;
}
void AetherPDTestClient::clearSession()
{
    if (QThread::currentThread() != thread()) { return; }
    m_current = {};
    if (m_used) { requestStop(); }
    else { m_address = {}; m_token.clear(); }
}
qint64 AetherPDTestClient::remainingMs()
{
    if (QThread::currentThread() != thread() || m_state != State::Observing) { return 0; }
    const std::optional<qint64> stamp = now();
    if (!current() || !stamp || *stamp >= m_deadline || *stamp - m_observedAt >= kFreshMs) {
        fail(QStringLiteral("Test helper observation expired or lost its session"));
        return 0;
    }
    return m_deadline - *stamp;
}
qint64 AetherPDTestClient::cleanupRemainingUpperMs()
{
    if (remainingMs() <= 0) { return 0; }
    return std::max(qint64{0}, m_cleanupUpper - m_lastNow);
}
void AetherPDTestClient::poll()
{
    if (QThread::currentThread() != thread() || m_state == State::Idle || m_state == State::Ended || m_state == State::Failed) { return; }
    const std::optional<qint64> stamp = now();
    if (!stamp || (m_state != State::Retiring && !current())) {
        fail(QStringLiteral("Test helper lost its clock or session")); return;
    }
    if (m_reply && *stamp - m_requestAt >= m_requestTimeout) {
        fail(QStringLiteral("Test helper request timed out")); return;
    }
    if (m_state == State::Observing && (*stamp >= m_deadline || *stamp - m_observedAt >= kFreshMs)) {
        fail(QStringLiteral("Test helper observation expired")); return;
    }
    if (m_state == State::Verifying && *stamp >= m_deadline) {
        fail(QStringLiteral("Test helper budget expired before verification")); return;
    }
    if (m_state == State::Retiring && *stamp - m_stopAt >= 10000) {
        fail(QStringLiteral("Test helper termination remains unconfirmed")); return;
    }
    if (!m_reply && (m_state == State::Verifying || *stamp - m_requestAt >= kPollMs)) {
        send(aetherpd::TestOperation::Status);
    }
}
void AetherPDTestClient::send(aetherpd::TestOperation operation)
{
    const std::optional<aetherpd::Request> descriptor = aetherpd::testRequest(operation,
        operation == aetherpd::TestOperation::Begin ? QString{} : m_testId);
    const std::optional<qint64> stamp = now();
    if (!descriptor || !stamp || !m_network || m_network->thread() != thread()) {
        fail(QStringLiteral("Test helper request is unavailable")); return;
    }
    QUrl url;
    url.setScheme(QStringLiteral("http")); url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort); url.setPath(descriptor->path);
    QNetworkRequest request(url);
    request.setTransferTimeout(descriptor->timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    QByteArray body;
    if (operation == aetherpd::TestOperation::Begin) {
        body = *aetherpd::testBeginBody(m_duration);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    m_requestAt = *stamp;
    m_requestTimeout = descriptor->timeoutMs;
    m_reply = descriptor->method == "GET" ? m_network->get(request) : m_network->post(request, body);
    QNetworkReply* reply = m_reply;
    const quint64 generation = m_generation;
    reply->setReadBufferSize(kReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply, generation] {
        if (m_reply == reply && generation == m_generation && reply->bytesAvailable() > kReplyBytes) {
            fail(QStringLiteral("Test helper response exceeded its size limit"));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, operation] { finish(reply, generation, operation); });
}
void AetherPDTestClient::finish(QNetworkReply* reply, quint64 generation, aetherpd::TestOperation operation)
{
    reply->deleteLater();
    if (m_reply != reply || generation != m_generation) { return; }
    m_reply = nullptr;
    const std::optional<qint64> stamp = now();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->read(kReplyBytes + 1);
    const std::optional<aetherpd::TestObservation> observation = aetherpd::parseTestObservation(body);
    // Preserve a valid original reply's identity for narrow cleanup even when
    // ownership or timing expired in transit. This does not publish readiness.
    if (operation == aetherpd::TestOperation::Begin && code == 200
        && reply->error() == QNetworkReply::NoError && observation) {
        m_testId = observation->testId;
        m_radioDeadline = observation->cleanupDueMs;
    }
    if (!stamp || *stamp - m_requestAt >= m_requestTimeout || (m_state != State::Retiring && !current())
        || reply->error() != QNetworkReply::NoError || !observation
        || code != (operation == aetherpd::TestOperation::Stop ? 202 : 200)) {
        fail(QStringLiteral("Test helper reply failed validation")); return;
    }
    if (operation == aetherpd::TestOperation::Begin) {
        if (observation->state != aetherpd::TestState::Observing || observation->remainingMs > m_duration) {
            fail(QStringLiteral("Test helper did not start within its budget")); return;
        }
        m_previousRemaining = observation->remainingMs;
        m_deadline = m_requestAt + observation->remainingMs;
        m_cleanupUpper = *stamp + observation->remainingMs + 1;
        m_state = State::Verifying;
        // Bind/setup latency can be long. Require a separate fresh status
        // round trip before making this helper available to the TX lease.
        poll();
        return;
    }
    if (observation->testId != m_testId || observation->cleanupDueMs != m_radioDeadline
        || observation->remainingMs > m_previousRemaining) {
        fail(QStringLiteral("Test helper identity or deadline changed")); return;
    }
    m_previousRemaining = observation->remainingMs;
    if (m_state == State::Retiring) {
        if (observation->state == aetherpd::TestState::Observing) {
            fail(QStringLiteral("Test helper remained active after stop")); return;
        }
        if (observation->state == aetherpd::TestState::Ended) { m_state = State::Ended; m_timer.stop(); }
        notify(); return;
    }
    if (observation->state != aetherpd::TestState::Observing) {
        fail(QStringLiteral("Test helper stopped observing")); return;
    }
    m_deadline = std::min(m_deadline, m_requestAt + observation->remainingMs);
    m_cleanupUpper = std::min(m_cleanupUpper, *stamp + observation->remainingMs + 1);
    m_observedAt = m_requestAt;
    if (*stamp >= m_deadline || *stamp - m_observedAt >= kFreshMs) {
        fail(QStringLiteral("Test helper reply arrived after its usable interval")); return;
    }
    m_state = State::Observing;
    notify();
}
} // namespace AetherSDR::txlin
