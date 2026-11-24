#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtGlobal>

#ifdef Q_OS_UNIX
#include <sys/un.h>
#endif

namespace AetherSDR::control {

// Longest path QLocalServer/QLocalSocket accept: sun_path less its NUL, and on
// Linux/QNX one byte more that Qt reserves for the abstract namespace
// (qlocalserver_unix.cpp, extraCharacters). Windows pipes have no such limit.
[[nodiscard]] inline qsizetype localSocketPathLimit()
{
#if defined(Q_OS_UNIX)
#if defined(Q_OS_LINUX) || defined(Q_OS_QNX)
    constexpr qsizetype reserved = 2;
#else
    constexpr qsizetype reserved = 1;
#endif
    return static_cast<qsizetype>(sizeof(sockaddr_un{}.sun_path)) - reserved;
#else
    return -1;
#endif
}

// Empty when the path fits or limit < 0; otherwise a reason naming the path,
// its encoded length, the limit and what decides the directory on this OS.
[[nodiscard]] inline QString localSocketPathTooLong(const QString& path, qsizetype limit)
{
    const qsizetype length = QFile::encodeName(path).size();
    if (limit < 0 || length <= limit) {
        return {};
    }
#if defined(Q_OS_MACOS)
    const QString hint = QStringLiteral(
        "the directory is ~/Library/Application Support/aethersdr under the user's home "
        "(CFFIXED_USER_HOME when set); TMPDIR and XDG_RUNTIME_DIR do not move it");
#else
    const QString hint = QStringLiteral(
        "the directory is $XDG_RUNTIME_DIR/aethersdr, or $TMPDIR/runtime-<user>/aethersdr "
        "when XDG_RUNTIME_DIR is unset or unusable; point it at a shorter directory");
#endif
    return QStringLiteral("local socket path is %1 bytes, over the %2-byte limit for a Unix "
                          "socket here: '%3'; %4")
        .arg(length)
        .arg(limit)
        .arg(path, hint);
}

} // namespace AetherSDR::control
