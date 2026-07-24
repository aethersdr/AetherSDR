#include "core/AetherDspModePolicy.h"

#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void expect(const char* name, bool condition)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main()
{
    for (const QString& mode :
         {QStringLiteral("DIGU"), QStringLiteral("DIGL"),
          QStringLiteral("RTTY"), QStringLiteral("CW"),
          QStringLiteral("CWL"), QStringLiteral("NT")}) {
        expect(qPrintable(mode + QStringLiteral(" disables AetherDSP")),
               aetherDspModeRequiresDisable(mode));
    }

    for (const QString& mode :
         {QStringLiteral("LSB"), QStringLiteral("USB"),
          QStringLiteral("AM"), QStringLiteral("SAM"),
          QStringLiteral("FM"), QStringLiteral("NFM")}) {
        expect(qPrintable(mode + QStringLiteral(" allows AetherDSP")),
               !aetherDspModeRequiresDisable(mode));
    }

    expect("audible CW slice restricts mixed stream",
           aetherDspMixRequiresDisable({
               {QStringLiteral("LSB"), false, 50.0f},
               {QStringLiteral("CW"), false, 50.0f},
           }));
    expect("muted CW slice does not restrict mixed stream",
           !aetherDspMixRequiresDisable({
               {QStringLiteral("LSB"), false, 50.0f},
               {QStringLiteral("CW"), true, 50.0f},
           }));
    expect("zero-gain digital slice does not restrict mixed stream",
           !aetherDspMixRequiresDisable({
               {QStringLiteral("USB"), false, 50.0f},
               {QStringLiteral("DIGU"), false, 0.0f},
           }));

    AetherDspModePolicy policy;
    AetherDspModePolicy::Action action =
        policy.update(false, QStringLiteral("NR2"));
    expect("compatible mode does not change NR2",
           action.kind == AetherDspModePolicy::ActionKind::None);

    action = policy.update(true, QStringLiteral("NR2"));
    expect("restriction visibly disables selected method",
           action.kind == AetherDspModePolicy::ActionKind::Disable
               && action.method == QStringLiteral("NR2"));
    expect("automatic disable persists original selection",
           policy.methodForPersistence({}) == QStringLiteral("NR2"));

    action = policy.update(true, {});
    expect("continued restriction does not repeat disable",
           action.kind == AetherDspModePolicy::ActionKind::None);

    action = policy.update(false, {});
    expect("clearing restriction restores selected method",
           action.kind == AetherDspModePolicy::ActionKind::Enable
               && action.method == QStringLiteral("NR2"));
    expect("pending restore remains persistent until engine confirms it",
           policy.methodForPersistence({}) == QStringLiteral("NR2"));
    action = policy.update(false, QStringLiteral("NR2"));
    expect("engine confirmation completes automatic restore",
           action.kind == AetherDspModePolicy::ActionKind::None
               && policy.methodForPersistence(QStringLiteral("NR2"))
                      == QStringLiteral("NR2"));

    action = policy.update(true, QStringLiteral("RN2"));
    expect("new restriction remembers current method",
           action.kind == AetherDspModePolicy::ActionKind::Disable
               && action.method == QStringLiteral("RN2"));
    policy.noteUserOverride();
    action = policy.update(true, QStringLiteral("NR4"));
    expect("user override wins while restriction remains",
           action.kind == AetherDspModePolicy::ActionKind::None);
    expect("user override replaces automatic persistence memory",
           policy.methodForPersistence(QStringLiteral("NR4"))
               == QStringLiteral("NR4"));
    action = policy.update(false, QStringLiteral("NR4"));
    expect("user override is not replaced when restriction clears",
           action.kind == AetherDspModePolicy::ActionKind::None);

    // Restore, then a restriction that recurs BEFORE the engine confirms the
    // enable (rapid mode churn). The restored method is still remembered, but a
    // fresh restriction must re-disable it rather than silently skip and leave
    // it processing a CW/digital stream.
    AetherDspModePolicy racePolicy;
    racePolicy.update(true, QStringLiteral("NR2"));   // disable, remembers NR2
    action = racePolicy.update(false, {});            // restore -> Enable, keeps NR2
    expect("race: clearing restriction emits restore",
           action.kind == AetherDspModePolicy::ActionKind::Enable
               && action.method == QStringLiteral("NR2"));
    action = racePolicy.update(true, QStringLiteral("NR2")); // re-restrict before clear
    expect("race: re-restriction before engine confirm re-disables",
           action.kind == AetherDspModePolicy::ActionKind::Disable
               && action.method == QStringLiteral("NR2"));

    return failures == 0 ? 0 : 1;
}
