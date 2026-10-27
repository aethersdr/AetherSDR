#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateSettings.h"
#include "core/UpdateStager.h"
#include "core/ZipArchive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>

#include "monocypher-ed25519.h"

#include <algorithm>
#include <filesystem>

// UpdateStager against tests/fixtures/update: a v26.10.2 install and the
// v26.10.3 manifest and patch bundle that tools/build_update_manifest.py
// built from it. The bundle is the CI tool's real output, so applying it here
// proves the app's zstd decoder reads `zstd --patch-from` as written. Every
// case from docs/architecture/update-manifest.md's negative list is here.
// Signatures use a test key made at runtime; no private key is in the tree.
// No sockets: downloads are file:// URLs.

using namespace AetherSDR;
namespace fs = std::filesystem;

namespace {

const QString kFixture = QStringLiteral(UPDATE_FIXTURE_DIR);

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

struct TestKey {
    uint8_t secret[64];
    uint8_t pub[32];
    TestKey()
    {
        uint8_t seed[32];
        for (int i = 0; i < 32; ++i) {
            seed[i] = static_cast<uint8_t>(0x53 + i);
        }
        crypto_ed25519_key_pair(secret, pub, seed);
    }
    QByteArray sign(const QByteArray& message) const
    {
        uint8_t sig[64];
        crypto_ed25519_sign(sig, secret, reinterpret_cast<const uint8_t*>(message.constData()),
                            static_cast<size_t>(message.size()));
        return QByteArray(reinterpret_cast<const char*>(sig), 64);
    }
    QByteArray publicKey() const { return QByteArray(reinterpret_cast<const char*>(pub), 32); }
};

QJsonObject fixtureManifest()
{
    return QJsonDocument::fromJson(readFile(kFixture + QStringLiteral("/release/manifest.json"))).object();
}

QByteArray bytes(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

UpdateStager::Request request(const QString& running = QStringLiteral("26.10.2"))
{
    UpdateStager::Request r;
    r.runningVersion = running;
    r.platform = QStringLiteral("windows");
    r.arch = QStringLiteral("x86_64");
    r.githubLatestTag = QStringLiteral("v26.10.3");
    return r;
}

// A copy of the fixture install that a test may modify.
QString copyInstall(const QTemporaryDir& tmp, const QString& name = QStringLiteral("install"))
{
    const QString dst = tmp.filePath(name);
    std::error_code ec;
    fs::copy(fs::path(kFixture.toStdString() + "/install-v26.10.2"), fs::path(dst.toStdString()),
             fs::copy_options::recursive, ec);
    return dst;
}

QByteArray bundleBytes()
{
    return readFile(kFixture
                    + QStringLiteral("/release/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar"));
}

// The v26.10.3 release tree, made by applying the fixture's patch: what the
// release's portable zip holds.
QHash<QString, QByteArray> releaseTree(const QTemporaryDir& tmp)
{
    const QString install = copyInstall(tmp, QStringLiteral("for-zip"));
    UpdateManifest m;
    QString problem;
    UpdateManifest::parse(readFile(kFixture + QStringLiteral("/release/manifest.json")), &m, &problem);
    const QString tree = tmp.filePath(QStringLiteral("v26.10.3-tree"));
    UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0], m.payloads[0].files, tree,
                             &problem);
    QHash<QString, QByteArray> files;
    for (const UpdateManifest::File& f : m.payloads[0].files) {
        files.insert(f.path, readFile(tree + QLatin1Char('/') + f.path));
    }
    return files;
}

// A portable zip the way Compress-Archive writes one: '\' separators and
// folder entries. extra adds (or, with an empty value, drops) entries.
// reversed: the same files in the other order, so the same size and other
// bytes.
QByteArray portableZip(const QHash<QString, QByteArray>& tree, const QList<ZipEntryData>& extra = {},
                       const QStringList& drop = {}, bool reversed = false)
{
    QList<ZipEntryData> entries{{QStringLiteral("plugins\\"), {}}};
    QStringList names = tree.keys();
    std::sort(names.begin(), names.end());
    if (reversed) {
        std::reverse(names.begin(), names.end());
    }
    for (const QString& name : names) {
        if (!drop.contains(name)) {
            entries.append({QString(name).replace(QLatin1Char('/'), QLatin1Char('\\')), tree.value(name)});
        }
    }
    entries.append(extra);
    return writeDeflatedZip(entries);
}

QString sha256Hex(const QByteArray& b)
{
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}

} // namespace

class UpdateStagerTest : public QObject {
    Q_OBJECT

    TestKey m_key;
    std::unique_ptr<TestSettingsProfile> m_profile;

    UpdateManifest parsed(const QJsonObject& o)
    {
        UpdateManifest m;
        QString problem;
        if (!UpdateManifest::parse(bytes(o), &m, &problem)) {
            qWarning() << problem;
        }
        return m;
    }

    bool parses(const QJsonObject& o)
    {
        UpdateManifest m;
        QString problem;
        return UpdateManifest::parse(bytes(o), &m, &problem);
    }

private slots:
    void initTestCase()
    {
        m_profile = std::make_unique<TestSettingsProfile>(QStringLiteral("update-stager"));
        QVERIFY(m_profile->isValid());
        AppSettings::instance().load();
        UpdateStager::setTrustedKeysForTesting({m_key.publicKey()});
    }

    // ── The trust anchor ────────────────────────────────────────────────

    void embeddedKeyIsTheCommittedPublicKey()
    {
        // The app's built-in key and packaging/update-signing.pub must be
        // the same key, or every real manifest would be refused.
        const QByteArray pem = readFile(QStringLiteral(UPDATE_SIGNING_PUB));
        QByteArray b64;
        for (const QByteArray& line : pem.split('\n')) {
            if (!line.startsWith("-----")) {
                b64 += line.trimmed();
            }
        }
        const QByteArray der = QByteArray::fromBase64(b64);
        QCOMPARE(der.size(), 44);   // SPKI: a 12-byte Ed25519 prefix and the raw key
        QCOMPARE(der.left(12).toHex(), QByteArray("302a300506032b6570032100"));
        QCOMPARE(UpdateStager::builtInKey(), der.right(32));
    }

    void thePublishedManifestVerifiesWithTheBuiltInKey()
    {
        // The v26.10.3 manifest and signature exactly as the release
        // workflow published them on updates.aethersdr.com (2026-10-24):
        // CI's signing key is the one the app trusts, and a real manifest
        // parses and offers v26.10.2 its patch.
        const QString dir = kFixture + QStringLiteral("/v26.10.3-published/");
        const QByteArray json = readFile(dir + QStringLiteral("manifest.json"));
        const QByteArray sig = readFile(dir + QStringLiteral("manifest.json.sig"));
        UpdateStager::setTrustedKeysForTesting({UpdateStager::builtInKey()});
        const bool trusted = UpdateStager::verify(json, sig);
        QByteArray tampered = json;
        tampered.replace("26.10.3", "26.10.9");
        const bool tamperedTrusted = UpdateStager::verify(tampered, sig);
        UpdateStager::setTrustedKeysForTesting({m_key.publicKey()});
        QVERIFY(trusted);
        QVERIFY(!tamperedTrusted);
        UpdateManifest m;
        QString problem;
        QVERIFY2(UpdateManifest::parse(json, &m, &problem), qPrintable(problem));
        QCOMPARE(m.keyId, QStringLiteral("788fb6e31f7ecba4"));
        QCOMPARE(m.payloads.size(), 3);
        const UpdateManifest::Payload* p = nullptr;
        const UpdateManifest::Patch* q = nullptr;
        for (const char* arch : {"x86_64", "arm64"}) {
            UpdateStager::Request r = request();
            r.platform = QString::fromLatin1(arch) == QLatin1String("x86_64") ? QStringLiteral("windows")
                                                                              : QStringLiteral("linux");
            r.arch = QString::fromLatin1(arch);
            QCOMPARE(UpdateStager::choose(m, r, {}, &p, &q), UpdateStager::Decision::Patch);
            QCOMPARE(q->from, QStringLiteral("26.10.2"));
        }
    }

    // ── Signature ───────────────────────────────────────────────────────

    void signatureIsCheckedOnExactBytes()
    {
        const QByteArray m = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        QVERIFY(UpdateStager::verify(m, m_key.sign(m)));
        QByteArray tampered = m;
        tampered[tampered.size() / 2] = tampered[tampered.size() / 2] ^ 1;
        QVERIFY(!UpdateStager::verify(tampered, m_key.sign(m)));
        QVERIFY(!UpdateStager::verify(m, QByteArray(64, '\0')));
        QVERIFY(!UpdateStager::verify(m, m_key.sign(m).toBase64()));   // raw 64 bytes only
        QVERIFY(!UpdateStager::verify(m, m_key.sign(m).left(63)));
        QVERIFY(!UpdateStager::verify(m, m_key.sign(m) + "x"));   // a valid signature plus trailing bytes
    }

    // ── Parse ───────────────────────────────────────────────────────────

    void fixtureParses()
    {
        const UpdateManifest m = parsed(fixtureManifest());
        QCOMPARE(m.tag, QStringLiteral("v26.10.3"));
        QCOMPARE(m.payloads.size(), 1);
        QCOMPARE(m.payloads[0].patches.size(), 1);
        QCOMPARE(m.payloads[0].patches[0].ops.size(), 5);
    }

    void parseRefusesWhatTheContractRefuses()
    {
        const QJsonObject good = fixtureManifest();
        auto with = [&](auto&& edit) {
            QJsonObject o = good;
            edit(o);
            return o;
        };
        auto payload0 = [](QJsonObject& o, auto&& edit) {
            QJsonArray ps = o[QStringLiteral("payloads")].toArray();
            QJsonObject p = ps[0].toObject();
            edit(p);
            ps[0] = p;
            o[QStringLiteral("payloads")] = ps;
        };
        auto op = [&](QJsonObject& o, int i, const char* key, const QJsonValue& v) {
            payload0(o, [&](QJsonObject& p) {
                QJsonArray qs = p[QStringLiteral("patches")].toArray();
                QJsonObject q = qs[0].toObject();
                QJsonArray ops = q[QStringLiteral("ops")].toArray();
                QJsonObject x = ops[i].toObject();
                x[QLatin1String(key)] = v;
                ops[i] = x;
                q[QStringLiteral("ops")] = ops;
                qs[0] = q;
                p[QStringLiteral("patches")] = qs;
            });
        };
        QVERIFY(parses(good));
        QVERIFY(!parses(with([](QJsonObject& o) { o[QStringLiteral("schema")] = 2; })));
        QVERIFY(!parses(with([](QJsonObject& o) { o[QStringLiteral("version")] = QStringLiteral("26.10.4"); })));
        QVERIFY(!parses(with([&](QJsonObject& o) {
            payload0(o, [](QJsonObject& p) {
                QJsonObject f = p[QStringLiteral("full")].toObject();
                f[QStringLiteral("url")] = QStringLiteral("https://evil.example/AetherSDR.zip");
                p[QStringLiteral("full")] = f;
            });
        })));
        QVERIFY(!parses(with([&](QJsonObject& o) {
            payload0(o, [](QJsonObject& p) {
                QJsonObject f = p[QStringLiteral("full")].toObject();
                f[QStringLiteral("url")] = QStringLiteral(
                    "https://github.com/aethersdr/AetherSDR/releases/download/v26.10.2/AetherSDR.zip");
                p[QStringLiteral("full")] = f;
            });
        })));   // another release's download
        QVERIFY(!parses(with([&](QJsonObject& o) {
            payload0(o, [](QJsonObject& p) {
                QJsonObject f = p[QStringLiteral("full")].toObject();
                f[QStringLiteral("url")] = QStringLiteral(
                    "https://github.com/aethersdr/AetherSDR/releases/download/v26.10.3/..%2F..%2Fx.zip");
                p[QStringLiteral("full")] = f;
            });
        })));   // an encoded traversal: refused by the app's one download allowlist
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 2, "window_log", 32); })));
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 2, "path", QStringLiteral("../AetherSDR.exe")); })));
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 2, "path", QStringLiteral("/etc/passwd")); })));
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 2, "path", QStringLiteral("C:/x.dll")); })));
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 2, "member", QStringLiteral("../x")); })));
        QVERIFY(!parses(with([&](QJsonObject& o) { op(o, 3, "member", QStringLiteral("000002.zst")); })));
        QVERIFY(!parses(with([&](QJsonObject& o) {   // a removal after a write
            op(o, 2, "op", QStringLiteral("remove"));
            op(o, 1, "op", QStringLiteral("symlink"));
            op(o, 1, "target", QStringLiteral("AetherSDR.exe"));
        })));
        QVERIFY(!parses(with([&](QJsonObject& o) {
            op(o, 3, "op", QStringLiteral("symlink"));
            op(o, 3, "target", QStringLiteral("../../../etc"));
        })));
        QVERIFY(!parses(with([&](QJsonObject& o) {
            op(o, 3, "op", QStringLiteral("symlink"));
            op(o, 3, "target", QStringLiteral("/etc"));
        })));
        QVERIFY(!parses(with([](QJsonObject& o) { o[QStringLiteral("payloads")] = QJsonArray(); })));
    }

    // ── Choose ──────────────────────────────────────────────────────────

    void chooseRefusesReplayFreezeAndOldReleases()
    {
        const UpdateManifest m = parsed(fixtureManifest());
        const UpdateManifest::Payload* p = nullptr;
        const UpdateManifest::Patch* q = nullptr;
        using D = UpdateStager::Decision;
        QCOMPARE(UpdateStager::choose(m, request(), {}, &p, &q), D::Patch);
        QVERIFY(p && q && q->from == QLatin1String("26.10.2"));
        QCOMPARE(UpdateStager::choose(m, request(QStringLiteral("26.10.3")), {}, &p, &q), D::NotNewer);
        QCOMPARE(UpdateStager::choose(m, request(QStringLiteral("26.10.4")), {}, &p, &q), D::NotNewer);
        QCOMPARE(UpdateStager::choose(m, request(), m.generatedAt.addSecs(1), &p, &q), D::Replay);
        QCOMPARE(UpdateStager::choose(m, request(), m.generatedAt, &p, &q), D::Patch);
        UpdateStager::Request frozen = request();
        frozen.githubLatestTag = QStringLiteral("v26.11.1");
        QCOMPARE(UpdateStager::choose(m, frozen, {}, &p, &q), D::NotLatest);
        QCOMPARE(UpdateStager::choose(m, request(QStringLiteral("26.9.5")), {}, &p, &q), D::FullDownload);
        UpdateStager::Request mac = request();
        mac.platform = QStringLiteral("macos");
        QCOMPARE(UpdateStager::choose(m, mac, {}, &p, &q), D::NoPayload);
        QCOMPARE(UpdateStager::choose(m, request(QStringLiteral("26.10.2.0")), {}, &p, &q), D::Patch);
    }

    // ── Apply ───────────────────────────────────────────────────────────

    void appliesTheToolsBundleExactly()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        writeFile(install + QStringLiteral("/unins000.dat"), "installer's own");
        const UpdateManifest m = parsed(fixtureManifest());
        const QString stage = tmp.filePath(QStringLiteral("staging/26.10.3"));
        QString problem;
        QVERIFY2(UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, stage, &problem),
                 qPrintable(problem));
        for (const UpdateManifest::File& f : m.payloads[0].files) {
            const QByteArray data = readFile(stage + QLatin1Char('/') + f.path);
            QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()),
                     f.sha256);
        }
        QVERIFY(!QFile::exists(stage + QStringLiteral("/gone.dll")));
        QVERIFY(QFileInfo(stage + QStringLiteral("/plugins/old")).isFile());   // a folder became a file
        QCOMPARE(readFile(stage + QStringLiteral("/unins000.dat")), QByteArray("installer's own"));
        QCOMPARE(readFile(install + QStringLiteral("/gone.dll")),
                 readFile(kFixture + QStringLiteral("/install-v26.10.2/gone.dll")));   // install untouched
    }

    void aDriftedInstallIsAHardFail()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        writeFile(install + QStringLiteral("/AetherSDR.exe"), "modified by the user");
        const UpdateManifest m = parsed(fixtureManifest());
        const QString stage = tmp.filePath(QStringLiteral("staging"));
        QString problem;
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, stage, &problem));
        QVERIFY(problem.contains(QLatin1String("starts from")));
        QVERIFY(!QFileInfo::exists(stage));   // staging removed on failure
    }

    void aCorruptBundleIsAHardFail()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const UpdateManifest m = parsed(fixtureManifest());
        QByteArray bundle = bundleBytes();
        bundle[512 + 40] = bundle[512 + 40] ^ 0x5a;   // inside 000000.zst (67 bytes at 512): the delta
        QString problem;
        QVERIFY(!UpdateStager::applyPatch(install, bundle, m.payloads[0].patches[0], m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s")), &problem));
        QByteArray header = bundleBytes();
        header[10] = 'Z';   // a member header
        QVERIFY(!UpdateStager::applyPatch(install, header, m.payloads[0].patches[0], m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s")), &problem));
    }

    void aResultThatDoesNotMatchIsAHardFail()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        UpdateManifest m = parsed(fixtureManifest());
        UpdateManifest::Patch patch = m.payloads[0].patches[0];
        QString problem;
        patch.ops[2].sha256 = QString(64, QLatin1Char('0'));
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), patch, m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s1")), &problem));
        patch = m.payloads[0].patches[0];
        patch.ops[2].size -= 1;   // the output would exceed what was signed
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), patch, m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s2")), &problem));
        QVERIFY(problem.contains(QLatin1String("exceeds")));
        patch = m.payloads[0].patches[0];
        patch.ops[3].size += 1;   // the output stops short of what was signed
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), patch, m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s3")), &problem));
        patch = m.payloads[0].patches[0];
        patch.ops.removeAt(3);   // a file the release has is never written
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), patch, m.payloads[0].files,
                                          tmp.filePath(QStringLiteral("s4")), &problem));
        QVERIFY(problem.contains(QLatin1String("plugins/new.dll")));
    }

    void neverWritesThroughALink()
    {
#ifndef Q_OS_WIN
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QString outside = tmp.filePath(QStringLiteral("outside"));
        QDir().mkpath(outside);
        // The install's plugins folder is a link to somewhere else.
        fs::remove_all(fs::path(install.toStdString() + "/plugins"));
        fs::create_directory_symlink(fs::path(outside.toStdString()),
                                     fs::path(install.toStdString() + "/plugins"));
        const UpdateManifest m = parsed(fixtureManifest());
        UpdateManifest::Patch patch;
        patch.ops = {m.payloads[0].patches[0].ops[3]};   // add plugins/new.dll
        QString problem;
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), patch, {}, tmp.filePath(QStringLiteral("s")),
                                          &problem));
        QVERIFY(problem.contains(QLatin1String("link")));
        QVERIFY(QDir(outside).isEmpty());
#endif
    }

    void onlyFoldersTheRemovalsEmptiedAreRemoved()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        QDir().mkpath(install + QStringLiteral("/logs"));   // empty in the install, not the update's
        const UpdateManifest m = parsed(fixtureManifest());
        const QString stage = tmp.filePath(QStringLiteral("s1"));
        QString problem;
        QVERIFY2(UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, stage, &problem),
                 qPrintable(problem));
        QVERIFY(QFileInfo(stage + QStringLiteral("/logs")).isDir());

        // A patch of removals only still prunes what it emptied.
        UpdateManifest::Patch removals;
        removals.ops = {m.payloads[0].patches[0].ops[1]};   // remove plugins/old/x.dll
        const QString stage2 = tmp.filePath(QStringLiteral("s2"));
        QVERIFY2(UpdateStager::applyPatch(install, bundleBytes(), removals, {}, stage2, &problem),
                 qPrintable(problem));
        QVERIFY(!QFileInfo::exists(stage2 + QStringLiteral("/plugins/old")));
        QVERIFY(!QFileInfo::exists(stage2 + QStringLiteral("/plugins")));
        QVERIFY(QFileInfo(stage2 + QStringLiteral("/logs")).isDir());
    }

    void theExecutableBitIsChecked()
    {
#ifndef Q_OS_WIN
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        UpdateManifest m = parsed(fixtureManifest());
        for (UpdateManifest::File& f : m.payloads[0].files) {
            if (f.path == QLatin1String("Qt6Core.dll")) {
                f.executable = true;   // untouched by the patch, so only the copy can carry it
            }
        }
        QString problem;
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, tmp.filePath(QStringLiteral("s")), &problem));
        QVERIFY(problem.contains(QLatin1String("executable")));
#endif
    }

    void aMissingBundleOffersTheFullDownload()
    {
        // The manifest is trusted; only its bundle cannot be fetched. That is
        // not "the update failed": the full download may still work.
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QByteArray manifest = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        const QString site = tmp.filePath(QStringLiteral("site"));
        writeFile(site + QStringLiteral("/latest/manifest.json"), manifest);
        writeFile(site + QStringLiteral("/latest/manifest.json.sig"), m_key.sign(manifest));
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("empty"))));
        UpdateSettings::setAcceptedManifestUtc({});
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(site + QStringLiteral("/latest/manifest.json"));
        UpdateStager stager;
        QSignalSpy full(&stager, &UpdateStager::fullDownloadNeeded);
        QSignalSpy failed(&stager, &UpdateStager::failed);
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(full.size() + failed.size(), 1, 20000);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(full.first().at(2).toString(), QStringLiteral("26.10.3"));
        QVERIFY(!stager.isBusy());
    }

    void aLinkInTheInstallIsNeverFollowedByANewFile()
    {
#ifndef Q_OS_WIN
        // Review of #6747: a link left in the install at the name a new
        // file is first written under used to be followed out of staging
        // and overwrite the install. New files are now created
        // exclusively under random names.
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QByteArray before = readFile(install + QStringLiteral("/Qt6Core.dll"));
        fs::create_symlink(fs::path((install + QStringLiteral("/Qt6Core.dll")).toStdString()),
                           fs::path((install + QStringLiteral("/AetherSDR.exe.update-new")).toStdString()));
        fs::create_symlink(fs::path((install + QStringLiteral("/Qt6Core.dll")).toStdString()),
                           fs::path((install + QStringLiteral("/plugins/new.dll.update-new")).toStdString()));
        const UpdateManifest m = parsed(fixtureManifest());
        QString problem;
        QVERIFY2(UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, tmp.filePath(QStringLiteral("staging")),
                                          &problem),
                 qPrintable(problem));
        QCOMPARE(readFile(install + QStringLiteral("/Qt6Core.dll")), before);
        QCOMPARE(readFile(install + QStringLiteral("/AetherSDR.exe")),
                 readFile(kFixture + QStringLiteral("/install-v26.10.2/AetherSDR.exe")));
#endif
    }

    void aCancelledWorkerNeverReportsForTheNextRequest()
    {
        // Review of #6747: cancel A while its apply is queued, start B; only
        // B's result may come back, and A's work must not be left behind.
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QByteArray manifest = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        const QString site = tmp.filePath(QStringLiteral("site"));
        writeFile(site + QStringLiteral("/latest/manifest.json"), manifest);
        writeFile(site + QStringLiteral("/latest/manifest.json.sig"), m_key.sign(manifest));
        QDir().mkpath(tmp.filePath(QStringLiteral("dl/v26.10.3")));
        QFile::copy(kFixture + QStringLiteral("/release/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar"),
                    tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar")));
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("dl"))));
        UpdateSettings::setAcceptedManifestUtc({});

        // Hold the pool so both applies queue behind one blocker.
        QThreadPool* pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore release;
        auto blocker = QtConcurrent::run([&release] { release.acquire(); });

        UpdateStager stager;
        QSignalSpy staged(&stager, &UpdateStager::staged);
        QSignalSpy progress(&stager, &UpdateStager::progress);
        UpdateStager::Request a = request();
        a.installRoot = install;
        a.manifestUrl = QUrl::fromLocalFile(site + QStringLiteral("/latest/manifest.json"));
        a.stagingRoot = tmp.filePath(QStringLiteral("first"));
        UpdateStager::Request b = a;
        b.stagingRoot = tmp.filePath(QStringLiteral("second"));

        stager.stage(a);
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty()
                                     && progress.last().first().toString().contains(QLatin1String("Applying")),
                                 20000);
        stager.cancel();
        UpdateSettings::setAcceptedManifestUtc({});
        progress.clear();
        stager.stage(b);
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty()
                                     && progress.last().first().toString().contains(QLatin1String("Applying")),
                                 20000);
        release.release();
        blocker.waitForFinished();
        QTRY_COMPARE_WITH_TIMEOUT(staged.size(), 1, 20000);
        QTest::qWait(200);   // A's worker has finished too; nothing more arrives
        pool->setMaxThreadCount(threads);
        QCOMPARE(staged.size(), 1);
        QCOMPARE(staged.first().first().toString(), tmp.filePath(QStringLiteral("second/26.10.3")));
        QVERIFY(QFileInfo(tmp.filePath(QStringLiteral("second/26.10.3/plugins/new.dll"))).isFile());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(tmp.filePath(QStringLiteral("first")))
                                     || QDir(tmp.filePath(QStringLiteral("first"))).isEmpty(),
                                 5000);
        QVERIFY(!stager.isBusy());
    }

    void stagingNeverOverlapsTheInstall()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const UpdateManifest m = parsed(fixtureManifest());
        QString problem;
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0], m.payloads[0].files,
                                          install + QStringLiteral("/staging"), &problem));
        QVERIFY(!UpdateStager::applyPatch(install, bundleBytes(), m.payloads[0].patches[0], m.payloads[0].files,
                                          tmp.path(), &problem));
        QVERIFY(QFile::exists(install + QStringLiteral("/AetherSDR.exe")));
        QVERIFY(!UpdateStager::applyPatch(QString(), bundleBytes(), m.payloads[0].patches[0],
                                          m.payloads[0].files, tmp.filePath(QStringLiteral("s")), &problem));
    }

    // ── The full release (no patch applies) ─────────────────────────────

    void theFullReleaseUnpacksExactlyItsFilesAndKeepsTheUninstaller()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        writeFile(install + QStringLiteral("/unins000.exe"), "uninstaller");
        writeFile(install + QStringLiteral("/unins000.dat"), "its record");
        writeFile(install + QStringLiteral("/settings.ini"), "not the release's");
        const UpdateManifest m = parsed(fixtureManifest());
        const QString staging = tmp.filePath(QStringLiteral("staging"));
        QString problem;
        QVERIFY2(UpdateStager::applyFull(install, portableZip(releaseTree(tmp)), m.payloads[0].files, staging,
                                         &problem),
                 qPrintable(problem));
        QCOMPARE(readFile(staging + QStringLiteral("/plugins/new.dll")).size(), 4000);
        QCOMPARE(readFile(staging + QStringLiteral("/unins000.exe")), QByteArray("uninstaller"));
        QCOMPARE(readFile(staging + QStringLiteral("/unins000.dat")), QByteArray("its record"));
        QVERIFY(!QFile::exists(staging + QStringLiteral("/settings.ini")));
        QVERIFY(!QFile::exists(staging + QStringLiteral("/gone.dll")));
        QCOMPARE(QDir(staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 5);
        QVERIFY(QFile::exists(install + QStringLiteral("/gone.dll")));   // the install untouched
    }

    void aFullReleaseThatIsNotExactlyTheManifestIsRefused()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const UpdateManifest m = parsed(fixtureManifest());
        const QHash<QString, QByteArray> tree = releaseTree(tmp);
        const QString staging = tmp.filePath(QStringLiteral("staging"));
        const auto refused = [&](const QByteArray& zip) {
            QString problem;
            const bool ok = UpdateStager::applyFull(install, zip, m.payloads[0].files, staging, &problem);
            return !ok && !QFileInfo::exists(staging) && !problem.isEmpty();
        };
        QVERIFY(refused(portableZip(tree, {{QStringLiteral("extra.dll"), "x"}})));
        QVERIFY(refused(portableZip(tree, {{QStringLiteral("..\\evil.dll"), "x"}})));
        QVERIFY(refused(portableZip(tree, {{QStringLiteral("../plugins\\"), {}}})));
        QVERIFY(refused(portableZip(tree, {}, {QStringLiteral("plugins/new.dll")})));
        // One file swapped for one outside the tree: nothing is written there.
        QVERIFY(refused(portableZip(tree, {{QStringLiteral("..\\evil.dll"), "x"}}, {QStringLiteral("plugins/old")})));
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("evil.dll"))));
        QHash<QString, QByteArray> changed = tree;
        changed[QStringLiteral("Qt6Core.dll")][0] = 'X';
        QVERIFY(refused(portableZip(changed)));
        changed = tree;
        changed[QStringLiteral("plugins/old")] += "longer than signed";
        QVERIFY(refused(portableZip(changed)));
        QVERIFY(refused(QByteArray("not a zip")));
        // A link cannot come from a zip.
        UpdateManifest::File link;
        link.path = QStringLiteral("lib.so");
        link.symlink = true;
        link.target = QStringLiteral("Qt6Core.dll");
        QString problem;
        QVERIFY(!UpdateStager::applyFull(install, portableZip(tree), m.payloads[0].files + QList{link}, staging,
                                         &problem));
        // Overlapping folders, and a cancel.
        QVERIFY(!UpdateStager::applyFull(install, portableZip(tree), m.payloads[0].files,
                                         install + QStringLiteral("/staging"), &problem));
        std::atomic_bool cancelled{true};
        QVERIFY(!UpdateStager::applyFull(install, portableZip(tree), m.payloads[0].files, staging, &problem,
                                         &cancelled));
        QVERIFY(!QFileInfo::exists(staging));
    }

    // The fixture manifest with a full download that is the zip in dl/, and
    // (unless keepPatch) no patch, signed by the test key and published in
    // site/latest/.
    QString publishFull(const QTemporaryDir& tmp, const QByteArray& zip, bool keepPatch,
                        const QByteArray& bundle = bundleBytes())
    {
        QJsonObject o = fixtureManifest();
        QJsonArray payloads = o.value(QLatin1String("payloads")).toArray();
        QJsonObject p = payloads.first().toObject();
        QJsonObject full = p.value(QLatin1String("full")).toObject();
        full[QStringLiteral("sha256")] = sha256Hex(zip);
        full[QStringLiteral("size")] = zip.size();
        p[QStringLiteral("full")] = full;
        if (!keepPatch) {
            p[QStringLiteral("patches")] = QJsonArray();
        }
        payloads[0] = p;
        o[QStringLiteral("payloads")] = payloads;
        const QByteArray manifest = bytes(o);
        const QString site = tmp.filePath(QStringLiteral("site"));
        writeFile(site + QStringLiteral("/latest/manifest.json"), manifest);
        writeFile(site + QStringLiteral("/latest/manifest.json.sig"), m_key.sign(manifest));
        writeFile(tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-portable.zip")), zip);
        writeFile(tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar")),
                  bundle);
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("dl"))));
        UpdateSettings::setAcceptedManifestUtc({});
        return site + QStringLiteral("/latest/manifest.json");
    }

    void withNoPatchTheFullReleaseIsStagedOnlyWhenAsked()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        writeFile(install + QStringLiteral("/unins000.exe"), "uninstaller");
        const QString manifest = publishFull(tmp, portableZip(releaseTree(tmp)), false);
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(manifest);
        UpdateStager stager;
        QSignalSpy staged(&stager, &UpdateStager::staged);
        QSignalSpy full(&stager, &UpdateStager::fullDownloadNeeded);
        QSignalSpy failed(&stager, &UpdateStager::failed);

        stager.stage(r);   // fullFallback off: the old answer
        QTRY_COMPARE_WITH_TIMEOUT(full.size() + staged.size() + failed.size(), 1, 20000);
        QCOMPARE(full.size(), 1);
        QVERIFY(!QFileInfo::exists(r.stagingRoot + QStringLiteral("/26.10.3")));

        UpdateSettings::setAcceptedManifestUtc({});
        r.fullFallback = true;
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(full.size() + staged.size() + failed.size(), 2, 20000);
        QCOMPARE(staged.size(), 1);
        QCOMPARE(staged.first().at(1).toString(), QStringLiteral("26.10.3"));
        const QString dir = staged.first().at(0).toString();
        QCOMPARE(readFile(dir + QStringLiteral("/unins000.exe")), QByteArray("uninstaller"));
        QVERIFY(!QFile::exists(dir + QStringLiteral("/gone.dll")));
        QVERIFY(!stager.isBusy());
    }

    void aBundleThatCannotBeDownloadedFallsBackToTheFullRelease()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QString manifest = publishFull(tmp, portableZip(releaseTree(tmp)), true);
        QFile::remove(tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar")));
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(manifest);
        r.fullFallback = true;
        UpdateStager stager;
        QSignalSpy staged(&stager, &UpdateStager::staged);
        QSignalSpy full(&stager, &UpdateStager::fullDownloadNeeded);
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(full.size() + staged.size(), 1, 20000);
        QCOMPARE(staged.size(), 1);
        QVERIFY(!QFile::exists(staged.first().at(0).toString() + QStringLiteral("/gone.dll")));
        QVERIFY(!stager.isBusy());
    }

    void aBrokenPatchFallsBackToTheFullRelease()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        QByteArray bundle = bundleBytes();
        bundle[bundle.size() / 2] = bundle[bundle.size() / 2] ^ 1;   // fails its signed hash
        const QString manifest = publishFull(tmp, portableZip(releaseTree(tmp)), true, bundle);
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(manifest);
        r.fullFallback = true;
        UpdateStager stager;
        QSignalSpy staged(&stager, &UpdateStager::staged);
        QSignalSpy full(&stager, &UpdateStager::fullDownloadNeeded);
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(full.size() + staged.size(), 1, 20000);
        QCOMPARE(staged.size(), 1);
        QVERIFY(!QFile::exists(staged.first().at(0).toString() + QStringLiteral("/gone.dll")));

        // A full release that fails its hash too, though it is a good zip of
        // the same files: the browser, once.
        const QHash<QString, QByteArray> tree = releaseTree(tmp);
        const QByteArray other = portableZip(tree, {}, {}, true);
        QCOMPARE(other.size(), portableZip(tree).size());
        QVERIFY(other != portableZip(tree));
        writeFile(tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-portable.zip")), other);
        UpdateSettings::setAcceptedManifestUtc({});
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(full.size(), 1, 20000);
        QTest::qWait(100);
        QCOMPARE(staged.size(), 1);
        QCOMPARE(full.size(), 1);   // once, not at least once
        QCOMPARE(full.first().first().toString(), QStringLiteral("The downloaded update does not match its signed hash."));
        QVERIFY(!stager.isBusy());
    }

    // ── What stays staged between runs ──────────────────────────────────

    void aStagedReleaseIsRecordedAndRevalidated()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QString manifest = publishFull(tmp, portableZip(releaseTree(tmp)), true);
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(manifest);
        {
            UpdateStager stager;
            QSignalSpy staged(&stager, &UpdateStager::staged);
            stager.stage(r);
            QTRY_COMPARE_WITH_TIMEOUT(staged.size(), 1, 20000);
        }
        const QString root = r.stagingRoot;
        QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 (QStringList{QStringLiteral("26.10.3"), QStringLiteral("26.10.3.json"),
                              QStringLiteral("26.10.3.manifest.json"), QStringLiteral("26.10.3.manifest.json.sig")}));

        // The next run finds it, for this install and version only.
        UpdateStager::StagedRelease s =
            UpdateStager::stagedRelease(root, QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.3"), {});
        QCOMPARE(s.version, QStringLiteral("26.10.3"));
        QCOMPARE(s.dir, root + QStringLiteral("/26.10.3"));
        QCOMPARE(s.from, QStringLiteral("26.10.2"));
        QCOMPARE(UpdateStager::stagedRelease(root, QStringLiteral("26.10.2"), install, QString(), {}).version,
                 QStringLiteral("26.10.3"));   // latest not known yet
        QString problem;
        QVERIFY2(UpdateStager::revalidate(root, s, QStringLiteral("windows"), QStringLiteral("x86_64"), &problem),
                 qPrintable(problem));
        QVERIFY(!UpdateStager::revalidate(root, s, QStringLiteral("linux"), QStringLiteral("x86_64"), &problem));

        // A file changed after staging, or the stored manifest, fails it.
        writeFile(s.dir + QStringLiteral("/plugins/old"), "tampered!!!!");
        QVERIFY(!UpdateStager::revalidate(root, s, QStringLiteral("windows"), QStringLiteral("x86_64"), &problem));
        QVERIFY(problem.contains(QLatin1String("plugins/old")));
        const QByteArray stored = readFile(root + QStringLiteral("/26.10.3.manifest.json"));
        writeFile(root + QStringLiteral("/26.10.3.manifest.json"), QByteArray(stored).replace("26.10.2", "26.10.1"));
        QVERIFY(!UpdateStager::revalidate(root, s, QStringLiteral("windows"), QStringLiteral("x86_64"), &problem));
        QVERIFY(problem.contains(QLatin1String("not signed")));
        QVERIFY(!UpdateStager::revalidate(root, UpdateStager::StagedRelease{}, QStringLiteral("windows"),
                                          QStringLiteral("x86_64"), &problem));
    }

    void aStagedReleaseThatIsNoLongerTheOneToInstallIsDiscarded()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QString manifest = publishFull(tmp, portableZip(releaseTree(tmp)), true);
        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(manifest);
        const QString root = r.stagingRoot;
        const auto stageOnce = [&] {
            UpdateSettings::setAcceptedManifestUtc({});
            UpdateStager stager;
            QSignalSpy staged(&stager, &UpdateStager::staged);
            stager.stage(r);
            return QTest::qWaitFor([&] { return staged.size() == 1; }, 20000);
        };
        const auto discarded = [&](const QString& running, const QString& installRoot, const QString& latest,
                                   const QString& rolledBack) {
            const UpdateStager::StagedRelease s =
                UpdateStager::stagedRelease(root, running, installRoot, latest, rolledBack);
            return s.version.isEmpty() && QDir(root).isEmpty();
        };
        QVERIFY(stageOnce());
        QVERIFY(discarded(QStringLiteral("26.10.3"), install, QStringLiteral("v26.10.3"), {}));   // updated already
        QVERIFY(stageOnce());
        QVERIFY(discarded(QStringLiteral("26.10.1"), install, QStringLiteral("v26.10.3"), {}));   // staged for another
        QVERIFY(stageOnce());
        QVERIFY(discarded(QStringLiteral("26.10.2"), tmp.filePath(QStringLiteral("other")), QStringLiteral("v26.10.3"), {}));
        QVERIFY(stageOnce());
        QVERIFY(discarded(QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.4"), {}));   // superseded
        QVERIFY(stageOnce());
        QVERIFY(discarded(QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.3"), QStringLiteral("26.10.3")));
        QVERIFY(stageOnce());
        // Review of #6771: the record is unsigned and keeps no tag; one
        // written into it (a "v26.10.4" naming the new latest) is never read,
        // so a superseded (but signed) release cannot pass for it.
        {
            const QString rec = root + QStringLiteral("/26.10.3.json");
            QJsonObject o = QJsonDocument::fromJson(readFile(rec)).object();
            QVERIFY(!o.contains(QStringLiteral("tag")));
            o[QStringLiteral("tag")] = QStringLiteral("v26.10.4");
            writeFile(rec, QJsonDocument(o).toJson());
        }
        QVERIFY(discarded(QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.4"), {}));
        QVERIFY(stageOnce());
        // An empty install in the record is no install, never the working directory.
        {
            const QString rec = root + QStringLiteral("/26.10.3.json");
            QJsonObject o = QJsonDocument::fromJson(readFile(rec)).object();
            o[QStringLiteral("install")] = QString();
            writeFile(rec, QJsonDocument(o).toJson());
        }
        bool emptyDiscarded = false;
        {
            // The working directory is the process's: always put back.
            struct Cwd {
                QString was = QDir::currentPath();
                ~Cwd() { QDir::setCurrent(was); }
            } restore;
            QDir::setCurrent(install);
            emptyDiscarded = discarded(QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.3"), {});
        }
        QVERIFY(emptyDiscarded);
        QVERIFY(stageOnce());
        QFile::remove(root + QStringLiteral("/26.10.3.manifest.json.sig"));   // a record half written
        QVERIFY(discarded(QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.3"), {}));

        // What a crashed run left goes; the staged release stays.
        QVERIFY(stageOnce());
        QDir().mkpath(root + QStringLiteral("/.work-crashed/plugins"));
        QDir().mkpath(root + QStringLiteral("/26.10.1"));
        writeFile(root + QStringLiteral("/26.10.1.json"), "{}");
        const UpdateStager::StagedRelease s =
            UpdateStager::stagedRelease(root, QStringLiteral("26.10.2"), install, QStringLiteral("v26.10.3"), {});
        QCOMPARE(s.version, QStringLiteral("26.10.3"));
        QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 4);
        // Review of #6771: the folder is the helper's state folder too; the
        // sweep removes only what staging wrote.
        writeFile(root + QStringLiteral("/swap-journal.txt"), "selftest 26.10.3");
        writeFile(root + QStringLiteral("/swap-result.txt"), "RolledBack 26.10.3\n");
        writeFile(root + QStringLiteral("/repair-plan.txt"), "C:/AetherSDR\n26.10.3\n");
        writeFile(root + QStringLiteral("/updater.log"), "log");
        writeFile(root + QStringLiteral("/AetherSDRUpdater.exe"), "helper");
        writeFile(root + QStringLiteral("/selftest-26.10.3.txt"), "26.10.3");
        // Staging's own leftovers beside them do go: a crashed work folder
        // and records QSaveFile left half written.
        QDir().mkpath(root + QStringLiteral("/.work-crashed"));
        writeFile(root + QStringLiteral("/26.10.3.json.a1B2c3"), "{");
        writeFile(root + QStringLiteral("/26.10.3.manifest.json.sig.Zz9Yy8"), "x");
        UpdateStager::stagedRelease(root, QStringLiteral("26.10.3"), install, QStringLiteral("v26.10.3"), {});
        for (const char* name : {".work-crashed", "26.10.3.json.a1B2c3", "26.10.3.manifest.json.sig.Zz9Yy8"}) {
            QVERIFY2(!QFileInfo::exists(root + QLatin1Char('/') + QLatin1String(name)), name);
        }
        for (const char* name : {"swap-journal.txt", "swap-result.txt", "repair-plan.txt", "updater.log",
                                 "AetherSDRUpdater.exe", "selftest-26.10.3.txt"}) {
            QVERIFY2(QFile::exists(root + QLatin1Char('/') + QLatin1String(name)), name);
        }
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/26.10.3")));   // staging's own, discarded
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/26.10.3.json")));

        QVERIFY(UpdateStager::stagedRelease(tmp.filePath(QStringLiteral("none")), QStringLiteral("26.10.2"), install,
                                            {}, {})
                    .version.isEmpty());
    }

    // ── The whole stage(), over file:// URLs ────────────────────────────

    void stagesEndToEndAndRefusesAForgedManifest()
    {
        QTemporaryDir tmp;
        const QString install = copyInstall(tmp);
        const QByteArray manifest = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        const QString site = tmp.filePath(QStringLiteral("site"));
        writeFile(site + QStringLiteral("/latest/manifest.json"), manifest);
        writeFile(site + QStringLiteral("/latest/manifest.json.sig"), m_key.sign(manifest));
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("dl"))));
        QDir().mkpath(tmp.filePath(QStringLiteral("dl/v26.10.3")));
        QFile::copy(kFixture + QStringLiteral("/release/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar"),
                    tmp.filePath(QStringLiteral("dl/v26.10.3/AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar")));
        UpdateSettings::setAcceptedManifestUtc({});

        UpdateStager::Request r = request();
        r.installRoot = install;
        r.stagingRoot = tmp.filePath(QStringLiteral("staging"));
        r.manifestUrl = QUrl::fromLocalFile(site + QStringLiteral("/latest/manifest.json"));
        UpdateStager stager;
        QSignalSpy staged(&stager, &UpdateStager::staged);
        QSignalSpy failed(&stager, &UpdateStager::failed);
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(staged.size() + failed.size(), 1, 20000);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(staged.first().at(1).toString(), QStringLiteral("26.10.3"));
        QVERIFY(QFileInfo(staged.first().at(0).toString() + QStringLiteral("/plugins/new.dll")).isFile());
        QVERIFY(UpdateSettings::acceptedManifestUtc().isValid());

        // The same manifest signed by another key is refused before parsing.
        TestKey other;
        uint8_t seed[32] = {1};
        crypto_ed25519_key_pair(other.secret, other.pub, seed);
        writeFile(site + QStringLiteral("/latest/manifest.json.sig"), other.sign(manifest));
        stager.stage(r);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QVERIFY(failed.first().first().toString().contains(QLatin1String("not signed")));
    }
};

QTEST_GUILESS_MAIN(UpdateStagerTest)
#include "update_stager_test.moc"
