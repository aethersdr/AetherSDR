#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateInstaller.h"
#include "core/UpdateSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

// UpdateInstaller, the app's side of AetherSDRUpdater (RFC #5304 Phase 3):
// the helper's command line, reading what it did once, remembering a
// rolled-back release, and the self-test marker. No processes are started.

using namespace AetherSDR;

class UpdateInstallerTest : public QObject {
    Q_OBJECT

    std::unique_ptr<TestSettingsProfile> m_profile;

private slots:
    void initTestCase()
    {
        m_profile = std::make_unique<TestSettingsProfile>(QStringLiteral("update-installer"));
        QVERIFY(m_profile->isValid());
        AppSettings::instance().load();
    }

    void theHelpersCommandLineIsProtocolOne()
    {
        const QStringList a = UpdateInstaller::helperArguments(
            4242, QStringLiteral("C:/Users/Op/AppData/Local/Programs/AetherSDR"),
            QStringLiteral("C:/Users/Op/AppData/Local/AetherSDR/update/26.10.4"),
            QStringLiteral("C:/Users/Op/AppData/Local/AetherSDR/update"), QStringLiteral("26.10.4"), false);
        QCOMPARE(a.mid(0, 2), (QStringList{QStringLiteral("--protocol"), QStringLiteral("1")}));
        QCOMPARE(a.at(a.indexOf(QStringLiteral("--wait-pid")) + 1), QStringLiteral("4242"));
        QCOMPARE(a.at(a.indexOf(QStringLiteral("--version")) + 1), QStringLiteral("26.10.4"));
        QVERIFY(!a.contains(QStringLiteral("--restart")));
        // Each value is one argument, spaces and all: QProcess quotes it.
        const QStringList spaced = UpdateInstaller::helperArguments(
            1, QStringLiteral("D:/Ham Radio/AetherSDR"), QStringLiteral("x"), QStringLiteral("y"),
            QStringLiteral("26.10.4"), true);
        QCOMPARE(spaced.at(spaced.indexOf(QStringLiteral("--install")) + 1),
                 QDir::toNativeSeparators(QStringLiteral("D:/Ham Radio/AetherSDR")));
        QCOMPARE(spaced.last(), QStringLiteral("--restart"));
    }

    void aRollbackIsRememberedAndTheResultReadOnce()
    {
        QTemporaryDir dir;
        UpdateSettings::setRolledBackVersion(QString());
        QFile f(dir.filePath(QStringLiteral("swap-result.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("RolledBack 26.10.4\n");
        f.close();
        const auto r = UpdateInstaller::takeSwapResult(dir.path());
        QCOMPARE(r.outcome, QStringLiteral("RolledBack"));
        QCOMPARE(r.version, QStringLiteral("26.10.4"));
        QCOMPARE(UpdateSettings::rolledBackVersion(), QStringLiteral("26.10.4"));
        QVERIFY(!f.exists());   // read once
        QVERIFY(UpdateInstaller::takeSwapResult(dir.path()).outcome.isEmpty());
    }

    void anUpdateDoesNotTouchTheRollbackMemory()
    {
        QTemporaryDir dir;
        UpdateSettings::setRolledBackVersion(QStringLiteral("26.10.4"));
        QFile f(dir.filePath(QStringLiteral("swap-result.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("Updated 26.10.5\n");
        f.close();
        QCOMPARE(UpdateInstaller::takeSwapResult(dir.path()).outcome, QStringLiteral("Updated"));
        QCOMPARE(UpdateSettings::rolledBackVersion(), QStringLiteral("26.10.4"));
    }

    void aGarbledResultIsIgnored()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("swap-result.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("RolledBack");
        f.close();
        UpdateSettings::setRolledBackVersion(QString());
        QVERIFY(UpdateInstaller::takeSwapResult(dir.path()).outcome.isEmpty());
        QVERIFY(UpdateSettings::rolledBackVersion().isEmpty());
        // Review of #6767: a forged result is neither logged nor kept.
        for (const QByteArray& forged :
             {QByteArray("RolledBack 26.10.4\x1b[2Jinjected"), QByteArray("Owned 26.10.4"),
              QByteArray("RolledBack 26.10"), QByteArray("RolledBack v26.10.4")}) {
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(forged);
            f.close();
            QVERIFY(UpdateInstaller::takeSwapResult(dir.path()).outcome.isEmpty());
            QVERIFY(UpdateSettings::rolledBackVersion().isEmpty());
            QVERIFY(!f.exists());   // read once, even when refused
        }
        UpdateSettings::setRolledBackVersion(QStringLiteral("not a version"));
        QVERIFY(UpdateSettings::rolledBackVersion().isEmpty());   // only parsed digits are kept
    }

    void theSelfTestReportsItsVersion()
    {
        QTemporaryDir dir;
        const QString marker = dir.filePath(QStringLiteral("selftest-26.10.4.txt"));
        QVERIFY(UpdateInstaller::writeSelfTestMarker(marker, QStringLiteral("26.10.4")));
        QFile f(marker);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("26.10.4"));   // exactly what UpdateSwap compares
        QVERIFY(!UpdateInstaller::writeSelfTestMarker(dir.filePath(QStringLiteral("no/such/dir/m")),
                                                      QStringLiteral("26.10.4")));
    }

    void onlyAnInstalledCopyUpdatesInPlace()
    {
#ifndef Q_OS_WIN
        // This test binary is no installed release: it says why, and hands
        // nothing over. Not run as an AppImage, whatever the environment.
        qunsetenv("APPIMAGE");
        QString why;
        QVERIFY(!UpdateInstaller::canInstallInPlace(&why));
        QString problem;
        QVERIFY(!UpdateInstaller::launchHelper(QStringLiteral("x"), QStringLiteral("26.10.4"), true, &problem));
#ifdef Q_OS_LINUX
        QVERIFY(why.contains(QLatin1String("--install")));
        QVERIFY(problem.contains(QLatin1String("--install")));
#else
        QVERIFY(!why.isEmpty() && !problem.isEmpty());
#endif
        QVERIFY(!QFileInfo::exists(QDir(UpdateInstaller::stateDir()).filePath(QStringLiteral("swap-result.txt"))));
#endif
    }

    void aReleaseFailingAtQuitIsTriedThreeTimesThenRolledBack()
    {
#ifdef Q_OS_WIN
        QSKIP("the Linux switch");
#endif
        QTemporaryDir tmp;
        const QString base = tmp.filePath(QStringLiteral("install"));
        const QString state = tmp.filePath(QStringLiteral("state"));
        QVERIFY(QDir().mkpath(base + QStringLiteral("/26.10.3/usr/bin")));
        QVERIFY(QFile::link(QStringLiteral("26.10.3"), base + QStringLiteral("/current")));
        const auto stage = [&](const QString& version) {
            const QString dir = state + QLatin1Char('/') + version;
            QDir().mkpath(dir + QStringLiteral("/usr/bin"));
            QFile f(dir + QStringLiteral("/usr/bin/AetherSDR"));
            f.open(QIODevice::WriteOnly);
            return dir;
        };
        int tests = 0;
        const auto fails = [&](const QString&, const QString&) {
            ++tests;
            return false;
        };
        const QString staged = stage(QStringLiteral("26.10.4"));
        QString problem;
        for (int quit = 1; quit < UpdateInstaller::kQuitSwitchTries; ++quit) {
            QVERIFY(!UpdateInstaller::switchRelease(base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                                    fails, false, state, &problem));
            QVERIFY(QFileInfo::exists(staged + QStringLiteral("/usr/bin/AetherSDR")));   // kept, to try again
            QCOMPARE(UpdateSettings::quitSwitchTries(QStringLiteral("26.10.4")), quit);
            QVERIFY(UpdateSettings::rolledBackVersion().isEmpty());
        }
        // The third quit is final: removed, rolled back, and told at the next start.
        QVERIFY(!UpdateInstaller::switchRelease(base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                                fails, false, state, &problem));
        QCOMPARE(tests, UpdateInstaller::kQuitSwitchTries);
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/26.10.4")));
        QCOMPARE(UpdateSettings::rolledBackVersion(), QStringLiteral("26.10.4"));
        const UpdateInstaller::SwapResult r = UpdateInstaller::takeSwapResult(state);
        QCOMPARE(r.outcome, QStringLiteral("RolledBack"));
        QCOMPARE(r.version, QStringLiteral("26.10.4"));
        // On Restart to update the first failure is final, and shown there.
        const QString next = stage(QStringLiteral("26.10.5"));
        QVERIFY(!UpdateInstaller::switchRelease(base, next, QStringLiteral("26.10.5"), QStringLiteral("26.10.3"),
                                                fails, true, state, &problem));
        QVERIFY(!QFileInfo::exists(next));
        QCOMPARE(UpdateSettings::rolledBackVersion(), QStringLiteral("26.10.5"));
        QVERIFY(UpdateInstaller::takeSwapResult(state).outcome.isEmpty());
        // A release that passes is in place, and recorded as Updated.
        const QString good = stage(QStringLiteral("26.10.6"));
        QVERIFY2(UpdateInstaller::switchRelease(base, good, QStringLiteral("26.10.6"), QStringLiteral("26.10.3"),
                                                [](const QString&, const QString&) { return true; }, false, state,
                                                &problem),
                 qPrintable(problem));
        QCOMPARE(UpdateInstaller::takeSwapResult(state).outcome, QStringLiteral("Updated"));
        UpdateSettings::setRolledBackVersion(QString());
    }
};

QTEST_GUILESS_MAIN(UpdateInstallerTest)
#include "update_installer_test.moc"
