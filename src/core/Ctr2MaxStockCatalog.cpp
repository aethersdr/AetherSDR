#include "Ctr2MaxStockCatalog.h"

#include "CappedDownload.h"
#include "EspFirmwareCatalog.h"
#include "LogManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>

namespace AetherSDR {

namespace {

constexpr int kFormat = 1;
constexpr qint64 kMaxCatalogBytes = 256 * 1024;
constexpr qint64 kMaxSignatureBytes = 1024;
constexpr qint64 kMaxImageBytes = 16 * 1024 * 1024;   // a whole flash

QString tr(const char* text)
{
    return QCoreApplication::translate("Ctr2MaxStockCatalog", text);
}

// Lynovation's release keys, built into AetherSDR (Ed25519, 32 bytes as
// hex): the public half of the key Lynovation signs its catalog with. None
// yet; until one is here, only a dev-overrides build with
// AETHER_LYNOVATION_TRUST_KEYS fetches anything.
const char* const kBuiltInKeys[] = {
    nullptr,
};

QUrl& urlOverride()
{
    static QUrl url;
    return url;
}

std::optional<QList<QByteArray>>& keysOverride()
{
    static std::optional<QList<QByteArray>> keys;
    return keys;
}

} // namespace

const char* const Ctr2MaxStockCatalog::kCatalogUrl =
    "https://ctr2.lynovation.com/firmware/catalog.json";

void Ctr2MaxStockCatalog::setCatalogUrlForTesting(const QUrl& url)
{
    urlOverride() = url;
}

QUrl Ctr2MaxStockCatalog::catalogUrl()
{
    if (urlOverride().isValid())
        return urlOverride();
#if AETHER_FIRMWARE_DEV_OVERRIDES
    const QByteArray env = qgetenv("AETHER_LYNOVATION_CATALOG");
    if (!env.isEmpty()) {
        const QUrl url = QUrl::fromUserInput(QString::fromUtf8(env));
        qCWarning(lcFirmware) << "AETHER_LYNOVATION_CATALOG: using the CTR2-Max firmware catalog at"
                              << url.toString() << "instead of" << kCatalogUrl;
        return url;
    }
#endif
    return QUrl(QString::fromLatin1(kCatalogUrl));
}

QList<QByteArray> Ctr2MaxStockCatalog::trustedKeys()
{
    if (keysOverride())
        return *keysOverride();
    QList<QByteArray> keys;
    for (const char* hex : kBuiltInKeys) {
        if (hex)
            keys.append(QByteArray::fromHex(hex));
    }
#if AETHER_FIRMWARE_DEV_OVERRIDES
    for (const QByteArray& hex : qgetenv("AETHER_LYNOVATION_TRUST_KEYS").split(',')) {
        const QByteArray key = QByteArray::fromHex(hex.trimmed());
        if (key.size() == 32) {
            qCWarning(lcFirmware)
                << "AETHER_LYNOVATION_TRUST_KEYS: also trusting CTR2-Max firmware catalogs signed by"
                << key.toHex();
            keys.append(key);
        }
    }
#endif
    return keys;
}

void Ctr2MaxStockCatalog::setTrustedKeysForTesting(const QList<QByteArray>& keys)
{
    keysOverride() = keys;
}

bool Ctr2MaxStockCatalog::parse(const QByteArray& json, const QByteArray& signature,
                                const QUrl& base, Ctr2MaxStockCatalog* out, QString* problem)
{
    // The signature first: nothing in an unsigned catalog is read.
    if (!EspFirmwareCatalog::verifyWith(trustedKeys(), json, signature)) {
        *problem = tr("The CTR2-Max firmware list is not signed by Lynovation.");
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        *problem = tr("The CTR2-Max firmware list could not be read.");
        return false;
    }
    if (doc.object().value("format").toInt() != kFormat) {
        *problem = tr("The CTR2-Max firmware list needs a newer AetherSDR.");
        return false;
    }
    Ctr2MaxStockCatalog catalog;
    for (const QJsonValue& v : doc.object().value("products").toArray()) {
        const QJsonObject p = v.toObject();
        // The CTR2-Max's own firmware, for its chip; anything else is not
        // this list's.
        if (p.value("device").toString() != QLatin1String("CTR2-Max")
            || p.value("chip").toString() != QLatin1String("ESP32-S3"))
            continue;
        for (const QJsonValue& r : p.value("releases").toArray()) {
            const QJsonObject o = r.toObject();
            Release release;
            release.version = o.value("version").toString();
            release.file = base.resolved(QUrl(o.value("file").toString()));
            release.size = static_cast<qint64>(o.value("size").toDouble());
            const QByteArray hex = o.value("sha256").toString().toLatin1();
            release.sha256 = QByteArray::fromHex(hex);
            if (release.version.isEmpty() || !release.file.isValid()
                || o.value("file").toString().isEmpty() || release.size <= 0
                || release.size > kMaxImageBytes || release.sha256.size() != 32
                || release.sha256.toHex() != hex.toLower())
                continue;   // skip what this version does not understand
            catalog.releases.append(release);
        }
    }
    *out = catalog;
    return true;
}

Ctr2MaxStockDownloader::Ctr2MaxStockDownloader(QObject* parent)
    : QObject(parent)
    , m_download(new CappedDownload(this))
{
    connect(m_download, &CappedDownload::finished, this, &Ctr2MaxStockDownloader::onFinished);
    connect(m_download, &CappedDownload::failed, this, &Ctr2MaxStockDownloader::fail);
    connect(m_download, &CappedDownload::progress, this, [this](qint64 done, qint64 total) {
        if (m_stage == Stage::Release)
            emit progress(done, total);
    });
}

bool Ctr2MaxStockDownloader::isBusy() const
{
    return m_download->isBusy();
}

void Ctr2MaxStockDownloader::fetchCatalog(const QUrl& url)
{
    m_catalogUrl = url;
    m_stage = Stage::Catalog;
    m_download->get(url, kMaxCatalogBytes, tr("The CTR2-Max firmware list is too large."));
}

void Ctr2MaxStockDownloader::fetchRelease(const Ctr2MaxStockCatalog::Release& release)
{
    m_release = release;
    m_stage = Stage::Release;
    m_download->get(release.file, release.size,
                    tr("The download is larger than the CTR2-Max firmware list says."));
}

void Ctr2MaxStockDownloader::cancel()
{
    m_download->cancel();
    m_stage = Stage::Idle;
}

void Ctr2MaxStockDownloader::fail(const QString& message)
{
    m_stage = Stage::Idle;
    emit failed(message);
}

void Ctr2MaxStockDownloader::onFinished(const QUrl&, const QByteArray& body)
{
    switch (m_stage) {
    case Stage::Catalog:
        m_catalog = body;
        m_stage = Stage::Signature;
        m_download->get(QUrl(m_catalogUrl.toString() + QStringLiteral(".sig")),
                        kMaxSignatureBytes, tr("The CTR2-Max firmware list's signature is too large."));
        return;
    case Stage::Signature: {
        Ctr2MaxStockCatalog catalog;
        QString problem;
        if (!Ctr2MaxStockCatalog::parse(m_catalog, body, m_catalogUrl, &catalog, &problem)) {
            fail(problem);
            return;
        }
        m_stage = Stage::Idle;
        emit catalogReady(catalog);
        return;
    }
    case Stage::Release:
        m_stage = Stage::Idle;
        if (body.size() != m_release.size
            || QCryptographicHash::hash(body, QCryptographicHash::Sha256) != m_release.sha256) {
            fail(tr("The downloaded firmware is not the file the CTR2-Max firmware list names."));
            return;
        }
        emit releaseReady(m_release, body);
        return;
    case Stage::Idle:
        return;
    }
}

} // namespace AetherSDR
