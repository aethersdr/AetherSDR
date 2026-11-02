#pragma once

#include "UpdateStager.h"

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace AetherSDR {

// Install update, in place (RFC #5304 Phase 3): one per app, held by
// MainWindow, so a staged release outlives the Updates window that started
// it. It stages the latest release with UpdateStager (a patch, or the full
// release), keeps what is staged across launches (stagedRelease()), and
// hands it to AetherSDRUpdater.exe on Restart to update or as the app quits
// (UpdateInstaller). The Updates window and the title-bar badge show its
// state.
//
// Everything outside it is in Environment, so update_controller_test drives
// the whole flow on any platform with file:// downloads and a fake helper.
class UpdateController : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,      // nothing staged, nothing running
        Staging,   // downloading and applying; progress() reports it
        Staged,    // stagedVersion() is ready for Restart to update or the quit
        Installing,   // Restart to update: checking it and handing it over
                      // (on Linux, switching it in), on a worker thread
        Handed,    // the helper has it and is waiting for AetherSDR to exit
    };

    struct Environment {
        // What every stage() sends: running version, platform, arch,
        // install and staging roots, manifest URL, full fallback. The
        // latest tag is filled in per call.
        UpdateStager::Request request;
        // UpdateInstaller::canInstallInPlace.
        std::function<bool(QString* why)> canInstall;
        // UpdateInstaller::launchHelper.
        std::function<bool(const QString& stagedDir, const QString& version, bool restart,
                           QString* problem)>
            launchHelper;
        // UpdateSettings::rolledBackVersion: a release the helper rolled
        // back is never staged or offered again.
        std::function<QString()> rolledBackVersion;

        // Install for updates (RFC #5304 Phase 6), on Linux: the AppImage
        // file AetherSDR runs from ($APPIMAGE); empty when it isn't one.
        QString appImage;
        // Where Install for updates puts it, as shown ("~/.local/share/...").
        QString installFolder;
        // The release installed for updates (LinuxInstall's current); empty:
        // nothing is installed.
        std::function<QString()> installedVersion;
        // Installs the running release, the AppImage's own tree, for
        // updates (LinuxInstall::install), never over an install made
        // meanwhile. Runs on a worker thread.
        std::function<bool(QString* problem, QString* note)> installRunning;
        // Moves file to the trash. Runs on a worker thread.
        std::function<bool(const QString& file)> trash;
        // Starts the installed release once this process has exited.
        std::function<bool()> startInstalled;

        // macOS (RFC #5304 Phase 4): the source stamped into the installed DAX
        // driver (MacInstall::daxDriverSourceOf): nullopt when none is
        // installed, empty when it predates the stamp.
        std::function<std::optional<QString>()> installedDaxDriverSource;
    };

    // This app's: the install it runs from, UpdateInstaller::stateDir(),
    // the real helper.
    static Environment forThisApp();

    // What AetherSDRUpdater did after the last run ("Updated", "RolledBack",
    // ...; empty outcome: nothing), as main.cpp read it at start, or
    // "Installed" once, in the copy Install for updates restarted into.
    struct LastUpdate {
        QString outcome;
        QString version;
    };
    static LastUpdate lastUpdate();

    explicit UpdateController(Environment env, QObject* parent = nullptr);

    // Whether Install update can be offered at all (why, when not).
    bool canInstallInPlace(QString* why = nullptr) const;
    // A release the helper rolled back here: never staged, offered or
    // badged again.
    bool isRolledBack(const QString& version) const;

    // Install for updates: the AppImage whose release can be installed for
    // updates, when AetherSDR runs as one and nothing is installed yet;
    // empty otherwise.
    QString appImageToInstall() const;
    // The release installed for updates; empty when none.
    QString installedVersion() const;
    // The AppImage AetherSDR runs from; empty when it isn't one.
    QString appImage() const { return m_env.appImage; }
    QString installFolder() const { return m_env.installFolder; }
    bool isInstallingAppImage() const { return m_installingAppImage; }
    // Copies the running release into the install for updates on a worker
    // thread, then, with trash, moves the AppImage to the trash. Ends in
    // appImageInstalled.
    void installAppImage(bool trash);

    // A staged release's DAX driver differs from the installed one: the
    // Updates window says to install it from that release's disk image (the
    // in-app update installs the app only).
    bool daxDriverChanges() const;
    // The rule itself: the release names a source, a driver is installed, and
    // its stamp differs (an unstamped driver, from before the stamp, too).
    static bool daxDriverChanges(const std::optional<QString>& installed, const QString& staged);

    State state() const { return m_state; }
    // The release staged, or being staged; empty when Idle.
    QString version() const { return m_version; }

    // What is staged from an earlier run, for the running version and this
    // install. latestVersion is GitHub's latest ("26.10.4"; empty when it is
    // not known yet, at startup): a staged release that is no longer the
    // latest is discarded. While Staging nothing is swept: the answer is
    // kept, and a download it supersedes is discarded when it finishes.
    void refreshStaged(const QString& latestVersion);

    // Install update: stage latestVersion. A full fallback that cannot be
    // done in place ends in browserNeeded().
    void install(const QString& latestVersion);
    // Stops staging; nothing is kept.
    void cancel();
    // Skip this version: a staged release of that version is removed.
    void discard(const QString& version);

    // Restart to update: checks the staged files again and hands them to
    // the helper, which starts the new version once AetherSDR has quit, on
    // a worker thread (on Linux the switch and its self-test run there, up
    // to a minute for a release that hangs). False with problem when
    // nothing is staged; otherwise restartReady says how it went, and the
    // caller quits when it was handed over.
    bool restartToUpdate(QString* problem);
    // As the app quits: the same, silently, without the restart, waited
    // for. False when nothing was handed over.
    bool installOnQuit();
    // macOS: forgets a swap readied for this exit, because another instance
    // is starting from the bundle (a UI-scale restart). It stays staged.
    void dropPendingSwap();

signals:
    void stateChanged();
    void progress(const QString& step, qint64 done, qint64 total);
    // Staging version can't be done in place: offer the download in the
    // browser.
    void browserNeeded(const QString& reason, const QString& version);
    // Staging version didn't work (the manifest couldn't be trusted or
    // fetched, it isn't published yet, or the release was rolled back here),
    // or a newer release superseded it while it downloaded: then version is
    // the newer one and reason says so. Every attempt ends in staged,
    // browserNeeded or failed.
    void failed(const QString& reason, const QString& version);
    // Restart to update finished: handed over (quit now), or not, with
    // problem (the staged release is discarded when it can't be used again).
    void restartReady(bool handed, const QString& problem, const QString& version);
    // Install for updates finished. restart: it is installed and starts
    // once AetherSDR exits (quit now). Otherwise problem says what went
    // wrong: it wasn't installed, or it was but the AppImage couldn't be
    // moved to the trash or the installed copy started, and nothing restarts.
    void appImageInstalled(bool restart, const QString& problem);

private:
    // What a hand-over did, worked out off the GUI thread.
    struct HandOver {
        bool handed = false;
        bool changed = false;    // the staged files changed on disk
        bool consumed = false;   // the staged tree is gone (Linux's switch used it up)
        QString problem;
    };
    static HandOver runHandOver(const Environment& env, const UpdateStager::StagedRelease& staged, bool restart);
    bool finishHandOver(const HandOver& h, const UpdateStager::StagedRelease& staged, QString* problem);
    // A staging attempt for attempted ended without a release.
    void endAttempt(const QString& attempted, const QString& reason, bool browser);
    // A newer latest has been heard than version.
    bool superseded(const QString& version) const;
    // Idle, telling the window the newer latest replaced attempted.
    void supersede(const QString& attempted);
    void setState(State s, const QString& version);

    Environment m_env;
    UpdateStager* m_stager;
    State m_state{State::Idle};
    bool m_installingAppImage{false};
    // The staged release's DAX driver source, read once per staged version.
    mutable QString m_daxSourceVersion;
    mutable QString m_daxSource;
    QString m_version;
    UpdateStager::StagedRelease m_staged;
    QString m_latest;   // GitHub's latest as last heard; empty: not known
};

} // namespace AetherSDR
