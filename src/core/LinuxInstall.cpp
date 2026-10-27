#include "LinuxInstall.h"

#include "LogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace AetherSDR {

namespace {

const QString kCurrent = QStringLiteral("current");
const QString kAppRun = QStringLiteral("AppRun");

// A release folder's name: the version, "26.10.4".
bool isVersion(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("^[0-9]{1,4}(\\.[0-9]{1,6}){2,3}$"));
    return re.match(name).hasMatch();
}

fs::path toPath(const QString& s)
{
    return fs::path(s.toStdString());
}

QString unique(const QString& base, const QString& what)
{
    return QDir(base).filePath(QLatin1Char('.') + what + QLatin1Char('-')
                               + QUuid::createUuid().toString(QUuid::WithoutBraces));
}

// <base>/current -> version, by renaming a new link over the old one: a
// reader sees the old release or the new one, never neither.
bool makeCurrent(const QString& base, const QString& version, QString* problem)
{
    const QString link = unique(base, QStringLiteral("current"));
    std::error_code ec;
    fs::create_symlink(toPath(version), toPath(link), ec);
    if (!ec) {
        fs::rename(toPath(link), toPath(QDir(base).filePath(kCurrent)), ec);
    }
    if (ec) {
        fs::remove(toPath(link), ec);
        *problem = QStringLiteral("%1 could not be made the current release.").arg(version);
        return false;
    }
    return true;
}

// The release current names, or empty.
QString currentVersion(const QString& base)
{
    std::error_code ec;
    const fs::path target = fs::read_symlink(toPath(QDir(base).filePath(kCurrent)), ec);
    return ec ? QString() : QString::fromStdString(target.string());
}

// source to dest, which must not exist, without ever leaving a partial
// dest: a rename on one filesystem, otherwise a copy beside dest renamed in.
bool moveTree(const QString& source, const QString& dest, bool keepSource, QString* problem)
{
    std::error_code ec;
    if (!keepSource) {
        fs::rename(toPath(source), toPath(dest), ec);
        if (!ec) {
            return true;
        }
    }
    const QString partial = unique(QFileInfo(dest).absolutePath(), QStringLiteral("partial"));
    fs::copy(toPath(source), toPath(partial), fs::copy_options::recursive | fs::copy_options::copy_symlinks,
             ec);
    if (!ec) {
        fs::rename(toPath(partial), toPath(dest), ec);
    }
    if (ec) {
        fs::remove_all(toPath(partial), ec);
        *problem = QStringLiteral("The release could not be copied into %1.").arg(dest);
        return false;
    }
    if (!keepSource) {
        fs::remove_all(toPath(source), ec);
    }
    return true;
}

// A path in a desktop entry's Exec key: quoted when it holds a reserved
// character, escaped as a string value, and with % doubled (Desktop Entry
// Specification, "The Exec key").
QString execArgument(const QString& path)
{
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    QString arg = path;
    arg.replace(QLatin1Char('%'), QStringLiteral("%%"));
    bool quote = false;
    for (const QChar c : arg) {
        quote = quote || reserved.contains(c);
    }
    if (quote) {
        QString q;
        for (const QChar c : arg) {
            if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$')
                || c == QLatin1Char('\\')) {
                q += QLatin1Char('\\');
            }
            q += c;
        }
        arg = QLatin1Char('"') + q + QLatin1Char('"');
    }
    arg.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));   // the string value's own escaping
    return arg;
}

// The release's own entry, run from current: Exec and TryExec name
// <base>/current/AppRun by path, so it needs nothing on PATH.
bool writeDesktopEntry(const QString& tree, const QString& run, const QString& applications,
                       QString* problem)
{
    QFile in(QDir(tree).filePath(QStringLiteral("usr/share/applications/AetherSDR.desktop")));
    if (!in.open(QIODevice::ReadOnly)) {
        *problem = QStringLiteral("The release has no menu entry.");
        return false;
    }
    QStringList out;
    for (const QString& line : QString::fromUtf8(in.readAll()).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("TryExec="))) {
            continue;
        }
        if (line.startsWith(QLatin1String("Exec="))) {
            QString tryExec = run;
            out << QStringLiteral("TryExec=") + tryExec.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            out << QStringLiteral("Exec=") + execArgument(run);
            continue;
        }
        out << line;
    }
    QDir().mkpath(applications);
    QSaveFile f(QDir(applications).filePath(QStringLiteral("AetherSDR.desktop")));
    if (!f.open(QIODevice::WriteOnly) || f.write(out.join(QLatin1Char('\n')).toUtf8()) < 0 || !f.commit()) {
        *problem = QStringLiteral("The menu entry could not be written.");
        return false;
    }
    return true;
}

// at -> target, replacing a link already there but never a file of the
// user's own. False when such a file is in the way.
bool linkTo(const QString& at, const QString& target)
{
    std::error_code ec;
    const fs::path p = toPath(at);
    const fs::file_status st = fs::symlink_status(p, ec);
    if (fs::exists(st) && !fs::is_symlink(st)) {
        return false;
    }
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = toPath(unique(QFileInfo(at).absolutePath(), QFileInfo(at).fileName()));
    fs::create_symlink(toPath(target), tmp, ec);
    if (!ec) {
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

LinuxInstall::Paths LinuxInstall::forThisUser()
{
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return {QDir(data).filePath(QStringLiteral("AetherSDR/install")),
            QDir(QDir::homePath()).filePath(QStringLiteral(".local/bin")),
            QDir(data).filePath(QStringLiteral("applications")),
            QDir(data).filePath(QStringLiteral("icons/hicolor"))};
}

QString LinuxInstall::binaryDir()
{
    QString exe = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
    if (exe.isEmpty()) {
        exe = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    }
    return QFileInfo(exe).absolutePath();
}

QString LinuxInstall::treeOf(const QString& appDir, const QString& base)
{
    const QFileInfo bin(appDir);
    QDir tree(bin.canonicalFilePath());
    if (bin.canonicalFilePath().isEmpty() || tree.dirName() != QLatin1String("bin") || !tree.cdUp()
        || tree.dirName() != QLatin1String("usr") || !tree.cdUp() || !isVersion(tree.dirName())) {
        return {};
    }
    QDir parent(tree);
    const QString canonicalBase = QFileInfo(base).canonicalFilePath();
    if (!parent.cdUp() || canonicalBase.isEmpty() || parent.canonicalPath() != canonicalBase) {
        return {};
    }
    return tree.canonicalPath();
}

bool LinuxInstall::install(const QString& tree, const QString& version, const Paths& paths,
                           QString* problem, QString* note)
{
    if (!isVersion(version)) {
        *problem = QStringLiteral("%1 is not a release version.").arg(version);
        return false;
    }
    const QString source = QFileInfo(tree).canonicalFilePath();
    if (source.isEmpty() || !QFileInfo(QDir(source).filePath(kAppRun)).exists()
        || !QFileInfo::exists(QDir(source).filePath(QStringLiteral("usr/bin/AetherSDR")))) {
        *problem = QStringLiteral("%1 is not an AetherSDR release folder.").arg(tree);
        return false;
    }
    if (!QDir().mkpath(paths.base)) {
        *problem = QStringLiteral("%1 could not be made.").arg(paths.base);
        return false;
    }
    const QString dest = QDir(paths.base).filePath(version);
    // Run again from the installed tree, only the links are redone.
    if (QFileInfo(dest).canonicalFilePath() != source) {
        std::error_code ec;
        if (QFileInfo::exists(dest)) {
            // Moved aside rather than removed in place: a running
            // AetherSDR of that version keeps the files it has open.
            const QString old = unique(paths.base, QStringLiteral("old"));
            fs::rename(toPath(dest), toPath(old), ec);
            fs::remove_all(toPath(old), ec);
        }
        if (QFileInfo::exists(dest) || !moveTree(source, dest, true, problem)) {
            if (problem->isEmpty()) {
                *problem = QStringLiteral("%1 is in the way.").arg(dest);
            }
            return false;
        }
    }
    if (!makeCurrent(paths.base, version, problem)) {
        return false;
    }
    const QString current = QDir(paths.base).filePath(kCurrent);
    const QString run = QDir(current).filePath(kAppRun);
    if (!writeDesktopEntry(dest, run, paths.applications, problem)) {
        return false;
    }
    // The release's icons, by the same relative paths, through current.
    const QString iconRoot = QDir(dest).filePath(QStringLiteral("usr/share/icons/hicolor"));
    QDirIterator icons(iconRoot, QDir::Files, QDirIterator::Subdirectories);
    while (icons.hasNext()) {
        const QString rel = QDir(iconRoot).relativeFilePath(icons.next());
        linkTo(QDir(paths.icons).filePath(rel),
               QDir(current).filePath(QStringLiteral("usr/share/icons/hicolor/") + rel));
    }
    const QString command = QDir(paths.bin).filePath(QStringLiteral("aethersdr"));
    if (!linkTo(command, run) && note) {
        *note = QStringLiteral("%1 is a file of your own, so it was left alone. AetherSDR runs as %2.")
                    .arg(command, run);
    }
    qCInfo(lcUpdater).noquote() << "installed v" + version + " in" << dest;
    return true;
}

bool LinuxInstall::selfTest(const QString& tree, const QString& version)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return false;
    }
    const QString marker = dir.filePath(QStringLiteral("selftest"));
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(QDir(tree).filePath(kAppRun), {QStringLiteral("--update-selftest"), marker});
    if (!p.waitForStarted(10000)) {
        qCWarning(lcUpdater).noquote() << "self-test of v" + version + " could not be started";
        return false;
    }
    if (!p.waitForFinished(30000)) {
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

bool LinuxInstall::switchTo(const QString& base, const QString& stagedDir, const QString& version,
                            const QString& keep, const SelfTest& test, QString* problem)
{
    if (!isVersion(version) || version == keep) {
        *problem = QStringLiteral("%1 is not a release to switch to.").arg(version);
        return false;
    }
    if (currentVersion(base) == version) {
        *problem = QStringLiteral("AetherSDR %1 is already the current release.").arg(version);
        return false;
    }
    const QString dest = QDir(base).filePath(version);
    std::error_code ec;
    fs::remove_all(toPath(dest), ec);   // a leftover: current never names it
    if (!moveTree(stagedDir, dest, false, problem)) {
        return false;
    }
    if (!test(dest, version)) {
        fs::remove_all(toPath(dest), ec);
        *problem = QStringLiteral("AetherSDR %1 failed its start-up check, so it wasn't installed.")
                       .arg(version);
        return false;
    }
    if (!makeCurrent(base, version, problem)) {
        fs::remove_all(toPath(dest), ec);
        return false;
    }
    // The new release and the one it replaced stay; older releases and
    // anything a crashed switch left go.
    for (const QFileInfo& e : QDir(base).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot
                                                       | QDir::System)) {
        const QString name = e.fileName();
        const bool leftover = name.startsWith(QLatin1String(".partial-"))
                              || name.startsWith(QLatin1String(".current-"))
                              || name.startsWith(QLatin1String(".old-"));
        if ((isVersion(name) && name != version && name != keep) || leftover) {
            fs::remove_all(toPath(e.filePath()), ec);
        }
    }
    qCInfo(lcUpdater).noquote() << "v" + version + " is now the current release in" << base;
    return true;
}

bool LinuxInstall::startAfterExit(qint64 pid, const QString& program)
{
    // 600 polls of 0.2 s: two minutes for the quit's own prompts and
    // shutdown, then nothing is started.
    static const QString script = QStringLiteral(
        "i=0; while kill -0 \"$1\" 2>/dev/null; do i=$((i+1)); "
        "if [ \"$i\" -gt 600 ]; then exit 1; fi; sleep 0.2; done; exec \"$2\"");
    return QProcess::startDetached(QStringLiteral("/bin/sh"),
                                   {QStringLiteral("-c"), script, QStringLiteral("aethersdr-restart"),
                                    QString::number(pid), program});
}

} // namespace AetherSDR
