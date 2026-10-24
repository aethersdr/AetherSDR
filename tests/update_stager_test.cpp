#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateSettings.h"
#include "core/UpdateStager.h"

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
