#include "UpdateController.h"

#include "LinuxInstall.h"
#include "LogManager.h"
#include "UpdateInstaller.h"
#include "UpdateSettings.h"
#include "VersionNumber.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSysInfo>
#include <QtConcurrent/QtConcurrentRun>

namespace AetherSDR {

UpdateController::Environment UpdateController::forThisApp()
{
    Environment env;
    env.request.runningVersion = QCoreApplication::applicationVersion();
#if defined(Q_OS_WIN)
    env.request.platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    env.request.platform = QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    env.request.platform = QStringLiteral("linux");
#endif
    env.request.arch = QSysInfo::buildCpuArchitecture();
    env.request.installRoot = QCoreApplication::applicationDirPath();
#if defined(Q_OS_LINUX)
    // The release tree, whose root the manifest's paths start from (AppRun,
    // usr/bin/AetherSDR), not the binary's own folder.
    if (const QString tree = LinuxInstall::treeOf(LinuxInstall::binaryDir(), LinuxInstall::forThisUser().base);
        !tree.isEmpty()) {
        env.request.installRoot = tree;
    }
#endif
    env.request.stagingRoot = UpdateInstaller::stateDir();
    // Windows: the portable zip is the tree setup.exe installs, so either
    // install can take it when no patch applies. Linux: the tarball is the
    // installed tree.
    env.request.fullFallback = true;
    env.canInstall = [](QString* why) { return UpdateInstaller::canInstallInPlace(why); };
    env.launchHelper = [](const QString& dir, const QString& version, bool restart, QString* problem) {
        return UpdateInstaller::launchHelper(dir, version, restart, problem);
    };
    env.rolledBackVersion = [] { return UpdateSettings::rolledBackVersion(); };
    return env;
}

UpdateController::LastUpdate UpdateController::lastUpdate()
{
    const UpdateInstaller::SwapResult r = UpdateInstaller::lastSwapResult();
    return {r.outcome, r.version};
}

UpdateController::UpdateController(Environment env, QObject* parent)
    : QObject(parent)
    , m_env(std::move(env))
    , m_stager(new UpdateStager(this))
{
    connect(m_stager, &UpdateStager::progress, this, &UpdateController::progress);
    connect(m_stager, &UpdateStager::staged, this, [this](const QString& dir, const QString& version) {
        // A newer release was announced while this one downloaded: it is
        // no longer the one to install.
        if (superseded(version)) {
            UpdateStager::discard(m_env.request.stagingRoot, version);
            supersede(version);
            return;
        }
        m_staged = {version, dir, m_env.request.runningVersion};
        setState(State::Staged, version);
    });
    connect(m_stager, &UpdateStager::fullDownloadNeeded, this,
            [this](const QString& reason, const UpdateManifest::Asset&, const QString& version) {
                // The stager names the release it tried.
                endAttempt(version.isEmpty() ? m_version : version, reason, true);
            });
    connect(m_stager, &UpdateStager::upToDate, this, [this] {
        // The signed manifest lags GitHub's latest until it is published.
        endAttempt(m_version,
                   tr("The update for this release isn't published yet. Try again later, or download it."),
                   false);
    });
    connect(m_stager, &UpdateStager::failed, this,
            [this](const QString& reason) { endAttempt(m_version, reason, false); });
}

bool UpdateController::superseded(const QString& version) const
{
    return !m_latest.isEmpty() && VersionNumber::parse(m_latest) > VersionNumber::parse(version);
}

void UpdateController::supersede(const QString& attempted)
{
    qCInfo(lcUpdater).noquote() << "update to v" + attempted + " superseded by v" + m_latest;
    setState(State::Idle, {});
    emit failed(tr("AetherSDR %1 came out while %2 was downloading. Install update gets the newer one.")
                    .arg(m_latest, attempted),
                m_latest);
}

void UpdateController::endAttempt(const QString& attempted, const QString& reason, bool browser)
{
    if (m_state != State::Staging || attempted.isEmpty()) {
        return;   // cancelled or discarded meanwhile: nothing to report
    }
    // An outcome belongs to the release attempted; one a newer latest has
    // superseded meanwhile says nothing about the release now offered.
    if (superseded(attempted)) {
        qCInfo(lcUpdater).noquote() << "update to v" + attempted + " ended: " + reason;
        supersede(attempted);
        return;
    }
    setState(State::Idle, {});
    if (browser) {
        emit browserNeeded(reason, attempted);
    } else {
        emit failed(reason, attempted);
    }
}

bool UpdateController::canInstallInPlace(QString* why) const
{
    return m_env.canInstall && m_env.canInstall(why);
}

bool UpdateController::isRolledBack(const QString& version) const
{
    return !version.isEmpty() && m_env.rolledBackVersion && version == m_env.rolledBackVersion();
}

void UpdateController::setState(State s, const QString& version)
{
    if (s == m_state && version == m_version) {
        return;
    }
    m_state = s;
    m_version = version;
    if (s != State::Staged && s != State::Installing && s != State::Handed) {
        m_staged = {};
    }
    emit stateChanged();
}

void UpdateController::refreshStaged(const QString& latestVersion)
{
    if (!latestVersion.isEmpty()) {
        m_latest = latestVersion;
    }
    if (m_state == State::Staging || m_state == State::Installing || m_state == State::Handed) {
        return;   // the sweep would remove the work in progress
    }
    const QString rolledBack = m_env.rolledBackVersion ? m_env.rolledBackVersion() : QString();
    const UpdateStager::Request& r = m_env.request;
    m_staged = UpdateStager::stagedRelease(r.stagingRoot, r.runningVersion, r.installRoot,
                                           latestVersion.isEmpty() ? QString()
                                                                   : QLatin1Char('v') + latestVersion,
                                           rolledBack);
    if (m_staged.version.isEmpty()) {
        setState(State::Idle, {});
    } else {
        const UpdateStager::StagedRelease kept = m_staged;
        setState(State::Staged, kept.version);
        m_staged = kept;
    }
}

void UpdateController::install(const QString& latestVersion)
{
    if (m_state == State::Staging || m_state == State::Installing || m_state == State::Handed
        || latestVersion.isEmpty()) {
        return;
    }
    if (m_state == State::Staged && m_version == latestVersion) {
        return;   // already staged
    }
    m_latest = latestVersion;
    if (isRolledBack(latestVersion)) {
        emit failed(tr("This release was rolled back after it failed to start, so it isn't installed again."),
                    latestVersion);
        return;
    }
    UpdateStager::Request r = m_env.request;
    r.githubLatestTag = QLatin1Char('v') + latestVersion;
    setState(State::Staging, latestVersion);
    m_stager->stage(r);
}

void UpdateController::cancel()
{
    if (m_state != State::Staging) {
        return;
    }
    m_stager->cancel();
    qCInfo(lcUpdater).noquote() << "staging the update to v" + m_version + " cancelled";
    setState(State::Idle, {});
}

void UpdateController::discard(const QString& version)
{
    if (m_state == State::Staging && m_version == version) {
        cancel();
        return;
    }
    if (m_state == State::Staged && m_version == version) {
        UpdateStager::discard(m_env.request.stagingRoot, version);
        setState(State::Idle, {});
    }
}

UpdateController::HandOver UpdateController::runHandOver(const Environment& env,
                                                         const UpdateStager::StagedRelease& staged, bool restart)
{
    HandOver h;
    const UpdateStager::Request& r = env.request;
    QString why;
    // The staged files are checked once more, against the signed manifest
    // kept beside them: nothing changed since staging goes in.
    if (!UpdateStager::revalidate(r.stagingRoot, staged, r.platform, r.arch, &why)) {
        qCWarning(lcUpdater).noquote() << "staged update to v" + staged.version + " refused:" << why;
        h.changed = true;
        return h;
    }
    if (!env.launchHelper || !env.launchHelper(staged.dir, staged.version, restart, &h.problem)) {
        // On Linux the hand-over is the switch itself: a release that failed
        // its self-test there is gone, and nothing is staged any more.
        h.consumed = !QFileInfo::exists(staged.dir);
        return h;
    }
    h.handed = true;
    return h;
}

bool UpdateController::finishHandOver(const HandOver& h, const UpdateStager::StagedRelease& staged,
                                      QString* problem)
{
    const QString& root = m_env.request.stagingRoot;
    if (h.changed || h.consumed) {
        UpdateStager::discard(root, staged.version);
        setState(State::Idle, {});
        *problem = h.changed ? tr("The downloaded update changed on disk, so it was removed. Install it again.")
                             : h.problem;
        return false;
    }
    if (!h.handed) {
        *problem = h.problem.isEmpty() ? tr("The updater could not be started.") : h.problem;
        setState(State::Staged, staged.version);
        m_staged = staged;
        return false;
    }
    setState(State::Handed, staged.version);
    m_staged = staged;
    return true;
}

bool UpdateController::restartToUpdate(QString* problem)
{
    if (m_state != State::Staged || m_staged.version.isEmpty()) {
        *problem = tr("No update is ready to install.");
        return false;
    }
    const UpdateStager::StagedRelease staged = m_staged;
    setState(State::Installing, staged.version);
    auto* watcher = new QFutureWatcher<HandOver>(this);
    connect(watcher, &QFutureWatcher<HandOver>::finished, this, [this, watcher, staged] {
        QString why;
        const bool handed = finishHandOver(watcher->result(), staged, &why);
        watcher->deleteLater();
        emit restartReady(handed, why, staged.version);
    });
    watcher->setFuture(QtConcurrent::run([env = m_env, staged] { return runHandOver(env, staged, true); }));
    return true;
}

bool UpdateController::installOnQuit()
{
    if (m_state != State::Staged || m_staged.version.isEmpty()) {
        return false;
    }
    // AetherSDR is closing: waited for here, on its way out.
    const UpdateStager::StagedRelease staged = m_staged;
    QString problem;
    if (!finishHandOver(runHandOver(m_env, staged, false), staged, &problem)) {
        qCWarning(lcUpdater).noquote() << "update not installed on quit:" << problem;
        return false;
    }
    return true;
}

} // namespace AetherSDR
