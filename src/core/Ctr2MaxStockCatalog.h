#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

namespace AetherSDR {

class CappedDownload;

// The CTR2-Max's stock firmware as Lynovation publishes it for AetherSDR to
// fetch: a catalog on Lynovation's site, signed with Lynovation's own
// Ed25519 key, listing each release's merged flash image (.bin, the same file
// as the download on lynovation.com) by URL, size and SHA-256. AetherSDR
// fetches the catalog and its signature, then a release's .bin, from
// Lynovation's site, and installs it as a file opened by hand
// (Ctr2MaxStockFirmware): AetherSDR never hosts the firmware. Format 1:
//
//   catalog.json:
//   { "format": 1,
//     "products": [ { "name": "CTR2-Max firmware", "device": "CTR2-Max", "chip": "ESP32-S3",
//                     "releases": [ { "version": "v1.02.00n",
//                                     "file": "https://ctr2.lynovation.com/.../CTR2-Max_v10200n.bin",
//                                     "size": 1622720,
//                                     "sha256": "<64 hex digits>" } ] } ] }
//
//   catalog.json.sig, beside it: the Ed25519 signature of catalog.json's
//   exact bytes, base64 (as `openssl pkeyutl -sign -rawin ... | base64 -w0`).
//
// Releases are listed newest first. Nothing in a catalog whose signature is
// not a trusted Lynovation key's is used.
struct Ctr2MaxStockCatalog {
    struct Release {
        QString version;   // "v1.02.00n"
        QUrl file;
        qint64 size{0};
        QByteArray sha256;
    };
    QList<Release> releases;

    static const char* const kCatalogUrl;
    // kCatalogUrl. A build configured with AETHER_FIRMWARE_DEV_OVERRIDES
    // (never for release) takes AETHER_LYNOVATION_CATALOG instead, and logs it.
    static QUrl catalogUrl();
    static void setCatalogUrlForTesting(const QUrl& url);
    // Lynovation's release keys (Ed25519, 32 bytes each): those built in. A
    // dev-overrides build also trusts any given as hex in
    // AETHER_LYNOVATION_TRUST_KEYS (comma-separated), for Lynovation to test
    // its own catalog before publishing it.
    static QList<QByteArray> trustedKeys();
    static void setTrustedKeysForTesting(const QList<QByteArray>& keys);

    // Checks signature against the trusted keys first, then reads json.
    static bool parse(const QByteArray& json, const QByteArray& signature, const QUrl& base,
                      Ctr2MaxStockCatalog* out, QString* problem);
};

// Fetches the catalog (and its signature), or a release's .bin, checking the
// .bin against the catalog's size and SHA-256. Http(s) and file URLs alike;
// each download is capped as it arrives.
class Ctr2MaxStockDownloader : public QObject {
    Q_OBJECT

public:
    explicit Ctr2MaxStockDownloader(QObject* parent = nullptr);

    void fetchCatalog(const QUrl& url = Ctr2MaxStockCatalog::catalogUrl());
    void fetchRelease(const Ctr2MaxStockCatalog::Release& release);
    void cancel();
    bool isBusy() const;

signals:
    void catalogReady(const AetherSDR::Ctr2MaxStockCatalog& catalog);
    void progress(qint64 done, qint64 total);
    // The release's .bin, as checked against the catalog.
    void releaseReady(const AetherSDR::Ctr2MaxStockCatalog::Release& release,
                      const QByteArray& image);
    void failed(const QString& message);

private:
    enum class Stage { Idle, Catalog, Signature, Release };

    void onFinished(const QUrl& url, const QByteArray& body);
    void fail(const QString& message);

    CappedDownload* m_download;
    Stage m_stage{Stage::Idle};
    QUrl m_catalogUrl;
    QByteArray m_catalog;
    Ctr2MaxStockCatalog::Release m_release;
};

} // namespace AetherSDR
