#pragma once

#include <QString>

#include <functional>

namespace AetherSDR {

// The Linux install the in-app updater can update (RFC #5304 Phase 5): each
// release's tree in a folder of its own, and a link naming the one to run.
//
//   <data>/AetherSDR/install/<version>/   a release's tree (AppRun, usr/...)
//   <data>/AetherSDR/install/current      -> <version>
//   ~/.local/bin/aethersdr                 -> <base>/current/AppRun
//   <data>/applications/AetherSDR.desktop  runs <base>/current/AppRun
//
// <data> is $XDG_DATA_HOME (~/.local/share). The install has a folder of its
// own because <data>/AetherSDR already holds the user's data (settings,
// models), which an uninstall or a cleanup must never reach.
//
// The tree is the AppImage's payload, which finds its libraries and plugins
// beside its own binary (RUNPATH $ORIGIN, a relative qt.conf), so it runs
// from any folder. A new release goes in beside the running one and
// `current` is switched by a rename, which is atomic: nothing runnable is
// ever half in place and the running AetherSDR is never disturbed, so the
// app switches itself as it quits, with no helper. The new tree must pass
// its self-test first. The release it replaces is kept, the one to go back
// to by hand; older ones are removed.
class LinuxInstall {
public:
    struct Paths {
        QString base;           // <data>/AetherSDR/install
        QString bin;            // ~/.local/bin
        QString applications;   // <data>/applications
        QString icons;          // <data>/icons/hicolor
    };
    static Paths forThisUser();

    // The running binary's real folder, <tree>/usr/bin for an installed
    // release. Not QCoreApplication::applicationDirPath(): started through
    // AppRun or current, Qt reports the link's folder, not the binary's.
    static QString binaryDir();

    // The release tree the binary in appDir (<tree>/usr/bin) belongs to,
    // when that tree is a version folder of base. Empty otherwise: an
    // AppImage, a build folder, a distribution's package.
    static QString treeOf(const QString& appDir, const QString& base);

    // AetherSDR --install, from an unpacked tarball (or, later, from inside
    // the AppImage): copies tree into <base>/<version>, makes it current,
    // and points the menu entry, its icon and the aethersdr command at
    // current. A file of the user's own at ~/.local/bin/aethersdr is left
    // alone; note says so.
    static bool install(const QString& tree, const QString& version, const Paths& paths,
                        QString* problem, QString* note = nullptr);

    // Runs <tree>/AppRun --update-selftest and waits for it (30 s at most):
    // true when it wrote version as its marker.
    using SelfTest = std::function<bool(const QString& tree, const QString& version)>;
    static bool selfTest(const QString& tree, const QString& version);

    // The switch: moves stagedDir to <base>/<version>, runs test on it, and
    // only if it passes makes it current. Every release folder except
    // version and keep (the running release) is then removed. When the test
    // fails, the new folder is removed and current is untouched.
    static bool switchTo(const QString& base, const QString& stagedDir, const QString& version,
                         const QString& keep, const SelfTest& test, QString* problem);

    // Starts program once process pid has exited, giving up after two
    // minutes: Restart to update's relaunch, without a second instance
    // overlapping the one that is closing.
    static bool startAfterExit(qint64 pid, const QString& program);
};

} // namespace AetherSDR
