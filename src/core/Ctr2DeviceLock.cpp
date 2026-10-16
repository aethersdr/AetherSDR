#include "Ctr2DeviceLock.h"

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <memory>

namespace AetherSDR {

namespace {

struct Taken {
    explicit Taken(const QString& file) : lock(file) {}
    ~Taken() { lock.unlock(); }
    QLockFile lock;
};

QMutex& registryLock()
{
    static QMutex mutex;
    return mutex;
}

// The devices this process holds, by lock file; an entry lives while any
// holder does.
QHash<QString, std::weak_ptr<Taken>>& registry()
{
    static QHash<QString, std::weak_ptr<Taken>> taken;
    return taken;
}

QString& directoryOverride()
{
    static QString directory;
    return directory;
}

QString lockFileFor(const QString& path)
{
    QString dir = directoryOverride();
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (dir.isEmpty())
            dir = QDir::tempPath();
    }
    QDir().mkpath(dir);
    const QByteArray id =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QDir(dir).filePath(QStringLiteral("aethersdr-ctr2-%1.lock").arg(QString::fromLatin1(id)));
}

} // namespace

void Ctr2DeviceLock::setDirectoryForTesting(const QString& directory)
{
    directoryOverride() = directory;
}

Ctr2DeviceLock::Holder Ctr2DeviceLock::acquire(const QString& path, qint64* otherPid)
{
    *otherPid = 0;
    const QString file = lockFileFor(path);
    const QMutexLocker locker(&registryLock());
    if (std::shared_ptr<Taken> held = registry().value(file).lock())
        return held;   // this process has it already
    auto taken = std::make_shared<Taken>(file);
    // A lock is never stale by age: only when its process is gone, which
    // tryLock() checks.
    taken->lock.setStaleLockTime(0);
    if (!taken->lock.tryLock(0)) {
        QString host, app;
        taken->lock.getLockInfo(otherPid, &host, &app);
        return nullptr;
    }
    registry().insert(file, taken);
    return taken;
}

} // namespace AetherSDR
