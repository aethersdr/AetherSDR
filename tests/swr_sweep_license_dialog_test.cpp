// Headless tests for SwrSweepLicenseDialog::confirm().
// Covers the remembered-answer short-circuit and the force-override path.
// Build: CMake target `swr_sweep_license_dialog_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/SwrSweepLicenseDialog.h"

#include <QApplication>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

constexpr const char* kSettingsKey = "SwrSweepLicenseConfirmed";

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// confirm(nullptr, false) returns true without opening a dialog when the
// operator has previously ticked "Remember my answer" and accepted.
void testRememberedAnswerSkipsDialog()
{
    auto& s = AppSettings::instance();
    s.setValue(kSettingsKey, "True");
    s.save();

    // If confirm() tried to show a modal it would block forever in an
    // offscreen harness — returning true here proves it short-circuited.
    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/false);
    report("remembered answer skips dialog and returns true", result);

    s.remove(kSettingsKey);
    s.save();
}

// confirm(nullptr, false) without a saved answer must NOT return true
// immediately — it would attempt to show the dialog. In an offscreen harness
// that exec() returns QDialog::Rejected (no user to click), so confirm()
// returns false. This proves the short-circuit only fires when the key is set.
void testNoRememberedAnswerDoesNotSkip()
{
    auto& s = AppSettings::instance();
    s.remove(kSettingsKey);
    s.save();

    // exec() in offscreen mode returns Rejected — confirm returns false.
    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/false);
    report("no remembered answer does not auto-confirm", !result);
}

// confirm(nullptr, true) must NOT short-circuit even when the key is set.
// In an offscreen harness exec() returns Rejected, so confirm() returns false.
// A true return here would mean force was ignored, which is a regression.
void testForceOverridesRememberedAnswer()
{
    auto& s = AppSettings::instance();
    s.setValue(kSettingsKey, "True");
    s.save();

    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/true);
    // exec() → Rejected in offscreen → false expected.
    report("force=true overrides remembered answer and shows dialog", !result);

    s.remove(kSettingsKey);
    s.save();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-swr-sweep-license-dialog-test"));
    if (!settingsProfile.isValid())
        return 1;

    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("SwrSweepLicenseDialog::confirm() test harness\n\n");

    testRememberedAnswerSkipsDialog();
    testNoRememberedAnswerDoesNotSkip();
    testForceOverridesRememberedAnswer();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

#include "swr_sweep_license_dialog_test.moc"
