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

QStringList walk(const QStringList& modes, QString from, int direction, int steps,
                 const QStringList& radioModes = {})
{
    QStringList visited;
    for (int i = 0; i < steps; ++i) {
        from = AetherSDR::nextCycledMode(modes, from, direction, radioModes);
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

    const QStringList down = walk(modes, QStringLiteral("USB"), -1, 14);
    ok &= expect(down == QStringList({"FDVL", "FDVU", "RTTY", "DIGL", "DIGU", "DFM",
                                      "NFM", "FM", "SAM", "AM", "CWL", "CW", "LSB",
                                      "USB"}),
                 "Mode Down from USB visits every other mode and returns to USB");
    const QStringList up = walk(modes, QStringLiteral("USB"), +1, 14);
    ok &= expect(up == QStringList({"LSB", "CW", "CWL", "AM", "SAM", "FM", "NFM",
                                    "DFM", "DIGU", "DIGL", "RTTY", "FDVU", "FDVL",
                                    "USB"}),
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

    // The radio's mode_list: modes it does not report are skipped (#6028).
    // MEASURED (#6028, FLEX-8400 fw 4.2.20.41343 with the FreeDV waveform,
    // Mac log aethersdr-20260929-162513.log line 181, 2026-09-29).
    const QStringList flex8400 = {"LSB", "USB", "AM", "CW", "DIGL", "DIGU", "SAM",
                                  "FM", "NFM", "DFM", "RTTY", "FDVU", "FDVL"};
    ok &= expect(nextCycledMode(modes, QStringLiteral("CW"), +1, flex8400)
                     == QStringLiteral("AM"),
                 "Mode Up from CW skips CWL, which the radio does not report");
    ok &= expect(nextCycledMode(modes, QStringLiteral("AM"), -1, flex8400)
                     == QStringLiteral("CW"),
                 "Mode Down from AM skips CWL");
    const QStringList reported = walk(modes, QStringLiteral("USB"), +1, 13, flex8400);
    ok &= expect(reported == QStringList({"LSB", "CW", "AM", "SAM", "FM", "NFM",
                                          "DFM", "DIGU", "DIGL", "RTTY", "FDVU",
                                          "FDVL", "USB"}),
                 "Mode Up from USB visits only reported modes and returns to USB");

    // FreeDV waveform modes (#6028): a stop when the radio reports them.
    ok &= expect(modes.contains(QStringLiteral("FDVU"))
                     && modes.contains(QStringLiteral("FDVL")),
                 "FDVU and FDVL have mode triggers and shortcuts");
    ok &= expect(nextCycledMode(modes, QStringLiteral("RTTY"), +1, flex8400)
                     == QStringLiteral("FDVU"),
                 "Mode Up from RTTY reaches FDVU on a radio that reports it");
    ok &= expect(nextCycledMode(modes, QStringLiteral("FDVU"), +1, flex8400)
                     == QStringLiteral("FDVL"),
                 "Mode Up from FDVU reaches FDVL");
    // CONSTRUCTED: the measured list without the FreeDV waveform's modes.
    QStringList noFreeDv = flex8400;
    noFreeDv.removeAll(QStringLiteral("FDVU"));
    noFreeDv.removeAll(QStringLiteral("FDVL"));
    ok &= expect(nextCycledMode(modes, QStringLiteral("RTTY"), +1, noFreeDv)
                     == QStringLiteral("USB"),
                 "Mode Up from RTTY skips FDVU/FDVL when the radio does not report them");
    ok &= expect(nextCycledMode(modes, QStringLiteral("USB"), -1, noFreeDv)
                     == QStringLiteral("RTTY"),
                 "Mode Down from USB skips FDVL/FDVU when the radio does not report them");
    ok &= expect(registry.activateMode(DigitalVoiceModeId::DStar),
                 "D-STAR service activates");
    ok &= expect(nextCycledMode(modes, QStringLiteral("DIGU"), -1, flex8400)
                     == QStringLiteral("DFM"),
                 "a running helper does not bring back a mode the radio does not report");
    registry.deactivateMode(DigitalVoiceModeId::DStar);
    ok &= expect(nextCycledMode(modes, QStringLiteral("CW"), +1, {})
                     == QStringLiteral("CWL"),
                 "no reported list keeps the whole list");
    // CONSTRUCTED: a list sharing no entry with ours.
    ok &= expect(nextCycledMode(modes, QStringLiteral("USB"), +1, {"XYZ"}).isEmpty(),
                 "no reported entry in the list yields no mode");

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
