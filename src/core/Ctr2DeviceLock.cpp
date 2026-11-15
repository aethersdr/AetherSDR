#include "Ctr2DeviceLock.h"

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <memory>

namespace AetherSDR {

namespace {

struct Taken {
    explicit Taken(const QString& file) : lock(file) {}
    QLockFile lock;
    int holders = 0;
};

QMutex& registryLock()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, std::shared_ptr<Taken>>& registry()
{
    static QHash<QString, std::shared_ptr<Taken>> taken;
    return taken;
}

// Each acquisition has one lease; copies of its Holder share that lease.
// Final release removes the lock while still holding the acquisition mutex.
struct Lease {
    explicit Lease(const QString& path) : file(path) {}
    ~Lease()
    {
        const QMutexLocker locker(&registryLock());
        const auto entry = registry().find(file);
        if (--entry.value()->holders == 0) {
            registry().erase(entry);
        }
    }
    QString file;
};

QString& directoryOverride()
{
    static QString directory;
    return directory;
}

QString lockFileFor(const QString& path)
{
    QString dir = directoryOverride();
    if (dir.isEmpty()) {
#ifdef Q_OS_UNIX
        // RuntimeLocation can fall back under TMPDIR. A fixed OS temporary
        // root and numeric user ID keep every instance in one namespace.
        dir = QStringLiteral("/tmp/aethersdr-ctr2-%1").arg(getuid());
        const QByteArray native = dir.toLocal8Bit();
        ::mkdir(native.constData(), 0700);
        struct stat info {};
        if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode)
            || info.st_uid != getuid() || (info.st_mode & 0077) != 0) {
            return {};
        }
#else
        // On Windows Qt resolves this through the native user known folder,
        // independently of TMP/TEMP and application/settings names.
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
        if (dir.isEmpty()) {
            return {};
        }
        dir = QDir(dir).filePath(QStringLiteral("aethersdr-ctr2-locks"));
#endif
    }
    if (!QDir().mkpath(dir)) {
        return {};
    }
    const QByteArray id =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(dir).filePath(QStringLiteral("aethersdr-ctr2-%1.lock").arg(QString::fromLatin1(id)));
}

} // namespace

void Ctr2DeviceLock::setDirectoryForTesting(const QString& directory)
{
    const QMutexLocker locker(&registryLock());
    directoryOverride() = directory;
}

Ctr2DeviceLock::Holder Ctr2DeviceLock::acquire(const QString& path, qint64* otherPid)
{
    *otherPid = 0;
    const QMutexLocker locker(&registryLock());
    const QString file = lockFileFor(path);
    if (file.isEmpty()) {
        return nullptr;
    }
    std::shared_ptr<Taken>& taken = registry()[file];
    if (!taken) {
        taken = std::make_shared<Taken>(file);
        // A lock is never stale by age: only when its process is gone.
        taken->lock.setStaleLockTime(0);
        if (!taken->lock.tryLock(0)) {
            QString host, app;
            taken->lock.getLockInfo(otherPid, &host, &app);
            registry().remove(file);
            return nullptr;
        }
    }
    ++taken->holders;
    return std::make_shared<Lease>(file);
}

} // namespace AetherSDR
