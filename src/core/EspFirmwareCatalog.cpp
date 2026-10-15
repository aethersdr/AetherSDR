#include "EspFirmwareCatalog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <optional>

extern "C" {
#include "monocypher-ed25519.h"
}

namespace AetherSDR {

namespace {

constexpr int kFormat = 1;
constexpr qint64 kMaxManifestBytes = 64 * 1024;
constexpr qint64 kMaxImageBytes = 16 * 1024 * 1024;

QString tr(const char* text)
{
    return QCoreApplication::translate("EspFirmwareCatalog", text);
}

// Firmware-release public keys built into AetherSDR (Ed25519, 32 bytes as
// hex). A manifest signed by any of them is trusted; keep the old key here
// for a while after adding a new one.
const char* const kBuiltInKeys[] = {
    // AetherCTR2 firmware releases (created 2026-10-14).
    "c5caf07300756c8fa37cc4f47b600a302162635cad5e8e2f85101f2308b1bb19",
};

std::optional<QList<QByteArray>>& keysOverride()
{
    static std::optional<QList<QByteArray>> keys;
    return keys;
}

} // namespace

const char* const EspFirmwareCatalog::kCatalogUrl = "https://www.aethersdr.com/firmware/catalog.json";

QUrl EspFirmwareCatalog::catalogUrl()
{
    const QByteArray env = qgetenv("AETHER_FIRMWARE_CATALOG");
    return QUrl::fromUserInput(QString::fromUtf8(env.isEmpty() ? QByteArray(kCatalogUrl) : env));
}

bool EspFirmwareCatalog::parse(const QByteArray& json, const QUrl& base, EspFirmwareCatalog* out,
                               QString* problem)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        *problem = tr("The firmware catalog could not be read.");
        return false;
    }
    if (doc.object().value("format").toInt() != kFormat) {
        *problem = tr("The firmware catalog needs a newer AetherSDR.");
        return false;
    }
    EspFirmwareCatalog catalog;
    for (const QJsonValue& v : doc.object().value("products").toArray()) {
        const QJsonObject o = v.toObject();
        Product p;
        p.name = o.value("name").toString();
        p.device = o.value("device").toString();
        p.chip = o.value("chip").toString();
        p.description = o.value("description").toString();
        if (p.name.isEmpty())
            continue;   // skip what this version does not understand
        for (const QJsonValue& r : o.value("releases").toArray()) {
            const QString version = r.toObject().value("version").toString();
            const QString manifest = r.toObject().value("manifest").toString();
            if (version.isEmpty() || manifest.isEmpty())
                continue;
            p.version = version;
            p.manifest = base.resolved(QUrl(manifest));
            catalog.products.append(p);
        }
    }
    *out = catalog;
    return true;
}

QList<QByteArray> EspFirmwareCatalog::trustedKeys()
{
    if (keysOverride())
        return *keysOverride();
    QList<QByteArray> keys;
    for (const char* hex : kBuiltInKeys)
        keys.append(QByteArray::fromHex(hex));
    for (const QByteArray& hex : qgetenv("AETHER_FIRMWARE_TRUST_KEYS").split(',')) {
        const QByteArray key = QByteArray::fromHex(hex.trimmed());
        if (key.size() == 32)
            keys.append(key);
    }
    return keys;
}

void EspFirmwareCatalog::setTrustedKeysForTesting(const QList<QByteArray>& keys)
{
    keysOverride() = keys;
}

bool EspFirmwareCatalog::verify(const QByteArray& manifest, const QByteArray& signature)
{
    QByteArray sig = signature;
    if (sig.size() != 64)
        sig = QByteArray::fromBase64(signature.trimmed());
    if (sig.size() != 64)
        return false;
    for (const QByteArray& key : trustedKeys()) {
        if (key.size() == 32
            && crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sig.constData()),
                                    reinterpret_cast<const uint8_t*>(key.constData()),
                                    reinterpret_cast<const uint8_t*>(manifest.constData()),
                                    static_cast<size_t>(manifest.size()))
                   == 0)
            return true;
    }
    return false;
}

EspFirmwareDownloader::EspFirmwareDownloader(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    // A stalled connection fails instead of hanging the install window.
    m_nam->setTransferTimeout(30000);
}

void EspFirmwareDownloader::fetchCatalog(const QUrl& url)
{
    cancel();
    m_url = url;
    get(url, Stage::Catalog);
}

void EspFirmwareDownloader::fetchRelease(const QUrl& manifestUrl, const QString& expectedVersion)
{
    cancel();
    m_url = manifestUrl;
    m_expectedVersion = expectedVersion;
    m_files.clear();
    m_image = 0;
    get(manifestUrl, Stage::Manifest);
}

void EspFirmwareDownloader::cancel()
{
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_stage = Stage::Idle;
}

void EspFirmwareDownloader::get(const QUrl& url, Stage stage)
{
    m_stage = stage;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &EspFirmwareDownloader::onFinished);
    if (stage == Stage::Image) {
        const QString file = m_manifest.images[m_image].file;
        connect(m_reply, &QNetworkReply::downloadProgress, this,
                [this, file](qint64 done, qint64 total) {
                    emit progress(tr("Downloading %1").arg(file), done, total);
                });
    }
}

void EspFirmwareDownloader::fail(const QString& message)
{
    m_stage = Stage::Idle;
    emit failed(message);
}

void EspFirmwareDownloader::onFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    const QUrl url = reply->url();
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Could not download %1: %2").arg(url.fileName(), reply->errorString()));
        return;
    }
    const QByteArray body = reply->readAll();
    QString problem;

    switch (m_stage) {
    case Stage::Catalog: {
        EspFirmwareCatalog catalog;
        if (!EspFirmwareCatalog::parse(body, url, &catalog, &problem)) {
            fail(problem);
            return;
        }
        m_stage = Stage::Idle;
        emit catalogReady(catalog);
        return;
    }
    case Stage::Manifest:
        if (body.size() > kMaxManifestBytes) {
            fail(tr("The firmware manifest is too large."));
            return;
        }
        m_manifestBytes = body;
        get(QUrl(m_url.toString() + QStringLiteral(".sig")), Stage::Signature);
        return;
    case Stage::Signature:
        if (!EspFirmwareCatalog::verify(m_manifestBytes, body)) {
            fail(tr("This firmware is not signed by a key AetherSDR trusts, so it was not "
                    "installed."));
            return;
        }
        if (!EspFirmwareManifest::parse(m_manifestBytes, &m_manifest, &problem)) {
            fail(problem);
            return;
        }
        if (!m_expectedVersion.isEmpty() && m_manifest.version != m_expectedVersion) {
            fail(tr("The firmware list names version %1, but its release is %2.")
                     .arg(m_expectedVersion, m_manifest.version));
            return;
        }
        nextImage();
        return;
    case Stage::Image: {
        const EspFirmwareManifest::Image& image = m_manifest.images[m_image];
        if (static_cast<quint32>(body.size()) != image.size
            || QCryptographicHash::hash(body, QCryptographicHash::Sha256) != image.sha256) {
            fail(tr("%1 did not download intact.").arg(image.file));
            return;
        }
        m_files.insert(image.file, body);
        ++m_image;
        nextImage();
        return;
    }
    case Stage::Idle:
        return;
    }
}

void EspFirmwareDownloader::nextImage()
{
    if (m_image >= m_manifest.images.size()) {
        m_stage = Stage::Idle;
        emit releaseReady(m_manifest, m_files);
        return;
    }
    const EspFirmwareManifest::Image& image = m_manifest.images[m_image];
    if (image.size > kMaxImageBytes) {
        fail(tr("%1 is too large.").arg(image.file));
        return;
    }
    get(m_url.resolved(QUrl(image.file)), Stage::Image);
}

} // namespace AetherSDR
