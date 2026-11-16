#include "AetherPDClient.h"
#include "AetherPDTransportPolicy.h"

#include "TailnetShimClient.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {
namespace {
constexpr qint64 kMaxReplyBytes = 16 * 1024;

bool mayPreempt(aetherpd::Operation next, aetherpd::Operation current)
{
    using aetherpd::Operation;
    if (next == Operation::Stop) {
        return current != Operation::Stop;
    }
    if (next == Operation::Start) {
        return current == Operation::Status;
    }
    return next == Operation::CancelCalibration
        && (current == Operation::Status || current == Operation::ArmCalibration || current == Operation::ArmMeasurement);
}
} // namespace

AetherPDClient::AetherPDClient(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}

AetherPDClient::AetherPDClient(QNetworkAccessManager& network, QObject* parent)
    : QObject(parent), m_network(&network)
{
    initialize();
}

void AetherPDClient::initialize()
{
    m_ownedNetwork.setParent(this);
    m_deadline.setParent(this);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_timedOut = true;
            m_reply->abort();
        }
    });
    connect(m_network, &QObject::destroyed, this, [this] {
        // QNAM can destroy its replies before QObject emits destroyed, so
        // QPointer alone no longer records the request that was pending.
        const bool pending = m_deadline.isActive();
        m_deadline.stop();
        m_reply = nullptr;
        if (pending) {
            emit failed(m_operation, Failure::Transport);
        }
    });
}

AetherPDClient::~AetherPDClient()
{
    if (m_network) {
        disconnect(m_network, nullptr, this, nullptr);
    }
    m_deadline.stop();
    if (m_reply) {
        QPointer<QNetworkReply> reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        if (reply) {
            reply->deleteLater();
        }
    }
}

void AetherPDClient::cancelPending()
{
    m_deadline.stop();
    if (m_reply) {
        QPointer<QNetworkReply> old = m_reply;
        const aetherpd::Operation operation = m_operation;
        m_reply = nullptr;
        old->abort();
        emit requestCanceled(operation);
    }
}

void AetherPDClient::setRadioAddress(const QHostAddress& address)
{
    if (m_address != address) {
        m_address = address;
        cancelPending();
    }
}

bool AetherPDClient::submit(aetherpd::Operation operation, const QString& adminToken,
                            std::optional<aetherpd::StartIdentity> identity,
                            std::optional<quint32> toneSpacingHz)
{
    const std::optional<aetherpd::Request> descriptor = toneSpacingHz
        ? std::optional<aetherpd::Request>(aetherpd::measurementToneRequest())
        : operation == aetherpd::Operation::Start && identity && identity->iq
            ? std::optional<aetherpd::Request>(aetherpd::selectedStartRequest()) : aetherpd::request(operation);
    const std::optional<QByteArray> body = operation == aetherpd::Operation::Start
        ? (identity ? aetherpd::startBody(*identity) : std::nullopt)
        : (identity ? std::nullopt : toneSpacingHz ? aetherpd::measurementToneBody(*toneSpacingHz)
                                                  : std::optional<QByteArray>(QByteArray{}));
    if ((toneSpacingHz && operation != aetherpd::Operation::ArmMeasurement)
        || !descriptor || !body || !m_network || m_network->thread() != thread()
        || !aetherpd::lanAddress(m_address) || !aetherpd::validAdminToken(adminToken)) {
        emit failed(operation, Failure::InvalidRequest);
        return false;
    }
    if (m_reply) {
        if (!mayPreempt(operation, m_operation)) {
            emit failed(operation, Failure::Busy);
            return false;
        }
        const QHostAddress address = m_address;
        const QPointer<AetherPDClient> guard(this);
        cancelPending();
        // Cancellation is observable; a synchronous observer may change the
        // selected radio or submit another request before this call resumes.
        if (!guard) {
            return false;
        }
        if (m_address != address) {
            emit failed(operation, Failure::InvalidRequest);
            return false;
        }
        if (m_reply || !m_network) {
            emit failed(operation, Failure::Busy);
            return false;
        }
    }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort);
    url.setPath(descriptor->path);
    QNetworkRequest request(url);
    request.setTransferTimeout(descriptor->timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + adminToken.toLatin1());
    if (operation == aetherpd::Operation::Start || toneSpacingHz) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    m_operation = operation;
    m_timedOut = false;
    m_oversized = false;
    m_reply = descriptor->method == "GET" ? m_network->get(request) : m_network->post(request, *body);
    QNetworkReply* reply = m_reply;
    reply->setReadBufferSize(kMaxReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (m_reply == reply && reply->bytesAvailable() > kMaxReplyBytes) {
            m_oversized = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] { finish(reply, operation); });
    m_deadline.start(descriptor->timeoutMs);
    return true;
}

void AetherPDClient::finish(QNetworkReply* reply, aetherpd::Operation operation)
{
    reply->deleteLater();
    if (m_reply != reply) {
        return;
    }
    m_reply = nullptr;
    m_deadline.stop();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->read(kMaxReplyBytes + 1);
    if (m_timedOut) {
        emit failed(operation, Failure::Timeout);
    } else if (m_oversized || body.size() > kMaxReplyBytes) {
        emit failed(operation, Failure::OversizedReply);
    } else if (code == 401) {
        emit failed(operation, Failure::Unauthorized);
    } else if (code != 0 && (code < 200 || code >= 300)) {
        emit failed(operation, Failure::Refused);
    } else if (reply->error() != QNetworkReply::NoError || code == 0) {
        emit failed(operation, Failure::Transport);
    } else if (operation == aetherpd::Operation::Status) {
        const std::optional<aetherpd::Status> status = code == 200 ? aetherpd::parseStatus(body) : std::nullopt;
        if (status) {
            emit statusReceived(*status);
        } else {
            emit failed(operation, Failure::InvalidReply);
        }
    } else {
        const std::optional<aetherpd::Acknowledgment> acknowledgment = aetherpd::parseAcknowledgment(operation, code, body);
        if (acknowledgment) {
            emit acknowledged(operation, *acknowledgment);
        } else {
            emit failed(operation, Failure::InvalidReply);
        }
    }
}

} // namespace AetherSDR
