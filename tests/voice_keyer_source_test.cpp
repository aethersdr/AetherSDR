// Radio vs Local voice keyer selection (RFC #4214) — pure policy, no Qt event loop.
// Run: ./build/voice_keyer_source_test

#include "models/VoiceKeyerSource.h"

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

const char* name(VoiceKeyerSource s)
{
    return s == VoiceKeyerSource::Radio ? "Radio" : "Local";
}

} // namespace

int main()
{
    using S = VoiceKeyerSourceSetting;
    using R = VoiceKeyerSource;

    // Arguments are (setting, radioHasKeyer, licenceSeen, licenceEnabled).

    // Auto follows the licence once the radio has reported it.
    report("auto_licensed_uses_radio",
           resolveVoiceKeyerSource(S::Auto, true, true, true) == R::Radio);
    report("auto_unlicensed_uses_local",
           resolveVoiceKeyerSource(S::Auto, true, true, false) == R::Local);

    // Before the radio reports the entitlement, stay on the radio DVK (fail
    // open, like the indicator) — an unknown licence is not a missing one.
    report("auto_unknown_licence_stays_on_radio",
           resolveVoiceKeyerSource(S::Auto, true, false, false) == R::Radio,
           name(resolveVoiceKeyerSource(S::Auto, true, false, false)));

    // A radio with no DVK at all has nothing for Auto to fall back to, whatever
    // the licence says — an HL2 or an Icom reports no entitlement because it has
    // no DVK, not because one is switched off. Picking Radio there left the
    // feature unreachable on every non-Flex radio.
    report("auto_on_a_radio_without_a_dvk_uses_local",
           resolveVoiceKeyerSource(S::Auto, false, false, false) == R::Local,
           name(resolveVoiceKeyerSource(S::Auto, false, false, false)));
    report("auto_on_a_radio_without_a_dvk_ignores_a_licensed_report",
           resolveVoiceKeyerSource(S::Auto, false, true, true) == R::Local);

    // An explicit choice wins over the licence either way.
    report("forced_local_on_licensed_radio",
           resolveVoiceKeyerSource(S::Local, true, true, true) == R::Local);
    report("forced_radio_on_unlicensed_radio",
           resolveVoiceKeyerSource(S::Radio, true, true, false) == R::Radio);

    // Persisted text round-trips; anything unrecognised is Auto.
    report("setting_names_round_trip",
           parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Radio)) == S::Radio
               && parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Local)) == S::Local
               && parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Auto)) == S::Auto);
    report("unrecognised_text_is_auto",
           parseVoiceKeyerSourceSetting(QStringLiteral("Local")) == S::Auto
               && parseVoiceKeyerSourceSetting(QString()) == S::Auto);

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
