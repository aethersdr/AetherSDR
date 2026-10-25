#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateInstaller.h"
#include "core/UpdateSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

    void launchingIsWindowsOnly()
    {
#ifndef Q_OS_WIN
        QString problem;
        QVERIFY(!UpdateInstaller::launchHelper(QStringLiteral("x"), QStringLiteral("26.10.4"), true, &problem));
        QVERIFY(problem.contains(QLatin1String("Windows")));
#endif
    }
};

QTEST_GUILESS_MAIN(UpdateInstallerTest)
#include "update_installer_test.moc"
