// Mode Up / Down must not park on a mode the slice refuses (#6034).
//
// Built with AETHER_ENABLE_DIGITAL_VOICE_HELPER, as the app is by default
// (ENABLE_DSTAR=ON), so DSTR stays in the cycled list and the only thing that
// decides whether it can be selected is whether the D-STAR helper is running.

#include "core/DigitalVoiceFeature.h"
#include "core/DigitalVoiceModeRegistry.h"

#include <QCoreApplication>
#include <QStringList>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

QStringList walk(const QStringList& modes, QString from, int direction, int steps)
{
    QStringList visited;
    for (int i = 0; i < steps; ++i) {
        from = AetherSDR::nextCycledMode(modes, from, direction);
        visited.append(from);
    }
    return visited;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    using namespace AetherSDR;
    DigitalVoiceModeRegistry& registry = DigitalVoiceModeRegistry::instance();
    const QStringList modes = modeActionModes();
    ok &= expect(modes.contains(QStringLiteral("DSTR")),
                 "a helper build keeps DSTR in the cycled list");

    // Helper not running: DSTR is refused, so the cycle steps over it.
    registry.deactivateMode(DigitalVoiceModeId::DStar);
    ok &= expect(!digitalVoiceModeSelectable(QStringLiteral("DSTR")),
                 "DSTR is not selectable while the helper is not running");
    ok &= expect(digitalVoiceModeSelectable(QStringLiteral("DFM"))
                 && digitalVoiceModeSelectable(QStringLiteral("USB")),
                 "non-digital-voice modes are always selectable");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DIGU"), -1) == QStringLiteral("DFM"),
                 "Mode Down from DIGU skips DSTR to DFM");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DFM"), +1) == QStringLiteral("DIGU"),
                 "Mode Up from DFM skips DSTR to DIGU");

    const QStringList down = walk(modes, QStringLiteral("USB"), -1, 12);
    ok &= expect(down == QStringList({"RTTY", "DIGL", "DIGU", "DFM", "NFM", "FM",
                                      "SAM", "AM", "CWL", "CW", "LSB", "USB"}),
                 "Mode Down from USB visits every other mode and returns to USB");
    const QStringList up = walk(modes, QStringLiteral("USB"), +1, 12);
    ok &= expect(up == QStringList({"LSB", "CW", "CWL", "AM", "SAM", "FM", "NFM",
                                    "DFM", "DIGU", "DIGL", "RTTY", "USB"}),
                 "Mode Up from USB visits every other mode and returns to USB");

    // Helper running: DSTR is a normal stop in both directions.
    ok &= expect(registry.activateMode(DigitalVoiceModeId::DStar),
                 "D-STAR service activates");
    ok &= expect(digitalVoiceModeSelectable(QStringLiteral("DSTR")),
                 "DSTR is selectable while the helper is running");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DIGU"), -1) == QStringLiteral("DSTR"),
                 "Mode Down from DIGU reaches DSTR while the helper runs");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DFM"), +1) == QStringLiteral("DSTR"),
                 "Mode Up from DFM reaches DSTR while the helper runs");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DSTR"), +1) == QStringLiteral("DIGU"),
                 "Mode Up from DSTR continues to DIGU");
    registry.deactivateMode(DigitalVoiceModeId::DStar);

    ok &= expect(nextCycledMode({}, QStringLiteral("USB"), +1).isEmpty(),
                 "an empty list yields no mode");
    ok &= expect(nextCycledMode({QStringLiteral("DSTR")}, QStringLiteral("DSTR"), +1).isEmpty(),
                 "a list of only refused modes yields no mode instead of spinning");

    if (!ok) {
        return 1;
    }
    std::cout << "mode_cycle_test: all checks passed\n";
    return 0;
}
