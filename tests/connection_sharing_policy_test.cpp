// Unit tests for ConnectionSharingPolicy — the fail-closed gate that refuses
// to join a discovered radio another client is already using (#4448), now
// single-sourced for the ConnectionPanel manual-connect and
// MainWindow_Session auto-connect paths (M0, #5263).
//
// The load-bearing rows are the fail-closed ones: an unknown or empty family
// must refuse, because wedging someone's active QSO is worse than making the
// operator retry after the other client leaves.

#include "core/backends/ConnectionSharingPolicy.h"

#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main()
{
    // Flex shares: multiFLEX gives each GUI client its own session.
    report("flex shares", familySupportsSharedInUseConnect(QStringLiteral("flex")));
    report("flex shares case-insensitively",
           familySupportsSharedInUseConnect(QStringLiteral("Flex"))
               && familySupportsSharedInUseConnect(QStringLiteral("FLEX")));

    // Every other family served today is single-client at the protocol layer.
    report("hl2 refuses (HPSDR Protocol 1 single-client, #4448)",
           !familySupportsSharedInUseConnect(QStringLiteral("hl2")));
    report("icom refuses (RS-BA1 session is exclusive)",
           !familySupportsSharedInUseConnect(QStringLiteral("icom")));
    report("sim refuses",
           !familySupportsSharedInUseConnect(QStringLiteral("sim")));

    // Fail closed on anything unrecognized: a future family must OPT IN to
    // sharing, never inherit it from a default.
    report("unknown family refuses",
           !familySupportsSharedInUseConnect(QStringLiteral("yaesu")));
    report("empty family refuses",
           !familySupportsSharedInUseConnect(QString()));

    std::printf("%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
