#pragma once

#include "EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace AetherSDR {

// Firmware AetherSDR can download and install on ESP32-based controllers.
// The catalog (one JSON file, at kCatalogUrl) lists products and the
// releases of each, newest first, so an operator can move to any of them;
// each release points at its manifest (EspFirmwareManifest). The manifest
// must carry an Ed25519 signature by a trusted firmware-release key
// (manifest URL + ".sig", 64 bytes raw or base64), and its SHA-256s cover
// every image, which are fetched from beside it. Catalog format 1:
//
//   { "format": 1,
//     "products": [ { "name": "Example", "device": "Example Box", "chip": "ESP32-S3",
//                     "description": "...",
//                     "releases": [ { "version": "1.1.0",
//                                     "manifest": "https://.../1.1.0/manifest.json" },
//                                   { "version": "1.0.0", "manifest": "..." } ] } ] }
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
        QUrl manifest;
    };
    QList<Product> products;

    static const char* const kCatalogUrl;
    // kCatalogUrl, or AETHER_FIRMWARE_CATALOG from the environment.
    static QUrl catalogUrl();

    static bool parse(const QByteArray& json, const QUrl& base, EspFirmwareCatalog* out,
                      QString* problem);

    // Whether signature (64 bytes, or their base64) is a trusted key's
    // Ed25519 signature of manifest.
    static bool verify(const QByteArray& manifest, const QByteArray& signature);
    // The firmware-release public keys (32 bytes each): those built in, and
    // any given as hex in AETHER_FIRMWARE_TRUST_KEYS (comma-separated), for
    // testing a publisher's own builds.
    static QList<QByteArray> trustedKeys();
    // Replaces the trusted keys (tests).
    static void setTrustedKeysForTesting(const QList<QByteArray>& keys);
};

// Fetches the catalog, or a release: a manifest, its signature and its
// images, every one checked. Works for http(s) and file URLs alike, so a
// release unpacked on disk installs the same way.
class EspFirmwareDownloader : public QObject {
    Q_OBJECT

public:
    explicit EspFirmwareDownloader(QObject* parent = nullptr);

    void fetchCatalog(const QUrl& url = EspFirmwareCatalog::catalogUrl());
    // expectedVersion, when given, must be the manifest's version.
    void fetchRelease(const QUrl& manifestUrl, const QString& expectedVersion = {});
    void cancel();
    bool isBusy() const { return m_reply != nullptr; }

signals:
    void catalogReady(const AetherSDR::EspFirmwareCatalog& catalog);
    void progress(const QString& step, qint64 done, qint64 total);
    void releaseReady(const AetherSDR::EspFirmwareManifest& manifest,
                      const QHash<QString, QByteArray>& files);
    void failed(const QString& message);

private:
    enum class Stage { Idle, Catalog, Manifest, Signature, Image };

    void get(const QUrl& url, Stage stage);
    void onFinished();
    void nextImage();
    void fail(const QString& message);

    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply{nullptr};
    Stage m_stage{Stage::Idle};
    QUrl m_url;
    QString m_expectedVersion;
    QByteArray m_manifestBytes;
    EspFirmwareManifest m_manifest;
    QHash<QString, QByteArray> m_files;
    int m_image{0};
};

} // namespace AetherSDR
