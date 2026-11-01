#include "MacInstall.h"

#include "LogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUuid>
#include <QXmlStreamReader>

#include <filesystem>
#include <system_error>
#include <vector>

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#include <fcntl.h>      // AT_FDCWD
#include <stdio.h>      // renamex_np, RENAME_SWAP (macOS); renameat2, RENAME_EXCHANGE (Linux)
#include <string.h>     // strerror
#include <errno.h>
#include <sys/stat.h>   // stat, for the volume
#include <signal.h>     // kill
#include <unistd.h>     // getpid
#endif
#if defined(Q_OS_MACOS)
#include <libproc.h>    // proc_listallpids, proc_pidpath
#endif

namespace fs = std::filesystem;

namespace AetherSDR {

namespace {

const QString kBinary = QStringLiteral("Contents/MacOS/AetherSDR");

fs::path toPath(const QString& s)
{
    return fs::path(QFile::encodeName(s).toStdString());
}

// a and b trade places in one step. macOS's renamex_np(RENAME_SWAP); Linux
// has the same call as renameat2(RENAME_EXCHANGE), which lets the tests run
// there too.
bool exchange(const QString& a, const QString& b, QString* problem)
{
#if defined(Q_OS_MACOS)
    if (::renamex_np(QFile::encodeName(a).constData(), QFile::encodeName(b).constData(), RENAME_SWAP) == 0) {
        return true;
    }
    *problem = QString::fromLocal8Bit(strerror(errno));
    return false;
#elif defined(Q_OS_LINUX)
    if (::renameat2(AT_FDCWD, QFile::encodeName(a).constData(), AT_FDCWD, QFile::encodeName(b).constData(),
                    RENAME_EXCHANGE)
        == 0) {
        return true;
    }
    *problem = QString::fromLocal8Bit(strerror(errno));
    return false;
#else
    Q_UNUSED(a);
    Q_UNUSED(b);
    *problem = QStringLiteral("Swapping a release in place isn't available on this system.");
    return false;
#endif
}

// Whether process pid exists (EPERM: it does, as another user's).
bool processRuns(qint64 pid)
{
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void removeTree(const QString& path)
{
    std::error_code ec;
    fs::remove_all(toPath(path), ec);
}

} // namespace

QString MacInstall::bundleOf(const QString& appDir)
{
    QDir dir(QFileInfo(appDir).canonicalFilePath());
    if (dir.dirName() != QLatin1String("MacOS") || !dir.cdUp() || dir.dirName() != QLatin1String("Contents")
        || !dir.cdUp() || !QFileInfo::exists(dir.filePath(QStringLiteral("Contents/Info.plist")))) {
        return {};
    }
    return dir.absolutePath();
}

bool MacInstall::translocated(const QString& bundle)
{
    return bundle.contains(QLatin1String("/AppTranslocation/"));
}

bool MacInstall::sameVolume(const QString& a, const QString& b)
{
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    struct stat sa{};
    struct stat sb{};
    return ::stat(QFile::encodeName(a).constData(), &sa) == 0 && ::stat(QFile::encodeName(b).constData(), &sb) == 0
           && sa.st_dev == sb.st_dev;
#else
    Q_UNUSED(a);
    Q_UNUSED(b);
    return false;
#endif
}

bool MacInstall::teamOf(const QString& bundle, QString* team)
{
    team->clear();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/usr/bin/codesign"), {QStringLiteral("-dv"), QStringLiteral("--verbose=2"), bundle});
    if (!p.waitForStarted(10000) || !p.waitForFinished(30000) || p.exitStatus() != QProcess::NormalExit
        || p.exitCode() != 0) {
        p.kill();
        p.waitForFinished(5000);
        return false;
    }
    const QString said = QString::fromLocal8Bit(p.readAll());
    static const QRegularExpression line(QStringLiteral("^TeamIdentifier=(.*)$"),
                                         QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = line.match(said);
    if (!m.hasMatch()) {
        return false;
    }
    const QString value = m.captured(1).trimmed();
    static const QRegularExpression id(QStringLiteral("^[A-Z0-9]{10}$"));
    if (value == QLatin1String("not set")) {
        return true;   // ad hoc: no team
    }
    if (!id.match(value).hasMatch()) {
        return false;
    }
    *team = value;
    return true;
}

MacInstall::Signature MacInstall::verifySignature(const QString& tree, const QString& team, QString* problem)
{
    QStringList args{QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict")};
    if (!team.isEmpty()) {
        // A Developer ID Application certificate (Apple's marker OID) of the
        // team that signed this copy: a tree re-signed by anyone else in the
        // staging folder, or with another of the team's certificates, is
        // refused.
        args << QStringLiteral("-R")
             << QStringLiteral("=anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.13] "
                               "and certificate leaf[subject.OU] = \"%1\"")
                    .arg(team);
    }
    args << tree;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/usr/bin/codesign"), args);
    if (!p.waitForStarted(10000) || !p.waitForFinished(120000) || p.exitStatus() != QProcess::NormalExit) {
        p.kill();
        p.waitForFinished(5000);
        *problem = QStringLiteral("Its code signature could not be checked.");
        return Signature::NotChecked;
    }
    if (p.exitCode() != 0) {
        const QString said = QString::fromLocal8Bit(p.readAll()).trimmed();
        qCWarning(lcUpdater).noquote() << "code signature check failed:" << said;
        *problem = QStringLiteral("Its code signature doesn't match: %1").arg(said);
        return Signature::Invalid;
    }
    return Signature::Valid;
}

bool MacInstall::selfTest(const QString& tree, const QString& version)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return false;
    }
    const QString marker = dir.filePath(QStringLiteral("selftest"));
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(QDir(tree).filePath(kBinary), {QStringLiteral("--update-selftest"), marker});
    if (!p.waitForStarted(10000)) {
        qCWarning(lcUpdater).noquote() << "self-test of v" + version + " could not be started";
        return false;
    }
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished(5000);
        qCWarning(lcUpdater).noquote() << "self-test of v" + version + " did not finish";
        return false;
    }
    QFile f(marker);
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0
                    && f.open(QIODevice::ReadOnly) && f.readAll() == version.toUtf8();
    if (!ok) {
        qCWarning(lcUpdater).noquote() << "self-test of v" + version + " failed, exit code" << p.exitCode();
    }
    return ok;
}

bool MacInstall::otherInstanceRunning(const QString& bundle)
{
    const QString binary = QFileInfo(QDir(bundle).filePath(kBinary)).canonicalFilePath();
    if (binary.isEmpty()) {
        return false;
    }
#if defined(Q_OS_MACOS)
    const pid_t self = ::getpid();
    // proc_listallpids returns how many pids it wrote, not bytes.
    std::vector<pid_t> pids(16384);
    const int count = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < count && i < static_cast<int>(pids.size()); ++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[i] != self && ::proc_pidpath(pids[i], path, sizeof(path)) > 0
            && QFileInfo(QString::fromLocal8Bit(path)).canonicalFilePath() == binary) {
            return true;
        }
    }
#elif defined(Q_OS_LINUX)
    // The same question through /proc, so the tests run here too.
    const pid_t self = ::getpid();
    for (const QString& pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        const pid_t p = static_cast<pid_t>(pid.toLongLong(&number));
        if (number && p != self
            && QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid))).canonicalFilePath() == binary) {
            return true;
        }
    }
#endif
    return false;
}

std::optional<QString> MacInstall::daxDriverSourceOf(const QString& driverBundle)
{
    QFile f(QDir(driverBundle).filePath(QStringLiteral("Contents/Info.plist")));
    if (!f.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    // The top-level dict's AetherSDRDAXSource string, in an XML plist.
    QXmlStreamReader xml(&f);
    int depth = 0;
    bool wanted = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const auto name = xml.name();
            if (depth == 3 && name == QLatin1String("key")) {
                wanted = xml.readElementText() == QLatin1String("AetherSDRDAXSource");
                --depth;
            } else if (depth == 3 && wanted && name == QLatin1String("string")) {
                return xml.readElementText().trimmed();
            } else if (depth == 3) {
                wanted = false;
            }
        } else if (xml.isEndElement()) {
            --depth;
        }
    }
    return QString();
}

MacInstall::Checks MacInstall::real(const QString& runningBundle)
{
    QString team;
    const bool known = teamOf(runningBundle, &team);
    return {[known, team](const QString& tree, QString* problem) {
                // Not knowing who signed this copy never drops the pin: the
                // check waits for a time it can tell.
                if (!known) {
                    *problem = QStringLiteral("Its code signature could not be checked.");
                    return Signature::NotChecked;
                }
                return verifySignature(tree, team, problem);
            },
            &MacInstall::selfTest};
}

MacInstall::Prepared MacInstall::prepare(const QString& bundle, const QString& stagedDir, const QString& version,
                                         const Checks& checks, bool retry, QString* ready, QString* problem)
{
    if (bundle.isEmpty() || !QFileInfo::exists(QDir(stagedDir).filePath(kBinary))) {
        *problem = QStringLiteral("There is no release to put in place.");
        return Prepared::Refused;
    }
    const QString parent = QFileInfo(bundle).absolutePath();
    // A copy a run left beside the app when it exited before its swap; one
    // whose process still runs is that instance's, and stays.
    const QStringList left = QDir(parent).entryList({QStringLiteral(".AetherSDR-incoming-*")},
                                                    QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString& name : left) {
        bool number = false;
        const qint64 pid = name.section(QLatin1Char('-'), 2, 2).toLongLong(&number);
        if (!number || !processRuns(pid)) {
            removeTree(QDir(parent).filePath(name));
        }
    }
    QString tree = stagedDir;
    // The swap is one rename, so the new bundle must be on the app's volume:
    // staged elsewhere, it is copied beside the app under a hidden name.
    if (!sameVolume(stagedDir, parent)) {
        tree = QDir(parent).filePath(QStringLiteral(".AetherSDR-incoming-%1-%2")
                                         .arg(QCoreApplication::applicationPid())
                                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
        std::error_code ec;
        fs::copy(toPath(stagedDir), toPath(tree), fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            removeTree(tree);
            *problem = QStringLiteral("The release could not be copied beside %1: %2.")
                           .arg(bundle, QString::fromLocal8Bit(ec.message().c_str()));
            return Prepared::Refused;
        }
    }
    const auto drop = [&] {
        if (tree != stagedDir) {
            removeTree(tree);
        }
    };
    switch (checks.signature(tree, problem)) {
    case Signature::Valid:
        break;
    case Signature::NotChecked:   // nothing known against the release: kept
        drop();
        return Prepared::Kept;
    case Signature::Invalid:      // no second try changes a signature
        drop();
        removeTree(stagedDir);
        return Prepared::Failed;
    }
    if (!checks.selfTest(tree, version)) {
        drop();
        if (retry) {
            *problem = QStringLiteral("AetherSDR %1 didn't pass its start-up check; it is tried again next time.")
                           .arg(version);
            return Prepared::Kept;
        }
        removeTree(stagedDir);
        *problem = QStringLiteral("AetherSDR %1 failed its start-up check, so it wasn't installed.").arg(version);
        return Prepared::Failed;
    }
    *ready = tree;
    return Prepared::Ready;
}

bool MacInstall::swap(const QString& bundle, const QString& ready, const QString& previousDir, QString* problem)
{
    QString why;
    if (!exchange(ready, bundle, &why)) {
        *problem = QStringLiteral("The new release could not be put in place: %1.").arg(why);
        return false;
    }
    // ready now holds the bundle that was running: the previous version.
    const QString keepIn = previousDir.isEmpty() ? QString() : QFileInfo(previousDir).absolutePath();
    if (!keepIn.isEmpty() && QDir().mkpath(keepIn) && sameVolume(ready, keepIn)) {
        removeTree(previousDir);
        std::error_code ec;
        fs::rename(toPath(ready), toPath(previousDir), ec);
        if (!ec) {
            return true;
        }
    }
    removeTree(ready);
    return true;
}

} // namespace AetherSDR
