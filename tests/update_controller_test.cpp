#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateController.h"
#include "core/UpdateSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include "monocypher-ed25519.h"

#include <filesystem>

// UpdateController, the Install update / Restart to update flow (RFC #5304
// Phase 3), against tests/fixtures/update: a v26.10.2 install, and the
// v26.10.3 manifest and patch bundle CI built from it, signed here by a test
// key. Downloads are file:// URLs and the helper is a recording fake, so no
// socket or process is involved and it runs on every platform.

using namespace AetherSDR;
namespace fs = std::filesystem;

namespace {

const QString kFixture = QStringLiteral(UPDATE_FIXTURE_DIR);
const QString kBundle = QStringLiteral("AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar");

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(data);
    }
}

struct TestKey {
    uint8_t secret[64];
    uint8_t pub[32];
    TestKey()
    {
        uint8_t seed[32];
        for (int i = 0; i < 32; ++i) {
            seed[i] = static_cast<uint8_t>(0x61 + i);
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

// One launch of the fake helper.
struct Launch {
    QString dir;
    QString version;
    bool restart{false};
    bool onGuiThread{false};
};

// A scratch install, staging root and file:// release site, and a
// controller wired to them.
struct Rig {
    QTemporaryDir tmp;
    QString install;
    QString staging;
    QList<Launch> launches;
    bool helperStarts = true;
    bool selfTestFails = false;
    QString rolledBack;
    bool inPlace = true;

    explicit Rig(const TestKey& key, bool withBundle = true)
    {
        install = tmp.filePath(QStringLiteral("install"));
        staging = tmp.filePath(QStringLiteral("update"));
        std::error_code ec;
        fs::copy(fs::path((kFixture + QStringLiteral("/install-v26.10.2")).toStdString()),
                 fs::path(install.toStdString()), fs::copy_options::recursive, ec);
        const QByteArray manifest = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        writeFile(tmp.filePath(QStringLiteral("site/latest/manifest.json")), manifest);
        writeFile(tmp.filePath(QStringLiteral("site/latest/manifest.json.sig")), key.sign(manifest));
        if (withBundle) {
            writeFile(tmp.filePath(QStringLiteral("dl/v26.10.3/") + kBundle),
                      readFile(kFixture + QStringLiteral("/release/") + kBundle));
        }
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("dl"))));
        UpdateSettings::setAcceptedManifestUtc({});
    }

    UpdateController::Environment env()
    {
        UpdateController::Environment e;
        e.request.runningVersion = QStringLiteral("26.10.2");
        e.request.platform = QStringLiteral("windows");
        e.request.arch = QStringLiteral("x86_64");
        e.request.installRoot = install;
        e.request.stagingRoot = staging;
        e.request.manifestUrl = QUrl::fromLocalFile(tmp.filePath(QStringLiteral("site/latest/manifest.json")));
        e.request.fullFallback = true;
        e.canInstall = [this](QString* why) {
            if (!inPlace && why) {
                *why = QStringLiteral("not here");
            }
            return inPlace;
        };
        e.launchHelper = [this](const QString& dir, const QString& version, bool restart, QString* problem) {
            if (!helperStarts) {
                *problem = QStringLiteral("no helper");
                return false;
            }
            if (selfTestFails) {
                // Linux's switch: the release moved out of staging and,
                // failing its self-test, removed.
                QDir(dir).removeRecursively();
                *problem = QStringLiteral("failed its start-up check");
                return false;
            }
            launches.append({dir, version, restart, QThread::currentThread() == qApp->thread()});
            return true;
        };
        e.rolledBackVersion = [this] { return rolledBack; };
        return e;
    }
};

bool waitFor(UpdateController& c, UpdateController::State s)
{
    return QTest::qWaitFor([&] { return c.state() == s; }, 20000);
}

// Restart to update, to its end: whether it was handed over.
bool restart(UpdateController& c, QString* problem)
{
    QSignalSpy ready(&c, &UpdateController::restartReady);
    if (!c.restartToUpdate(problem)) {
        return false;
    }
    if (c.state() != UpdateController::State::Installing || !ready.wait(20000)) {
        *problem = QStringLiteral("never finished");
        return false;
    }
    *problem = ready.at(0).at(1).toString();
    return ready.at(0).at(0).toBool();
}

} // namespace

class UpdateControllerTest : public QObject {
    Q_OBJECT

    TestKey m_key;
    std::unique_ptr<TestSettingsProfile> m_profile;

private slots:
    void initTestCase()
    {
        m_profile = std::make_unique<TestSettingsProfile>(QStringLiteral("update-controller"));
        QVERIFY(m_profile->isValid());
        AppSettings::instance().load();
        UpdateStager::setTrustedKeysForTesting({m_key.publicKey()});
    }

    void installStagesTheLatestRelease()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        QSignalSpy changed(&c, &UpdateController::stateChanged);
        QSignalSpy progress(&c, &UpdateController::progress);
        QCOMPARE(c.state(), UpdateController::State::Idle);
        c.install(QStringLiteral("26.10.3"));
        QCOMPARE(c.state(), UpdateController::State::Staging);
        QCOMPARE(c.version(), QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        QCOMPARE(c.version(), QStringLiteral("26.10.3"));
        QVERIFY(!progress.isEmpty());
        QCOMPARE(changed.size(), 2);   // Staging, then Staged
        QVERIFY(QFileInfo(rig.staging + QStringLiteral("/26.10.3/plugins/new.dll")).isFile());
        QVERIFY(rig.launches.isEmpty());   // nothing installs until asked
        c.install(QStringLiteral("26.10.3"));   // already staged: nothing new starts
        QCOMPARE(c.state(), UpdateController::State::Staged);
    }

    void cancelKeepsNothing()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        c.cancel();
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(c.version().isEmpty());
        QTest::qWait(300);   // a worker still finishing reports nothing
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));
    }

    void theNextLaunchFindsWhatIsStaged()
    {
        Rig rig(m_key);
        {
            UpdateController c(rig.env());
            c.install(QStringLiteral("26.10.3"));
            QVERIFY(waitFor(c, UpdateController::State::Staged));
        }
        // Startup: GitHub's latest is not known yet.
        UpdateController next(rig.env());
        next.refreshStaged(QString());
        QCOMPARE(next.state(), UpdateController::State::Staged);
        QCOMPARE(next.version(), QStringLiteral("26.10.3"));
        // The check answers with the same latest: still staged.
        next.refreshStaged(QStringLiteral("26.10.3"));
        QCOMPARE(next.state(), UpdateController::State::Staged);
        // A newer release since: the staged one is discarded.
        next.refreshStaged(QStringLiteral("26.10.4"));
        QCOMPARE(next.state(), UpdateController::State::Idle);
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));
    }

    void aNewerReleaseAnnouncedDuringTheDownloadSupersedesIt()
    {
        // Review of #6777: the check answers 26.10.4 while 26.10.3 is
        // still downloading. The finished download is discarded, never
        // installed on the next quit.
        Rig rig(m_key);
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        c.refreshStaged(QStringLiteral("26.10.4"));
        QCOMPARE(c.state(), UpdateController::State::Staging);   // the worker is not swept
        QSignalSpy replaced(&c, &UpdateController::failed);
        QVERIFY(QTest::qWaitFor([&] { return c.state() == UpdateController::State::Idle; }, 20000));
        QCOMPARE(replaced.size(), 1);
        QCOMPARE(replaced.first().at(1).toString(), QStringLiteral("26.10.4"));
        QVERIFY(!c.installOnQuit());
        QVERIFY(rig.launches.isEmpty());
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));
        // The same latest heard during the download keeps it.
        UpdateSettings::setAcceptedManifestUtc({});
        UpdateController again(rig.env());
        again.install(QStringLiteral("26.10.3"));
        again.refreshStaged(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(again, UpdateController::State::Staged));
    }

    void aSupersededAttemptReportsTheNewerRelease()
    {
        // Ozy311's review of #6777: 26.10.4 is announced while 26.10.3
        // downloads, and 26.10.3 then can't be had at all (this rig has
        // neither the bundle nor the full zip, so it can't stage). Its
        // fallback says nothing about 26.10.3's download being the problem:
        // the window hears that 26.10.4 replaced it, attached to 26.10.4.
        Rig rig(m_key, false);
        UpdateController c(rig.env());
        QSignalSpy browser(&c, &UpdateController::browserNeeded);
        QSignalSpy failed(&c, &UpdateController::failed);
        c.install(QStringLiteral("26.10.3"));
        c.refreshStaged(QStringLiteral("26.10.4"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QCOMPARE(browser.size(), 0);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("26.10.4"));
        QVERIFY(failed.first().at(0).toString().contains(QLatin1String("26.10.4")));
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));
        // Without the newer release, the same fallback names its version.
        UpdateSettings::setAcceptedManifestUtc({});
        UpdateController again(rig.env());
        QSignalSpy browser2(&again, &UpdateController::browserNeeded);
        again.install(QStringLiteral("26.10.3"));
        QTRY_COMPARE_WITH_TIMEOUT(browser2.size(), 1, 20000);
        QCOMPARE(browser2.first().at(1).toString(), QStringLiteral("26.10.3"));
    }

    void aRolledBackReleaseIsNeverStagedOrOffered()
    {
        Rig rig(m_key);
        {
            UpdateController c(rig.env());
            c.install(QStringLiteral("26.10.3"));
            QVERIFY(waitFor(c, UpdateController::State::Staged));
        }
        rig.rolledBack = QStringLiteral("26.10.3");
        UpdateController c(rig.env());
        QVERIFY(c.isRolledBack(QStringLiteral("26.10.3")));
        QVERIFY(!c.isRolledBack(QStringLiteral("26.10.4")));
        QVERIFY(!c.isRolledBack(QString()));
        c.refreshStaged(QString());
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QSignalSpy failed(&c, &UpdateController::failed);
        c.install(QStringLiteral("26.10.3"));
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QCOMPARE(failed.size(), 1);
    }

    void skipDiscardsTheStagedRelease()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        c.discard(QStringLiteral("26.10.4"));   // another version: nothing
        QCOMPARE(c.state(), UpdateController::State::Staged);
        c.discard(QStringLiteral("26.10.3"));
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3.json")));
        QVERIFY(!c.installOnQuit());
        QVERIFY(rig.launches.isEmpty());
    }

    void restartHandsTheStagedTreeToTheHelperOnce()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        QString problem;
        QVERIFY(!restart(c, &problem));   // nothing staged
        QVERIFY(!problem.isEmpty());
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        QVERIFY2(restart(c, &problem), qPrintable(problem));
        QCOMPARE(c.state(), UpdateController::State::Handed);
        QCOMPARE(rig.launches.size(), 1);
        QVERIFY(!rig.launches[0].onGuiThread);   // the switch and self-test never block the window
        QCOMPARE(rig.launches[0].dir, rig.staging + QStringLiteral("/26.10.3"));
        QCOMPARE(rig.launches[0].version, QStringLiteral("26.10.3"));
        QVERIFY(rig.launches[0].restart);
        // The quit that follows hands nothing over a second time.
        QVERIFY(!c.installOnQuit());
        QCOMPARE(rig.launches.size(), 1);
    }

    void quittingInstallsWhatIsStaged()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        QVERIFY(!c.installOnQuit());   // nothing staged: nothing launched
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        QVERIFY(c.installOnQuit());
        QCOMPARE(rig.launches.size(), 1);
        QVERIFY(!rig.launches[0].restart);
        QCOMPARE(c.state(), UpdateController::State::Handed);
    }

    void stagedFilesThatChangedAreNeverHandedOver()
    {
        Rig rig(m_key);
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        writeFile(rig.staging + QStringLiteral("/26.10.3/plugins/new.dll"), "tampered");
        QString problem;
        QVERIFY(!restart(c, &problem));
        QVERIFY(!problem.isEmpty());
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(rig.launches.isEmpty());
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3")));   // to be staged again
    }

    void aHelperThatCannotStartKeepsTheReleaseStaged()
    {
        Rig rig(m_key);
        rig.helperStarts = false;
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        QString problem;
        QVERIFY(!restart(c, &problem));
        QCOMPARE(problem, QStringLiteral("no helper"));
        QCOMPARE(c.state(), UpdateController::State::Staged);
    }

    void aReleaseThatFailsTheSwitchLeavesNothingStaged()
    {
        Rig rig(m_key);
        rig.selfTestFails = true;
        UpdateController c(rig.env());
        c.install(QStringLiteral("26.10.3"));
        QVERIFY(waitFor(c, UpdateController::State::Staged));
        QString problem;
        QVERIFY(!restart(c, &problem));
        QCOMPARE(problem, QStringLiteral("failed its start-up check"));
        // Not Restart to update for a tree that is gone, and no record left
        // to find at the next launch.
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(!c.installOnQuit());
        QVERIFY(!QFileInfo::exists(rig.staging + QStringLiteral("/26.10.3.json")));
        c.refreshStaged(QStringLiteral("26.10.3"));
        QCOMPARE(c.state(), UpdateController::State::Idle);
    }

    void noPatchAndNoFullReleaseGoesToTheBrowser()
    {
        Rig rig(m_key, false);   // neither the bundle nor the full zip can be had
        UpdateController c(rig.env());
        QSignalSpy browser(&c, &UpdateController::browserNeeded);
        c.install(QStringLiteral("26.10.3"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.size(), 1, 20000);
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(c.version().isEmpty());
    }

    void anUntrustedManifestFails()
    {
        Rig rig(m_key);
        TestKey other;
        uint8_t seed[32] = {7};
        crypto_ed25519_key_pair(other.secret, other.pub, seed);
        const QByteArray manifest = readFile(kFixture + QStringLiteral("/release/manifest.json"));
        writeFile(rig.tmp.filePath(QStringLiteral("site/latest/manifest.json.sig")), other.sign(manifest));
        UpdateController c(rig.env());
        QSignalSpy failed(&c, &UpdateController::failed);
        c.install(QStringLiteral("26.10.3"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QCOMPARE(c.state(), UpdateController::State::Idle);
        QVERIFY(failed.first().first().toString().contains(QLatin1String("not signed")));
    }

    void inPlaceIsAskedOfTheEnvironment()
    {
        Rig rig(m_key);
        rig.inPlace = false;
        UpdateController c(rig.env());
        QString why;
        QVERIFY(!c.canInstallInPlace(&why));
        QCOMPARE(why, QStringLiteral("not here"));
    }
};

QTEST_GUILESS_MAIN(UpdateControllerTest)
#include "update_controller_test.moc"
