#include "EspFirmwareCatalog.h"

#include "CappedDownload.h"
#include "LogManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>
#include <utility>

extern "C" {
#include "monocypher-ed25519.h"
}

namespace AetherSDR {

namespace {

constexpr int kFormat = 1;
// What each download may be, at most; more is refused as it arrives, before
// it is buffered. A package holds at most a whole flash's images, plus its
// manifest and signature.
constexpr qint64 kMaxCatalogBytes = 256 * 1024;
constexpr qint64 kMaxPackageBytes = 17 * 1024 * 1024;
constexpr qint64 kMaxManifestBytes = 64 * 1024;
constexpr qint64 kMaxSignatureBytes = 1024;
constexpr qint64 kMaxImageBytes = 16 * 1024 * 1024;
constexpr int kMaxPackageFiles = 16;
constexpr int kTarBlock = 512;

const QString kManifestFile = QStringLiteral("manifest.json");
const QString kSignatureFile = QStringLiteral("manifest.json.sig");

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

namespace {
QUrl& catalogUrlOverride()
{
    static QUrl url;
    return url;
}
} // namespace

void EspFirmwareCatalog::setCatalogUrlForTesting(const QUrl& url)
{
    catalogUrlOverride() = url;
}

QUrl EspFirmwareCatalog::catalogUrl()
{
    if (catalogUrlOverride().isValid())
        return catalogUrlOverride();
#if AETHER_FIRMWARE_DEV_OVERRIDES
    const QByteArray env = qgetenv("AETHER_FIRMWARE_CATALOG");
    if (!env.isEmpty()) {
        const QUrl url = QUrl::fromUserInput(QString::fromUtf8(env));
        qCWarning(lcFirmware) << "AETHER_FIRMWARE_CATALOG: using the firmware catalog at"
                              << url.toString() << "instead of" << kCatalogUrl;
        return url;
    }
#endif
    return QUrl(QString::fromLatin1(kCatalogUrl));
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
            const QString package = r.toObject().value("package").toString();
            if (version.isEmpty() || package.isEmpty())
                continue;
            p.version = version;
            p.package = base.resolved(QUrl(package));
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
#if AETHER_FIRMWARE_DEV_OVERRIDES
    for (const QByteArray& hex : qgetenv("AETHER_FIRMWARE_TRUST_KEYS").split(',')) {
        const QByteArray key = QByteArray::fromHex(hex.trimmed());
        if (key.size() == 32) {
            qCWarning(lcFirmware) << "AETHER_FIRMWARE_TRUST_KEYS: also trusting firmware signed by"
                                  << key.toHex();
            keys.append(key);
        }
    }
#endif
    return keys;
}

void EspFirmwareCatalog::setTrustedKeysForTesting(const QList<QByteArray>& keys)
{
    keysOverride() = keys;
}

bool EspFirmwareCatalog::unpack(const QByteArray& package, QHash<QString, QByteArray>* files,
                                QString* problem)
{
    const auto broken = [problem] {
        *problem = tr("The firmware package is damaged.");
        return false;
    };
    files->clear();
    qsizetype at = 0;
    while (at + kTarBlock <= package.size()) {
        const QByteArray header = package.mid(at, kTarBlock);
        if (header.count('\0') == kTarBlock)
            return true;   // the end-of-archive marker
        // The header's checksum: its bytes summed, the checksum field as spaces.
        quint32 sum = 0;
        for (int i = 0; i < kTarBlock; ++i)
            sum += (i >= 148 && i < 156) ? quint32(' ') : quint32(quint8(header[i]));
        bool ok = false;
        const quint32 stored = header.mid(148, 8).trimmed().replace('\0', "").toUInt(&ok, 8);
        if (!ok || stored != sum || !header.mid(257, 5).startsWith("ustar"))
            return broken();
        const char type = header[156];
        if (type != '0' && type != '\0') {
            *problem = tr("The firmware package holds something other than files.");
            return false;
        }
        const QString name = QString::fromLatin1(header.left(100).split('\0').first());
        // One flat folder: no path, nothing hidden, nothing twice.
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.startsWith(QLatin1Char('.')) || header[345] != '\0' || files->contains(name)
            || files->size() >= kMaxPackageFiles) {
            *problem = tr("The firmware package holds an unexpected file.");
            return false;
        }
        const qint64 size = header.mid(124, 12).trimmed().replace('\0', "").toLongLong(&ok, 8);
        at += kTarBlock;
        if (!ok || size < 0 || size > package.size() - at)
            return broken();
        files->insert(name, package.mid(at, size));
        at += (size + kTarBlock - 1) / kTarBlock * kTarBlock;
    }
    return broken();   // no end-of-archive marker: cut short
}

bool EspFirmwareCatalog::verify(const QByteArray& manifest, const QByteArray& signature)
{
    return verifyWith(trustedKeys(), manifest, signature);
}

bool EspFirmwareCatalog::verifyWith(const QList<QByteArray>& keys, const QByteArray& message,
                                    const QByteArray& signature)
{
    QByteArray sig = signature;
    if (sig.size() != 64)
        sig = QByteArray::fromBase64(signature.trimmed());
    if (sig.size() != 64)
        return false;
    for (const QByteArray& key : keys) {
        if (key.size() == 32
            && crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sig.constData()),
                                    reinterpret_cast<const uint8_t*>(key.constData()),
                                    reinterpret_cast<const uint8_t*>(message.constData()),
                                    static_cast<size_t>(message.size()))
                   == 0)
            return true;
    }
    return false;
}

EspFirmwareDownloader::EspFirmwareDownloader(QObject* parent)
    : QObject(parent)
    , m_download(new CappedDownload(this))
{
    connect(m_download, &CappedDownload::finished, this, &EspFirmwareDownloader::onFinished);
    connect(m_download, &CappedDownload::failed, this, &EspFirmwareDownloader::fail);
    connect(m_download, &CappedDownload::progress, this, [this](qint64 done, qint64 total) {
        if (m_stage == Stage::Package)
            emit progress(tr("Downloading %1").arg(m_url.fileName()), done, total);
    });
}

bool EspFirmwareDownloader::isBusy() const
{
    return m_download->isBusy();
}

void EspFirmwareDownloader::fetchCatalog(const QUrl& url)
{
    cancel();
    m_url = url;
    get(url, Stage::Catalog);
}

void EspFirmwareDownloader::fetchRelease(const QUrl& packageUrl, const QString& expectedVersion)
{
    cancel();
    m_url = packageUrl;
    m_expectedVersion = expectedVersion;
    get(packageUrl, Stage::Package);
}

void EspFirmwareDownloader::cancel()
{
    m_download->cancel();
    m_stage = Stage::Idle;
}

void EspFirmwareDownloader::get(const QUrl& url, Stage stage)
{
    m_stage = stage;
    if (stage == Stage::Catalog)
        m_download->get(url, kMaxCatalogBytes, tr("The firmware catalog is too large."));
    else
        m_download->get(url, kMaxPackageBytes, tr("The firmware package is too large."));
}

void EspFirmwareDownloader::fail(const QString& message)
{
    m_stage = Stage::Idle;
    emit failed(message);
}

void EspFirmwareDownloader::onFinished(const QUrl& url, const QByteArray& body)
{
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
    case Stage::Package:
        m_stage = Stage::Idle;
        openPackage(body);
        return;
    case Stage::Idle:
        return;
    }
}

void EspFirmwareDownloader::openPackage(const QByteArray& package)
{
    QHash<QString, QByteArray> files;
    QString problem;
    if (!EspFirmwareCatalog::unpack(package, &files, &problem)) {
        fail(problem);
        return;
    }
    const QByteArray manifestBytes = files.value(kManifestFile);
    const QByteArray signature = files.value(kSignatureFile);
    if (manifestBytes.isEmpty() || manifestBytes.size() > kMaxManifestBytes
        || signature.size() > kMaxSignatureBytes) {
        fail(tr("The firmware package has no usable manifest."));
        return;
    }
    // The signature first: nothing else in the package is looked at until
    // the manifest is known to be a trusted release's.
    if (!EspFirmwareCatalog::verify(manifestBytes, signature)) {
        fail(tr("This firmware is not signed by a key AetherSDR trusts, so it was not "
                "installed."));
        return;
    }
    EspFirmwareManifest manifest;
    if (!EspFirmwareManifest::parse(manifestBytes, &manifest, &problem)) {
        fail(problem);
        return;
    }
    if (!m_expectedVersion.isEmpty() && manifest.version != m_expectedVersion) {
        fail(tr("The firmware list names version %1, but its release is %2.")
                 .arg(m_expectedVersion, manifest.version));
        return;
    }
    QHash<QString, QByteArray> images;
    for (const EspFirmwareManifest::Image& image : manifest.images) {
        const QByteArray bytes = files.value(image.file);
        if (image.size > kMaxImageBytes || static_cast<quint32>(bytes.size()) != image.size
            || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != image.sha256) {
            fail(tr("%1 in the firmware package is not intact.").arg(image.file));
            return;
        }
        images.insert(image.file, bytes);
    }
    emit releaseReady(manifest, images);
}

} // namespace AetherSDR
