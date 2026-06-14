#include "models/ProfileLoadCommand.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

void checkProfileLoad(const QString& command,
                      const QString& expectedType,
                      const QString& expectedName,
                      const char* name)
{
    const ProfileLoadCommand profileLoad = parseProfileLoadCommand(command);
    check(profileLoad.valid, name);
    check(profileLoad.type == expectedType, "profile load type capture");
    check(profileLoad.name == expectedName, "profile load name capture");
}

} // namespace

int main()
{
    checkProfileLoad(QStringLiteral("profile global load \"SO2R\""),
                     QStringLiteral("global"),
                     QStringLiteral("SO2R"),
                     "global profile load command parses");
    check(profileLoadMayRebuildRadioTopology(QStringLiteral("global")),
          "global profile load is topology-changing");

    checkProfileLoad(QStringLiteral(" profile TX load \"Low Power\" "),
                     QStringLiteral("tx"),
                     QStringLiteral("Low Power"),
                     "TX profile load command parses case-insensitively");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("tx")),
          "TX profile load is not topology-changing");

    checkProfileLoad(QStringLiteral("profile mic load \"Studio Mic\""),
                     QStringLiteral("mic"),
                     QStringLiteral("Studio Mic"),
                     "mic profile load command parses");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("mic")),
          "mic profile load is not topology-changing");

    check(!parseProfileLoadCommand(QStringLiteral("profile global save \"SO2R\"")).valid,
          "non-load profile command is ignored");
    check(kProfileLoadDeferredPanFlushDelayMs > kProfileLoadStateWriteHoldMs,
          "deferred pan flush runs after profile-load write hold");
    check(kProfileLoadPostHoldRecoveryDelayMs > kProfileLoadDeferredPanFlushDelayMs,
          "post-hold recovery runs after deferred pan flush");

    return failures == 0 ? 0 : 1;
}
