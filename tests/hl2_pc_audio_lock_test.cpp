// PC-audio lock must never be mistaken for "RX audio started".
//
// setPcAudioLocked(true) is how a host-modulating backend (HL2) pins PC audio
// on: all of its audio, both directions, is produced and captured on this
// computer, so there is no radio-side path to fall back to.
//
// The trap this test pins down: the lock sets the button checked through
// setPcAudioEnabled(), which runs under a QSignalBlocker. pcAudioToggled() --
// the signal whose handler actually opens the RX sink -- therefore never
// fires. The lock then DISABLES the button, so neither the operator nor the
// automation bridge can click it to recover. A connect path that leans on the
// lock to start RX leaves the operator deaf with the button showing ON, and
// with no error to explain it: the sink reports Stopped / device_open=false /
// zero bytes / NoError.
//
// That is exactly what shipped until wireRadioModel() started RX imperatively
// alongside TX. These assertions keep the contract visible: the lock changes
// only appearance and clickability, never stream state. If a refactor ever
// makes the lock emit, the imperative audioStartRx() becomes a double-start,
// and this test is the place that says so.

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

// The button is private; the automation bridge finds it the same way.
static QPushButton* pcAudioButton(TitleBar& bar)
{
    const auto buttons = bar.findChildren<QPushButton*>();
    for (QPushButton* b : buttons) {
        if (b->accessibleName() == QLatin1String("PC Audio"))
            return b;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    TitleBar bar;
    QPushButton* pc = pcAudioButton(bar);
    check(pc != nullptr, "PC Audio button is discoverable by accessibleName");
    if (!pc) {
        std::fprintf(stderr, "cannot continue without the button\n");
        return 1;
    }

    int toggledCount = 0;
    QObject::connect(&bar, &TitleBar::pcAudioToggled,
                     &bar, [&toggledCount](bool) { ++toggledCount; });

    // Start from the deaf state the bug reproduced from: PC audio off.
    bar.setPcAudioEnabled(false);
    check(!pc->isChecked(), "precondition: button starts unchecked");
    check(pc->isEnabled(),  "precondition: button starts enabled");
    toggledCount = 0;

    // Lock, as a host-modulating backend does on connect.
    bar.setPcAudioLocked(true);

    check(pc->isChecked(),
          "locking shows PC audio as ON");
    check(!pc->isEnabled(),
          "locking disables the button (operator/bridge cannot click to recover)");
    check(toggledCount == 0,
          "locking emits NO pcAudioToggled -- the RX sink is NOT opened by the "
          "lock, so the connect path must start RX imperatively");

    // Unlocking restores operator control and likewise drives no stream change.
    toggledCount = 0;
    bar.setPcAudioLocked(false);
    check(pc->isEnabled(),
          "unlocking re-enables the button");
    check(toggledCount == 0,
          "unlocking emits no pcAudioToggled either");

    // A real click still drives the stream — the lock must not have broken the
    // normal path that opens and closes the sink.
    toggledCount = 0;
    pc->click();
    check(toggledCount == 1,
          "an operator click still emits pcAudioToggled (normal RX start/stop path intact)");

    if (g_failures == 0)
        std::printf("hl2_pc_audio_lock_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
