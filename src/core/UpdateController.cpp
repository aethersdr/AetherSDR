#include "UpdateController.h"

#include "LogManager.h"
#include "UpdateInstaller.h"
#include "UpdateSettings.h"
#include "VersionNumber.h"

#include <QCoreApplication>
#include <QSysInfo>

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
    env.request.stagingRoot = UpdateInstaller::stateDir();
    // The portable zip is the tree setup.exe installs, so either install
    // can take it when no patch applies.
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
    if (s != State::Staged && s != State::Handed) {
        m_staged = {};
    }
    emit stateChanged();
}

void UpdateController::refreshStaged(const QString& latestVersion)
{
    if (!latestVersion.isEmpty()) {
        m_latest = latestVersion;
    }
    if (m_state == State::Staging || m_state == State::Handed) {
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
    if (m_state == State::Staging || m_state == State::Handed || latestVersion.isEmpty()) {
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

bool UpdateController::handOver(bool restart, QString* problemOut)
{
    QString scratch;
    QString* problem = problemOut ? problemOut : &scratch;
    if (m_state != State::Staged || m_staged.version.isEmpty()) {
        *problem = tr("No update is ready to install.");
        return false;
    }
    const UpdateStager::Request& r = m_env.request;
    QString why;
    // The staged files are checked once more, against the signed manifest
    // kept beside them: nothing changed since staging goes in.
    if (!UpdateStager::revalidate(r.stagingRoot, m_staged, r.platform, r.arch, &why)) {
        qCWarning(lcUpdater).noquote() << "staged update to v" + m_staged.version + " refused:" << why;
        UpdateStager::discard(r.stagingRoot, m_staged.version);
        setState(State::Idle, {});
        *problem = tr("The downloaded update changed on disk, so it was removed. Install it again.");
        return false;
    }
    if (!m_env.launchHelper || !m_env.launchHelper(m_staged.dir, m_staged.version, restart, &why)) {
        *problem = why.isEmpty() ? tr("The updater could not be started.") : why;
        return false;
    }
    const QString version = m_staged.version;
    const UpdateStager::StagedRelease handed = m_staged;
    setState(State::Handed, version);
    m_staged = handed;
    return true;
}

bool UpdateController::restartToUpdate(QString* problem)
{
    return handOver(true, problem);
}

bool UpdateController::installOnQuit()
{
    if (m_state != State::Staged) {
        return false;
    }
    QString problem;
    if (!handOver(false, &problem)) {
        qCWarning(lcUpdater).noquote() << "update not installed on quit:" << problem;
        return false;
    }
    return true;
}

} // namespace AetherSDR
