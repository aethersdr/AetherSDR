#pragma once

#include "MacInstall.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace AetherSDR {

// The app's side of putting a staged update in place (RFC #5304). On
// Windows (Phase 3) it hands the staged release to AetherSDRUpdater.exe,
// which swaps it in after AetherSDR exits (src/updater/UpdateSwap.h). The
// helper is copied out of the install before it runs, into stateDir(): it
// renames the install, so it must not be running from inside it. On Linux
// (Phase 5) there is no helper: the release goes in beside the running one
// and becomes current (LinuxInstall). On macOS (Phase 4) there is none
// either: the new bundle is checked at the hand-over and swapped in as the
// last thing before AetherSDR exits (MacInstall, finishSwapOnExit). Every
// way, the next start reads what happened.
class UpdateInstaller {
public:
    // Per-user app data, outside the install: the staged release, the
    // helper's copy, its journal, log and result. UpdateController stages
    // here.
    static QString stateDir();

    // Whether this copy can update itself in place. Windows: not a store
    // install, not elevated across a UAC boundary, AetherSDRUpdater.exe
    // beside the app, and the install's parent writable (the swap renames
    // the install there). Linux: an install AetherSDR --install made, its
    // folder writable. macOS: an app bundle, not translocated, its folder and
    // the bundle itself writable (the swap moves it). Otherwise why says so,
    // and the Updates window offers the download in the browser.
    static bool canInstallInPlace(QString* why = nullptr);

    // AetherSDRUpdater's command line (protocol 1, src/updater/main_win.cpp).
    static QStringList helperArguments(qint64 pid, const QString& installDir,
                                       const QString& stagedDir, const QString& stateDir,
                                       const QString& version, bool restart);

    // Windows: copies the helper beside the state and starts it, waiting for
    // this process to exit. macOS: prepareSwap, the swap itself waiting for
    // finishSwapOnExit. Linux: switches to the staged release now,
    // after its self-test (LinuxInstall::switchTo). A release that fails it
    // on Restart to update is removed and remembered as rolled back; as
    // AetherSDR quits it stays staged, to try again. restart: start the app
    // again once this one has exited (Restart to update); otherwise it
    // takes effect as AetherSDR quits. False with problem set when nothing
    // was handed over, and elsewhere.
    static bool launchHelper(const QString& stagedDir, const QString& version, bool restart,
                             QString* problem);

    // Linux's hand-over, the switch (LinuxInstall::switchTo) into base
    // beside keep, the running release: true when version is in place (or
    // a newer one already was). Restart to update runs in a live session,
    // so a failed self-test there is the release's fault and final. As
    // AetherSDR quits it may be the session's (a logout takes the display
    // away first), so the release is kept to try again at the next quit,
    // up to the third, which is final. A final failure is remembered as
    // rolled back; what happened is written to stateDir for the next start.
    static bool switchRelease(const QString& base, const QString& stagedDir, const QString& version,
                              const QString& keep,
                              const std::function<bool(const QString& tree, const QString& version)>& selfTest,
                              bool restart, const QString& stateDir, QString* problem);
    static constexpr int kQuitSwitchTries = 3;

    // macOS's hand-over: the staged bundle readied to replace bundle
    // (MacInstall::prepare), its signature and self-test passed, and the swap
    // recorded for finishSwapOnExit. A failed self-test is final on Restart
    // to update and retried at the next quit up to the third, as on Linux; a
    // bad signature is final at once. A final failure is remembered as
    // rolled back.
    static bool prepareSwap(const QString& bundle, const QString& stagedDir, const QString& version,
                            const MacInstall::Checks& checks, bool restart, const QString& stateDir,
                            QString* problem);
    // The last thing before AetherSDR exits (main.cpp, after the event loop
    // and the window are gone): the swap prepareSwap readied, the previous
    // bundle kept in stateDir/previous, the result written for the next
    // start, and with restart, the new bundle opened once this process has
    // exited. Nothing when no swap is ready. While another AetherSDR runs
    // from the bundle it waits (OldStillRunning). A swap that fails counts
    // as a failed try at quit; the third is final.
    static void finishSwapOnExit();
    // Whether a swap is ready for finishSwapOnExit.
    static bool swapPending();
    // Replaces MacInstall::otherInstanceRunning in finishSwapOnExit; null
    // restores it.
    static void setOtherInstanceCheckForTesting(std::function<bool(const QString& bundle)> check);
    // Forgets the readied swap, and any a preparation still running would
    // publish: another instance is starting from the bundle as this one
    // exits (a UI-scale restart). The release stays staged.
    static void dropPendingSwap();

    // What the helper did, from <stateDir>/swap-result.txt, once: the file
    // is removed when read. A rolled-back release is remembered in
    // Updates.rolledBackVersion, so it is never staged again.
    struct SwapResult {
        QString outcome;   // "Updated", "RolledBack", ... (UpdateSwap's names); empty: none
        QString version;
    };
    static SwapResult takeSwapResult(const QString& stateDir = UpdateInstaller::stateDir());
    // What the last takeSwapResult() read (main.cpp reads it at start, before
    // the window that reports it exists).
    static SwapResult lastSwapResult();

    // --update-selftest <marker>: what the new version runs after the swap.
    // Writes this build's version to marker. True when written.
    static bool writeSelfTestMarker(const QString& marker, const QString& version);
};

} // namespace AetherSDR
