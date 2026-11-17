#include "AetherPDPackageDownload.h"
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>

namespace AetherSDR {
namespace {
bool validMetadata(const QString& version, const QString& firmware, const std::array<QUrl, 8>& urls)
{
    static const QRegularExpression versionPattern(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){1,3}(?:-[A-Za-z0-9.]+)?\\z"));
    static const QRegularExpression firmwarePattern(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){3}\\z"));
    if (version.size() > 64 || !versionPattern.match(version).hasMatch() || firmware.size() >= 28
        || !firmwarePattern.match(firmware).hasMatch()) { return false; }
    for (const QUrl& url : urls) {
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()
            || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() || url.toEncoded().size() > 2048) { return false; }
    }
    return true;
}
} // namespace
bool AetherPDRelease::valid() const
{
    return (profile == 1 || profile == 2) && AetherPDPackage::validTrust(trust)
        && validMetadata(version, firmware, urls);
}
bool AetherPDFirstRelease::valid() const
{
    return profile == 1 && AetherPDFirstPackage::validTrust(trust)
        && validMetadata(version, firmware, urls);
}
AetherPDPackageDownload::AetherPDPackageDownload(QObject* parent)
    : QObject(parent), m_owned(std::make_unique<QNetworkAccessManager>()), m_network(m_owned.get()) { initialize(); }
AetherPDPackageDownload::AetherPDPackageDownload(QNetworkAccessManager& network, QObject* parent)
    : QObject(parent), m_network(&network) { initialize(); }
AetherPDPackageDownload::~AetherPDPackageDownload()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    cancel();
}
void AetherPDPackageDownload::initialize()
{
    m_deadline.setParent(this);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(120000);
    connect(&m_deadline, &QTimer::timeout, this, [this] { if (m_active) { fail(Failure::Timeout); } });
    connect(m_network, &QObject::destroyed, this, [this] { if (m_active) { fail(Failure::Network); } });
}
bool AetherPDPackageDownload::start(const AetherPDRelease& release)
{
    return begin(release);
}
bool AetherPDPackageDownload::start(const AetherPDFirstRelease& release)
{
    return begin(release);
}
const std::array<QUrl, 8>& AetherPDPackageDownload::urls() const
{
    return std::visit([](const auto& release) -> const std::array<QUrl, 8>& { return release.urls; }, m_release);
}
const std::array<AetherPDPackage::Pin, 8>& AetherPDPackageDownload::pins() const
{
    return std::visit([](const auto& release) -> const std::array<AetherPDPackage::Pin, 8>& { return release.trust.files; }, m_release);
}
bool AetherPDPackageDownload::begin(const Selection& release)
{
    if (QThread::currentThread() != thread() || m_active || !m_network || m_network->thread() != thread()
        || !std::visit([](const auto& selection) { return selection.valid(); }, release)) { return false; }
    m_release = release;
    m_files = {}; m_package.reset(); m_firstPackage.reset(); m_index = 0; m_received = 0; m_total = 0;
    for (const auto& pin : pins()) { m_total += pin.size; }
    m_active = true;
    m_deadline.start();
    next();
    return true;
}
void AetherPDPackageDownload::retireReply()
{
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        if (reply) { reply->deleteLater(); }
    }
}
void AetherPDPackageDownload::cancel()
{
    m_active = false;
    m_deadline.stop();
    m_files = {}; m_package.reset(); m_firstPackage.reset();
    retireReply();
}
void AetherPDPackageDownload::fail(Failure reason)
{
    const QPointer<AetherPDPackageDownload> self(this);
    cancel();
    if (self) { emit failed(reason); }
}
void AetherPDPackageDownload::next()
{
    if (!m_network) { fail(Failure::Network); return; }
    QNetworkRequest request(urls()[m_index]);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(30000);
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(64 * 1024);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_active || !m_reply) { return; }
        bool known = false;
        const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
        if (known && length > pins()[m_index].size) { fail(Failure::Oversize); }
    });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] { consume(); });
    connect(m_reply, &QNetworkReply::finished, this, &AetherPDPackageDownload::finished);
}
bool AetherPDPackageDownload::consume()
{
    if (!m_active || !m_reply) { return false; }
    QByteArray& bytes = m_files[m_index];
    const qint64 left = pins()[m_index].size - bytes.size();
    const QByteArray chunk = m_reply->read(left + 1);
    if (chunk.size() > left) { fail(Failure::Oversize); return false; }
    bytes += chunk;
    m_received += chunk.size();
    emit progress(m_received, m_total);
    return true;
}
void AetherPDPackageDownload::finished()
{
    if (!m_active || !m_reply) { return; }
    if (m_reply->error() != QNetworkReply::NoError
        || m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) { fail(Failure::Network); return; }
    const QPointer<AetherPDPackageDownload> self(this);
    const QPointer<QNetworkReply> reply = m_reply;
    if (!consume() || !self || !m_active || m_reply != reply) { return; }
    const auto& pin = pins()[m_index];
    if (m_files[m_index].size() != pin.size
        || QCryptographicHash::hash(m_files[m_index], QCryptographicHash::Sha256) != pin.sha256) { fail(Failure::Integrity); return; }
    retireReply();
    ++m_index;
    if (m_index < m_files.size()) { next(); return; }
    bool verified = false;
    if (const auto* release = std::get_if<AetherPDRelease>(&m_release)) {
        m_package = AetherPDPackage::verify(m_files, release->trust);
        verified = m_package && m_package->profile() == release->profile && m_package->firmware() == release->firmware;
    } else {
        const auto& first = std::get<AetherPDFirstRelease>(m_release);
        m_firstPackage = AetherPDFirstPackage::verify(m_files, first.trust);
        verified = m_firstPackage && m_firstPackage->firmware() == first.firmware;
    }
    m_files = {};
    if (!verified) {
        fail(Failure::Integrity); return;
    }
    m_deadline.stop(); m_active = false;
    emit ready();
}
} // namespace AetherSDR
