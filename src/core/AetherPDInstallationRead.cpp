#include "AetherPDInstallationRead.h"
#include "AetherPDTransportPolicy.h"
#include "TailnetShimClient.h"
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <limits>
#include <utility>

namespace AetherSDR {
namespace {
constexpr qint64 kReplyBytes = 2048;
constexpr int kDeadlineMs = 10000;
}
AetherPDInstallationRead::AetherPDInstallationRead(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}
AetherPDInstallationRead::AetherPDInstallationRead(QNetworkAccessManager& network, Clock clock, QObject* parent)
    : QObject(parent), m_network(&network), m_now(std::move(clock)) { initialize(); }
void AetherPDInstallationRead::initialize()
{
    m_ownedNetwork.setParent(this); m_deadline.setParent(this);
    m_clock.start();
    if (!m_now) { m_now = [this] { return m_clock.elapsed(); }; }
    m_deadline.setSingleShot(true); m_deadline.setInterval(kDeadlineMs);
    connect(&m_deadline, &QTimer::timeout, this, [this] { if (m_state == State::Reading) { end(State::Failed, Failure::Timeout); } });
    connect(m_network, &QObject::destroyed, this, [this] {
        m_reply = nullptr;
        if (m_state == State::Reading) { end(State::Failed, Failure::Network); }
    });
}
AetherPDInstallationRead::~AetherPDInstallationRead()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    m_deadline.stop(); retireReply();
}
bool AetherPDInstallationRead::begin(const QHostAddress& address, const QString& token)
{
    if (QThread::currentThread() != thread() || m_state != State::Idle || !m_network || m_network->thread() != thread()
        || !aetherpd::lanAddress(address) || !aetherpd::validAdminToken(token)) { return false; }
    const qint64 stamp = m_now();
    if (stamp < 0 || stamp > std::numeric_limits<qint64>::max() - kDeadlineMs) { return false; }
    m_lastStamp = stamp; m_expires = stamp + kDeadlineMs;
    QUrl url;
    url.setScheme(QStringLiteral("http")); url.setHost(address.toString()); url.setPort(kTailnetShimApiPort);
    url.setPath(QStringLiteral("/v1/aetherpd/installation"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + token.toLatin1());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(kDeadlineMs);
    m_state = State::Reading; m_deadline.start();
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(kReplyBytes + 1);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (m_state != State::Reading || !m_reply) { return; }
        bool known = false;
        const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
        if (known && length > kReplyBytes) { end(State::Failed, Failure::InvalidReply); }
    });
    connect(m_reply, &QIODevice::readyRead, this, [this] { consume(); });
    connect(m_reply, &QNetworkReply::finished, this, &AetherPDInstallationRead::receive);
    return true;
}
bool AetherPDInstallationRead::timely()
{
    const qint64 stamp = m_now();
    if (stamp < m_lastStamp || stamp < 0) { end(State::Failed, Failure::InvalidClock); return false; }
    if (stamp >= m_expires) { end(State::Failed, Failure::Timeout); return false; }
    m_lastStamp = stamp;
    return true;
}
bool AetherPDInstallationRead::consume()
{
    if (m_state != State::Reading || !m_reply || !timely()) { return false; }
    const qint64 left = kReplyBytes - m_body.size();
    const QByteArray chunk = m_reply->read(left + 1);
    if (chunk.size() > left) { end(State::Failed, Failure::InvalidReply); return false; }
    m_body += chunk;
    return true;
}
void AetherPDInstallationRead::receive()
{
    if (m_state != State::Reading || !m_reply) { return; }
    const QPointer<AetherPDInstallationRead> self(this);
    if (!consume() || !self || m_state != State::Reading || !m_reply) { return; }
    const int code = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403) { end(State::Failed, Failure::Unauthorized); return; }
    if (code != 200 || m_reply->error() != QNetworkReply::NoError) { end(State::Failed, Failure::Network); return; }
    m_observation = aetherpd::parseInstallationObservation(m_body);
    end(m_observation ? State::Ready : State::Failed, m_observation ? Failure::None : Failure::InvalidReply);
}
void AetherPDInstallationRead::retireReply()
{
    QPointer<QNetworkReply> reply = m_reply; m_reply = nullptr;
    if (reply) {
        disconnect(reply, nullptr, this, nullptr); reply->abort();
        if (reply) { reply->deleteLater(); }
    }
}
void AetherPDInstallationRead::end(State state, Failure failure)
{
    m_state = state; m_failure = failure; m_deadline.stop(); m_body.clear();
    if (state != State::Ready) { m_observation.reset(); }
    const QPointer<AetherPDInstallationRead> self(this);
    retireReply();
    if (self) { emit finished(); }
}
void AetherPDInstallationRead::cancel()
{
    if (QThread::currentThread() == thread() && m_state == State::Reading) { end(State::Canceled); }
}
} // namespace AetherSDR
