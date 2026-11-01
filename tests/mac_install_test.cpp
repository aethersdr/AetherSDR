#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MacInstall.h"
#include "core/UpdateInstaller.h"
#include "core/UpdateSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>

// MacInstall and the macOS hand-over (RFC #5304 Phase 4): the bundle the
// binary is in, the checks before the swap (injected here; the real ones are
// /usr/bin/codesign and the self-test), the swap of the staged bundle for the
// installed one with the previous version kept, and UpdateInstaller's
// prepareSwap and finishSwapOnExit around them. The swap is renamex_np on
// macOS and the same call, renameat2(RENAME_EXCHANGE), on Linux, so this runs
// on both. Every path is a temporary folder; /Applications is never named.

using namespace AetherSDR;
namespace fs = std::filesystem;

namespace {

void writeFile(const QString& path, const QByteArray& data, bool executable = false)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
    f.close();
    if (executable) {
        f.setPermissions(f.permissions() | QFile::ExeOwner | QFile::ExeUser);
    }
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString linkTarget(const QString& path)
{
    std::error_code ec;
    return QString::fromStdString(fs::read_symlink(fs::path(path.toStdString()), ec).string());
}

// An app bundle in macOS's shape, framework links and all. Its binary is a
// script: run with --update-selftest <marker>, it writes selfTestWrites there
// and exits with exitCode.
void makeBundle(const QString& bundle, const QByteArray& version, int exitCode = 0)
{
    writeFile(bundle + QStringLiteral("/Contents/Info.plist"), "<plist>" + version + "</plist>");
    writeFile(bundle + QStringLiteral("/Contents/MacOS/AetherSDR"),
              "#!/bin/sh\n[ \"$1\" = --update-selftest ] && printf %s '" + version + "' > \"$2\"\nexit "
                  + QByteArray::number(exitCode) + "\n",
              true);
    const QString fw = bundle + QStringLiteral("/Contents/Frameworks/QtGui.framework");
    writeFile(fw + QStringLiteral("/Versions/A/QtGui"), "qtgui " + version);
    std::error_code ec;
    fs::create_symlink("A", fs::path((fw + QStringLiteral("/Versions/Current")).toStdString()), ec);
    QVERIFY(!ec);
    fs::create_symlink("Versions/Current/QtGui", fs::path((fw + QStringLiteral("/QtGui")).toStdString()), ec);
    QVERIFY(!ec);
}

MacInstall::Checks passing()
{
    return {[](const QString&, QString*) { return true; }, [](const QString&, const QString&) { return true; }};
}

} // namespace

class MacInstallTest : public QObject {
    Q_OBJECT

    std::unique_ptr<TestSettingsProfile> m_profile;

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("the macOS install; Windows has its own (UpdateSwap)");
#endif
        m_profile = std::make_unique<TestSettingsProfile>(QStringLiteral("mac-install"));
        QVERIFY(m_profile->isValid());
        AppSettings::instance().load();
    }

    void theBundleIsTwoUpFromTheBinary()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        QCOMPARE(MacInstall::bundleOf(bundle + QStringLiteral("/Contents/MacOS")), QFileInfo(bundle).canonicalFilePath());
        // A build folder's bare binary, and a bundle without its Info.plist.
        QVERIFY(MacInstall::bundleOf(tmp.path()).isEmpty());
        QVERIFY(QFile::remove(bundle + QStringLiteral("/Contents/Info.plist")));
        QVERIFY(MacInstall::bundleOf(bundle + QStringLiteral("/Contents/MacOS")).isEmpty());
        QVERIFY(MacInstall::translocated(
            QStringLiteral("/private/var/folders/xy/T/AppTranslocation/1234/d/AetherSDR.app")));
        QVERIFY(!MacInstall::translocated(QStringLiteral("/Applications/AetherSDR.app")));
    }

    void aReleasePassingItsChecksIsReadyWhereItIsStaged()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString staged = tmp.filePath(QStringLiteral("update/26.11.2"));
        makeBundle(staged, "26.11.2");
        // A copy an exit that never came left beside the app goes.
        QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("Apps/.AetherSDR-incoming-dead1234"))));
        QStringList checked;
        MacInstall::Checks checks{[&](const QString& tree, QString*) {
                                      checked << QStringLiteral("signature ") + tree;
                                      return true;
                                  },
                                  [&](const QString& tree, const QString& v) {
                                      checked << QStringLiteral("self-test ") + tree + QLatin1Char(' ') + v;
                                      return true;
                                  }};
        QString ready;
        QString problem;
        QCOMPARE(MacInstall::prepare(bundle, staged, QStringLiteral("26.11.2"), checks, false, &ready, &problem),
                 MacInstall::Prepared::Ready);
        // One volume: no copy, the staged bundle itself, checked in that order.
        QCOMPARE(ready, staged);
        QCOMPARE(checked, (QStringList{QStringLiteral("signature ") + staged,
                                       QStringLiteral("self-test ") + staged + QStringLiteral(" 26.11.2")}));
        QCOMPARE(QDir(tmp.filePath(QStringLiteral("Apps"))).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("AetherSDR.app")});
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.1</plist>"));
    }

    void aBadSignatureIsFinalAndASelfTestFailureMayBeRetried()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString staged = tmp.filePath(QStringLiteral("update/26.11.2"));
        QString ready;
        QString problem;
        // A signature no second try changes: removed even with retry.
        makeBundle(staged, "26.11.2");
        MacInstall::Checks badSignature{[](const QString&, QString* p) {
                                            *p = QStringLiteral("a sealed resource is missing or invalid");
                                            return false;
                                        },
                                        [](const QString&, const QString&) { return true; }};
        QCOMPARE(MacInstall::prepare(bundle, staged, QStringLiteral("26.11.2"), badSignature, true, &ready, &problem),
                 MacInstall::Prepared::Failed);
        QVERIFY(problem.contains(QLatin1String("sealed resource")));
        QVERIFY(!QFileInfo::exists(staged));
        // A self-test that fails: kept to try again, or removed when final.
        makeBundle(staged, "26.11.2");
        MacInstall::Checks badTest{[](const QString&, QString*) { return true; },
                                   [](const QString&, const QString&) { return false; }};
        QCOMPARE(MacInstall::prepare(bundle, staged, QStringLiteral("26.11.2"), badTest, true, &ready, &problem),
                 MacInstall::Prepared::Kept);
        QVERIFY(QFileInfo::exists(staged + QStringLiteral("/Contents/MacOS/AetherSDR")));
        QCOMPARE(MacInstall::prepare(bundle, staged, QStringLiteral("26.11.2"), badTest, false, &ready, &problem),
                 MacInstall::Prepared::Failed);
        QVERIFY(!QFileInfo::exists(staged));
        // Nothing staged, or no bundle: nothing to do.
        QCOMPARE(MacInstall::prepare(bundle, staged, QStringLiteral("26.11.2"), passing(), false, &ready, &problem),
                 MacInstall::Prepared::Refused);
        makeBundle(staged, "26.11.2");
        QCOMPARE(MacInstall::prepare(QString(), staged, QStringLiteral("26.11.2"), passing(), false, &ready, &problem),
                 MacInstall::Prepared::Refused);
        // The installed bundle is never touched by any of it.
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.1</plist>"));
    }

    void theSwapTradesTheBundlesAndKeepsThePrevious()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString previous = tmp.filePath(QStringLiteral("update/previous"));
        for (const char* v : {"26.11.2", "26.11.3"}) {
            const QString staged = tmp.filePath(QStringLiteral("update/") + QString::fromLatin1(v));
            makeBundle(staged, v);
            QString problem;
            QVERIFY2(MacInstall::swap(bundle, staged, previous, &problem), qPrintable(problem));
            QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")),
                     QByteArray("<plist>") + v + "</plist>");
            QVERIFY(!QFileInfo::exists(staged));
        }
        // The previous version is the one before, links intact: one is kept.
        QCOMPARE(readFile(previous + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.2</plist>"));
        const QString fw = previous + QStringLiteral("/Contents/Frameworks/QtGui.framework");
        QCOMPARE(linkTarget(fw + QStringLiteral("/Versions/Current")), QStringLiteral("A"));
        QCOMPARE(readFile(fw + QStringLiteral("/QtGui")), QByteArray("qtgui 26.11.2"));
        QCOMPARE(linkTarget(bundle + QStringLiteral("/Contents/Frameworks/QtGui.framework/QtGui")),
                 QStringLiteral("Versions/Current/QtGui"));
        QCOMPARE(QDir(tmp.filePath(QStringLiteral("Apps"))).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("AetherSDR.app")});
        // Nothing to swap in: false, and the bundle is as it was.
        QString problem;
        QVERIFY(!MacInstall::swap(bundle, tmp.filePath(QStringLiteral("update/26.11.9")), previous, &problem));
        QVERIFY(!problem.isEmpty());
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.3</plist>"));
    }

    // The real check: codesign on a bundle signed here (ad hoc; releases are
    // Developer ID signed), then with one sealed resource changed.
    void theRealSignatureCheckCatchesAChangedFile()
    {
#ifndef Q_OS_MACOS
        QSKIP("codesign is macOS's");
#else
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("AetherSDR.app"));
        writeFile(bundle + QStringLiteral("/Contents/Info.plist"),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                  "<key>CFBundleExecutable</key><string>AetherSDR</string>"
                  "<key>CFBundleIdentifier</key><string>com.aethersdr.test</string>"
                  "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n");
        writeFile(bundle + QStringLiteral("/Contents/MacOS/AetherSDR"), "#!/bin/sh\nexit 0\n", true);
        writeFile(bundle + QStringLiteral("/Contents/Resources/qt.conf"), "[Paths]\n");
        QCOMPARE(QProcess::execute(QStringLiteral("/usr/bin/codesign"),
                                   {QStringLiteral("--force"), QStringLiteral("--sign"), QStringLiteral("-"), bundle}),
                 0);
        QString problem;
        QVERIFY2(MacInstall::verifySignature(bundle, &problem), qPrintable(problem));
        writeFile(bundle + QStringLiteral("/Contents/Resources/qt.conf"), "[Paths]\nPrefix=elsewhere\n");
        QVERIFY(!MacInstall::verifySignature(bundle, &problem));
        QVERIFY2(problem.contains(QLatin1String("doesn't match")), qPrintable(problem));
#endif
    }

    void theSelfTestRunsTheBundlesBinaryAndReadsItsVersion()
    {
        QTemporaryDir tmp;
        const QString good = tmp.filePath(QStringLiteral("good"));
        makeBundle(good, "26.11.2");
        QVERIFY(MacInstall::selfTest(good, QStringLiteral("26.11.2")));
        QVERIFY(!MacInstall::selfTest(good, QStringLiteral("26.11.3")));   // the old binary
        const QString crashes = tmp.filePath(QStringLiteral("crashes"));
        makeBundle(crashes, "26.11.2", 1);
        QVERIFY(!MacInstall::selfTest(crashes, QStringLiteral("26.11.2")));
        QVERIFY(!MacInstall::selfTest(tmp.filePath(QStringLiteral("none")), QStringLiteral("26.11.2")));
    }

    // UpdateInstaller around it: ready at the hand-over, in as AetherSDR
    // exits, the result written for the next start.
    void aReadiedSwapGoesInOnExit()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString state = tmp.filePath(QStringLiteral("update"));
        const QString staged = state + QStringLiteral("/26.11.2");
        makeBundle(staged, "26.11.2");
        writeFile(staged + QStringLiteral(".json"), "{}");   // the stager's record
        QString problem;
        QVERIFY2(UpdateInstaller::prepareSwap(bundle, staged, QStringLiteral("26.11.2"), passing(), false, state,
                                              &problem),
                 qPrintable(problem));
        QVERIFY(UpdateInstaller::swapPending());
        // Nothing changes until the exit.
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.1</plist>"));
        UpdateInstaller::finishSwapOnExit();
        QVERIFY(!UpdateInstaller::swapPending());
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.2</plist>"));
        QCOMPARE(readFile(state + QStringLiteral("/previous/Contents/Info.plist")),
                 QByteArray("<plist>26.11.1</plist>"));
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(staged + QStringLiteral(".json")));
        QCOMPARE(readFile(state + QStringLiteral("/swap-result.txt")), QByteArray("Updated 26.11.2\n"));
        UpdateInstaller::finishSwapOnExit();   // once
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.2</plist>"));
    }

    void aDroppedSwapLeavesTheReleaseStaged()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString state = tmp.filePath(QStringLiteral("update"));
        const QString staged = state + QStringLiteral("/26.11.2");
        makeBundle(staged, "26.11.2");
        QString problem;
        QVERIFY(UpdateInstaller::prepareSwap(bundle, staged, QStringLiteral("26.11.2"), passing(), false, state,
                                             &problem));
        UpdateInstaller::dropPendingSwap();
        UpdateInstaller::finishSwapOnExit();
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.1</plist>"));
        QVERIFY(QFileInfo::exists(staged + QStringLiteral("/Contents/MacOS/AetherSDR")));
        QVERIFY(!QFileInfo::exists(state + QStringLiteral("/swap-result.txt")));
    }

    void aReleaseFailingAtQuitIsTriedThreeTimesThenRolledBack()
    {
        QTemporaryDir tmp;
        const QString bundle = tmp.filePath(QStringLiteral("Apps/AetherSDR.app"));
        makeBundle(bundle, "26.11.1");
        const QString state = tmp.filePath(QStringLiteral("update"));
        const QString staged = state + QStringLiteral("/26.11.2");
        makeBundle(staged, "26.11.2");
        UpdateSettings::setRolledBackVersion({});
        UpdateSettings::setQuitSwitchTries(QStringLiteral("26.11.2"), 0);
        MacInstall::Checks failing{[](const QString&, QString*) { return true; },
                                   [](const QString&, const QString&) { return false; }};
        QString problem;
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(!UpdateInstaller::prepareSwap(bundle, staged, QStringLiteral("26.11.2"), failing, false, state,
                                                  &problem));
            QVERIFY(QFileInfo::exists(staged + QStringLiteral("/Contents/MacOS/AetherSDR")));
            QCOMPARE(UpdateSettings::quitSwitchTries(QStringLiteral("26.11.2")), i);
            QVERIFY(UpdateSettings::rolledBackVersion().isEmpty());
        }
        QVERIFY(!UpdateInstaller::prepareSwap(bundle, staged, QStringLiteral("26.11.2"), failing, false, state,
                                              &problem));
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(UpdateSettings::rolledBackVersion(), QStringLiteral("26.11.2"));
        QCOMPARE(readFile(state + QStringLiteral("/swap-result.txt")), QByteArray("RolledBack 26.11.2\n"));
        QVERIFY(!UpdateInstaller::swapPending());
        QCOMPARE(readFile(bundle + QStringLiteral("/Contents/Info.plist")), QByteArray("<plist>26.11.1</plist>"));
    }
};

QTEST_GUILESS_MAIN(MacInstallTest)
#include "mac_install_test.moc"
