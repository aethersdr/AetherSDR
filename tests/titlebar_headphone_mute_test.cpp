// Title-bar headphone mute must be reconcilable from radio status (#4722).
//
// The truth path is radio -> RadioModel::applyDelta -> audioOutputChanged ->
// TitleBar::setHeadphoneMuted. Two details of that setter are load-bearing and
// invisible, which is why they get a test rather than a comment:
//
//   1. setHeadphoneMuted() runs under a QSignalBlocker, so the `toggled`
//      lambda in the constructor -- which is what normally swaps the mark --
//      does NOT run. The explicit repaint in the setter is therefore
//      required, not redundant: drop it and the button's checked state and its
//      mark disagree with each other.
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
#include <QImage>
#include <QPushButton>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// The mute mark used to be a 🔇 / 🎧 emoji set with setText(), and this test
// compared those bytes. The unified title bar replaced both with painter-drawn
// thin-line icons -- emoji render in the platform's own colour and weight and
// cannot follow the theme. The contract this test exists for is unchanged; the
// assertions now read the rendered icon instead of the label text, which is
// also stronger, since it survives the next change of mark.
static QImage markOf(QPushButton* b)
{
    return b->icon().pixmap(QSize(20, 20)).toImage();
}

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
    const QImage unmutedMark = markOf(hp);
    check(!unmutedMark.isNull(), "precondition: the headphone mark is drawn");

    // Command leg -- an operator click is still a request to the radio, and is
    // reported exactly once.
    hp->click();
    check(emitted == 1 && lastEmitted,
          "clicking the button emits headphoneMuteChanged(true) once");
    const QImage mutedMark = markOf(hp);
    check(mutedMark != unmutedMark, "clicking swaps the mark to muted");

    // Truth leg -- reconciling from radio status moves the control WITHOUT
    // emitting. If this ever emits, the radio's status echo becomes a fresh
    // command back to the radio.
    emitted = 0;
    bar.setHeadphoneMuted(false);
    check(emitted == 0,
          "setHeadphoneMuted() emits nothing -- reconcile is not a command");
    check(!hp->isChecked(), "setHeadphoneMuted(false) unchecks the button");
    check(markOf(hp) == unmutedMark,
          "setHeadphoneMuted(false) restores the headphone mark "
          "(the explicit repaint, since the blocker suppressed toggled)");

    bar.setHeadphoneMuted(true);
    check(emitted == 0, "setHeadphoneMuted(true) also emits nothing");
    check(hp->isChecked(), "setHeadphoneMuted(true) checks the button");
    check(markOf(hp) == mutedMark,
          "setHeadphoneMuted(true) shows the muted mark");

    // A repeated status echo -- Flex re-sends `audio` on every mixer change --
    // must not thrash the control.
    bar.setHeadphoneMuted(true);
    check(emitted == 0 && hp->isChecked() && markOf(hp) == mutedMark,
          "a repeated reconcile to the same value is a no-op");

    if (g_failures == 0)
        std::fprintf(stderr, "titlebar_headphone_mute_test: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
