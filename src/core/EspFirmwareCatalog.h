#pragma once

#include "EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

namespace AetherSDR {

class CappedDownload;

// Firmware AetherSDR can download and install on ESP32-based controllers.
// The catalog (one JSON file, at kCatalogUrl) lists products and the
// releases of each, newest first, so an operator can move to any of them;
// each release points at its package: an uncompressed ustar archive (.asdr)
// of plain files in one folder, manifest.json (EspFirmwareManifest),
// manifest.json.sig and the images the manifest names. The manifest must
// carry an Ed25519 signature by a trusted firmware-release key (64 bytes raw
// or base64), and its SHA-256s cover every image. Catalog format 1:
//
//   { "format": 1,
//     "products": [ { "name": "Example", "device": "Example Box", "chip": "ESP32-S3",
//                     "description": "...",
//                     "releases": [ { "version": "1.1.0",
//                                     "package": "https://.../example-1.1.0.asdr" },
//                                   { "version": "1.0.0", "package": "..." } ] } ] }
//
// The installer checks that the manifest's version is the one listed.
struct EspFirmwareCatalog {
    // One release of a product: the catalog's products, flattened.
    struct Product {
        QString name;
        QString device;   // the hardware it is for, as shown; may be empty
        QString chip;
        QString description;
        QString version;
        QUrl package;
    };
    QList<Product> products;

    static const char* const kCatalogUrl;
    // kCatalogUrl. A build configured with AETHER_FIRMWARE_DEV_OVERRIDES
    // (off by default, never for release) takes AETHER_FIRMWARE_CATALOG from
    // the environment instead, and logs that it did.
    static QUrl catalogUrl();
    // Tests: another catalog URL (a file:// one keeps them off the network).
    static void setCatalogUrlForTesting(const QUrl& url);

    static bool parse(const QByteArray& json, const QUrl& base, EspFirmwareCatalog* out,
                      QString* problem);
    // The files in a package, by name. Refuses anything but plain files in
    // one folder, a name twice, and a damaged or cut-short archive.
    static bool unpack(const QByteArray& package, QHash<QString, QByteArray>* files,
                       QString* problem);

    // Whether signature (64 bytes, or their base64) is a trusted key's
    // Ed25519 signature of manifest.
    static bool verify(const QByteArray& manifest, const QByteArray& signature);
    // Whether signature (64 bytes, or their base64) is an Ed25519 signature
    // of message by one of keys (32 bytes each).
    static bool verifyWith(const QList<QByteArray>& keys, const QByteArray& message,
                           const QByteArray& signature);
    // The firmware-release public keys (32 bytes each): those built in. A
    // build configured with AETHER_FIRMWARE_DEV_OVERRIDES also trusts any
    // given as hex in AETHER_FIRMWARE_TRUST_KEYS (comma-separated), for
    // testing a publisher's own builds, and logs each one.
    static QList<QByteArray> trustedKeys();
    // Replaces the trusted keys (tests).
    static void setTrustedKeysForTesting(const QList<QByteArray>& keys);
};

// Fetches the catalog, or a release's package, and checks the package: the
// manifest's signature first, then its version and every image. Works for
// http(s) and file URLs alike. Each download is capped as it arrives,
// whatever the server claims.
class EspFirmwareDownloader : public QObject {
    Q_OBJECT

public:
    explicit EspFirmwareDownloader(QObject* parent = nullptr);

    void fetchCatalog(const QUrl& url = EspFirmwareCatalog::catalogUrl());
    // expectedVersion, when given, must be the manifest's version.
    void fetchRelease(const QUrl& packageUrl, const QString& expectedVersion = {});
    void cancel();
    bool isBusy() const;

signals:
    void catalogReady(const AetherSDR::EspFirmwareCatalog& catalog);
    void progress(const QString& step, qint64 done, qint64 total);
    void releaseReady(const AetherSDR::EspFirmwareManifest& manifest,
                      const QHash<QString, QByteArray>& files);
    void failed(const QString& message);

private:
    enum class Stage { Idle, Catalog, Package };

    void get(const QUrl& url, Stage stage);
    void onFinished(const QUrl& url, const QByteArray& body);
    void openPackage(const QByteArray& package);
    void fail(const QString& message);

    CappedDownload* m_download;
    Stage m_stage{Stage::Idle};
    QUrl m_url;
    QString m_expectedVersion;
};

} // namespace AetherSDR
