// Standalone test harness for WhatsNewDialog release-notes targeting.
//
// Regression: Help -> What's New showed "GitHub returned HTTP 404" because it
// asked GitHub for the release tagged for this exact build
// (releases/tags/v<currentVersion>), which frequently does not exist (dev
// builds, point releases, release lag).  The Help entry point now asks for the
// repo's newest published release instead; the first-run upgrade path still
// targets the specific version.
//
// Build: CMake target `whats_new_dialog_test`. Exit 0 = pass.
//
// The dialog kicks off a live QNetworkAccessManager request in its
// constructor, but this test never spins the event loop, so the reply's
// finished handler never runs and no network is required.  The status label
// is written synchronously from the constructor and is the observable proxy
// for which GitHub endpoint was chosen.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/WhatsNewDialog.h"

#include <QApplication>
#include <QLabel>
#include <QString>

#include <cstdio>
#include <memory>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

QString statusText(const WhatsNewDialog& dialog)
{
    auto* label = dialog.findChild<QLabel*>("whatsNewStatusLabel");
    return label ? label->text() : QString();
}

// Help -> What's New: must target the newest published release, never a
// build-specific tag that would 404.
void testHelpMenuTargetsLatestRelease()
{
    std::unique_ptr<WhatsNewDialog> dialog(WhatsNewDialog::showAll(nullptr));
    const QString status = statusText(*dialog);

    report("help menu: status label is present", !status.isEmpty(), status.toStdString());
    report("help menu: notes are described as 'latest'",
           status.contains("latest", Qt::CaseInsensitive), status.toStdString());
    report("help menu: status does not pin a build-specific tag",
           !status.contains(QCoreApplication::applicationVersion()),
           status.toStdString());
}

// First-run upgrade path: still targets the specific version the user moved to.
void testUpgradePathTargetsSpecificVersion()
{
    std::unique_ptr<WhatsNewDialog> dialog(
        new WhatsNewDialog(QStringLiteral("26.9.0"), QStringLiteral("26.9.5"),
                           nullptr, /*showUpgrade=*/false,
                           /*currentVersionOnly=*/false));
    const QString status = statusText(*dialog);

    report("upgrade path: status names the target version",
           status.contains(QStringLiteral("26.9.5")), status.toStdString());
    report("upgrade path: status is not the 'latest' phrasing",
           !status.contains("latest", Qt::CaseInsensitive), status.toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-whats-new-dialog-test"));
    if (!settingsProfile.isValid())
        return 1;

    QApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("26.9.5"));
    AppSettings::instance().load();

    std::printf("WhatsNewDialog release-notes targeting test harness\n\n");

    testHelpMenuTargetsLatestRelease();
    testUpgradePathTargetsSpecificVersion();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
