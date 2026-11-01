#include "MacInstall.h"

#include "LogManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

#include <filesystem>
#include <system_error>

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#include <fcntl.h>      // AT_FDCWD
#include <stdio.h>      // renamex_np, RENAME_SWAP (macOS); renameat2, RENAME_EXCHANGE (Linux)
#include <string.h>     // strerror
#include <errno.h>
#include <sys/stat.h>   // stat, for the volume
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

bool MacInstall::verifySignature(const QString& tree, QString* problem)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/usr/bin/codesign"),
            {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), tree});
    if (!p.waitForStarted(10000) || !p.waitForFinished(120000)) {
        p.kill();
        p.waitForFinished(5000);
        *problem = QStringLiteral("Its code signature could not be checked.");
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString said = QString::fromLocal8Bit(p.readAll()).trimmed();
        qCWarning(lcUpdater).noquote() << "code signature check failed:" << said;
        *problem = QStringLiteral("Its code signature doesn't match: %1").arg(said);
        return false;
    }
    return true;
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

MacInstall::Checks MacInstall::real()
{
    return {&MacInstall::verifySignature, &MacInstall::selfTest};
}

MacInstall::Prepared MacInstall::prepare(const QString& bundle, const QString& stagedDir, const QString& version,
                                         const Checks& checks, bool retry, QString* ready, QString* problem)
{
    if (bundle.isEmpty() || !QFileInfo::exists(QDir(stagedDir).filePath(kBinary))) {
        *problem = QStringLiteral("There is no release to put in place.");
        return Prepared::Refused;
    }
    const QString parent = QFileInfo(bundle).absolutePath();
    // A copy an earlier run left beside the app (it exited before its swap).
    const QStringList left = QDir(parent).entryList({QStringLiteral(".AetherSDR-incoming-*")},
                                                    QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString& name : left) {
        removeTree(QDir(parent).filePath(name));
    }
    QString tree = stagedDir;
    // The swap is one rename, so the new bundle must be on the app's volume:
    // staged elsewhere, it is copied beside the app under a hidden name.
    if (!sameVolume(stagedDir, parent)) {
        tree = QDir(parent).filePath(QStringLiteral(".AetherSDR-incoming-")
                                     + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
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
    // No second try changes a signature.
    if (!checks.signature(tree, problem)) {
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
    const QString keepIn = QFileInfo(previousDir).absolutePath();
    QDir().mkpath(keepIn);
    if (!previousDir.isEmpty() && sameVolume(ready, keepIn)) {
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
