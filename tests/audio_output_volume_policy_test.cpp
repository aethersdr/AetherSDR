// One master slider drives two different outputs. Before AudioOutputVolumePolicy
// they also shared one stored number, so a level set for the radio's speaker came
// back as the headphone level on the next launch.
//
// What this pins:
//   * the split applies to ANAN and to NOTHING ELSE -- every other family, and
//     a caller with no radio at all, keeps the historical key and default, so
//     this change is invisible to them;
//   * where it does apply, the two sinks read and write DIFFERENT keys, so
//     neither can overwrite the other's level;
//   * the radio-side default is 0 and the PC-side default is 100 -- a radio
//     whose audio this client originates comes up silent and is raised
//     deliberately, which is the whole point of the change;
//   * PC Audio ON always means the PC key, on every family: the radio-side key
//     is reached by the sink being the radio's, not by the radio being an ANAN;
//   * the defaults are the STRINGS AppSettings::value() takes as its fallback.
//     A numeric literal there compiles and converts, and the mistake would only
//     show on a profile that has never stored the key -- which is exactly the
//     first-launch case this is for.
//
// storedVolumePercent() and pcAudioEnabled() are not covered here: they read the
// live AppSettings singleton, and their two inputs (the key and its default) are
// what these cases pin.

#include "core/AudioOutputVolumePolicy.h"

#include <QString>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    namespace P = AudioOutputVolumePolicy;

    const QString anan = QStringLiteral("anan");
    const QString flex = QStringLiteral("flex");

    // ---- who separates the two levels ----
    check(P::separatesRadioOutputLevel(anan),
          "ANAN keeps its own output level: this client originates that audio");
    check(P::separatesRadioOutputLevel(QStringLiteral("ANAN")),
          "the family test is case-insensitive, like every other family compare");
    check(!P::separatesRadioOutputLevel(flex),
          "a radio that owns its own mixer is left alone");
    check(!P::separatesRadioOutputLevel(QStringLiteral("hl2")),
          "the split is ANAN-only today -- widening it is a separate decision");
    check(!P::separatesRadioOutputLevel(QString()),
          "no radio yet means no split, so a caller that cannot name a family is safe");

    // ---- the two sinks are not the same stored number, where it applies ----
    check(P::pcAudioKey() != P::radioOutputKey(),
          "the PC sink and the radio output store their levels under different keys");
    check(P::volumeKey(true, anan) == P::pcAudioKey(),
          "PC Audio on selects the PC sink's key even on a radio that splits them");
    check(P::volumeKey(false, anan) == P::radioOutputKey(),
          "PC Audio off on an ANAN selects the radio output's key");

    // ---- and every other family is untouched ----
    check(P::volumeKey(false, flex) == P::pcAudioKey(),
          "PC Audio off on a Flex still reads the historical key");
    check(P::volumeKey(false, QString()) == P::pcAudioKey(),
          "with no family the historical key is the fallback, not the radio one");
    check(P::volumeDefault(false, flex) == P::pcAudioDefault(),
          "a Flex keeps the historical default: it must not come up at 0");
    check(P::volumeDefault(false, QString()) == P::pcAudioDefault(),
          "neither does a caller with no radio");

    // The old shared key keeps its name, so an existing profile's PC level is
    // not orphaned by this change.
    check(P::pcAudioKey() == QStringLiteral("MasterVolume"),
          "the PC sink keeps the historical key, so a stored PC level survives the split");

    // ---- the defaults ----
    check(P::volumeDefault(false, anan) == QStringLiteral("0"),
          "with nothing stored the ANAN's output starts SILENT, not at full scale");
    check(P::volumeDefault(true, anan) == QStringLiteral("100"),
          "the PC sink's own default is unchanged");
    check(P::volumeDefault(true, anan) != P::volumeDefault(false, anan),
          "the two defaults are deliberately different; one shared default is the bug");

    // ---- the defaults are strings, matching AppSettings::value()'s fallback ----
    bool ok = false;
    const int radioDefault = P::radioOutputDefault().toInt(&ok);
    check(ok && radioDefault == 0, "the radio default parses as the integer 0");
    const int pcDefault = P::pcAudioDefault().toInt(&ok);
    check(ok && pcDefault == 100, "the PC default parses as the integer 100");

    // ---- every value is in slider range ----
    for (const QString& family : {anan, flex, QString()}) {
        for (bool pc : {false, true}) {
            const int v = P::volumeDefault(pc, family).toInt();
            check(v >= 0 && v <= 100, "a default outside 0..100 would be clamped silently");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "audio_output_volume_policy_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
