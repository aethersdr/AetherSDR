// Standalone test harness for the AetherControl (FlexControlDialog) window
// never demanding more vertical space than the screen can show (issue #3662).
//
// The non-compact layout has a ~1066 px intrinsic minimum height. On a screen
// whose available (taskbar/menu-bar-excluded) height is smaller than that --
// e.g. a 1920x1080 panel after the Windows taskbar, or any DPI-scaled laptop --
// the old code pinned that intrinsic minimum as a hard floor, so the window
// opened taller than the screen with its bottom clipped and no way to shrink
// it. The fix auto-engages the existing compact layout (which fits) whenever
// the full layout would exceed the available screen height.
//
// Invariant under test: the dialog's enforced minimum height is never greater
// than the available height of the screen it lives on. On a screen too short
// for the full layout, compact mode must auto-engage.
//
// Build: CMake target `flex_control_dialog_size_test`. Exit 0 = pass.

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

// The non-compact stack (header 70 + knob panel 420 + control strip ~110 +
// status frame 148 + aux grid ~200 + title bar 18 + margins) floors around
// 1066 px. Any screen shorter than this exercises the auto-compact fallback.
constexpr int kFullLayoutFloor = 1000;

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
    QApplication app(argc, argv);
    std::printf("FlexControlDialog screen-fit test harness (#3662)\n\n");

    // Start from the non-compact layout so the auto-engage path is exercised.
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
    const bool engaged = compact && compact->isChecked();

    // Headline invariant: the window can never force a minimum height taller
    // than the screen offers. Pre-fix this is the intrinsic ~1066 px and fails
    // on any screen shorter than that.
    report("enforced minimum height fits the screen", minH <= availH,
           "minH=" + std::to_string(minH) + " availH=" + std::to_string(availH));

    // On a screen too short for the full layout, compact must auto-engage so
    // nothing is clipped.
    if (availH < kFullLayoutFloor) {
        report("compact auto-engaged on a short screen", engaged,
               "availH=" + std::to_string(availH) + " < floor "
                   + std::to_string(kFullLayoutFloor));
    } else {
        std::printf("[info] screen is tall enough (availH=%d >= %d); "
                    "non-compact layout retained, no auto-engage expected\n",
                    availH, kFullLayoutFloor);
    }

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
