#include "core/ExternalFilePath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

using namespace AetherSDR::ExternalFilePath;

namespace {
int failures = 0;
void check(bool condition, const char* what)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}
bool touch(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return 1;
    }
    const QString local = temporary.path() + "/Local";
    const QString roaming = temporary.path() + "/Roaming";
    const QString cache = local + "/Packages/Test_family/LocalCache";
    const QList<AppDataMapping> mappings{{local, cache + "/Local"},
                                       {roaming, cache + "/Roaming"}};
    const QString relative = "/AetherSDR/logs/log-日本語 & test.log";
    const QString logical = local + relative;
    const QString physical = cache + "/Local" + relative;
    const QString oldFile = local + "/AetherSDR/logs/old.log";
    if (!touch(physical) || !touch(oldFile)) {
        return 1;
    }

    check(resolve(logical, mappings) == physical, "redirected-only log uses physical path");
    check(QFileInfo(resolve(logical, mappings)).absolutePath() == QFileInfo(physical).absolutePath(),
          "external folder comes from resolved file even when original folder exists");
    check(resolve(oldFile, mappings) == oldFile, "real-only file retains its original path");
    check(touch(logical), "create both-layer fixture");
    check(resolve(logical, mappings) == physical, "redirected file wins when both layers exist");
    check(resolve(physical, mappings) == physical, "already physical path is not redirected twice");
    check(resolve(logical, {}) == logical, "missing mappings preserve input");
    check(resolve(logical, {{local, local}}) == logical, "unpackaged identity mapping preserves input");
    check(resolve(logical, {{QString(), cache + "/Local"}}) == logical,
          "failed original known-folder lookup preserves input");
    check(resolve(logical, {{local, QString()}}) == logical,
          "failed redirected known-folder lookup preserves input");
    check(resolve(QString(), mappings).isEmpty(), "empty path remains empty");
    const QString absent = local + "/AetherSDR/logs/missing.log";
    check(resolve(absent, mappings) == absent, "missing candidate preserves input");
    const QString outside = temporary.path() + "/Local-other/AetherSDR/logs/old.log";
    check(resolve(outside, mappings) == outside, "prefix sibling is not an AppData child");
    check(resolve(local + "/../outside.log", mappings) == local + "/../outside.log",
          "parent traversal outside AppData is not redirected");
    check(resolve(local.toUpper() + relative, mappings) == physical,
          "Windows root matching is case insensitive");
    check(resolve(QString(logical).replace('/', '\\'), mappings) == physical,
          "native Windows separators resolve identically");
    const QString zipRelative = "/AetherSDR/logs/support/support-bundle.zip";
    const QString zipPhysical = cache + "/Local" + zipRelative;
    check(touch(zipPhysical), "create support bundle fixture");
    check(resolve(local + zipRelative, mappings) == zipPhysical,
          "support bundle resolves at file level");
    const QString roamPhysical = cache + "/Roaming/AetherSDR/report.txt";
    check(touch(roamPhysical), "create roaming fixture");
    check(resolve(roaming + "/AetherSDR/report.txt", mappings) == roamPhysical,
          "roaming known-folder mapping resolves independently");
    check(resolve(temporary.path() + "/outside.log") == temporary.path() + "/outside.log",
          "platform resolver preserves a path with no redirected file");
    return failures == 0 ? 0 : 1;
}
