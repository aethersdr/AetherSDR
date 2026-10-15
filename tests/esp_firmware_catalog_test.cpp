// EspFirmwareCatalog and EspFirmwareDownloader over file:// URLs: a release
// installs only with a manifest signed by a trusted Ed25519 key, and only
// with every image intact; the catalog resolves manifest links against its
// own location. Keys are made here; no network.

#include "core/EspFirmwareCatalog.h"

extern "C" {
#include "monocypher-ed25519.h"
}

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::usleep(200);
    }
    return predicate();
}

struct Key {
    QByteArray secret{64, '\0'};
    QByteArray pub{32, '\0'};
    explicit Key(char seed)
    {
        uint8_t s[32];
        for (int i = 0; i < 32; ++i)
            s[i] = static_cast<uint8_t>(seed + i);
        crypto_ed25519_key_pair(reinterpret_cast<uint8_t*>(secret.data()),
                                reinterpret_cast<uint8_t*>(pub.data()), s);
    }
    QByteArray sign(const QByteArray& message) const
    {
        QByteArray sig(64, '\0');
        crypto_ed25519_sign(reinterpret_cast<uint8_t*>(sig.data()),
                            reinterpret_cast<const uint8_t*>(secret.constData()),
                            reinterpret_cast<const uint8_t*>(message.constData()),
                            static_cast<size_t>(message.size()));
        return sig;
    }
};

void write(const QString& path, const QByteArray& data)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

QString hex(quint32 v)
{
    return QStringLiteral("0x%1").arg(v, 0, 16);
}

QByteArray manifestFor(const QByteArray& app)
{
    return QJsonDocument(QJsonObject{
                             {"format", 1},
                             {"product", "Example"},
                             {"version", "2.0.0"},
                             {"chip", "ESP32-S3"},
                             {"flashSize", hex(0x400000)},
                             {"boot", QJsonObject{{"otadata", hex(0xE000)},
                                                  {"slots", QJsonArray{hex(0x10000), hex(0x110000)}},
                                                  {"ownSlots", QJsonArray{1}}}},
                             {"images", QJsonArray{QJsonObject{
                                            {"file", "app.bin"},
                                            {"offset", hex(0x110000)},
                                            {"size", app.size()},
                                            {"sha256", QString::fromLatin1(QCryptographicHash::hash(
                                                           app, QCryptographicHash::Sha256).toHex())}}}},
                         })
        .toJson();
}

void testVerify()
{
    const Key trusted('a'), other('b');
    EspFirmwareCatalog::setTrustedKeysForTesting({trusted.pub});
    const QByteArray m = "manifest bytes";
    check(EspFirmwareCatalog::verify(m, trusted.sign(m)), "a trusted signature verifies");
    check(EspFirmwareCatalog::verify(m, trusted.sign(m).toBase64() + "\n"),
          "and in base64, with a trailing newline");
    check(!EspFirmwareCatalog::verify(m + " ", trusted.sign(m)), "a changed manifest does not");
    check(!EspFirmwareCatalog::verify(m, other.sign(m)), "another key's signature does not");
    check(!EspFirmwareCatalog::verify(m, QByteArray(10, 'x')), "garbage does not");
    EspFirmwareCatalog::setTrustedKeysForTesting({});
    check(!EspFirmwareCatalog::verify(m, trusted.sign(m)), "nothing verifies with no trusted keys");
}

void testCatalog()
{
    const QByteArray json = QJsonDocument(QJsonObject{
        {"format", 1},
        {"products", QJsonArray{
             QJsonObject{{"name", "Example"}, {"device", "Example Box"}, {"chip", "ESP32-S3"},
                         {"releases", QJsonArray{
                              QJsonObject{{"version", "1.1.0"}, {"manifest", "example/1.1.0/manifest.json"}},
                              QJsonObject{{"version", "1.0.0"}, {"manifest", "example/1.0.0/manifest.json"}},
                              QJsonObject{{"version", "0.9.0"}},
                          }}},
             QJsonObject{{"name", "No releases"}},
         }},
    }).toJson();
    EspFirmwareCatalog c;
    QString problem;
    check(EspFirmwareCatalog::parse(json, QUrl("https://example.org/firmware/catalog.json"), &c,
                                    &problem),
          "the catalog parses");
    check(c.products.size() == 2, "each release is listed; ones without a manifest are skipped");
    check(c.products.value(0).version == QStringLiteral("1.1.0")
              && c.products.value(1).version == QStringLiteral("1.0.0"),
          "releases keep the catalog's order, with their versions");
    check(c.products.value(1).name == QStringLiteral("Example")
              && c.products.value(1).device == QStringLiteral("Example Box")
              && c.products.value(1).chip == QStringLiteral("ESP32-S3"),
          "every release carries its product's name, device and chip");
    check(c.products.value(0).manifest
              == QUrl("https://example.org/firmware/example/1.1.0/manifest.json"),
          "manifest links resolve against the catalog");
    check(!EspFirmwareCatalog::parse(R"({"format":2})", QUrl(), &c, &problem),
          "a newer catalog format is refused");
}

struct Result {
    bool done{false};
    bool ok{false};
    QString message;
    EspFirmwareManifest manifest;
    QHash<QString, QByteArray> files;
};

Result fetch(const QUrl& url, const QString& expectedVersion = {})
{
    EspFirmwareDownloader d;
    Result r;
    QObject::connect(&d, &EspFirmwareDownloader::releaseReady,
                     [&](const EspFirmwareManifest& m, const QHash<QString, QByteArray>& f) {
                         r = {true, true, {}, m, f};
                     });
    QObject::connect(&d, &EspFirmwareDownloader::failed, [&](const QString& message) {
        r.done = true;
        r.message = message;
    });
    d.fetchRelease(url, expectedVersion);
    waitUntil([&] { return r.done; });
    return r;
}

void testDownload()
{
    const Key trusted('a'), other('b');
    EspFirmwareCatalog::setTrustedKeysForTesting({trusted.pub});
    QTemporaryDir dir;
    const QDir d(dir.path());
    QByteArray app(5000, 'x');
    app[0] = static_cast<char>(0xE9);
    const QByteArray manifest = manifestFor(app);
    write(d.filePath("app.bin"), app);
    write(d.filePath("manifest.json"), manifest);
    write(d.filePath("manifest.json.sig"), trusted.sign(manifest).toBase64());
    const QUrl url = QUrl::fromLocalFile(d.filePath("manifest.json"));

    Result r = fetch(url);
    check(r.ok, "a signed release with intact images downloads");
    check(r.manifest.version == QStringLiteral("2.0.0") && r.files.value("app.bin") == app,
          "with its manifest and image");

    r = fetch(url, QStringLiteral("2.0.0"));
    check(r.ok, "the version the catalog lists matches");
    r = fetch(url, QStringLiteral("2.1.0"));
    check(!r.ok && r.message.contains(QStringLiteral("2.1.0")),
          "a release that is not the version the catalog lists is refused");

    write(d.filePath("app.bin"), app.left(4999) + "y");
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("intact")), "a changed image is refused");
    write(d.filePath("app.bin"), app);

    write(d.filePath("manifest.json.sig"), other.sign(manifest));
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("not signed")),
          "a manifest signed by an untrusted key is refused");

    QFile::remove(d.filePath("manifest.json.sig"));
    r = fetch(url);
    check(!r.ok, "a manifest without a signature is refused");

    // Signed, but invalid as a manifest: still refused, after the signature.
    const QByteArray bad = manifest.left(manifest.size() / 2);
    write(d.filePath("manifest.json"), bad);
    write(d.filePath("manifest.json.sig"), trusted.sign(bad));
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("manifest")), "a broken manifest is refused");
}

// Every download is capped as it arrives: more than the limit is refused,
// whatever its content.
void testLimits()
{
    const Key trusted('a');
    EspFirmwareCatalog::setTrustedKeysForTesting({trusted.pub});
    QTemporaryDir dir;
    const QDir d(dir.path());
    QByteArray app(5000, 'x');
    app[0] = static_cast<char>(0xE9);
    const QByteArray manifest = manifestFor(app);
    const QUrl url = QUrl::fromLocalFile(d.filePath("manifest.json"));

    // A manifest of 32 MiB: the manifest cap is 64 KiB.
    write(d.filePath("manifest.json"), manifest + QByteArray(32 * 1024 * 1024, ' '));
    Result r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("too large")), "an oversized manifest is refused");

    write(d.filePath("manifest.json"), manifest);
    write(d.filePath("manifest.json.sig"), trusted.sign(manifest).toBase64() + QByteArray(4096, '\n'));
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("too large")), "an oversized signature is refused");

    write(d.filePath("manifest.json.sig"), trusted.sign(manifest).toBase64());
    write(d.filePath("app.bin"), app + QByteArray(1024 * 1024, 'z'));
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("intact")),
          "an image longer than its manifest says is refused");

    write(d.filePath("catalog.json"), QByteArray(R"({"format":1,"products":[]})")
                                          + QByteArray(1024 * 1024, ' '));
    EspFirmwareDownloader cd;
    bool done = false;
    QString message;
    QObject::connect(&cd, &EspFirmwareDownloader::catalogReady,
                     [&](const EspFirmwareCatalog&) { done = true; });
    QObject::connect(&cd, &EspFirmwareDownloader::failed, [&](const QString& m) {
        done = true;
        message = m;
    });
    cd.fetchCatalog(QUrl::fromLocalFile(d.filePath("catalog.json")));
    waitUntil([&] { return done; });
    check(message.contains(QStringLiteral("too large")), "an oversized catalog is refused");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testVerify();
    testCatalog();
    testDownload();
    testLimits();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("esp_firmware_catalog_test: all checks passed\n");
    return 0;
}
