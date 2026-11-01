#include "UpdateInstaller.h"

#include "LinuxInstall.h"
#include "LogManager.h"
#include "UpdateChecker.h"
#include "UpdateSettings.h"
#include "UpdateStager.h"
#include "updater/UpdateSwap.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <mutex>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AetherSDR {

namespace {
const QString kHelper = QStringLiteral("AetherSDRUpdater.exe");
using updater::kProtocol;   // one number for the app and the helper

// The outcomes UpdateSwap writes, by its own names.
bool knownOutcome(const QString& o)
{
    using updater::Outcome;
    for (Outcome x : {Outcome::Updated, Outcome::RolledBack, Outcome::OldStillRunning, Outcome::NotPossible,
                      Outcome::SwapUndone, Outcome::Broken}) {
        if (o == QLatin1String(updater::outcomeName(x))) {
            return true;
        }
    }
    return false;
}
// What happened, for the next start to read (takeSwapResult).
void writeSwapResult(const QString& stateDir, updater::Outcome outcome, const QString& version)
{
    QDir().mkpath(stateDir);
    QSaveFile f(QDir(stateDir).filePath(QStringLiteral("swap-result.txt")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray(updater::outcomeName(outcome)) + ' ' + version.toUtf8() + '\n');
        f.commit();
    }
}

// The swap prepareSwap readied, for finishSwapOnExit. The checks run on a
// worker thread, so it is read and written under one lock; generation is
// bumped by every drop, and a preparation that started before one never
// publishes its result.
struct PendingSwap {
    QString bundle;
    QString staged;
    QString ready;
    QString version;
    QString stateDir;
    bool restart = false;
};
struct SwapState {
    std::mutex lock;
    PendingSwap pending;
    quint64 generation = 0;
};
SwapState& swapState()
{
    static SwapState s;
    return s;
}

// MacInstall::otherInstanceRunning, replaced by tests.
std::function<bool(const QString& bundle)>& otherInstance()
{
    static std::function<bool(const QString&)> f = &MacInstall::otherInstanceRunning;
    return f;
}

#ifdef Q_OS_WIN
// Elevated past a UAC boundary: the split token's full half. With UAC off,
// or as the built-in Administrator, there is no lower level to escape from.
bool processElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return true;   // unknown: never assume it is safe
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
    CloseHandle(token);
    return !ok || type == TokenElevationTypeFull;
}
#endif
} // namespace

QString UpdateInstaller::stateDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("update"));
}

QStringList UpdateInstaller::helperArguments(qint64 pid, const QString& installDir,
                                             const QString& stagedDir, const QString& stateDir,
                                             const QString& version, bool restart)
{
    QStringList args{QStringLiteral("--protocol"), QString::number(kProtocol),
                     QStringLiteral("--wait-pid"), QString::number(pid),
                     QStringLiteral("--install"), QDir::toNativeSeparators(installDir),
                     QStringLiteral("--staged"), QDir::toNativeSeparators(stagedDir),
                     QStringLiteral("--state-dir"), QDir::toNativeSeparators(stateDir),
                     QStringLiteral("--version"), version};
    if (restart) {
        args << QStringLiteral("--restart");
    }
    return args;
}

bool UpdateInstaller::canInstallInPlace(QString* why)
{
    QString reason;
#ifdef Q_OS_WIN
    const QString install = QCoreApplication::applicationDirPath();
    if (!UpdateChecker::storeName().isEmpty()) {
        reason = QStringLiteral("Updates for this copy come from %1.").arg(UpdateChecker::storeName());
    } else if (processElevated()) {
        reason = QStringLiteral("AetherSDR is running as administrator. Run the installer to update a "
                                "system-wide install.");
    } else if (!QFileInfo::exists(QDir(install).filePath(kHelper))) {
        reason = QStringLiteral("This copy has no updater.");
    } else {
        // The swap renames the install beside itself: try what it will do.
        QDir parent(install);
        parent.cdUp();
        QTemporaryFile probe(parent.filePath(QStringLiteral(".aethersdr-update-XXXXXX")));
        if (!probe.open()) {
            reason = QStringLiteral("The folder AetherSDR is installed in can't be changed by this user.");
        }
    }
#elif defined(Q_OS_LINUX)
    const LinuxInstall::Paths paths = LinuxInstall::forThisUser();
    if (LinuxInstall::treeOf(LinuxInstall::binaryDir(), paths.base).isEmpty()) {
        reason = qEnvironmentVariableIsSet("APPIMAGE")
                     ? QStringLiteral("The AppImage updates by downloading the new one.")
                     : QStringLiteral("This copy wasn't installed with AetherSDR --install.");
    } else {
        // The new release goes in beside this one: try what the switch will do.
        QTemporaryFile probe(QDir(paths.base).filePath(QStringLiteral(".aethersdr-update-XXXXXX")));
        if (!probe.open()) {
            reason = QStringLiteral("The folder AetherSDR is installed in can't be changed by this user.");
        }
    }
#elif defined(Q_OS_MACOS)
    const QString bundle = MacInstall::bundleOf(QCoreApplication::applicationDirPath());
    if (bundle.isEmpty()) {
        reason = QStringLiteral("This copy isn't an app bundle.");
    } else if (MacInstall::translocated(bundle)) {
        reason = QStringLiteral("macOS is running this copy from a temporary place. Move AetherSDR to your "
                                "Applications folder, then open it from there.");
    } else {
        // The swap moves the bundle within its folder: try what it will do.
        QTemporaryFile probe(QDir(QFileInfo(bundle).absolutePath()).filePath(QStringLiteral(".aethersdr-update-XXXXXX")));
        if (!probe.open() || !QFileInfo(bundle).isWritable()) {
            reason = QStringLiteral("The folder AetherSDR is installed in can't be changed by this user.");
        }
    }
#else
    reason = QStringLiteral("Installing updates in place isn't available on this system.");
#endif
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

bool UpdateInstaller::launchHelper(const QString& stagedDir, const QString& version, bool restart,
                                   QString* problem)
{
#ifdef Q_OS_WIN
    // In place is per-user only. Elevated, the helper would run from a
    // user-writable folder at the elevated level, and a swapped-in tree
    // would keep that folder's permissions inside Program Files.
    if (processElevated()) {
        *problem = QStringLiteral("AetherSDR is running as administrator. "
                                  "Run the installer to update a system-wide install.");
        return false;
    }
    const QString install = QCoreApplication::applicationDirPath();
    const QString state = stateDir();
    QDir().mkpath(state);
    const QString source = QDir(install).filePath(kHelper);
    const QString copy = QDir(state).filePath(kHelper);
    QFile::remove(copy);
    if (!QFile::copy(source, copy)) {
        *problem = QStringLiteral("The updater could not be copied out of the install.");
        return false;
    }
    const QStringList args = helperArguments(QCoreApplication::applicationPid(), install, stagedDir,
                                             state, version, restart);
    if (!QProcess::startDetached(copy, args, state)) {
        *problem = QStringLiteral("The updater could not be started.");
        return false;
    }
    qCInfo(lcUpdater).noquote() << "updater started for v" + version
                                       + (restart ? " (restart)" : " (on quit)");
    return true;
#elif defined(Q_OS_LINUX)
    // No helper on Linux: the new release goes in beside this one and becomes
    // current now, which the running app never notices; the result is
    // reported at the next start, as the helper's is.
    const LinuxInstall::Paths paths = LinuxInstall::forThisUser();
    const QString running = LinuxInstall::treeOf(LinuxInstall::binaryDir(), paths.base);
    if (running.isEmpty()) {
        *problem = QStringLiteral("This copy wasn't installed with AetherSDR --install.");
        return false;
    }
    if (!switchRelease(paths.base, stagedDir, version, QFileInfo(running).fileName(), LinuxInstall::selfTest,
                       restart, stateDir(), problem)) {
        qCWarning(lcUpdater).noquote() << "update to v" + version + " not installed:" << *problem;
        return false;
    }
    const QString state = stateDir();
    // The tree has moved into the install (or was not needed): its record
    // goes with it.
    UpdateStager::discard(state, version);
    if (restart) {
        const QString run = QDir(paths.base).filePath(QStringLiteral("current/AppRun"));
        if (!LinuxInstall::startAfterExit(QCoreApplication::applicationPid(), {run})) {
            qCWarning(lcUpdater) << "the restart could not be arranged; AetherSDR quits without it";
        }
    }
    qCInfo(lcUpdater).noquote() << "v" + version + " installed" + (restart ? " (restart)" : " (on quit)");
    return true;
#elif defined(Q_OS_MACOS)
    // No helper on macOS either: checked now, swapped in as AetherSDR exits.
    const QString bundle = MacInstall::bundleOf(QCoreApplication::applicationDirPath());
    if (!prepareSwap(bundle, stagedDir, version, MacInstall::real(bundle), restart, stateDir(), problem)) {
        qCWarning(lcUpdater).noquote() << "update to v" + version + " not installed:" << *problem;
        return false;
    }
    qCInfo(lcUpdater).noquote() << "v" + version + " is ready; it goes in as AetherSDR exits"
                                       + (restart ? " (restart)" : "");
    return true;
#else
    Q_UNUSED(stagedDir);
    Q_UNUSED(version);
    Q_UNUSED(restart);
    *problem = QStringLiteral("Installing updates in place isn't available on this system.");
    return false;
#endif
}

bool UpdateInstaller::switchRelease(const QString& base, const QString& stagedDir, const QString& version,
                                    const QString& keep,
                                    const std::function<bool(const QString& tree, const QString& version)>& selfTest,
                                    bool restart, const QString& stateDir, QString* problem)
{
    using Switch = LinuxInstall::Switch;
    const int tried = restart ? 0 : UpdateSettings::quitSwitchTries(version);
    const Switch result = LinuxInstall::switchTo(base, stagedDir, version, keep, selfTest,
                                                 !restart && tried + 1 < kQuitSwitchTries, problem);
    if (result == Switch::Kept && !restart) {
        UpdateSettings::setQuitSwitchTries(version, tried + 1);
    }
    const auto report = [&](updater::Outcome outcome) { writeSwapResult(stateDir, outcome, version); };
    if (result == Switch::Failed) {
        UpdateSettings::setRolledBackVersion(version);   // never offered again
        if (!restart) {
            report(updater::Outcome::RolledBack);   // told at the next start
        }
    } else if (result == Switch::Switched) {
        report(updater::Outcome::Updated);
    }
    return result == Switch::Switched || result == Switch::Current;
}

namespace {
UpdateInstaller::SwapResult& lastRead()
{
    static UpdateInstaller::SwapResult r;
    return r;
}
} // namespace

UpdateInstaller::SwapResult UpdateInstaller::lastSwapResult()
{
    return lastRead();
}

UpdateInstaller::SwapResult UpdateInstaller::takeSwapResult(const QString& stateDir)
{
    lastRead() = {};
    const QString path = QDir(stateDir).filePath(QStringLiteral("swap-result.txt"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QStringList parts = QString::fromUtf8(f.readAll()).trimmed().split(QLatin1Char(' '));
    f.close();
    QFile::remove(path);
    // Checked before anything of it is logged or kept: the file is outside
    // the install, and only the helper's own words are believed.
    static const QRegularExpression version(QStringLiteral("^[0-9]{1,4}(\\.[0-9]{1,6}){2,3}$"));
    if (parts.size() != 2 || !knownOutcome(parts[0]) || !version.match(parts[1]).hasMatch()) {
        qCWarning(lcUpdater) << "ignoring an unreadable swap result";
        return {};
    }
    SwapResult r{parts[0], parts[1]};
    qCInfo(lcUpdater).noquote() << "last update:" << r.outcome << "v" + r.version;
    if (r.outcome == QLatin1String("RolledBack")) {
        UpdateSettings::setRolledBackVersion(r.version);
    }
    lastRead() = r;
    return r;
}

bool UpdateInstaller::prepareSwap(const QString& bundle, const QString& stagedDir, const QString& version,
                                  const MacInstall::Checks& checks, bool restart, const QString& stateDir,
                                  QString* problem)
{
    using Prepared = MacInstall::Prepared;
    quint64 generation = 0;
    {
        const std::lock_guard<std::mutex> g(swapState().lock);
        generation = swapState().generation;
    }
    const int tried = restart ? 0 : UpdateSettings::quitSwitchTries(version);
    QString ready;
    const Prepared result = MacInstall::prepare(bundle, stagedDir, version, checks,
                                                !restart && tried + 1 < kQuitSwitchTries, &ready, problem);
    if (result == Prepared::Kept && !restart) {
        UpdateSettings::setQuitSwitchTries(version, tried + 1);
    }
    if (result == Prepared::Failed) {
        UpdateSettings::setRolledBackVersion(version);   // never offered again
        if (!restart) {
            writeSwapResult(stateDir, updater::Outcome::RolledBack, version);   // told at the next start
        }
    }
    if (result != Prepared::Ready) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> g(swapState().lock);
        if (swapState().generation == generation) {
            swapState().pending = {bundle, stagedDir, ready, version, stateDir, restart};
            return true;
        }
    }
    // Dropped while the checks ran: another instance is starting from the
    // bundle. The release stays staged; a copy made beside the app goes.
    if (ready != stagedDir) {
        QDir(ready).removeRecursively();
    }
    *problem = QStringLiteral("AetherSDR is restarting, so the update waits for the next quit.");
    return false;
}

bool UpdateInstaller::swapPending()
{
    const std::lock_guard<std::mutex> g(swapState().lock);
    return !swapState().pending.ready.isEmpty();
}

void UpdateInstaller::setOtherInstanceCheckForTesting(std::function<bool(const QString& bundle)> check)
{
    otherInstance() = check ? std::move(check) : std::function<bool(const QString&)>(&MacInstall::otherInstanceRunning);
}

void UpdateInstaller::dropPendingSwap()
{
    const std::lock_guard<std::mutex> g(swapState().lock);
    swapState().pending = {};
    ++swapState().generation;
}

void UpdateInstaller::finishSwapOnExit()
{
    PendingSwap p;
    {
        const std::lock_guard<std::mutex> g(swapState().lock);
        p = swapState().pending;
        swapState().pending = {};
    }
    if (p.ready.isEmpty()) {
        return;
    }
    // A copy made beside the app goes whenever it isn't swapped in.
    const auto dropCopy = [&p] {
        if (p.ready != p.staged) {
            QDir(p.ready).removeRecursively();
        }
    };
    QString problem;
    if (otherInstance()(p.bundle)) {
        // Another AetherSDR runs from the bundle: it waits, as the Windows
        // helper does, and installs at a quit with no other instance.
        dropCopy();
        writeSwapResult(p.stateDir, updater::Outcome::OldStillRunning, p.version);
        qCInfo(lcUpdater).noquote() << "update to v" + p.version + " waits: another AetherSDR runs from" << p.bundle;
    } else if (MacInstall::swap(p.bundle, p.ready, QDir(p.stateDir).filePath(QStringLiteral("previous")), &problem)) {
        // The staged copy is the previous version now, or gone: its record goes.
        UpdateStager::discard(p.stateDir, p.version);
        writeSwapResult(p.stateDir, updater::Outcome::Updated, p.version);
        qCInfo(lcUpdater).noquote() << "v" + p.version + " installed in" << p.bundle;
    } else {
        // Nothing changed. Counted with the failed checks at quit: the third
        // is final, as on Linux, so a swap macOS keeps refusing is not
        // retried at every quit.
        dropCopy();
        writeSwapResult(p.stateDir, updater::Outcome::NotPossible, p.version);
        qCWarning(lcUpdater).noquote() << "update to v" + p.version + " not installed:" << problem;
        const int tried = UpdateSettings::quitSwitchTries(p.version) + 1;
        UpdateSettings::setQuitSwitchTries(p.version, tried);
        if (tried >= kQuitSwitchTries) {
            UpdateSettings::setRolledBackVersion(p.version);   // never offered again
            UpdateStager::discard(p.stateDir, p.version);
        }
    }
    // Restart to update starts AetherSDR again either way: the new bundle,
    // or the one that is still there.
    if (p.restart
        && !LinuxInstall::startAfterExit(QCoreApplication::applicationPid(),
                                         {QStringLiteral("/usr/bin/open"), QStringLiteral("-n"), p.bundle})) {
        qCWarning(lcUpdater) << "the restart could not be arranged";
    }
}

bool UpdateInstaller::writeSelfTestMarker(const QString& marker, const QString& version)
{
    QSaveFile f(marker);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(version.toUtf8());
    return f.commit();
}

} // namespace AetherSDR
