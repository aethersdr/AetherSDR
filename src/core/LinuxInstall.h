#pragma once

#include <QString>
#include <QStringList>

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

    // The release current names in base when it is installed (its folder is
    // there with AppRun); empty otherwise, a link left dangling included.
    static QString installedVersion(const QString& base);

    // AetherSDR --install, from an unpacked tarball or from inside the
    // AppImage (Install for updates, RFC #5304 Phase 6): copies tree into <base>/<version>, makes it current,
    // and points the menu entry, its icon and the aethersdr command at
    // current. A file or link of the user's own at ~/.local/bin/aethersdr is
    // left alone; note says so. If the menu entry can't be written, current
    // is not changed, a release folder it copied is removed again, and so is
    // a menu entry it added.
    //
    // Replace (--install) makes version current whatever current named.
    // KeepExisting (Install for updates) makes it current only if nothing
    // is current when it gets there, checked in the same step that makes the
    // link, so an install made while this one copies is never replaced; the
    // copy is then removed again. A current naming no installed release
    // (left by an earlier install) does not count, and is cleared.
    enum class Current { Replace, KeepExisting };
    static bool install(const QString& tree, const QString& version, const Paths& paths,
                        QString* problem, QString* note = nullptr, Current policy = Current::Replace);

    // Runs <tree>/AppRun --update-selftest and waits for it (60 s at most):
    // true when it wrote version as its marker.
    using SelfTest = std::function<bool(const QString& tree, const QString& version)>;
    static bool selfTest(const QString& tree, const QString& version);

    // What the switch did.
    enum class Switch {
        Switched,   // version is current; stagedDir is gone
        Current,    // current already ran version or a newer release; stagedDir removed
        Kept,       // nothing changed and stagedDir is back where it was, to try again
        Failed,     // it failed its self-test and was removed (only when !retry)
        Refused,    // current unchanged: not a release to switch to, or one that
                    // could be neither moved in nor put back (stagedDir is then gone)
    };
    // The switch: moves stagedDir to <base>/<version>, runs test on it, and
    // only if it passes makes it current. Afterwards every release folder
    // except version, keep (the running release), what current named and any
    // another AetherSDR runs from is removed. current is never changed otherwise: a failed test, or a
    // failure to change it, leaves it as it was. retry: a failed test puts
    // the tree back in stagedDir (Kept) rather than removing it (Failed), for
    // a test that may have failed for want of a display, as AetherSDR quits.
    static Switch switchTo(const QString& base, const QString& stagedDir, const QString& version,
                           const QString& keep, const SelfTest& test, bool retry, QString* problem);

    // What an AppImage's runtime sets ($APPIMAGE, $APPDIR, ...): an
    // installed release started from inside the AppImage is run without
    // them, or it would take itself for the AppImage.
    static QStringList appImageVariables();

    // Starts program once process pid has exited, giving up after two
    // minutes: Restart to update's relaunch, without a second instance
    // overlapping the one that is closing. The variables named in unset are
    // removed from what program starts with, and it inherits no descriptor
    // beyond the standard three.
    static bool startAfterExit(qint64 pid, const QString& program, const QStringList& unset = {});
};

} // namespace AetherSDR
