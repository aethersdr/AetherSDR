// Which radio-mixer controls exist on the connected radio
// (MixerControlAvailability.h).
//
// On a Hermes-Lite 2 the title-bar headphone mute and slider, the MIDI
// Headphone/Master Volume knobs and the WheelHeadphoneVolume controller action
// all sent `mixer headphone|lineout ...` wire text that RadioModel::sendCmd
// drops for want of a command plane: the control moved and nothing happened.
// That radio has no headphone output, so the headphone pair is unavailable
// there -- dimmed with its reason -- and the MIDI knob and wheel refuse.
//
// Three things are pinned here:
//   1. the headphone predicate: unavailable ONLY when connected with no
//      command plane. A Flex (command plane) keeps the pair; with no radio
//      connected the pair is available, as every capability gate restores
//      on disconnect.
//   2. the master-knob predicate: this computer's output only when connected,
//      with no command plane, and with PC Audio on -- the title bar's master
//      slider path. A Flex keeps its wire text.
//   3. what the title bar DOES: unavailable dims the mute and slider and puts
//      the same reason on the tooltip and the accessibleDescription, naming
//      the controls that do drive this computer's output; available restores
//      the constructor's wording exactly. Neither direction touches the slider
//      value or the mute state, and neither emits a command -- so the
//      connect/disconnect edge never writes a level no radio reported.
//
// MainWindow consumes (1) at the title bar (applyTxAudioCapabilities), the MIDI
// Headphone Volume knob and WheelHeadphoneVolume, and (2) at the MIDI Master
// Volume knob; this is their truth table, not a copy of it.
//
// Mutation-checked: each mutation below breaks the build or fails the named
// rows, and nothing else in this test --
//   - `!connected ||` dropped from headphoneControlsAvailable: the build (the
//     "disconnected" static_assert); with the static_asserts off, the
//     "disconnected -> available" row;
//   - `|| hasCommandPlane` dropped: the build (the "command plane"
//     static_assert); with them off, the "connected WITH a command plane" row;
//   - both setEnabled(available) calls skipped in setHeadphoneAvailable: the
//     two "dim:" rows;
//   - both setAccessibleDescription(reason) calls skipped: the eight
//     accessibleDescription rows.

#include "TestSettingsProfile.h"

#include "gui/MixerControlAvailability.h"
#include "gui/TitleBar.h"

#include <QApplication>
#include <QPushButton>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Compile-time too: the predicates are constexpr so a consumer can rely on them.
static_assert(!headphoneControlsAvailable(true, false),
              "HL2-shaped session: no headphone output");
static_assert(headphoneControlsAvailable(true, true),
              "a command plane keeps the radio's headphone mixer");
static_assert(headphoneControlsAvailable(false, false),
              "disconnected: nothing to be honest about");
static_assert(masterKnobDrivesLocalOutput(true, false, true),
              "HL2 with PC Audio on: the master knob is this computer's output");
static_assert(!masterKnobDrivesLocalOutput(true, true, true),
              "a command plane keeps the lineout wire text");

template <typename T>
static T* byAccessibleName(TitleBar& bar, const char* name)
{
    const auto widgets = bar.findChildren<T*>();
    for (T* w : widgets) {
        if (w->accessibleName() == QLatin1String(name))
            return w;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-mixer-control-availability-test"));
    QApplication app(argc, argv);

    // ── 1. The headphone predicate ──────────────────────────────────────────
    check(!headphoneControlsAvailable(true, false),
          "connected, no command plane -> headphone pair unavailable (HL2)");
    check(headphoneControlsAvailable(true, true),
          "connected WITH a command plane -> available (Flex keeps the pair)");
    check(headphoneControlsAvailable(false, false),
          "disconnected -> available, not dimmed");
    check(headphoneControlsAvailable(false, true),
          "disconnected with a stale command plane -> available");

    // ── 2. The master-knob predicate ────────────────────────────────────────
    check(masterKnobDrivesLocalOutput(true, false, true),
          "no command plane, PC Audio on -> master knob drives this computer's output");
    check(!masterKnobDrivesLocalOutput(true, true, true),
          "Flex with PC Audio on -> lineout wire text, as on main");
    check(!masterKnobDrivesLocalOutput(true, true, false),
          "Flex with PC Audio off -> lineout wire text");
    check(!masterKnobDrivesLocalOutput(true, false, false),
          "no command plane, PC Audio off -> no output here: refuse");
    check(!masterKnobDrivesLocalOutput(false, false, true),
          "disconnected -> not re-pointed");

    // ── 3. What the title bar does ──────────────────────────────────────────
    TitleBar bar;
    auto* mute = byAccessibleName<QPushButton>(bar, "Headphone mute");
    auto* slider = byAccessibleName<QSlider>(bar, "Headphone volume");
    check(mute && slider, "headphone mute and slider are discoverable by accessibleName");
    if (!mute || !slider)
        return 1;

    const QString radioMuteTip = mute->toolTip();
    const QString radioMuteDesc = mute->accessibleDescription();
    const QString radioSliderTip = slider->toolTip();
    const QString radioSliderDesc = slider->accessibleDescription();
    check(bar.headphoneAvailable(), "precondition: the pair starts available");
    check(mute->isEnabled() && slider->isEnabled(), "precondition: the pair starts live");

    int emitted = 0;
    QObject::connect(&bar, &TitleBar::headphoneMuteChanged, &bar, [&](bool) { ++emitted; });
    QObject::connect(&bar, &TitleBar::headphoneVolumeChanged, &bar, [&](int) { ++emitted; });

    bar.setHeadphoneVolume(37);
    bar.setHeadphoneMuted(true);
    emitted = 0;

    bar.setHeadphoneAvailable(false);
    check(!bar.headphoneAvailable(), "unavailable state is reported back");
    check(!mute->isEnabled(), "dim: the mute is disabled");
    check(!slider->isEnabled(), "dim: the slider is disabled");
    check(mute->accessibleDescription().startsWith(QLatin1String("Unavailable:")),
          "mute's accessibleDescription states it is unavailable");
    check(mute->accessibleDescription().contains(QLatin1String("no headphone output")),
          "mute's accessibleDescription gives the reason: no headphone output");
    check(mute->accessibleDescription().contains(QLatin1String("speaker button")),
          "mute's accessibleDescription names the speaker mute as this computer's mute");
    check(mute->toolTip() == mute->accessibleDescription(),
          "mute's tooltip and accessibleDescription say the same thing");
    check(slider->accessibleDescription().startsWith(QLatin1String("Unavailable:")),
          "slider's accessibleDescription states it is unavailable");
    check(slider->accessibleDescription().contains(QLatin1String("no headphone output")),
          "slider's accessibleDescription gives the reason: no headphone output");
    check(slider->accessibleDescription().contains(QLatin1String("master volume")),
          "slider's accessibleDescription names the master volume as this computer's level");
    check(slider->toolTip() == slider->accessibleDescription(),
          "slider's tooltip and accessibleDescription say the same thing");
    check(slider->value() == 37 && mute->isChecked(),
          "dimming leaves the slider value and the mute state alone");
    check(emitted == 0, "dimming emits no command");

    bar.setHeadphoneAvailable(true);
    check(bar.headphoneAvailable(), "available state is reported back");
    check(mute->isEnabled() && slider->isEnabled(), "available: the pair is live again");
    check(mute->toolTip() == radioMuteTip && mute->accessibleDescription() == radioMuteDesc,
          "available: the mute's constructor wording is restored exactly");
    check(slider->toolTip() == radioSliderTip
              && slider->accessibleDescription() == radioSliderDesc,
          "available: the slider's constructor wording is restored exactly");
    check(slider->value() == 37 && mute->isChecked(),
          "restoring leaves the slider value and the mute state alone (disconnect edge)");
    check(emitted == 0, "restoring emits no command either");

    // Re-asserting the current state is a no-op: a Flex's every capability
    // edge pushes `available`, and that must change nothing on the pair.
    bar.setHeadphoneAvailable(true);
    check(mute->accessibleDescription() == radioMuteDesc && emitted == 0,
          "re-asserting available changes nothing");

    std::printf("mixer_control_availability_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
