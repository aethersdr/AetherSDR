// Headless tests for SwrSweepLicenseDialog::confirm().
// Covers the remembered-answer short-circuit and when the answer is stored.
// Build: CMake target `swr_sweep_license_dialog_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/SwrSweepLicenseDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <string>

using namespace AetherSDR;

namespace {

constexpr const char* kSettingsKey = "SwrSweepLicenseConfirmed";

int g_failed = 0;

struct Dismissal {
    bool fired = false;
    bool shown = false;
};

// Schedule a one-shot that finds the modal SwrSweepLicenseDialog once exec()
// has shown it and closes it. A dialog that should not have opened therefore
// fails its case instead of blocking the run.
std::shared_ptr<Dismissal> dismissNextDialog(bool accept, bool tickRemember)
{
    auto dismissal = std::make_shared<Dismissal>();
    QTimer::singleShot(0, [=] {
        dismissal->fired = true;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* dlg = qobject_cast<SwrSweepLicenseDialog*>(w);
            if (!dlg || !dlg->isVisible())
                continue;
            dismissal->shown = true;
            if (tickRemember)
                if (auto* cb = dlg->findChild<QCheckBox*>())
                    cb->setChecked(true);
            accept ? dlg->accept() : dlg->reject();
            return;
        }
    });
    return dismissal;
}

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// Runs the one-shot if confirm() returned without entering exec(), so it cannot
// close the next case's dialog.
void drain(const std::shared_ptr<Dismissal>& dismissal)
{
    QDeadlineTimer deadline(1000);
    while (!dismissal->fired && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void clearRememberedAnswer()
{
    auto& s = AppSettings::instance();
    s.remove(kSettingsKey);
    s.save();
}

bool answerIsRemembered()
{
    return AppSettings::instance().value(kSettingsKey, "False").toString() == "True";
}

void testRememberedAnswerSkipsDialog()
{
    auto& s = AppSettings::instance();
    s.setValue(kSettingsKey, "True");
    s.save();

    auto dismissal = dismissNextDialog(/*accept=*/false, /*tick=*/false);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr);
    drain(dismissal);
    report("remembered answer skips dialog and returns true", result && !dismissal->shown);

    clearRememberedAnswer();
}

void testNoRememberedAnswerShowsDialog()
{
    clearRememberedAnswer();

    auto dismissal = dismissNextDialog(/*accept=*/false, /*tick=*/false);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr);
    drain(dismissal);
    report("no remembered answer shows dialog; cancel returns false", dismissal->shown && !result);
}

void testAcceptWithRememberPersists()
{
    clearRememberedAnswer();

    auto dismissal = dismissNextDialog(/*accept=*/true, /*tick=*/true);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr);
    drain(dismissal);
    report("accept with remember ticked persists the answer",
           dismissal->shown && result && answerIsRemembered());

    clearRememberedAnswer();
}

void testAcceptWithoutRememberDoesNotPersist()
{
    clearRememberedAnswer();

    auto dismissal = dismissNextDialog(/*accept=*/true, /*tick=*/false);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr);
    drain(dismissal);
    report("accept without remember stores nothing",
           dismissal->shown && result && !answerIsRemembered());
}

void testCancelWithRememberDoesNotPersist()
{
    clearRememberedAnswer();

    auto dismissal = dismissNextDialog(/*accept=*/false, /*tick=*/true);
    const bool result = SwrSweepLicenseDialog::confirm(nullptr);
    drain(dismissal);
    report("cancel with remember ticked stores nothing",
           dismissal->shown && !result && !answerIsRemembered());
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
    testNoRememberedAnswerShowsDialog();
    testAcceptWithRememberPersists();
    testAcceptWithoutRememberDoesNotPersist();
    testCancelWithRememberDoesNotPersist();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

