#include "RigctlPty.h"
#include "LogManager.h"
#include "RigctlProtocol.h"
#include "models/RadioModel.h"

#include <QDir>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QStandardPaths>

#ifndef _WIN32
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>

#ifdef __APPLE__
#include <util.h>       // openpty() on macOS
#else
#include <pty.h>        // openpty() on Linux
#endif
#endif // !_WIN32

namespace AetherSDR {

QString RigctlPty::defaultSymlinkPath(int sliceIndex)
{
    // Per GHSA-qxhr-cwrc-pvrm — keep this symlink out of /tmp (no
    // cross-user collisions; no TOCTOU window on non-sticky-bit /tmp).
    //
    // Letter scheme matches the existing CAT applet UI conventions:
    //   slice 0 → "A", 1 → "B", 2 → "C", 3 → "D", >= 4 → numeric.
    QString leaf;
    if (sliceIndex >= 0 && sliceIndex < 4) {
        const char letter = static_cast<char>('A' + sliceIndex);
        leaf = QStringLiteral("cat-%1").arg(QChar(letter));
    } else {
        leaf = QStringLiteral("cat-%1").arg(sliceIndex);
    }

    // Linux: prefer $XDG_RUNTIME_DIR (systemd /run/user/${UID}, mode 0700,
    //        tmpfs, cleaned on logout). Falls back to QStandardPaths
    //        CacheLocation if XDG isn't set (containers, minimal init).
    // macOS: QStandardPaths::CacheLocation = ~/Library/Caches/AetherSDR
    //        (per-user, mode 0700 by default).
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache/aethersdr");

    // QStandardPaths::CacheLocation already includes the application
    // name (e.g. ~/Library/Caches/AetherSDR); RuntimeLocation does not.
    // Add an aethersdr/ subdir only if the base doesn't already carry
    // an AetherSDR-named segment.
    if (!base.contains(QStringLiteral("aethersdr"), Qt::CaseInsensitive)) {
        base += QStringLiteral("/aethersdr");
    }
    return base + QChar('/') + leaf;
}

RigctlPty::RigctlPty(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{}

RigctlPty::~RigctlPty()
{
    stop();
}

bool RigctlPty::start()
{
#ifdef _WIN32
    qCWarning(lcCat) << "RigctlPty: PTY not supported on Windows";
    return false;
#else
    if (m_masterFd >= 0)
        return true;  // already running

    char slaveName[256] = {};
    if (openpty(&m_masterFd, &m_slaveFd, slaveName, nullptr, nullptr) != 0) {
        qCWarning(lcCat) << "RigctlPty: openpty() failed";
        return false;
    }

    m_slavePath = QString::fromLocal8Bit(slaveName);

    // Set master FD to non-blocking
    int flags = fcntl(m_masterFd, F_GETFL);
    fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    // Configure slave terminal: raw mode, no echo
    struct termios tio;
    if (tcgetattr(m_slaveFd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(m_slaveFd, TCSANOW, &tio);
    }

    // Create the convenience symlink for CAT-software auto-discovery
    // (GHSA-qxhr-cwrc-pvrm — per-user dir + atomic replace).
    //
    // Ensure the parent dir exists. On Linux $XDG_RUNTIME_DIR is
    // created mode 0700 by systemd; CacheLocation is mode 0755 by
    // default. We tighten our subdir to mode 0700 so another local
    // user can't readlink() the PTY path even if they share a primary
    // group.
    const QFileInfo info(m_symlinkPath);
    const QString parentDir = info.absolutePath();
    if (!QDir().mkpath(parentDir)) {
        qCWarning(lcCat) << "RigctlPty: failed to mkpath" << parentDir;
    }
    if (::chmod(parentDir.toLocal8Bit().constData(), 0700) != 0) {
        // Best-effort hardening — log so we notice if it ever fails on
        // the CacheLocation fallback path (where the dir may have
        // landed at the umask default rather than 0700).
        qCWarning(lcCat) << "RigctlPty: chmod 0700 failed on" << parentDir
                         << "errno=" << errno;
    }

    // Atomic replacement via symlink(tmp) + rename(tmp, final) avoids
    // the TOCTOU window the old unlink-then-symlink had on non-sticky
    // /tmp filesystems. rename() is atomic across the same filesystem
    // and is the canonical way to swap a symlink in place.
    const QByteArray finalPath = m_symlinkPath.toLocal8Bit();
    const QByteArray tmpPath = finalPath + ".tmp";
    ::unlink(tmpPath.constData());    // clean stale .tmp from prior run
    if (::symlink(slaveName, tmpPath.constData()) != 0) {
        qCWarning(lcCat) << "RigctlPty: symlink(tmp) failed:" << m_symlinkPath;
    } else if (::rename(tmpPath.constData(), finalPath.constData()) != 0) {
        ::unlink(tmpPath.constData());
        qCWarning(lcCat) << "RigctlPty: rename(tmp,final) failed:" << m_symlinkPath;
    }

    // Set up protocol handler
    m_protocol = new RigctlProtocol(m_model);
    m_protocol->setSliceIndex(m_sliceIndex);

    // Watch for data on the master FD
    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &RigctlPty::onDataReady);

    qCInfo(lcCat) << "RigctlPty: started on" << m_slavePath << "symlink:" << m_symlinkPath;
    emit started(m_symlinkPath);
    return true;
#endif
}

void RigctlPty::stop()
{
#ifdef _WIN32
    return;
#else
    if (m_masterFd < 0)
        return;

    delete m_notifier;
    m_notifier = nullptr;

    delete m_protocol;
    m_protocol = nullptr;

    ::close(m_masterFd);
    ::close(m_slaveFd);
    m_masterFd = -1;
    m_slaveFd = -1;
    m_buffer.clear();

    // Remove symlink
    ::unlink(m_symlinkPath.toLocal8Bit().constData());

    qCInfo(lcCat) << "RigctlPty: stopped";
    emit stopped();
#endif
}

void RigctlPty::onDataReady()
{
#ifdef _WIN32
    return;
#else
    char buf[4096];
    ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // PTY closed by the other end — keep it open for reconnection
        }
        return;
    }

    m_buffer.append(buf, static_cast<int>(n));

    // Process complete lines
    while (true) {
        int nlPos = m_buffer.indexOf('\n');
        if (nlPos < 0) {
            // Also try \r as line terminator (some serial software uses \r only)
            nlPos = m_buffer.indexOf('\r');
            if (nlPos < 0) break;
        }

        QString line = QString::fromUtf8(m_buffer.left(nlPos));
        m_buffer.remove(0, nlPos + 1);

        // Skip empty lines
        if (line.trimmed().isEmpty())
            continue;

        QString response = m_protocol->handleLine(line);
        if (!response.isEmpty()) {
            QByteArray data = response.toUtf8();
            ::write(m_masterFd, data.constData(), data.size());
        }
    }
#endif
}

} // namespace AetherSDR
