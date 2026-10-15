// EspFirmwareCatalog and EspFirmwareDownloader over file:// URLs: a release
// package (.asdr, a ustar archive) installs only with a manifest signed by a
// trusted Ed25519 key, and only with every image intact; a damaged or odd
// archive is refused; the catalog resolves package links against its own
// location. Archives and keys are made here; no network.

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

// One ustar entry, as GNU tar --format=ustar writes it.
QByteArray tarEntry(const QByteArray& name, const QByteArray& data, char type = '0')
{
    QByteArray h(512, '\0');
    const auto put = [&h](int at, const QByteArray& field) { h.replace(at, field.size(), field); };
    put(0, name.left(100));
    put(100, "0000644");
    put(108, "0000000");
    put(116, "0000000");
    put(124, QByteArray::number(data.size(), 8).rightJustified(11, '0'));
    put(136, "00000000000");
    put(148, "        ");
    h[156] = type;
    put(257, QByteArray("ustar\0" "00", 8));
    quint32 sum = 0;
    for (char c : h)
        sum += quint8(c);
    put(148, QByteArray::number(sum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
    QByteArray body = data;
    body.append(QByteArray((512 - data.size() % 512) % 512, '\0'));
    return h + body;
}

QByteArray tarOf(const QList<QPair<QByteArray, QByteArray>>& files)
{
    QByteArray t;
    for (const auto& f : files)
        t += tarEntry(f.first, f.second);
    return t + QByteArray(1024, '\0');
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
                              QJsonObject{{"version", "1.1.0"}, {"package", "example/example-1.1.0.asdr"}},
                              QJsonObject{{"version", "1.0.0"}, {"package", "example/example-1.0.0.asdr"}},
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
    check(c.products.size() == 2, "each release is listed; ones without a package are skipped");
    check(c.products.value(0).version == QStringLiteral("1.1.0")
              && c.products.value(1).version == QStringLiteral("1.0.0"),
          "releases keep the catalog's order, with their versions");
    check(c.products.value(1).name == QStringLiteral("Example")
              && c.products.value(1).device == QStringLiteral("Example Box")
              && c.products.value(1).chip == QStringLiteral("ESP32-S3"),
          "every release carries its product's name, device and chip");
    check(c.products.value(0).package
              == QUrl("https://example.org/firmware/example/example-1.1.0.asdr"),
          "package links resolve against the catalog");
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

QByteArray testApp()
{
    QByteArray app(5000, 'x');
    app[0] = static_cast<char>(0xE9);
    return app;
}

void testDownload()
{
    const Key trusted('a'), other('b');
    EspFirmwareCatalog::setTrustedKeysForTesting({trusted.pub});
    QTemporaryDir dir;
    const QDir d(dir.path());
    const QByteArray app = testApp();
    const QByteArray manifest = manifestFor(app);
    const QString path = d.filePath("example-2.0.0.asdr");
    const QUrl url = QUrl::fromLocalFile(path);
    const auto package = [&](const QByteArray& m, const QByteArray& sig, const QByteArray& a) {
        write(path, tarOf({{"app.bin", a}, {"manifest.json", m}, {"manifest.json.sig", sig}}));
    };

    package(manifest, trusted.sign(manifest).toBase64(), app);
    Result r = fetch(url);
    check(r.ok, "a signed package with intact images opens");
    check(r.manifest.version == QStringLiteral("2.0.0") && r.files.value("app.bin") == app
              && r.files.size() == 1,
          "with its manifest and only the images it names");

    r = fetch(url, QStringLiteral("2.0.0"));
    check(r.ok, "the version the catalog lists matches");
    r = fetch(url, QStringLiteral("2.1.0"));
    check(!r.ok && r.message.contains(QStringLiteral("2.1.0")),
          "a release that is not the version the catalog lists is refused");

    package(manifest, trusted.sign(manifest).toBase64(), app.left(4999) + "y");
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("intact")), "a changed image is refused");

    package(manifest, other.sign(manifest), app);
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("not signed")),
          "a manifest signed by an untrusted key is refused");

    write(path, tarOf({{"app.bin", app}, {"manifest.json", manifest}}));
    r = fetch(url);
    check(!r.ok, "a package without a signature is refused");

    write(path, tarOf({{"manifest.json", manifest}, {"manifest.json.sig", trusted.sign(manifest)}}));
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("app.bin")), "a package missing an image is refused");

    // Signed, but invalid as a manifest: still refused, after the signature.
    const QByteArray bad = manifest.left(manifest.size() / 2);
    package(bad, trusted.sign(bad), app);
    r = fetch(url);
    check(!r.ok && r.message.contains(QStringLiteral("manifest")), "a broken manifest is refused");
}

// Anything but plain files in one folder, or a damaged archive, is refused
// before any of it is trusted.
void testUnpack()
{
    QHash<QString, QByteArray> files;
    QString problem;
    const QByteArray good = tarOf({{"a.bin", "abc"}, {"b.bin", QByteArray(700, 'b')}});
    check(EspFirmwareCatalog::unpack(good, &files, &problem) && files.size() == 2
              && files.value("a.bin") == "abc" && files.value("b.bin") == QByteArray(700, 'b'),
          "a package's files unpack by name");

    check(!EspFirmwareCatalog::unpack(good.left(good.size() - 1024 - 100), &files, &problem),
          "a cut-short package is refused");
    check(!EspFirmwareCatalog::unpack(good.left(512 + 512), &files, &problem),
          "a package without its end marker is refused");

    QByteArray corrupt = good;
    corrupt[10] = 'Z';   // in the first header's name: the checksum no longer matches
    check(!EspFirmwareCatalog::unpack(corrupt, &files, &problem), "a damaged header is refused");

    for (const QByteArray& name : {QByteArray("../a.bin"), QByteArray("dir/a.bin"),
                                   QByteArray(".hidden"), QByteArray("a\\b.bin")}) {
        check(!EspFirmwareCatalog::unpack(tarOf({{name, "x"}}), &files, &problem),
              "a name with a path, or a hidden one, is refused");
    }
    check(!EspFirmwareCatalog::unpack(tarOf({{"a.bin", "x"}, {"a.bin", "y"}}), &files, &problem),
          "a name twice is refused");
    check(!EspFirmwareCatalog::unpack(tarEntry("link", {}, '2') + QByteArray(1024, '\0'), &files,
                                      &problem),
          "a link is refused");
    check(!EspFirmwareCatalog::unpack(tarEntry("dir", {}, '5') + QByteArray(1024, '\0'), &files,
                                      &problem),
          "a folder is refused");

    // A size larger than the archive.
    QByteArray huge = tarEntry("a.bin", "abc");
    QByteArray h = huge.left(512);
    h.replace(124, 11, "77777777777");
    h.replace(148, 8, "        ");
    quint32 sum = 0;
    for (char c : h)
        sum += quint8(c);
    h.replace(148, 8, QByteArray::number(sum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
    check(!EspFirmwareCatalog::unpack(h + huge.mid(512) + QByteArray(1024, '\0'), &files, &problem),
          "an entry larger than the package is refused");

    QList<QPair<QByteArray, QByteArray>> many;
    for (int i = 0; i < 40; ++i)
        many.append({"f" + QByteArray::number(i), "x"});
    check(!EspFirmwareCatalog::unpack(tarOf(many), &files, &problem), "too many files are refused");
}

// Every download is capped as it arrives: more than the limit is refused.
void testLimits()
{
    QTemporaryDir dir;
    const QDir d(dir.path());

    // 32 MiB: the package cap is 17 MiB.
    write(d.filePath("big.asdr"), QByteArray(32 * 1024 * 1024, '\0'));
    const Result r = fetch(QUrl::fromLocalFile(d.filePath("big.asdr")));
    check(!r.ok && r.message.contains(QStringLiteral("too large")), "an oversized package is refused");

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
    testUnpack();
    testLimits();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("esp_firmware_catalog_test: all checks passed\n");
    return 0;
}
