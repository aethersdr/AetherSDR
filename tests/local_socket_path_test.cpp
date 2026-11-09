#include "core/control/LocalSocketPath.h"

#include <QString>

#include <cstdio>

using AetherSDR::control::localSocketPathLimit;
using AetherSDR::control::localSocketPathTooLong;

namespace {

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

QString pathOfBytes(qsizetype bytes)
{
    const QString prefix = QStringLiteral("/r/aethersdr/control-");
    return prefix + QString(bytes - prefix.size(), QLatin1Char('x'));
}

} // namespace

int main()
{
    bool ok = true;

#if defined(Q_OS_MACOS)
    ok &= check(localSocketPathLimit() == 103, "macOS limit is sun_path[104] less its NUL");
#elif defined(Q_OS_LINUX)
    ok &= check(localSocketPathLimit() == 106, "Linux limit is sun_path[108] less NUL and abstract byte");
#elif defined(Q_OS_WIN)
    ok &= check(localSocketPathLimit() < 0, "Windows pipes are not length-checked");
#endif

    const qsizetype limit = 103;
    ok &= check(localSocketPathTooLong(pathOfBytes(limit), limit).isEmpty(),
                "a path exactly at the limit fits");
    ok &= check(localSocketPathTooLong(pathOfBytes(40), limit).isEmpty(),
                "a short path fits");
    ok &= check(localSocketPathTooLong(pathOfBytes(197), -1).isEmpty(),
                "a negative limit disables the check");

    const QString over = pathOfBytes(limit + 1);
    const QString overReason = localSocketPathTooLong(over, limit);
    ok &= check(!overReason.isEmpty(), "one byte over the limit is refused");
    ok &= check(overReason.contains(over), "the reason names the path");
    ok &= check(overReason.contains(QStringLiteral("104 bytes")), "the reason gives the length");
    ok &= check(overReason.contains(QStringLiteral("103-byte limit")), "the reason gives the limit");
#if defined(Q_OS_MACOS)
    ok &= check(overReason.contains(QStringLiteral("CFFIXED_USER_HOME")),
                "the macOS reason names the home override that moves the directory");
#else
    ok &= check(overReason.contains(QStringLiteral("XDG_RUNTIME_DIR"))
                    && overReason.contains(QStringLiteral("TMPDIR")),
                "the Unix reason names XDG_RUNTIME_DIR and its TMPDIR fallback");
#endif

    const QString deep = pathOfBytes(197);
    ok &= check(localSocketPathTooLong(deep, limit).contains(QStringLiteral("197 bytes")),
                "a 197-byte runtime path is refused with its length");

#if defined(Q_OS_UNIX)
    // "/r/" plus 30 U+00E9 is 33 QChars. QFile::encodeName makes it 63 bytes on
    // Linux (UTF-8) and 93 on macOS (NFD: e + U+0301). Windows uses the ANSI code
    // page, where it is 33 bytes, and has no limit, so the check is Unix-only.
    const QString wide = QStringLiteral("/r/") + QString(30, QChar(0x00e9));
    ok &= check(!localSocketPathTooLong(wide, 50).isEmpty(),
                "the limit is measured in encoded bytes, not characters");
#endif

    if (ok) {
        std::printf("local_socket_path_test: all checks passed\n");
    }
    return ok ? 0 : 1;
}
