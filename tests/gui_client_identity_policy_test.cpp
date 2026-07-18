#include "core/GuiClientIdentityPolicy.h"

#include <cstdio>
#include <QLockFile>
#include <QTemporaryDir>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main()
{
    const QString a1 = GuiClientIdentityPolicy::automationClientId("worktree-a");
    const QString a2 = GuiClientIdentityPolicy::automationClientId("worktree-a");
    const QString b = GuiClientIdentityPolicy::automationClientId("worktree-b");
    check(a1 == a2, "automation identity is stable across launches");
    check(a1 != b, "different worktrees receive different GUI IDs");
    check(!QUuid(a1).isNull(), "derived automation identity is a valid UUID");

    check(GuiClientIdentityPolicy::protocolSafeStation(" Codex / GPT-5.6 ")
              == QStringLiteral("Codex-/-GPT-5.6"),
          "agent name is safe for the unquoted station protocol field");
    check(GuiClientIdentityPolicy::protocolSafeStation(QString(80, 'x')).size() <= 48,
          "station name is bounded");

    check(GuiClientIdentityPolicy::automationAgentName(
              QStringLiteral(" Codex "), QStringLiteral("Legacy"), QStringLiteral("Label"))
              == QStringLiteral("Codex"),
          "explicit automation agent name wins and is trimmed");
    check(GuiClientIdentityPolicy::automationAgentName(
              QStringLiteral(" "), QStringLiteral(" Legacy "), QStringLiteral("Label"))
              == QStringLiteral("Legacy"),
          "legacy automation station remains compatible");
    check(GuiClientIdentityPolicy::automationAgentName(
              QString(), QStringLiteral(" "), QStringLiteral(" Bridge label "))
              == QStringLiteral("Bridge label"),
          "automation label is the final configured fallback");
    check(GuiClientIdentityPolicy::automationAgentName(
              QString(), QString(), QStringLiteral(" "))
              == QStringLiteral("Automation"),
          "automation agent name has a neutral default");

    check(!GuiClientIdentityPolicy::shouldSelectDistinctId(
              false, QStringLiteral("Desk"), QStringLiteral("desk"), true),
          "known same-station persistent predecessor is reclaimed");
    check(GuiClientIdentityPolicy::shouldSelectDistinctId(
              false, QStringLiteral("Desk"), QStringLiteral("Laptop"), true),
          "different-station persistent collision rotates");
    check(GuiClientIdentityPolicy::shouldSelectDistinctId(
              false, QStringLiteral("Desk"), QStringLiteral("Desk"), false),
          "same-named unproven remote session rotates");
    check(GuiClientIdentityPolicy::shouldSelectDistinctId(
              true, QStringLiteral("Codex"), QStringLiteral("Codex"), true),
          "process-scoped collision always selects a distinct ID");

    QTemporaryDir temp;
    QLockFile first(temp.filePath(QStringLiteral("gui-client.lock")));
    QLockFile second(temp.filePath(QStringLiteral("gui-client.lock")));
    first.setStaleLockTime(0);
    second.setStaleLockTime(0);
    check(temp.isValid() && first.tryLock(0),
          "first local process claims the persistent identity lock");
    check(!second.tryLock(0),
          "second local process cannot claim the persistent identity lock");

    return failures == 0 ? 0 : 1;
}
