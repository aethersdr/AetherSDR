#include "core/LinuxInstall.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <filesystem>
#include <thread>

// LinuxInstall (RFC #5304 Phase 5): the install --install makes, the switch
// to a staged release with its self-test and cleanup, and the relaunch after
// exit. Every path is a temporary folder passed in; the user's own
// ~/.local is never named.

using namespace AetherSDR;
namespace fs = std::filesystem;

namespace {

const QByteArray kDesktop = "[Desktop Entry]\nName=AetherSDR\nComment=Linux-native SDR client for FlexRadio\n"
                            "Exec=AetherSDR\nIcon=aethersdr\nType=Application\nTerminal=false\n"
                            "StartupWMClass=AetherSDR\n";

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

// A release tree as the tarball holds it. Its binary is a script: run with
// --update-selftest <marker>, it writes selfTestWrites there and exits with
// exitCode, which is what the self-test reads.
void makeTree(const QString& tree, const QByteArray& selfTestWrites, int exitCode = 0)
{
    writeFile(tree + QStringLiteral("/usr/bin/AetherSDR"),
              "#!/bin/sh\n[ \"$1\" = --update-selftest ] && printf %s '" + selfTestWrites + "' > \"$2\"\nexit "
                  + QByteArray::number(exitCode) + "\n",
              true);
    writeFile(tree + QStringLiteral("/usr/lib/libQt6Core.so.6"), "qt");
    writeFile(tree + QStringLiteral("/usr/share/applications/AetherSDR.desktop"), kDesktop);
    writeFile(tree + QStringLiteral("/usr/share/icons/hicolor/256x256/apps/aethersdr.png"), "png");
    std::error_code ec;
    fs::create_symlink("usr/bin/AetherSDR", fs::path((tree + QStringLiteral("/AppRun")).toStdString()), ec);
    QVERIFY(!ec);
}

LinuxInstall::Paths pathsIn(const QString& root)
{
    return {root + QStringLiteral("/data/AetherSDR/install"), root + QStringLiteral("/bin"),
            root + QStringLiteral("/data/applications"), root + QStringLiteral("/data/icons/hicolor")};
}

} // namespace

class LinuxInstallTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("the Linux install");
#endif
    }

    void theTreeIsAVersionFolderOfTheBase()
    {
        QTemporaryDir tmp;
        const QString base = tmp.filePath(QStringLiteral("install"));
        QDir().mkpath(base + QStringLiteral("/26.10.4/usr/bin"));
        QDir().mkpath(base + QStringLiteral("/latest/usr/bin"));
        QDir().mkpath(base + QStringLiteral("/26.10.4/bin"));
        QDir().mkpath(tmp.filePath(QStringLiteral("elsewhere/26.10.4/usr/bin")));
        const QString tree = QFileInfo(base + QStringLiteral("/26.10.4")).canonicalFilePath();
        QCOMPARE(LinuxInstall::treeOf(base + QStringLiteral("/26.10.4/usr/bin"), base), tree);
        QVERIFY(LinuxInstall::treeOf(base + QStringLiteral("/latest/usr/bin"), base).isEmpty());
        QVERIFY(LinuxInstall::treeOf(base + QStringLiteral("/26.10.4/bin"), base).isEmpty());
        QVERIFY(LinuxInstall::treeOf(tmp.filePath(QStringLiteral("elsewhere/26.10.4/usr/bin")), base).isEmpty());
        QVERIFY(LinuxInstall::treeOf(base + QStringLiteral("/26.10.4/usr/bin"), QString()).isEmpty());
        QVERIFY(LinuxInstall::treeOf(QString(), base).isEmpty());
        // Reached through current, it is still that version's tree.
        std::error_code ec;
        fs::create_symlink("26.10.4", fs::path((base + QStringLiteral("/current")).toStdString()), ec);
        QCOMPARE(LinuxInstall::treeOf(base + QStringLiteral("/current/usr/bin"), base), tree);
    }

    void installCopiesTheReleaseAndPointsTheMenuIconAndCommandAtCurrent()
    {
        QTemporaryDir tmp;
        // A home with a space and a dollar in it: the menu entry must quote.
        const QString root = tmp.filePath(QStringLiteral("Ham $Radio"));
        const LinuxInstall::Paths paths = pathsIn(root);
        const QString source = tmp.filePath(QStringLiteral("AetherSDR-v26.10.4-x86_64"));
        makeTree(source, "26.10.4");
        QString problem;
        QString note;
        QVERIFY2(LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem, &note), qPrintable(problem));
        QVERIFY(note.isEmpty());
        const QString dest = paths.base + QStringLiteral("/26.10.4");
        QCOMPARE(readFile(dest + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QCOMPARE(linkTarget(dest + QStringLiteral("/AppRun")), QStringLiteral("usr/bin/AetherSDR"));
        QVERIFY(QFileInfo(dest + QStringLiteral("/usr/bin/AetherSDR")).isExecutable());
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.4"));
        QVERIFY(QFileInfo::exists(source + QStringLiteral("/usr/bin/AetherSDR")));   // the tarball's copy kept

        const QString run = paths.base + QStringLiteral("/current/AppRun");
        const QString desktop = QString::fromUtf8(readFile(paths.applications + QStringLiteral("/AetherSDR.desktop")));
        QString quoted = run;
        quoted.replace(QLatin1Char('$'), QStringLiteral("\\$"));
        quoted = QLatin1Char('"') + quoted + QLatin1Char('"');
        quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        QVERIFY2(desktop.contains(QStringLiteral("\nExec=") + quoted + QLatin1Char('\n')), qPrintable(desktop));
        QVERIFY(desktop.contains(QStringLiteral("\nTryExec=") + run + QLatin1Char('\n')));
        QVERIFY(desktop.contains(QStringLiteral("\nIcon=aethersdr\n")));
        QVERIFY(desktop.contains(QStringLiteral("\nStartupWMClass=AetherSDR\n")));
        QCOMPARE(desktop.count(QStringLiteral("Exec=")), 2);   // Exec and TryExec, once each

        QCOMPARE(linkTarget(paths.icons + QStringLiteral("/256x256/apps/aethersdr.png")),
                 paths.base + QStringLiteral("/current/usr/share/icons/hicolor/256x256/apps/aethersdr.png"));
        QCOMPARE(readFile(paths.icons + QStringLiteral("/256x256/apps/aethersdr.png")), QByteArray("png"));
        QCOMPARE(linkTarget(paths.bin + QStringLiteral("/aethersdr")), run);
    }

    void installLeavesTheUsersOwnCommandAndIconAlone()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        writeFile(paths.bin + QStringLiteral("/aethersdr"), "#!/bin/sh\necho mine\n", true);
        writeFile(paths.icons + QStringLiteral("/256x256/apps/aethersdr.png"), "my own icon");
        const QString source = tmp.filePath(QStringLiteral("tarball"));
        makeTree(source, "26.10.4");
        QString problem;
        QString note;
        QVERIFY2(LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem, &note), qPrintable(problem));
        QVERIFY(note.contains(paths.bin + QStringLiteral("/aethersdr")));
        QCOMPARE(readFile(paths.bin + QStringLiteral("/aethersdr")), QByteArray("#!/bin/sh\necho mine\n"));
        QCOMPARE(readFile(paths.icons + QStringLiteral("/256x256/apps/aethersdr.png")), QByteArray("my own icon"));
    }

    void installLeavesTheUsersOwnLinkAloneAndRedoesItsOwn()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString command = paths.bin + QStringLiteral("/aethersdr");
        QDir().mkpath(paths.bin);
        // The user's own command, a link to their own build.
        std::error_code ec;
        fs::create_symlink("/home/someone/src/AetherSDR/build/AetherSDR", fs::path(command.toStdString()), ec);
        QVERIFY(!ec);
        const QString source = tmp.filePath(QStringLiteral("tarball"));
        makeTree(source, "26.10.4");
        QString problem;
        QString note;
        QVERIFY2(LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem, &note), qPrintable(problem));
        QVERIFY(note.contains(command));
        QCOMPARE(linkTarget(command), QStringLiteral("/home/someone/src/AetherSDR/build/AetherSDR"));
        // Once it is the installer's own link, installing again is quiet.
        QVERIFY(QFile::remove(command));
        QVERIFY2(LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem, &note), qPrintable(problem));
        note.clear();
        QVERIFY2(LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem, &note), qPrintable(problem));
        QVERIFY(note.isEmpty());
        QCOMPARE(linkTarget(command), paths.base + QStringLiteral("/current/AppRun"));
    }

    void currentRunsThroughoutAReinstall()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString first = tmp.filePath(QStringLiteral("first"));
        makeTree(first, "26.10.4");
        QString problem;
        QVERIFY2(LinuxInstall::install(first, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        // Another copy of that version, slow to copy: current is watched
        // from another thread all the while.
        const QString second = tmp.filePath(QStringLiteral("second"));
        makeTree(second, "26.10.4");
        for (int i = 0; i < 3000; ++i) {
            writeFile(second + QStringLiteral("/usr/share/doc/%1.txt").arg(i), "doc");
        }
        const QString run = paths.base + QStringLiteral("/current/AppRun");
        std::atomic_bool done{false};
        int missing = 0;
        int looks = 0;
        bool ok = false;
        std::thread installer([&] {
            ok = LinuxInstall::install(second, QStringLiteral("26.10.4"), paths, &problem);
            done = true;
        });
        while (!done) {
            ++looks;
            if (!QFileInfo(run).isExecutable()) {
                ++missing;
            }
        }
        installer.join();
        QVERIFY2(ok, qPrintable(problem));
        QVERIFY(looks > 10);
        QCOMPARE(missing, 0);
        QVERIFY(QFileInfo::exists(paths.base + QStringLiteral("/current/usr/share/doc/2999.txt")));
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.4"), QStringLiteral("current")}));
    }

    void aReleaseWithNoMenuEntryChangesNothing()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString old = tmp.filePath(QStringLiteral("old"));
        makeTree(old, "26.10.3");
        QString problem;
        QVERIFY2(LinuxInstall::install(old, QStringLiteral("26.10.3"), paths, &problem), qPrintable(problem));
        const QString odd = tmp.filePath(QStringLiteral("odd"));
        makeTree(odd, "26.10.4");
        QVERIFY(QFile::remove(odd + QStringLiteral("/usr/share/applications/AetherSDR.desktop")));
        QVERIFY(!LinuxInstall::install(odd, QStringLiteral("26.10.4"), paths, &problem));
        QVERIFY(problem.contains(QLatin1String("menu entry")));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.3"));
    }

    void aFolderAnotherAetherSDRRunsFromIsKept()
    {
        if (!QFileInfo::exists(QStringLiteral("/proc/self/exe")) || !QFileInfo::exists(QStringLiteral("/bin/sleep"))) {
            QSKIP("needs /proc and /bin/sleep");
        }
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        for (const char* v : {"26.10.2", "26.10.3", "26.10.4"}) {
            const QString t = tmp.filePath(QString::fromLatin1(v));
            makeTree(t, v);
            QString problem;
            QVERIFY2(LinuxInstall::install(t, QString::fromLatin1(v), paths, &problem), qPrintable(problem));
        }
        // A second AetherSDR runs from 26.10.2, and another from 26.10.4.
        const auto runFrom = [&](const QString& version, QProcess& p) {
            const QString bin = paths.base + QLatin1Char('/') + version + QStringLiteral("/usr/bin/running");
            QVERIFY(QFile::copy(QStringLiteral("/bin/sleep"), bin));
            p.start(bin, {QStringLiteral("60")});
            QVERIFY(p.waitForStarted(5000));
        };
        QProcess older;
        QProcess same;
        runFrom(QStringLiteral("26.10.2"), older);
        runFrom(QStringLiteral("26.10.4"), same);
        // 26.10.4 installed again: the copy it runs from is set aside, not removed.
        const QString again = tmp.filePath(QStringLiteral("again"));
        makeTree(again, "26.10.4");
        QString problem;
        QVERIFY2(LinuxInstall::install(again, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        const QStringList aside = QDir(paths.base).entryList({QStringLiteral(".old-*")}, QDir::Dirs | QDir::Hidden);
        QCOMPARE(aside.size(), 1);
        QVERIFY(QFileInfo::exists(paths.base + QLatin1Char('/') + aside.first() + QStringLiteral("/usr/bin/running")));
        // 26.10.3 runs and 26.10.5 goes in: 26.10.2 stays while it runs.
        const auto switchIn = [&](const char* v, const char* keep) {
            const QString staged = tmp.filePath(QStringLiteral("state/update/") + QString::fromLatin1(v));
            makeTree(staged, v);
            QString why;
            return LinuxInstall::switchTo(paths.base, staged, QString::fromLatin1(v), QString::fromLatin1(keep),
                                          [](const QString&, const QString&) { return true; }, false, &why);
        };
        QCOMPARE(switchIn("26.10.5", "26.10.3"), LinuxInstall::Switch::Switched);
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{aside.first(), QStringLiteral("26.10.2"), QStringLiteral("26.10.3"),
                              QStringLiteral("26.10.4"), QStringLiteral("26.10.5"), QStringLiteral("current")}));
        // Once they have exited, the next switch removes them.
        older.kill();
        same.kill();
        QVERIFY(older.waitForFinished(5000) && same.waitForFinished(5000));
        QCOMPARE(switchIn("26.10.6", "26.10.5"), LinuxInstall::Switch::Switched);
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.5"), QStringLiteral("26.10.6"), QStringLiteral("current")}));
    }

    void installAgainReplacesThatVersionOrOnlyRedoesTheLinks()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString first = tmp.filePath(QStringLiteral("first"));
        makeTree(first, "26.10.4");
        QString problem;
        QVERIFY2(LinuxInstall::install(first, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        const QString dest = paths.base + QStringLiteral("/26.10.4");
        // From the installed tree itself: nothing is copied or removed.
        QFile::remove(paths.applications + QStringLiteral("/AetherSDR.desktop"));
        QVERIFY2(LinuxInstall::install(dest, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        QCOMPARE(readFile(dest + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QVERIFY(QFileInfo::exists(paths.applications + QStringLiteral("/AetherSDR.desktop")));
        // Another copy of the same version replaces it whole.
        const QString second = tmp.filePath(QStringLiteral("second"));
        makeTree(second, "26.10.4");
        writeFile(second + QStringLiteral("/usr/lib/libQt6Core.so.6"), "qt, again");
        writeFile(dest + QStringLiteral("/usr/lib/stale.so"), "from the first");
        QVERIFY2(LinuxInstall::install(second, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        QCOMPARE(readFile(dest + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt, again"));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/usr/lib/stale.so")));
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.4"), QStringLiteral("current")}));
    }

    void aReinstallThatCannotBeCopiedKeepsTheInstalledRelease()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString first = tmp.filePath(QStringLiteral("first"));
        makeTree(first, "26.10.4");
        QString problem;
        QVERIFY2(LinuxInstall::install(first, QStringLiteral("26.10.4"), paths, &problem), qPrintable(problem));
        // Another copy of the same version with a library that can't be read.
        const QString second = tmp.filePath(QStringLiteral("second"));
        makeTree(second, "26.10.4");
        QFile lib(second + QStringLiteral("/usr/lib/libQt6Core.so.6"));
        QVERIFY(lib.setPermissions(QFileDevice::Permissions()));
        if (lib.open(QIODevice::ReadOnly)) {
            QSKIP("running as root: an unreadable file can still be read");
        }
        QVERIFY(!LinuxInstall::install(second, QStringLiteral("26.10.4"), paths, &problem));
        QVERIFY(!problem.isEmpty());
        lib.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(QFileInfo(paths.base + QStringLiteral("/current/AppRun")).isExecutable());
        QCOMPARE(readFile(paths.base + QStringLiteral("/current/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.4"), QStringLiteral("current")}));
    }

    void installRefusesWhatIsNotARelease()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString source = tmp.filePath(QStringLiteral("tarball"));
        makeTree(source, "26.10.4");
        QString problem;
        QVERIFY(!LinuxInstall::install(source, QStringLiteral("latest"), paths, &problem));
        QVERIFY(!LinuxInstall::install(source, QStringLiteral("../26.10.4"), paths, &problem));
        QVERIFY(!LinuxInstall::install(tmp.filePath(QStringLiteral("nothing")), QStringLiteral("26.10.4"), paths,
                                       &problem));
        QFile::remove(source + QStringLiteral("/AppRun"));
        QVERIFY(!LinuxInstall::install(source, QStringLiteral("26.10.4"), paths, &problem));
        QVERIFY(!problem.isEmpty());
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/current")));
        QVERIFY(!QFileInfo::exists(paths.applications));
    }

    void theSwitchMakesAPassingReleaseCurrentAndKeepsOnlyTheOneItReplaced()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        for (const char* v : {"26.10.2", "26.10.3"}) {
            const QString t = tmp.filePath(QString::fromLatin1(v));
            makeTree(t, v);
            QString problem;
            QVERIFY2(LinuxInstall::install(t, QString::fromLatin1(v), paths, &problem), qPrintable(problem));
        }
        QDir().mkpath(paths.base + QStringLiteral("/.partial-left-by-a-crash"));
        writeFile(paths.base + QStringLiteral("/README"), "not a release");
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        QStringList tested;
        const auto passes = [&tested](const QString& tree, const QString& version) {
            tested << tree << version;
            return true;
        };
        QString problem;
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        passes, false, &problem),
                 LinuxInstall::Switch::Switched);
        // The test ran on the tree in its final place, before it became current.
        QCOMPARE(tested, (QStringList{paths.base + QStringLiteral("/26.10.4"), QStringLiteral("26.10.4")}));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.4"));
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(QDir(paths.base).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.3"), QStringLiteral("26.10.4"), QStringLiteral("current"),
                              QStringLiteral("README")}));
        // The menu entry and command follow current with no rewrite.
        QCOMPARE(QFileInfo(paths.bin + QStringLiteral("/aethersdr")).canonicalFilePath(),
                 QFileInfo(paths.base + QStringLiteral("/26.10.4/usr/bin/AetherSDR")).canonicalFilePath());
    }

    void aReleaseThatFailsItsSelfTestIsRemovedAndCurrentIsUntouched()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString old = tmp.filePath(QStringLiteral("old"));
        makeTree(old, "26.10.3");
        QString problem;
        QVERIFY(LinuxInstall::install(old, QStringLiteral("26.10.3"), paths, &problem));
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return false; }, false, &problem),
                 LinuxInstall::Switch::Failed);
        QVERIFY(problem.contains(QLatin1String("26.10.4")));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.3"));
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(QFileInfo::exists(paths.base + QStringLiteral("/26.10.3/usr/bin/AetherSDR")));
    }

    void asItQuitsAFailedReleaseIsKeptToTryAgain()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString old = tmp.filePath(QStringLiteral("old"));
        makeTree(old, "26.10.3");
        QString problem;
        QVERIFY(LinuxInstall::install(old, QStringLiteral("26.10.3"), paths, &problem));
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        // A self-test with no display (a logout): kept, current untouched.
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return false; }, true, &problem),
                 LinuxInstall::Switch::Kept);
        QCOMPARE(readFile(staged + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.3"));
        // The next try, with a display, goes in.
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return true; }, true, &problem),
                 LinuxInstall::Switch::Switched);
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.4"));
    }

    void aReleaseStagedOnAnotherFilesystemIsPutBack()
    {
        QTemporaryDir tmp;
        QTemporaryDir shm(QStringLiteral("/dev/shm/linux_install_test-XXXXXX"));
        if (!shm.isValid() || QStorageInfo(shm.path()).device() == QStorageInfo(tmp.path()).device()) {
            QSKIP("needs a second filesystem at /dev/shm");
        }
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString old = tmp.filePath(QStringLiteral("old"));
        makeTree(old, "26.10.3");
        QString problem;
        QVERIFY(LinuxInstall::install(old, QStringLiteral("26.10.3"), paths, &problem));
        const QString staged = shm.filePath(QStringLiteral("update/26.10.4"));
        makeTree(staged, "26.10.4");
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return false; }, true, &problem),
                 LinuxInstall::Switch::Kept);
        QCOMPARE(readFile(staged + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QCOMPARE(linkTarget(staged + QStringLiteral("/AppRun")), QStringLiteral("usr/bin/AetherSDR"));
        QVERIFY(QFileInfo(staged + QStringLiteral("/usr/bin/AetherSDR")).isExecutable());
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.3"));
        // current can't be changed (a folder no link renames over): put back too.
        QFile::remove(paths.base + QStringLiteral("/current"));
        QDir().mkpath(paths.base + QStringLiteral("/current/in-the-way"));
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return true; }, false, &problem),
                 LinuxInstall::Switch::Kept);
        QCOMPARE(readFile(staged + QStringLiteral("/usr/lib/libQt6Core.so.6")), QByteArray("qt"));
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
    }

    void aCurrentThatCannotBeChangedKeepsTheRelease()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString old = tmp.filePath(QStringLiteral("old"));
        makeTree(old, "26.10.3");
        QString problem;
        QVERIFY(LinuxInstall::install(old, QStringLiteral("26.10.3"), paths, &problem));
        // current is a folder, which no link can be renamed over.
        QFile::remove(paths.base + QStringLiteral("/current"));
        QDir().mkpath(paths.base + QStringLiteral("/current/in-the-way"));
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                        [](const QString&, const QString&) { return true; }, false, &problem),
                 LinuxInstall::Switch::Kept);
        QVERIFY(QFileInfo::exists(staged + QStringLiteral("/usr/bin/AetherSDR")));
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
        QVERIFY(QFileInfo::exists(paths.base + QStringLiteral("/current/in-the-way")));
    }

    void aReleaseAlreadyCurrentOrOlderIsNotSwitchedTo()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        for (const char* v : {"26.10.3", "26.10.5"}) {
            const QString t = tmp.filePath(QString::fromLatin1(v));
            makeTree(t, v);
            QString problem;
            QVERIFY2(LinuxInstall::install(t, QString::fromLatin1(v), paths, &problem), qPrintable(problem));
        }
        // 26.10.3 runs with 26.10.4 staged; 26.10.5 was installed by hand.
        const auto passes = [](const QString&, const QString&) { return true; };
        QString problem;
        for (const char* v : {"26.10.4", "26.10.5"}) {
            const QString staged = tmp.filePath(QStringLiteral("state/update/") + QString::fromLatin1(v));
            makeTree(staged, v);
            QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QString::fromLatin1(v), QStringLiteral("26.10.3"),
                                            passes, false, &problem),
                     LinuxInstall::Switch::Current);
            QVERIFY(!QFileInfo::exists(staged));
        }
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.5"));
        QVERIFY(QFileInfo::exists(paths.base + QStringLiteral("/26.10.5/usr/bin/AetherSDR")));
        QVERIFY(!QFileInfo::exists(paths.base + QStringLiteral("/26.10.4")));
    }

    void currentIsReadHoweverItsLinkIsWritten()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        for (const char* v : {"26.10.2", "26.10.3", "26.10.4", "26.10.5"}) {
            const QString t = tmp.filePath(QString::fromLatin1(v));
            makeTree(t, v);
            QString problem;
            QVERIFY2(LinuxInstall::install(t, QString::fromLatin1(v), paths, &problem), qPrintable(problem));
        }
        const QString current = paths.base + QStringLiteral("/current");
        const auto pointAt = [&](const QString& target) {
            std::error_code ec;
            fs::remove(fs::path(current.toStdString()), ec);
            fs::create_symlink(fs::path(target.toStdString()), fs::path(current.toStdString()), ec);
            return !ec;
        };
        int tests = 0;
        const auto fails = [&](const QString&, const QString&) {
            ++tests;
            return false;
        };
        // 26.10.3 runs with 26.10.4 staged; current names 26.10.4 by a full
        // path, then 26.10.5 as ./26.10.5: already current, and never a step back.
        for (const QString& target : {paths.base + QStringLiteral("/26.10.4"), QStringLiteral("./26.10.5")}) {
            QVERIFY(pointAt(target));
            const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
            makeTree(staged, "26.10.4");
            QString problem;
            QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.3"),
                                            fails, false, &problem),
                     LinuxInstall::Switch::Current);
            QCOMPARE(tests, 0);
            QCOMPARE(linkTarget(current), target);
            QVERIFY(QFileInfo(current + QStringLiteral("/AppRun")).isExecutable());
        }
        // current names 26.10.3 by a full path while 26.10.2 runs: the
        // switch to 26.10.6 keeps it.
        QVERIFY(pointAt(paths.base + QStringLiteral("/26.10.3")));
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.6"));
        makeTree(staged, "26.10.6");
        QString problem;
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.6"), QStringLiteral("26.10.2"),
                                        [](const QString&, const QString&) { return true; }, false, &problem),
                 LinuxInstall::Switch::Switched);
        QCOMPARE(QDir(paths.base).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.2"), QStringLiteral("26.10.3"), QStringLiteral("26.10.6"),
                              QStringLiteral("current")}));
        // A link to a folder of that name outside the install names no release.
        const QString elsewhere = tmp.filePath(QStringLiteral("elsewhere/26.10.9"));
        makeTree(elsewhere, "26.10.9");
        QVERIFY(pointAt(elsewhere));
        const QString next = tmp.filePath(QStringLiteral("state/update/26.10.7"));
        makeTree(next, "26.10.7");
        QCOMPARE(LinuxInstall::switchTo(paths.base, next, QStringLiteral("26.10.7"), QStringLiteral("26.10.6"),
                                        [](const QString&, const QString&) { return true; }, false, &problem),
                 LinuxInstall::Switch::Switched);
        QCOMPARE(linkTarget(current), QStringLiteral("26.10.7"));
        QVERIFY(QFileInfo::exists(elsewhere + QStringLiteral("/AppRun")));
    }

    void cleanupKeepsTheRunningReleaseAndWhatCurrentNamed()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        for (const char* v : {"26.10.1", "26.10.2", "26.10.3"}) {
            const QString t = tmp.filePath(QString::fromLatin1(v));
            makeTree(t, v);
            QString problem;
            QVERIFY(LinuxInstall::install(t, QString::fromLatin1(v), paths, &problem));
        }
        // 26.10.2 runs, current names 26.10.3 (set by hand), 26.10.4 goes in.
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        QString problem;
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.4"), QStringLiteral("26.10.2"),
                                        [](const QString&, const QString&) { return true; }, false, &problem),
                 LinuxInstall::Switch::Switched);
        QCOMPARE(QDir(paths.base).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("26.10.2"), QStringLiteral("26.10.3"), QStringLiteral("26.10.4"),
                              QStringLiteral("current")}));
    }

    void theSwitchRefusesWhatIsNotARelease()
    {
        QTemporaryDir tmp;
        const LinuxInstall::Paths paths = pathsIn(tmp.path());
        const QString source = tmp.filePath(QStringLiteral("t"));
        makeTree(source, "26.10.3");
        QString problem;
        QVERIFY(LinuxInstall::install(source, QStringLiteral("26.10.3"), paths, &problem));
        const QString staged = tmp.filePath(QStringLiteral("state/update/26.10.4"));
        makeTree(staged, "26.10.4");
        const auto passes = [](const QString&, const QString&) { return true; };
        // The running release, and no release at all.
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("26.10.3"), QStringLiteral("26.10.3"),
                                        passes, false, &problem),
                 LinuxInstall::Switch::Refused);
        QCOMPARE(LinuxInstall::switchTo(paths.base, staged, QStringLiteral("../x"), QStringLiteral("26.10.3"),
                                        passes, false, &problem),
                 LinuxInstall::Switch::Refused);
        // Nothing moved: the installed tree and the staged one are as they were.
        QVERIFY(QFileInfo::exists(staged + QStringLiteral("/usr/bin/AetherSDR")));
        QVERIFY(QFileInfo::exists(paths.base + QStringLiteral("/26.10.3/usr/bin/AetherSDR")));
        QCOMPARE(linkTarget(paths.base + QStringLiteral("/current")), QStringLiteral("26.10.3"));
    }

    void theSelfTestRunsAppRunAndReadsItsVersion()
    {
        QTemporaryDir tmp;
        const QString good = tmp.filePath(QStringLiteral("good"));
        makeTree(good, "26.10.4");
        QVERIFY(LinuxInstall::selfTest(good, QStringLiteral("26.10.4")));
        // The old binary left in place, a crash after writing, a missing AppRun.
        QVERIFY(!LinuxInstall::selfTest(good, QStringLiteral("26.10.5")));
        const QString crashes = tmp.filePath(QStringLiteral("crashes"));
        makeTree(crashes, "26.10.4", 1);
        QVERIFY(!LinuxInstall::selfTest(crashes, QStringLiteral("26.10.4")));
        QVERIFY(!LinuxInstall::selfTest(tmp.filePath(QStringLiteral("none")), QStringLiteral("26.10.4")));
    }

    void theRelaunchWaitsForTheQuittingProcess()
    {
        QTemporaryDir tmp;
        const QString started = tmp.filePath(QStringLiteral("started"));
        const QString program = tmp.filePath(QStringLiteral("program"));
        writeFile(program, "#!/bin/sh\ntouch '" + started.toUtf8() + "'\n", true);
        QProcess quitting;
        quitting.start(QStringLiteral("sleep"), {QStringLiteral("1")});
        QVERIFY(quitting.waitForStarted());
        QVERIFY(LinuxInstall::startAfterExit(quitting.processId(), program));
        QTest::qWait(400);
        QVERIFY(!QFileInfo::exists(started));
        QVERIFY(quitting.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 5000);
    }
};

QTEST_GUILESS_MAIN(LinuxInstallTest)
#include "linux_install_test.moc"
