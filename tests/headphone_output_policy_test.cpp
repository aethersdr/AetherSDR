// Where the headphone controls go on a radio with no headphone mixer of its
// own (HeadphoneOutputPolicy.h).
//
// On a Hermes-Lite 2 the title-bar headphone mute and slider, the MIDI
// Headphone/Master Volume knobs and the WheelHeadphoneVolume controller action
// all sent `mixer headphone ...` wire text that RadioModel::sendCmd drops for
// want of a command plane: the control moved and the audio did not. Their
// audio plays on this computer, so they now drive this computer's output.
//
// Two things are pinned here:
//   1. the predicate: follow the local output ONLY when connected, with no
//      command plane, and with PC Audio on. A Flex (command plane) keeps its
//      wire text; a radio playing through its own speaker (PC Audio off) has
//      no output here to follow; nothing is re-pointed while disconnected.
//   2. what the title bar SAYS: when the pair follows the local output, both
//      the tooltip and the accessibleDescription must say so -- a screen
//      reader otherwise announces "headphone audio" for a jack that does not
//      exist -- and switching back must restore the radio wording.
//
// MainWindow's routing (applyHeadphoneVolume/applyHeadphoneMute) consumes (1)
// directly; this is its truth table, not a copy of it.
//
// Not yet mutation-checked. The mutation to run before a PR: dropping
// `!hasCommandPlane` from the predicate fails the Flex row; dropping the
// `follows` branch in setHeadphoneFollowsLocalOutput() fails the description
// rows.

#include "TestSettingsProfile.h"

#include "gui/HeadphoneOutputPolicy.h"
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

// Compile-time too: the predicate is constexpr so a consumer can rely on it.
static_assert(headphoneFollowsLocalOutput(true, false, true),
              "HL2-shaped session follows the local output");
static_assert(!headphoneFollowsLocalOutput(true, true, true),
              "a command plane keeps the radio's mixer");

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
        QStringLiteral("aether-headphone-output-policy-test"));
    QApplication app(argc, argv);

    // ── 1. The predicate ────────────────────────────────────────────────────
    check(headphoneFollowsLocalOutput(true, false, true),
          "connected, no command plane, PC Audio on -> follows the local output (HL2)");
    check(!headphoneFollowsLocalOutput(true, true, true),
          "connected WITH a command plane -> radio mixer (Flex keeps its wire text)");
    check(!headphoneFollowsLocalOutput(true, true, false),
          "Flex with PC Audio off -> still the radio mixer");
    check(!headphoneFollowsLocalOutput(true, false, false),
          "no command plane but PC Audio off -> nothing here to follow");
    check(!headphoneFollowsLocalOutput(false, false, true),
          "disconnected -> not re-pointed");
    check(!headphoneFollowsLocalOutput(false, true, true),
          "disconnected with a stale command plane -> not re-pointed");

    // ── 2. What the title bar says ─────────────────────────────────────────
    TitleBar bar;
    auto* mute = byAccessibleName<QPushButton>(bar, "Headphone mute");
    auto* slider = byAccessibleName<QSlider>(bar, "Headphone volume");
    check(mute && slider, "headphone mute and slider are discoverable by accessibleName");
    if (!mute || !slider)
        return 1;

    const QString radioMute = mute->accessibleDescription();
    const QString radioSlider = slider->accessibleDescription();
    check(!bar.headphoneFollowsLocalOutput(), "precondition: starts on the radio mixer");

    int emitted = 0;
    QObject::connect(&bar, &TitleBar::headphoneMuteChanged, &bar, [&](bool) { ++emitted; });
    QObject::connect(&bar, &TitleBar::headphoneVolumeChanged, &bar, [&](int) { ++emitted; });

    bar.setHeadphoneFollowsLocalOutput(true);
    check(bar.headphoneFollowsLocalOutput(), "follows state is reported back");
    check(mute->accessibleDescription().contains(QLatin1String("this computer")),
          "mute's accessibleDescription names this computer's output");
    check(mute->toolTip() == mute->accessibleDescription(),
          "mute's tooltip and accessibleDescription say the same thing");
    check(slider->accessibleDescription().contains(QLatin1String("this computer")),
          "slider's accessibleDescription names this computer's output");
    check(slider->accessibleDescription().contains(QLatin1String("master volume")),
          "slider's description says it is the same level as the master volume");
    check(mute->isEnabled() && slider->isEnabled(),
          "the pair stays live -- it works, it is not dimmed");
    check(emitted == 0, "re-describing the controls emits no command");

    bar.setHeadphoneFollowsLocalOutput(false);
    check(mute->accessibleDescription() == radioMute,
          "back on a radio mixer, the mute's radio wording is restored");
    check(slider->accessibleDescription() == radioSlider,
          "back on a radio mixer, the slider's radio wording is restored");
    check(emitted == 0, "restoring the wording emits no command either");

    // The pair is driven from the model side with signals blocked: a mirror
    // from the master slider must never re-enter as a headphone command.
    bar.setHeadphoneFollowsLocalOutput(true);
    bar.setHeadphoneVolume(37);
    bar.setHeadphoneMuted(true);
    check(slider->value() == 37 && mute->isChecked(),
          "mirrored level and mute land on the pair");
    check(emitted == 0, "mirroring the shared output onto the pair emits nothing");

    std::printf("headphone_output_policy_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
