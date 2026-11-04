#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/SupportBundle.h"
#include "core/ZipArchive.h"
#include "TestSettingsProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QtCore/qlogging.h>

#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", message);
    if (!ok) {
        ++failures;
    }
}

bool hasMarker(const QString& path, const QByteArray& marker)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(marker);
}

void checkAlias(const QString& dir, const QString& target)
{
    const QFileInfo alias(dir + QStringLiteral("/aethersdr.log"));
#ifdef Q_OS_UNIX
    check(alias.isSymLink(), "latest-log alias is a POSIX symlink");
    check(alias.symLinkTarget() == target, "alias points at the active timestamped log");
    check(!QFileInfo::exists(alias.filePath() + QStringLiteral(".new")),
          "atomic alias update leaves no temporary link");
#elif defined(Q_OS_WIN)
    Q_UNUSED(target);
    check(!alias.exists() && !alias.isSymLink(), "Windows has no aethersdr.log alias");
    check(!QFileInfo::exists(alias.filePath() + QStringLiteral(".lnk")),
          "Windows does not create a renamed shortcut");
#endif
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("log-manager-file-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("LogRetention"),
        QStringLiteral("{\"ActiveLogMaxMb\":1}"));

    const QString dir = profile.path() + QStringLiteral("/logs é");
    check(QDir().mkpath(dir), "create isolated log directory");
    const QString previous = dir + QStringLiteral("/aethersdr-20000101-000000.log");
    {
        QFile oldLog(previous);
        check(oldLog.open(QIODevice::WriteOnly), "create previous session log");
        oldLog.write(QByteArray("previous session\n").repeated(20));
    }
    check(QFile::link(previous, dir + QStringLiteral("/aethersdr.log")),
          "seed legacy latest-log alias");

    LogManager& manager = LogManager::instance();
    const QString first = dir + QStringLiteral("/aethersdr-20000102-000000.log");
    if (!manager.startLogging(first, false)) {
        check(false, "start timestamped logging");
        return 1;
    }
    check(manager.logFilePath() == first, "active path is timestamped at startup");
    checkAlias(dir, first);

    const QMessageLogContext context(__FILE__, __LINE__, "main", "aether.test");
    manager.enqueueMessage(QtWarningMsg, context, QStringLiteral("startup-marker"));
    manager.flushLog();
    check(hasMarker(manager.logFilePath(), "startup-marker"), "active log is readable at startup");

    // Flush each batch so the real writer's size threshold drives rotation.
    const QString padding(64 * 1024, QLatin1Char('x'));
    for (int i = 0; i < 24 && manager.logCounters().rotationCount == 0; ++i) {
        manager.enqueueMessage(QtWarningMsg, context, padding);
        manager.flushLog();
    }
    check(manager.logCounters().rotationCount == 1, "real writer rotates at the configured cap");
    const QString rotated = manager.logFilePath();
    check(rotated != first && QFileInfo(rotated).fileName().startsWith("aethersdr-"),
          "rotation publishes a fresh timestamped path");
    manager.enqueueMessage(QtWarningMsg, context,
        QStringLiteral("after-rotation-marker ") + QString(256, QLatin1Char('y')));
    manager.flushLog();
    check(hasMarker(rotated, "after-rotation-marker"), "active log is readable after rotation");
    check(!hasMarker(first, "after-rotation-marker"), "rotation leaves the previous log intact");
    checkAlias(dir, rotated);

    const QString alias = dir + QStringLiteral("/aethersdr.log");
    QFile::remove(alias);
    const QByteArray shellLink = QByteArray::fromHex("4c0000000114020000000000c000000000000046")
        + QByteArray(492, '\0');
    {
        QFile staleAlias(alias);
        check(staleAlias.open(QIODevice::WriteOnly), "seed a stale shortcut newer than the real logs");
        staleAlias.write(shellLink);
    }
    const QString bundlePath = SupportBundle::createBundle({});
    QFile bundle(bundlePath);
    check(bundle.open(QIODevice::ReadOnly), "create readable support bundle");
    QString zipError;
    const QMap<QString, QByteArray> entries = readZipEntries(bundle.readAll(), &zipError);
    check(zipError.isEmpty(), "support bundle ZIP is valid");
    int bundledLogs = 0;
    bool hasRotatedMarker = false;
    for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
        if (entry.key().endsWith(QStringLiteral(".log"))) {
            ++bundledLogs;
            check(!entry.value().startsWith(shellLink.left(4)), "bundle contains log text, never a shell shortcut");
            hasRotatedMarker |= entry.value().contains("after-rotation-marker");
        }
    }
    check(bundledLogs == 3 && hasRotatedMarker, "bundle retains three real logs including the active log");
    manager.shutdownLogging();

    const QString directDir = profile.path() + QStringLiteral("/direct");
    check(QDir().mkpath(directDir), "create direct-log directory");
    const QString direct = directDir + QStringLiteral("/aethersdr.log");
    check(manager.startLogging(direct, false), "legacy direct-log callers still start");
    manager.enqueueMessage(QtWarningMsg, context, QStringLiteral("direct-marker"));
    manager.flushLog();
    check(hasMarker(direct, "direct-marker") && !QFileInfo(direct).isSymLink(),
          "alias management never replaces a live direct log");
    manager.shutdownLogging();
    return failures == 0 ? 0 : 1;
}
