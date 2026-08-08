// Title-bar headphone mute must be reconcilable from radio status (#4722).
//
// The truth path is radio -> RadioModel::applyDelta -> audioOutputChanged ->
// TitleBar::setHeadphoneMuted. Two details of that setter are load-bearing and
// invisible, which is why they get a test rather than a comment:
//
//   1. setHeadphoneMuted() runs under a QSignalBlocker, so the `toggled`
//      lambda in the constructor -- which is what normally swaps the glyph --
//      does NOT run. The explicit setText() in the setter is therefore
//      required, not redundant: drop it and the button's checked state and its
//      glyph disagree with each other.
//   2. That same blocker is what stops the reconcile from re-emitting
//      headphoneMuteChanged and echoing the radio's own status back at it as a
//      fresh command -- the Constitution Principle II feedback loop ("the
//      command path runs client -> radio; the truth path runs radio ->
//      client; the two must never form a feedback loop").
//
// Neither failure surfaces on a single client. Both show up the moment a
// second Multi-Flex client, Radio Setup, or the rig's own front panel changes
// headphone mute -- exactly the case #4722 was filed for.

#include "TestSettingsProfile.h"

#include "gui/TitleBar.h"

#include <QApplication>
#include <QPushButton>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static const char* kMuted   = "\xF0\x9F\x94\x87";  // 🔇
static const char* kUnmuted = "\xF0\x9F\x8E\xA7";  // 🎧

// The button is private; the automation bridge finds it the same way.
static QPushButton* headphoneButton(TitleBar& bar)
{
    const auto buttons = bar.findChildren<QPushButton*>();
    for (QPushButton* b : buttons) {
        if (b->accessibleName() == QLatin1String("Headphone mute"))
            return b;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    // TitleBar pulls ThemeManager, which reads AppSettings. Sandbox the store
    // before QApplication so the test never touches the operator's profile.
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-titlebar-headphone-mute-test"));

    QApplication app(argc, argv);

    TitleBar bar;
    QPushButton* hp = headphoneButton(bar);
    check(hp != nullptr, "headphone button is discoverable by accessibleName");
    if (!hp) {
        std::fprintf(stderr, "cannot continue without the button\n");
        return 1;
    }

    int emitted = 0;
    bool lastEmitted = false;
    QObject::connect(&bar, &TitleBar::headphoneMuteChanged, &bar,
                     [&](bool m) { ++emitted; lastEmitted = m; });

    check(!hp->isChecked(), "precondition: starts unmuted");
    check(hp->text() == QString::fromUtf8(kUnmuted),
          "precondition: starts on the headphone glyph");

    // Command leg -- an operator click is still a request to the radio, and is
    // reported exactly once.
    hp->click();
    check(emitted == 1 && lastEmitted,
          "clicking the button emits headphoneMuteChanged(true) once");
    check(hp->text() == QString::fromUtf8(kMuted),
          "clicking swaps the glyph to muted");

    // Truth leg -- reconciling from radio status moves the control WITHOUT
    // emitting. If this ever emits, the radio's status echo becomes a fresh
    // command back to the radio.
    emitted = 0;
    bar.setHeadphoneMuted(false);
    check(emitted == 0,
          "setHeadphoneMuted() emits nothing -- reconcile is not a command");
    check(!hp->isChecked(), "setHeadphoneMuted(false) unchecks the button");
    check(hp->text() == QString::fromUtf8(kUnmuted),
          "setHeadphoneMuted(false) restores the headphone glyph "
          "(the explicit setText, since the blocker suppressed toggled)");

    bar.setHeadphoneMuted(true);
    check(emitted == 0, "setHeadphoneMuted(true) also emits nothing");
    check(hp->isChecked(), "setHeadphoneMuted(true) checks the button");
    check(hp->text() == QString::fromUtf8(kMuted),
          "setHeadphoneMuted(true) shows the muted glyph");

    // A repeated status echo -- Flex re-sends `audio` on every mixer change --
    // must not thrash the control.
    bar.setHeadphoneMuted(true);
    check(emitted == 0 && hp->isChecked() &&
          hp->text() == QString::fromUtf8(kMuted),
          "a repeated reconcile to the same value is a no-op");

    if (g_failures == 0)
        std::fprintf(stderr, "titlebar_headphone_mute_test: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
