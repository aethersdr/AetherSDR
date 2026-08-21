#include "TciPeerProcess.h"

#include <QtGlobal>

#include <cstring>

#if defined(Q_OS_LINUX)
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#elif defined(Q_OS_MACOS)
#include <libproc.h>
#include <sys/proc_info.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <QSettings>
#include <vector>
#elif defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <QFileInfo>
#include <vector>
#endif

namespace AetherSDR {

namespace {

// Same host, two spellings: a loopback IPv4 client on an Any-bound listener
// shows up as ::ffff:127.0.0.1.  Compare on the IPv4 value when both sides
// have one, else on the raw address.
bool sameHost(const QHostAddress& a, const QHostAddress& b)
{
    bool a4 = false, b4 = false;
    const quint32 av = a.toIPv4Address(&a4);
    const quint32 bv = b.toIPv4Address(&b4);
    if (a4 && b4) return av == bv;
    if (a4 != b4) return false;
    return a == b;
}

#if defined(Q_OS_LINUX)

// /proc/net/tcp{,6} print each 32-bit word of the address as %08X of the
// native (little-endian) value, so "0100007F" is 127.0.0.1 and a v4-mapped
// loopback is "0000000000000000FFFF00000100007F".  Undo that word by word.
QHostAddress parseProcNetAddress(const QString& hex)
{
    if (hex.size() == 8) {
        bool ok = false;
        const quint32 w = hex.toUInt(&ok, 16);
        if (!ok) return {};
        // Bytes of the LE word in memory order are the IPv4 octets.
        const quint32 v4 = ((w & 0xFF) << 24) | ((w & 0xFF00) << 8)
                         | ((w & 0xFF0000) >> 8) | (w >> 24);
        return QHostAddress(v4);
    }
    if (hex.size() == 32) {
        Q_IPV6ADDR a6{};
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            const quint32 w = hex.mid(i * 8, 8).toUInt(&ok, 16);
            if (!ok) return {};
            a6[i * 4 + 0] = static_cast<quint8>(w & 0xFF);
            a6[i * 4 + 1] = static_cast<quint8>((w >> 8) & 0xFF);
            a6[i * 4 + 2] = static_cast<quint8>((w >> 16) & 0xFF);
            a6[i * 4 + 3] = static_cast<quint8>(w >> 24);
        }
        return QHostAddress(a6);
    }
    return {};
}

// The client's OWN row has local_address == our peer endpoint.  tcp6 first:
// on an Any-bound listener the common loopback-IPv4 client lives there in
// v4-mapped form, not in /proc/net/tcp.
bool findSocketInode(const QHostAddress& peer, quint16 port, quint64* inodeOut)
{
    for (const char* path : {"/proc/net/tcp6", "/proc/net/tcp"}) {
        QFile f(QString::fromLatin1(path));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream in(&f);
        in.readLine();  // header
        while (!in.atEnd()) {
            const QStringList col = in.readLine().simplified().split(QLatin1Char(' '));
            if (col.size() < 10) continue;
            const QStringList loc = col[1].split(QLatin1Char(':'));  // HEXADDR:HEXPORT
            if (loc.size() != 2) continue;
            bool ok = false;
            if (loc[1].toUShort(&ok, 16) != port || !ok) continue;
            if (!sameHost(parseProcNetAddress(loc[0]), peer)) continue;
            *inodeOut = col[9].toULongLong();
            return true;
        }
    }
    return false;
}

TciPeerProcessInfo resolveLinux(const QHostAddress& peer, quint16 port)
{
    TciPeerProcessInfo info;
    quint64 inode = 0;
    if (!findSocketInode(peer, port, &inode) || inode == 0) return info;
    const QString target = QStringLiteral("socket:[%1]").arg(inode);

    // Same-user processes only (unprivileged readlink on /proc/<pid>/fd) —
    // the normal case for a client the operator started.  Anything else just
    // fails to resolve.
    const QDir proc(QStringLiteral("/proc"));
    const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& pid : pids) {
        bool numeric = false;
        pid.toInt(&numeric);
        if (!numeric) continue;
        const QDir fdDir(QStringLiteral("/proc/%1/fd").arg(pid));
        const QStringList fds = fdDir.entryList(QDir::Files | QDir::System
                                                | QDir::NoDotAndDotDot);
        for (const QString& fd : fds) {
            if (QFile::symLinkTarget(fdDir.filePath(fd)) != target) continue;
            QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
            if (comm.open(QIODevice::ReadOnly | QIODevice::Text))
                info.name = QString::fromUtf8(comm.readAll()).trimmed();
            info.exePath = QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid));
            if (info.name.isEmpty() && !info.exePath.isEmpty())
                info.name = QFileInfo(info.exePath).fileName();
            info.resolved = !info.name.isEmpty() || !info.exePath.isEmpty();
            return info;
        }
    }
    return info;
}

#elif defined(Q_OS_MACOS)

// A macOS program is usually an app bundle, and the bundle's Info.plist
// carries the version the user sees in Finder (WSJT-X: "3.0.1").  Walk up
// from ".../Foo.app/Contents/MacOS/foo" to ".../Foo.app/Contents/Info.plist"
// and read it — a plain file read, never an execution of the client.  A bare
// executable (no bundle) yields an empty string.
QString bundleVersionForExecutable(const QString& exePath)
{
    const int macosDir = exePath.lastIndexOf(QStringLiteral("/Contents/MacOS/"));
    if (macosDir < 0) return {};
    const QString plist = exePath.left(macosDir) + QStringLiteral("/Contents/Info.plist");
    // NativeFormat on macOS reads property lists (binary or XML).
    QSettings info(plist, QSettings::NativeFormat);
    QString version = info.value(QStringLiteral("CFBundleShortVersionString")).toString().trimmed();
    if (version.isEmpty())
        version = info.value(QStringLiteral("CFBundleVersion")).toString().trimmed();
    return version;
}

QHostAddress sockinfoLocalAddress(const in_sockinfo& ini)
{
    if (ini.insi_vflag & INI_IPV6) {
        Q_IPV6ADDR a6{};
        static_assert(sizeof(a6) == sizeof(ini.insi_laddr.ina_6), "in6_addr size");
        memcpy(&a6, &ini.insi_laddr.ina_6, sizeof(a6));
        return QHostAddress(a6);
    }
    return QHostAddress(ntohl(ini.insi_laddr.ina_46.i46a_addr4.s_addr));
}

TciPeerProcessInfo resolveMac(const QHostAddress& peer, quint16 port)
{
    TciPeerProcessInfo info;
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return info;
    std::vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 16);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                          static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return info;
    const size_t count = static_cast<size_t>(bytes) / sizeof(pid_t);

    std::vector<proc_fdinfo> fds;
    for (size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;
        // Other users' processes refuse the fd listing (EPERM) and simply
        // contribute nothing — same-user clients are the normal case.
        const int fdBytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (fdBytes <= 0) continue;
        fds.resize(static_cast<size_t>(fdBytes) / sizeof(proc_fdinfo) + 8);
        const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                                     static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (got <= 0) continue;
        const size_t nfds = static_cast<size_t>(got) / sizeof(proc_fdinfo);
        for (size_t j = 0; j < nfds; ++j) {
            if (fds[j].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            socket_fdinfo si{};
            if (proc_pidfdinfo(pid, fds[j].proc_fd, PROC_PIDFDSOCKETINFO, &si,
                               sizeof(si)) != static_cast<int>(sizeof(si)))
                continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            const in_sockinfo& ini = si.psi.soi_proto.pri_tcp.tcpsi_ini;
            // Ports are reported in network byte order (as lsof reads them).
            if (ntohs(static_cast<quint16>(ini.insi_lport)) != port) continue;
            if (!sameHost(sockinfoLocalAddress(ini), peer)) continue;

            char path[PROC_PIDPATHINFO_MAXSIZE] = {};
            if (proc_pidpath(pid, path, sizeof(path)) > 0)
                info.exePath = QString::fromUtf8(path);
            char name[2 * MAXCOMLEN + 1] = {};
            if (proc_name(pid, name, sizeof(name)) > 0)
                info.name = QString::fromUtf8(name);
            if (info.name.isEmpty() && !info.exePath.isEmpty())
                info.name = info.exePath.section(QLatin1Char('/'), -1);
            if (!info.exePath.isEmpty())
                info.version = bundleVersionForExecutable(info.exePath);   // empty off-bundle
            info.resolved = !info.name.isEmpty() || !info.exePath.isEmpty();
            return info;
        }
    }
    return info;
}

#elif defined(Q_OS_WIN)

QString fileVersionString(const QString& exePath)
{
    const std::wstring w = exePath.toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (size == 0) return {};
    std::vector<char> buf(size);
    if (!GetFileVersionInfoW(w.c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len)
        || !ffi || len == 0)
        return {};
    return QStringLiteral("%1.%2.%3.%4")
        .arg(HIWORD(ffi->dwFileVersionMS)).arg(LOWORD(ffi->dwFileVersionMS))
        .arg(HIWORD(ffi->dwFileVersionLS)).arg(LOWORD(ffi->dwFileVersionLS));
}

bool findOwnerPid(const QHostAddress& peer, quint16 port, DWORD* pidOut)
{
    const quint16 wantPort = htons(port);
    // IPv4 table.
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        std::vector<char> buf(size ? size : 1);
        if (size && GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                        TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& r = t->table[i];
                if (static_cast<quint16>(r.dwLocalPort) != wantPort) continue;
                if (!sameHost(QHostAddress(ntohl(r.dwLocalAddr)), peer)) continue;
                *pidOut = r.dwOwningPid;
                return true;
            }
        }
    }
    // IPv6 table (covers ::1 and v4-mapped peers on an Any-bound listener).
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
        std::vector<char> buf(size ? size : 1);
        if (size && GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6,
                                        TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
                if (static_cast<quint16>(r.dwLocalPort) != wantPort) continue;
                Q_IPV6ADDR a6{};
                memcpy(&a6, r.ucLocalAddr, sizeof(a6));
                if (!sameHost(QHostAddress(a6), peer)) continue;
                *pidOut = r.dwOwningPid;
                return true;
            }
        }
    }
    return false;
}

TciPeerProcessInfo resolveWindows(const QHostAddress& peer, quint16 port)
{
    TciPeerProcessInfo info;
    DWORD pid = 0;
    if (!findOwnerPid(peer, port, &pid) || pid == 0) return info;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return info;
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(h, 0, path, &len) && len > 0)
        info.exePath = QString::fromWCharArray(path, static_cast<int>(len));
    CloseHandle(h);
    if (info.exePath.isEmpty()) return info;
    info.name = QFileInfo(info.exePath).completeBaseName();
    info.version = fileVersionString(info.exePath);   // empty when no resource
    info.resolved = true;
    return info;
}

#endif

} // namespace

TciPeerProcessInfo resolveLoopbackPeerProcess(const QHostAddress& peerAddr,
                                             quint16 peerPort)
{
    if (peerPort == 0 || !peerAddr.isLoopback()) return {};
#if defined(Q_OS_LINUX)
    return resolveLinux(peerAddr, peerPort);
#elif defined(Q_OS_MACOS)
    return resolveMac(peerAddr, peerPort);
#elif defined(Q_OS_WIN)
    return resolveWindows(peerAddr, peerPort);
#else
    return {};
#endif
}

} // namespace AetherSDR
