// Standalone test harness for the AetherControl (FlexControlDialog) window
// never demanding more vertical space than the screen can show (issue #3662).
//
// The non-compact layout has a ~1066 px intrinsic minimum height. On a screen
// whose available (taskbar/menu-bar-excluded) height is smaller than that --
// e.g. a 1920x1080 panel after the Windows taskbar, or any DPI-scaled laptop --
// the old code pinned that intrinsic minimum as a hard floor, so the window
// opened taller than the screen with its bottom clipped and no way to shrink
// it. A later fix auto-engaged the existing compact layout, but that made the
// Compact toggle impossible to turn off on those screens. The content now
// scrolls instead, preserving both screen fit and the user's mode choice.
//
// Invariant under test: the dialog's enforced minimum height is never greater
// than the available height of the screen it lives on, and Compact must remain
// a user-controlled toggle on every screen size.
//
// Build: CMake target `flex_control_dialog_size_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "gui/FlexControlDialog.h"

#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <cstdio>
#include <string>

#include "core/AppSettings.h"

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

QPushButton* findCompactButton(FlexControlDialog& dialog)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    for (QPushButton* b : buttons) {
        if (b->text() == QStringLiteral("Compact"))
            return b;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-flex-control-dialog-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("FlexControlDialog screen-fit test harness (#3662)\n\n");

    // Start from the non-compact layout. Screen fit must not change this user
    // preference behind their back.
    AppSettings::instance().setValue("FlexControlCompactMode", "False");

    const QScreen* scr = QApplication::primaryScreen();
    const int availH = scr ? scr->availableGeometry().height() : 0;
    report("primary screen reports an available height", availH > 0,
           "availH=" + std::to_string(availH));
    if (availH <= 0)
        return 1;

    FlexControlDialog dialog;

    QPushButton* compact = findCompactButton(dialog);
    report("compact toggle is present", compact != nullptr);

    const int minH = dialog.minimumHeight();

    // Headline invariant: the window can never force a minimum height taller
    // than the screen offers. Pre-fix this is the intrinsic ~1066 px and fails
    // on any screen shorter than that.
    report("enforced minimum height fits the screen", minH <= availH,
           "minH=" + std::to_string(minH) + " availH=" + std::to_string(availH));

    report("saved non-compact preference is preserved",
           compact && !compact->isChecked());

    if (compact) {
        compact->click();
        report("compact mode can be enabled", compact->isChecked());
        compact->click();
        report("compact mode can be disabled again", !compact->isChecked());
    }

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
