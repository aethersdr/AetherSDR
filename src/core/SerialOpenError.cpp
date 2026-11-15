#include "SerialOpenError.h"

#include <QStringList>

#include <cerrno>

#ifndef Q_OS_WIN
#include <QFile>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#endif

namespace AetherSDR {

SerialOpenFailureKind classifySerialOpenFailure(const SerialDeviceProbe& probe,
                                                bool qtNotFound, bool qtPermission,
                                                int nativeCode)
{
    if (!probe.exists
        && (probe.statErrno == ENOENT || probe.statErrno == ENODEV
            || probe.statErrno == ENXIO || probe.statErrno == ENOTDIR)) {
        return SerialOpenFailureKind::NotFound;
    }
    if (qtNotFound)
        return SerialOpenFailureKind::NotFound;
    if (probe.exists && (probe.accessErrno == EACCES || probe.accessErrno == EPERM))
        return SerialOpenFailureKind::Permission;
    if (nativeCode == EBUSY)
        return SerialOpenFailureKind::InUse;
    // The node is there and this user may read/write it, yet Qt reported a
    // permission error: that is the lock file or TIOCEXCL, i.e. in use.
    if (qtPermission && probe.exists)
        return SerialOpenFailureKind::InUse;
    if (qtPermission)
        return SerialOpenFailureKind::Permission;
    return SerialOpenFailureKind::Other;
}

QString serialGroupRemedy(const SerialDeviceProbe& probe)
{
    if (!probe.exists)
        return {};
    if (probe.accessErrno != EACCES && probe.accessErrno != EPERM)
        return {};
    // Group membership helps only when the group may read and write the node
    // and this process does not already carry it. A process that does carry
    // it and is still denied is an ACL/MAC problem, not a group one.
    if (!probe.groupReadWrite || probe.inGroup)
        return {};
    // Never steer anyone into root/tty/wheel: those are not serial groups.
    static const QStringList kNotARemedy{
        QStringLiteral("root"), QStringLiteral("tty"), QStringLiteral("wheel")};
    if (probe.groupName.isEmpty() || kNotARemedy.contains(probe.groupName))
        return {};
    return probe.groupName;
}

SerialOpenFailureKind classifyWin32SerialOpenFailure(unsigned long win32Error)
{
    switch (win32Error) {
    case 2:   // ERROR_FILE_NOT_FOUND
    case 3:   // ERROR_PATH_NOT_FOUND
        return SerialOpenFailureKind::NotFound;
    case 5:   // ERROR_ACCESS_DENIED: COM handles are exclusive
    case 32:  // ERROR_SHARING_VIOLATION
        return SerialOpenFailureKind::InUse;
    default:
        return SerialOpenFailureKind::Other;
    }
}

#ifndef Q_OS_WIN
SerialDeviceProbe probeSerialDevice(const QString& systemLocation)
{
    SerialDeviceProbe probe;
    const QByteArray path = QFile::encodeName(systemLocation);
    struct stat st {};
    if (::stat(path.constData(), &st) != 0) {
        probe.statErrno = errno;
        return probe;
    }
    probe.exists = true;
    if (::access(path.constData(), R_OK | W_OK) != 0)
        probe.accessErrno = errno;
    probe.groupReadWrite = (st.st_mode & S_IRGRP) && (st.st_mode & S_IWGRP);

    probe.inGroup = (::getegid() == st.st_gid);
    if (!probe.inGroup) {
        const int n = ::getgroups(0, nullptr);
        if (n > 0) {
            std::vector<gid_t> groups(static_cast<size_t>(n));
            const int got = ::getgroups(n, groups.data());
            for (int i = 0; i < got; ++i) {
                if (groups[static_cast<size_t>(i)] == st.st_gid) {
                    probe.inGroup = true;
                    break;
                }
            }
        }
    }

    // getgrgid_r: this runs on the external-controller thread.
    long bufSize = ::sysconf(_SC_GETGR_R_SIZE_MAX);
    if (bufSize <= 0)
        bufSize = 16384;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<char> buf(static_cast<size_t>(bufSize));
        struct group grp {};
        struct group* result = nullptr;
        const int rc = ::getgrgid_r(st.st_gid, &grp, buf.data(), buf.size(), &result);
        if (rc == ERANGE) {
            bufSize *= 2;
            continue;
        }
        if (rc == 0 && result && result->gr_name)
            probe.groupName = QString::fromLocal8Bit(result->gr_name);
        break;
    }
    return probe;
}
#endif

QString serialOpenFailureSummary(const SerialOpenFailure& f)
{
    QString s = f.error.isEmpty() ? QStringLiteral("Open failed") : f.error;
    s += QStringLiteral(": Can't open %1").arg(f.device);
    if (f.nativeCode != 0) {
#ifdef Q_OS_WIN
        s += QStringLiteral(" (Windows error %1)").arg(f.nativeCode);
#else
        s += QStringLiteral(" (errno %1)").arg(f.nativeCode);
#endif
    }
    return s;
}

QString serialOpenFailureAdvice(const SerialOpenFailure& f)
{
    const QString dev = f.device;
    switch (f.kind) {
    case SerialOpenFailureKind::Permission:
        if (!f.group.isEmpty()) {
            return QStringLiteral(
                "Add your user to the %1 group to access serial ports. "
                "%2 belongs to the %1 group and your session is not a member.\n\n"
                "Run this in a terminal:\n\n"
                "    sudo usermod -aG %1 \"$USER\"\n\n"
                "On Debian or Ubuntu you can use this instead:\n\n"
                "    sudo adduser \"$USER\" %1\n\n"
                "Then log out of your desktop session and log back in (group "
                "membership is only read at login), relaunch AetherSDR and "
                "click Open again. AetherSDR does not run these commands for you.")
                .arg(f.group, dev);
        }
#ifdef Q_OS_MACOS
        return QStringLiteral(
            "This user may not read and write %1. Inspect its owner, group "
            "and permissions:\n\n"
            "    ls -le %1\n"
            "    id\n\n"
            "Grant this user read/write access to the device, then click Open again.")
            .arg(dev);
#else
        return QStringLiteral(
            "This user may not read and write %1. Inspect its owner, group, "
            "permissions and ACL:\n\n"
            "    ls -l %1\n"
            "    getfacl %1\n"
            "    id\n\n"
            "Grant this user read/write access to the device (for example "
            "through the group that owns it, or a udev rule). If you change "
            "group membership, log out of your desktop session and back in "
            "before relaunching AetherSDR. Then click Open again.")
            .arg(dev);
#endif
    case SerialOpenFailureKind::NotFound:
        return QStringLiteral(
            "%1 does not exist. Check that the interface is plugged in, then "
            "choose it again from the port list: the device name can change "
            "when it is reconnected.")
            .arg(dev);
    case SerialOpenFailureKind::InUse:
#ifdef Q_OS_WIN
        return QStringLiteral(
            "Another program has %1 open (Windows serial ports are exclusive), "
            "or this account is denied access to it. Close the other program "
            "(another AetherSDR, a logger, a CAT or digital-mode program), then "
            "click Open again.")
            .arg(dev);
#else
        return QStringLiteral(
            "%1 is accessible to this user, but another program or a lock file "
            "is holding it (another AetherSDR, a logger, a CAT or digital-mode "
            "program, ModemManager, or a stale lock under /var/lock). Close the "
            "other program, then click Open again.")
            .arg(dev);
#endif
    case SerialOpenFailureKind::Other:
    case SerialOpenFailureKind::None:
        break;
    }
#ifdef Q_OS_WIN
    return QStringLiteral(
        "Check %1 in Device Manager, then click Open again.").arg(dev);
#else
    return QStringLiteral(
        "To inspect the device:\n\n"
        "    ls -l %1\n"
        "    id\n\n"
        "Then click Open again.")
        .arg(dev);
#endif
}

} // namespace AetherSDR
