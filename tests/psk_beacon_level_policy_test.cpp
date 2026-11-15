// The WSPR beacon's generated audio level: which default, and when a stored
// value outranks it.
//
// This exists because a mis-defaulted beacon level is SILENT. The beacon keys
// for 111.6 s with nobody watching, and a level that is 17 dB wrong produces a
// transmission that looks completely normal from inside the application — the
// panadapter, the meters and the status line all read the same as a correct
// one. The only report is that nobody spots you, which an operator reads as
// propagation.
//
// Two of the three cases below pin a bug that shipped in review rather than a
// hypothetical:
//
//   - the default was read once at construction, so an operator who opened PSK
//     Reporter BEFORE connecting kept the disconnected answer for the whole
//     session (PR #5651 review);
//   - the stored value was app-global, so a level chosen on a radio that
//     modulates on its own side silently became the unattended level for a
//     host-modulating radio, where it means something completely different.
//
// PskReporterMapDialog evaluates these same functions rather than its own copy,
// so what passes here is what the dialog runs (gui/PskBeaconLevelPolicy.h).

#include "gui/PskBeaconLevelPolicy.h"

#include <cstdio>
#include <optional>

using AetherSDR::psk::beaconLevelDefaultDbFs;
using AetherSDR::psk::beaconLevelToApplyDbFs;
using AetherSDR::psk::kBeaconLevelHostModulatedDbFs;
using AetherSDR::psk::kBeaconLevelRadioModulatedDbFs;
using AetherSDR::psk::legacyBeaconLevelAppliesTo;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

} // namespace

int main()
{
    // ── The default is a capability answer, not a family answer ─────────────
    //
    // hostModulates is TransmitModel::hostModulation() — hostModulates &&
    // canTransmit — and never a `family == "hl2"` test. The distinction is not
    // cosmetic: IcomCivBackend sets takesTxAudioOverSeam=true with
    // hostModulates=false, so the OTHER hostModulation() in the tree
    // (AudioEngine's) is true on an Icom and would hand it -3 dBFS, 17 dB into
    // a modulator the operator has already levelled.
    check(beaconLevelDefaultDbFs(true) == kBeaconLevelHostModulatedDbFs
              && beaconLevelDefaultDbFs(true) == -3,
          "a host-modulating backend generates near the top of the range");
    check(beaconLevelDefaultDbFs(false) == kBeaconLevelRadioModulatedDbFs
              && beaconLevelDefaultDbFs(false) == -20,
          "a radio that modulates on its own side keeps the quiet source");
    // The gap is the whole point of the change: 17 dB, and after #5646 removes
    // Hl2TxDsp's makeup half it is 17 dB on the air rather than 17 dB the ALC
    // hands straight back.
    check(beaconLevelDefaultDbFs(true) - beaconLevelDefaultDbFs(false) == 17,
          "the two defaults differ by the 17 dB the ALC used to supply");

    // ── Armed outranks everything ───────────────────────────────────────────
    //
    // Principle VI: the level that goes out is the level the operator saw when
    // they pressed the button. A radio status change arriving mid-slot — a
    // momentary disconnect, a capability republish — must not move it. This is
    // the first test in the function for that reason, and it holds even when
    // the answer would otherwise change.
    check(!beaconLevelToApplyDbFs(/*armed=*/true, std::nullopt, true).has_value(),
          "an armed beacon is never re-levelled, even with no stored value");
    check(!beaconLevelToApplyDbFs(/*armed=*/true, std::optional<int>(-40), false)
               .has_value(),
          "an armed beacon is never re-levelled, stored value or not");

    // ── A stored value is a deliberate choice ───────────────────────────────
    //
    // ...and it is stored PER RADIO, which is what makes "the operator set
    // this" mean "for this radio". Both directions matter: a quiet stored level
    // must survive on a host-modulating backend (the operator may be feeding an
    // amplifier), and a loud one must survive on a radio-modulating backend.
    check(beaconLevelToApplyDbFs(false, std::optional<int>(-40), true)
              == std::optional<int>(-40),
          "a stored level outranks the host-modulating default");
    check(beaconLevelToApplyDbFs(false, std::optional<int>(-3), false)
              == std::optional<int>(-3),
          "a stored level outranks the radio-modulating default");
    check(beaconLevelToApplyDbFs(false, std::nullopt, true)
              == std::optional<int>(-3),
          "with nothing stored for this radio, the capability default applies");
    check(beaconLevelToApplyDbFs(false, std::nullopt, false)
              == std::optional<int>(-20),
          "and the same on a radio that modulates on its own side");

    // A stored value equal to the OTHER backend's default is still a stored
    // value. The migration decides what gets imported; once a document exists,
    // this function must not second-guess it, or an operator who deliberately
    // chose -20 on their HL2 would be overridden on every connect.
    check(beaconLevelToApplyDbFs(false, std::optional<int>(-20), true)
              == std::optional<int>(-20),
          "a deliberate -20 on a host-modulating radio is not re-defaulted");

    // ── The legacy app-global key is imported only where it meant something ─
    //
    // While Hl2TxDsp's ALC still had its makeup half it normalised anything
    // from roughly -45 dBFS up onto alcTargetPeak, so on a host-modulating
    // backend the old control was inert: whatever sits in that key was never a
    // choice ABOUT the air, and freezing it into the one document that now
    // decides an unattended transmit level would import a non-choice.
    check(!legacyBeaconLevelAppliesTo(true),
          "the legacy app-global level is not claimed by a host-modulating radio");
    check(legacyBeaconLevelAppliesTo(false),
          "it is claimed where it did reach the air, through the radio's own gain");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
