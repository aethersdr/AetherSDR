#include "AetherPDInstallRequestClient.h"
#include "AetherPDProtocol.h"
#include "AetherPDTransportPolicy.h"
#include "TailnetShimClient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <utility>

namespace AetherSDR {
namespace {
constexpr qint64 kReplyBytes = 2048;
constexpr qint64 kLifetimeMs = 400'000;
constexpr qint64 kRequestMs = 10'000;
bool zero(const QByteArray& bytes) { return bytes == QByteArray(bytes.size(), '\0'); }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256); }
}
std::optional<QByteArray> AetherPDInstallRequestClient::publicRecord(const QByteArray& config, const QByteArray& approvedPlan)
{
    const std::optional<aetherpd::InstallationPlan> plan = aetherpd::parseInstallationPlan(approvedPlan);
    if (!plan || config.size() != 120 || !config.startsWith("PDXFER01")
        || config.mid(40, 8) != "PDDELIV1" || zero(config.mid(8, 32)) || zero(config.mid(56, 32))
        || qFromBigEndian<quint32>(config.constData() + 52) != 2
        || qFromBigEndian<quint32>(config.constData() + 48) != (plan->profile == 1 ? 64U : 7U)
        || config.mid(88, 32) != digest(approvedPlan)) { return std::nullopt; }
    return QByteArray("PDPUB001") + config.mid(40) + digest(QByteArray("PDKEY001") + config);
}
AetherPDInstallRequestClient::AetherPDInstallRequestClient(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}
AetherPDInstallRequestClient::AetherPDInstallRequestClient(QNetworkAccessManager& network, Clock clock, QObject* parent)
    : QObject(parent), m_network(&network), m_now(std::move(clock))
{
    initialize();
}
void AetherPDInstallRequestClient::initialize()
{
    m_ownedNetwork.setParent(this);
    m_timer.setParent(this);
    m_timer.setInterval(50);
    m_clock.start();
    if (!m_now) { m_now = [this] { return m_clock.elapsed(); }; }
    connect(&m_timer, &QTimer::timeout, this, &AetherPDInstallRequestClient::poll);
    connect(m_network, &QObject::destroyed, this, [this] {
        m_network = nullptr;
        if (m_state != State::Idle && m_state != State::Expired) { fail(Reason::TransportDestroyed, true); }
    });
}
AetherPDInstallRequestClient::~AetherPDInstallRequestClient()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    m_timer.stop();
    abandonReply();
}
std::optional<qint64> AetherPDInstallRequestClient::now()
{
    const qint64 stamp = m_now();
    if (stamp < m_lastNow || stamp < 0 || stamp > std::numeric_limits<qint64>::max() - kLifetimeMs) {
        return std::nullopt;
    }
    m_lastNow = stamp;
    return stamp;
}
bool AetherPDInstallRequestClient::begin(const QHostAddress& address, const QString& token,
                                        const QByteArray& privateConfig, const QByteArray& approvedPlan)
{
    if (QThread::currentThread() != thread() || m_state != State::Idle || !m_network
        || m_network->thread() != thread() || !aetherpd::lanAddress(address) || !aetherpd::validAdminToken(token)) { return false; }
    const std::optional<QByteArray> publicBytes = publicRecord(privateConfig, approvedPlan);
    const std::optional<qint64> stamp = now();
    if (!publicBytes || !stamp) { return false; }
    m_requestId = QString::fromLatin1(publicBytes->last(32).toHex());
    m_address = address;
    m_token = token;
    m_deadline = *stamp + kLifetimeMs;
    m_state = State::Unknown;
    m_timer.start();
    send(Operation::Begin, *stamp, privateConfig);
    return true;
}
bool AetherPDInstallRequestClient::inspect() { return followup(Operation::Inspect); }
bool AetherPDInstallRequestClient::requestStop() { return followup(Operation::Stop); }
bool AetherPDInstallRequestClient::followup(Operation operation)
{
    if (QThread::currentThread() != thread() || m_state == State::Idle || m_state == State::Expired || m_reply) { return false; }
    const std::optional<qint64> stamp = now();
    if (!stamp) { fail(Reason::InvalidClock, true); return false; }
    if (*stamp >= m_deadline) { fail(Reason::Deadline, true); return false; }
    if (!m_network || m_network->thread() != thread()) { fail(Reason::TransportDestroyed, true); return false; }
    send(operation, *stamp);
    return true;
}
void AetherPDInstallRequestClient::abandonReply()
{
    QPointer<QNetworkReply> old = m_reply;
    m_reply = nullptr;
    if (old) {
        disconnect(old, nullptr, this, nullptr);
        old->abort();
        if (old) { old->deleteLater(); }
    }
}
void AetherPDInstallRequestClient::fail(Reason reason, bool expire)
{
    m_state = expire ? State::Expired : State::Unknown;
    if (expire) { m_timer.stop(); m_token.clear(); }
    const QPointer<AetherPDInstallRequestClient> guard(this);
    abandonReply();
    if (guard) { emit failed(reason); }
}
void AetherPDInstallRequestClient::poll()
{
    if (QThread::currentThread() != thread() || m_state == State::Idle || m_state == State::Expired) { return; }
    const std::optional<qint64> stamp = now();
    if (!stamp) { fail(Reason::InvalidClock, true); return; }
    if (*stamp >= m_deadline) { fail(Reason::Deadline, true); return; }
    if (m_reply && *stamp >= m_requestDeadline) { fail(Reason::Deadline); }
}
void AetherPDInstallRequestClient::send(Operation operation, qint64 stamp, const QByteArray& body)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort);
    QString path = QStringLiteral("/v1/aetherpd/install-request");
    if (operation != Operation::Begin) { path += u'/' + m_requestId; }
    url.setPath(path);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(static_cast<int>(std::min(kRequestMs, m_deadline - stamp)));
    m_requestDeadline = std::min(stamp + kRequestMs, m_deadline);
    m_operation = operation;
    if (operation == Operation::Begin) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        m_reply = m_network->post(request, body);
    } else if (operation == Operation::Inspect) { m_reply = m_network->get(request); }
    else { m_reply = m_network->deleteResource(request); }
    QNetworkReply* reply = m_reply;
    reply->setReadBufferSize(kReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (m_reply == reply && reply->bytesAvailable() > kReplyBytes) { fail(Reason::InvalidReply); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { receive(reply); });
}
void AetherPDInstallRequestClient::receive(QNetworkReply* reply)
{
    reply->deleteLater();
    if (m_reply != reply) { return; }
    m_reply = nullptr;
    const std::optional<qint64> stamp = now();
    if (!stamp) { fail(Reason::InvalidClock, true); return; }
    if (*stamp >= m_deadline) { fail(Reason::Deadline, true); return; }
    if (*stamp >= m_requestDeadline) { fail(Reason::Deadline); return; }
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403) { fail(Reason::Unauthorized, true); return; }
    if (code == 409) { fail(Reason::Busy); return; }
    if (code == 404 && m_operation != Operation::Begin) {
        m_state = State::Missing; emit observed(m_state, -1); return;
    }
    if (reply->error() != QNetworkReply::NoError) { fail(Reason::Transport); return; }
    const QByteArray body = reply->read(kReplyBytes + 1);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (body.size() > kReplyBytes || !doc.isObject()) { fail(Reason::InvalidReply); return; }
    const QJsonObject object = doc.object();
    if (m_operation == Operation::Stop && code == 202 && object.size() == 1
        && object.value("state") == QStringLiteral("stop_requested")) {
        m_state = State::StopRequested; emit observed(m_state, -1); return;
    }
    if (object.value("request_id") != m_requestId || !object.value("installation_committed").isBool()
        || object.value("installation_committed").toBool()) { fail(Reason::InvalidReply); return; }
    const QString state = object.value("state").toString();
    int result = -1;
    State next = State::Unknown;
    if (m_operation == Operation::Begin && code == 202 && object.size() == 4
        && state == QStringLiteral("awaiting_inputs") && object.value("port").isDouble()
        && object.value("port").toDouble() == 48995) { next = State::AwaitingInputs; }
    if (m_operation != Operation::Begin && code == 200) {
        if (object.size() == 3 && state == QStringLiteral("running")) { next = State::Running; }
        if (object.size() == 4 && object.value("delivery_result").isDouble()) {
            const double value = object.value("delivery_result").toDouble();
            if (value == 0 || value == 3 || value == 4 || value == 5 || value == 6) {
                result = static_cast<int>(value);
                if (state == QStringLiteral("delivery_finished")) { next = State::DeliveryFinished; }
                if (state == QStringLiteral("unavailable")) { next = State::Unavailable; }
            }
        }
    }
    if (next == State::Unknown) { fail(Reason::InvalidReply); return; }
    m_state = next;
    emit observed(m_state, result);
}
} // namespace AetherSDR
