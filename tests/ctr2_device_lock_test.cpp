// One AetherSDR at a time on a CTR2's USB HID interface (Ctr2DeviceLock):
// another process holding the device is refused, with its process ID; this
// process may hold it several times over (relay, probe and updater overlap
// while a port closes) and frees it with its last holder; a lock left by a
// process that died is taken over. The other process is this test run again
// as a holder. No device, no network.

#include "core/Ctr2DeviceLock.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <iostream>
#include <string>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

const QString kPath = QStringLiteral("/dev/hidraw7");

// The other AetherSDR: takes the device, says so, holds it until stdin closes.
int holder(const QString& directory)
{
    Ctr2DeviceLock::setDirectoryForTesting(directory);
    qint64 other = 0;
    Ctr2DeviceLock::Holder held = Ctr2DeviceLock::acquire(kPath, &other);
    std::printf("%s\n", held ? "held" : "refused");
    std::fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    return 0;
}

bool startHolder(QProcess& p, const QString& directory)
{
    p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold"), directory});
    if (!p.waitForStarted(5000) || !p.waitForReadyRead(5000))
        return false;
    return p.readLine().trimmed() == "held";
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--hold")
        return holder(QString::fromLocal8Bit(argv[2]));

    QTemporaryDir dir;
    Ctr2DeviceLock::setDirectoryForTesting(dir.path());
    qint64 other = -1;

    {
        Ctr2DeviceLock::Holder relay = Ctr2DeviceLock::acquire(kPath, &other);
        Ctr2DeviceLock::Holder probe = Ctr2DeviceLock::acquire(kPath, &other);
        check(relay && probe, "this process may hold its own device more than once");
        check(Ctr2DeviceLock::acquire(QStringLiteral("/dev/hidraw8"), &other) != nullptr,
              "another device is another lock");
        relay.reset();
        QProcess p;
        check(!startHolder(p, dir.path()),
              "while any holder in this process remains, another process is refused");
        p.closeWriteChannel();
        p.waitForFinished(5000);
    }

    // The other AetherSDR holds it.
    QProcess other1;
    check(startHolder(other1, dir.path()), "with this process done, another process takes it");
    check(Ctr2DeviceLock::acquire(kPath, &other) == nullptr,
          "this process is refused while another AetherSDR holds the device");
    check(other == other1.processId(), "and is told which process holds it");
    // A relay runs for hours: a lock is never stale by age while its
    // process lives.
    const QStringList locks = QDir(dir.path()).entryList({QStringLiteral("*.lock")}, QDir::Files);
    QFile lockFile(QDir(dir.path()).filePath(locks.value(0)));
    check(locks.size() == 1 && lockFile.open(QIODevice::ReadWrite)
              && lockFile.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                                      QFileDevice::FileModificationTime),
          "the other process's lock file is there to age");
    lockFile.close();
    check(Ctr2DeviceLock::acquire(kPath, &other) == nullptr,
          "an hour-old lock held by a live process is still refused");
    other1.closeWriteChannel();   // it lets go
    check(other1.waitForFinished(5000), "the other process ends");
    check(Ctr2DeviceLock::acquire(kPath, &other) != nullptr,
          "once the other process has let go, this one takes the device");

    // An AetherSDR that crashed holding it.
    QProcess crashed;
    QTemporaryDir dir2;
    Ctr2DeviceLock::setDirectoryForTesting(dir2.path());
    check(startHolder(crashed, dir2.path()), "another process takes a device");
    crashed.kill();
    crashed.waitForFinished(5000);
    check(Ctr2DeviceLock::acquire(kPath, &other) != nullptr,
          "a lock left by a process that died is taken over");

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_device_lock_test: all checks passed\n");
    return 0;
}
