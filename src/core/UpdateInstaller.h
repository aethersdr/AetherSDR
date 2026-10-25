#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// The app's side of putting a staged update in place (RFC #5304 Phase 3,
// Windows): it hands the staged release to AetherSDRUpdater.exe, which swaps
// it in after AetherSDR exits (src/updater/UpdateSwap.h), and at the next
// start reads what the helper did.
//
// The helper is copied out of the install before it runs, into stateDir():
// it renames the install, so it must not be running from inside it.
class UpdateInstaller {
public:
    // Per-user app data, outside the install: the staged release, the
    // helper's copy, its journal, log and result. The Updates window stages
    // here (next PR).
    static QString stateDir();

    // AetherSDRUpdater's command line (protocol 1, src/updater/main_win.cpp).
    static QStringList helperArguments(qint64 pid, const QString& installDir,
                                       const QString& stagedDir, const QString& stateDir,
                                       const QString& version, bool restart);

    // Copies the helper beside the state and starts it, waiting for this
    // process to exit. restart: start the app again when done (Restart to
    // update); otherwise the swap happens as AetherSDR quits. Windows only;
    // false with problem set elsewhere or when the helper cannot start.
    static bool launchHelper(const QString& stagedDir, const QString& version, bool restart,
                             QString* problem);

    // What the helper did, from <stateDir>/swap-result.txt, once: the file
    // is removed when read. A rolled-back release is remembered in
    // Updates.rolledBackVersion, so it is never staged again.
    struct SwapResult {
        QString outcome;   // "Updated", "RolledBack", ... (UpdateSwap's names); empty: none
        QString version;
    };
    static SwapResult takeSwapResult(const QString& stateDir = UpdateInstaller::stateDir());

    // --update-selftest <marker>: what the new version runs after the swap.
    // Writes this build's version to marker. True when written.
    static bool writeSelfTestMarker(const QString& marker, const QString& version);
};

} // namespace AetherSDR
