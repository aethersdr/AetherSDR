#include "AetherPDInstallationMonitor.h"
#include "AetherPDTransportPolicy.h"
#include "TailnetShimClient.h"

#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <utility>

namespace AetherSDR {
namespace {
constexpr qint64 kReplyBytes = 2048;
constexpr qint64 kPollMs = 500;
constexpr qint64 kRequestMs = 2500;
constexpr int kMaxObservationMs = 400'000;
}
AetherPDInstallationMonitor::AetherPDInstallationMonitor(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}
AetherPDInstallationMonitor::AetherPDInstallationMonitor(QNetworkAccessManager& network, Clock clock, QObject* parent)
    : QObject(parent), m_network(&network), m_now(std::move(clock))
{
    initialize();
}
void AetherPDInstallationMonitor::initialize()
{
    m_ownedNetwork.setParent(this);
    m_timer.setParent(this);
    m_timer.setInterval(50);
    m_clock.start();
    if (!m_now) { m_now = [this] { return m_clock.elapsed(); }; }
    connect(&m_timer, &QTimer::timeout, this, &AetherPDInstallationMonitor::poll);
    connect(m_network, &QObject::destroyed, this, [this] {
        m_network = nullptr;
        if (m_state == State::Waiting) { end(State::Unconfirmed, Reason::TransportDestroyed); }
    });
}
AetherPDInstallationMonitor::~AetherPDInstallationMonitor()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    m_timer.stop();
    abandonReply();
}
std::optional<qint64> AetherPDInstallationMonitor::now()
{
    const qint64 stamp = m_now();
    if (stamp < m_lastNow || stamp < 0 || stamp > std::numeric_limits<qint64>::max() - kMaxObservationMs - 1) {
        return std::nullopt;
    }
    m_lastNow = stamp;
    return stamp;
}
bool AetherPDInstallationMonitor::begin(const QHostAddress& address, const QString& token,
                                        const QByteArray& approvedPlan, int timeoutMs)
{
    const std::optional<aetherpd::InstallationPlan> plan = aetherpd::parseInstallationPlan(approvedPlan);
    return plan && beginPlan(address, token, *plan, timeoutMs);
}
bool AetherPDInstallationMonitor::beginFirstInstallation(const QHostAddress& address, const QString& newToken,
    const QByteArray& approvedPlan, const QByteArray& publisherKey, int timeoutMs)
{
    const std::optional<aetherpd::InstallationPlan> plan = aetherpd::parseFirstInstallationPlan(approvedPlan, publisherKey);
    if (!plan || !aetherpd::validAdminToken(newToken)
        || QCryptographicHash::hash(newToken.toLatin1(), QCryptographicHash::Sha256) != approvedPlan.mid(148, 32)) {
        return false;
    }
    return beginPlan(address, newToken, *plan, timeoutMs);
}
bool AetherPDInstallationMonitor::beginPlan(const QHostAddress& address, const QString& token,
    const aetherpd::InstallationPlan& plan, int timeoutMs)
{
    if (QThread::currentThread() != thread() || m_state != State::Idle || !m_network
        || m_network->thread() != thread() || !aetherpd::lanAddress(address) || !aetherpd::validAdminToken(token)
        || timeoutMs < 1000 || timeoutMs > kMaxObservationMs) { return false; }
    const std::optional<qint64> stamp = now();
    if (!stamp) { return false; }
    m_plan = plan;
    m_address = address;
    m_token = token;
    m_deadline = *stamp + timeoutMs;
    m_state = State::Waiting;
    m_timer.start();
    send(*stamp);
    return true;
}
void AetherPDInstallationMonitor::abandonReply()
{
    QPointer<QNetworkReply> old = m_reply;
    m_reply = nullptr;
    if (old) {
        disconnect(old, nullptr, this, nullptr);
        old->abort();
        if (old) { old->deleteLater(); }
    }
}
void AetherPDInstallationMonitor::end(State state, Reason reason)
{
    m_state = state;
    m_reason = reason;
    m_timer.stop();
    m_token.clear();
    const QPointer<AetherPDInstallationMonitor> guard(this);
    abandonReply();
    if (guard) { emit finished(state, reason); }
}
void AetherPDInstallationMonitor::cancel()
{
    if (QThread::currentThread() == thread() && m_state == State::Waiting) { end(State::Canceled, Reason::None); }
}
void AetherPDInstallationMonitor::poll()
{
    if (QThread::currentThread() != thread() || m_state != State::Waiting) { return; }
    const std::optional<qint64> stamp = now();
    if (!stamp) { end(State::Unconfirmed, Reason::InvalidClock); return; }
    if (*stamp >= m_deadline) { end(State::Unconfirmed, Reason::Deadline); return; }
    if (!m_network || m_network->thread() != thread()) { end(State::Unconfirmed, Reason::TransportDestroyed); return; }
    if (m_reply) {
        if (*stamp >= m_requestDeadline) {
            m_nextPoll = *stamp + kPollMs;
            abandonReply();
        }
        return;
    }
    if (*stamp >= m_nextPoll) { send(*stamp); }
}
void AetherPDInstallationMonitor::send(qint64 stamp)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort);
    url.setPath(QStringLiteral("/v1/aetherpd/installation"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(static_cast<int>(std::min(kRequestMs, m_deadline - stamp)));
    m_requestDeadline = std::min(stamp + kRequestMs, m_deadline);
    m_reply = m_network->get(request);
    QNetworkReply* reply = m_reply;
    reply->setReadBufferSize(kReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (m_reply == reply && reply->bytesAvailable() > kReplyBytes) { end(State::Unconfirmed, Reason::InvalidReply); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { receive(reply); });
}
void AetherPDInstallationMonitor::receive(QNetworkReply* reply)
{
    reply->deleteLater();
    if (m_reply != reply || m_state != State::Waiting) { return; }
    m_reply = nullptr;
    const std::optional<qint64> stamp = now();
    if (!stamp) { end(State::Unconfirmed, Reason::InvalidClock); return; }
    if (*stamp >= m_deadline) { end(State::Unconfirmed, Reason::Deadline); return; }
    m_nextPoll = *stamp + kPollMs;
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->read(kReplyBytes + 1);
    if (body.size() > kReplyBytes) { end(State::Unconfirmed, Reason::InvalidReply); return; }
    if (code == 401 || code == 403) { end(State::Unconfirmed, Reason::Unauthorized); return; }
    // A restart, unavailable mirror, or older shim is not an installation result.
    if (code == 0 || code == 404 || code == 503) { return; }
    if (code != 200) { end(State::Unconfirmed, Reason::InvalidReply); return; }
    if (reply->error() != QNetworkReply::NoError || *stamp >= m_requestDeadline) { return; }
    const std::optional<aetherpd::InstallationObservation> observed = aetherpd::parseInstallationObservation(body);
    if (!observed) { end(State::Unconfirmed, Reason::InvalidReply); return; }
    switch (aetherpd::matchInstallation(*m_plan, *observed)) {
    case aetherpd::InstallationMatch::Committed: end(State::Committed, Reason::None); return;
    case aetherpd::InstallationMatch::RolledBack: end(State::RolledBack, Reason::None); return;
    case aetherpd::InstallationMatch::Mismatch: end(State::Unconfirmed, Reason::Mismatch); return;
    case aetherpd::InstallationMatch::OtherPlan: return;
    }
}
} // namespace AetherSDR
