// Headless tests for SwrSweepLicenseDialog::confirm().
// Covers the remembered-answer short-circuit and the force-override path.
// Build: CMake target `swr_sweep_license_dialog_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/SwrSweepLicenseDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <string>

using namespace AetherSDR;

namespace {

constexpr const char* kSettingsKey = "SwrSweepLicenseConfirmed";

int g_failed = 0;

// Schedule a one-shot that finds the modal SwrSweepLicenseDialog once exec()
// has shown it and closes it. Returns a flag recording whether it appeared.
std::shared_ptr<bool> dismissNextDialog(bool accept, bool tickRemember)
{
    auto shown = std::make_shared<bool>(false);
    QTimer::singleShot(0, [=] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* dlg = qobject_cast<SwrSweepLicenseDialog*>(w);
            if (!dlg || !dlg->isVisible())
                continue;
            *shown = true;
            if (tickRemember)
                if (auto* cb = dlg->findChild<QCheckBox*>())
                    cb->setChecked(true);
            accept ? dlg->accept() : dlg->reject();
            return;
        }
    });
    return shown;
}

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
// immediately — it must show the dialog. The timer dismisses it and records
// that it appeared; cancelling returns false.
void testNoRememberedAnswerDoesNotSkip()
{
    auto& s = AppSettings::instance();
    s.remove(kSettingsKey);
    s.save();

    auto shown = dismissNextDialog(/*accept=*/false, /*tick=*/false);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/false);
    report("no remembered answer shows dialog; cancel returns false", *shown && !result);
}

// confirm(nullptr, true) must NOT short-circuit even when the key is set.
// The timer dismisses the dialog and records that it appeared; cancelling
// returns false. A true return here would mean force was ignored.
void testForceOverridesRememberedAnswer()
{
    auto& s = AppSettings::instance();
    s.setValue(kSettingsKey, "True");
    s.save();

    auto shown = dismissNextDialog(/*accept=*/false, /*tick=*/false);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/true);
    report("force=true overrides remembered answer and shows dialog", *shown && !result);

    s.remove(kSettingsKey);
    s.save();
}

// Accepting with "Remember my answer" ticked persists the key, so the next
// non-forced confirm() short-circuits.
void testAcceptWithRememberPersists()
{
    auto& s = AppSettings::instance();
    s.remove(kSettingsKey);
    s.save();

    auto shown = dismissNextDialog(/*accept=*/true, /*tick=*/true);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr, /*force=*/false);
    const bool persisted = s.value(kSettingsKey, "False").toString() == "True";
    report("accept with remember ticked persists the answer",
           *shown && result && persisted);

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
    testAcceptWithRememberPersists();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

