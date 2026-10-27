// Real controller failure signals; no radio, model download or inference.
#include "TestSettingsProfile.h"
#include "AetherBuildIdentity.h"
#include "asr/AsrCrashMarker.h"
#include "asr/AsrEngine.h"
#include "asr/AsrModelManager.h"
#include "core/AppSettings.h"
#include "gui/AsrAudioTap.h"
#include "gui/CopyAssistController.h"
#include "gui/CopyAssistPanel.h"
#include "gui/CopyAssistSettings.h"

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QString>
#include <array>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void expect(bool ok, const char* description)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
    failures += !ok;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-copy-assist-failure-test"));
    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral("test"));
    AppSettings::instance().load();
    // A recorded discovery fault keeps this fixture out of GPU discovery.
    AsrAttempt fault;
    fault.stage = QString::fromLatin1(kAsrStageDiscovery);
    fault.appVersion = QStringLiteral("test+") + QStringLiteral(AETHER_BUILD_SHA);
    CopyAssistSettings::setValue(QStringLiteral("AsrLastFault"), asrAttemptToJson(fault));
    CopyAssistPanel panel;
    CopyAssistController controller(nullptr, &panel);
    panel.show();
    app.processEvents();
    auto* status = panel.findChild<QLabel*>(QStringLiteral("CopyAssistStatus"));
    auto* busy = panel.findChild<QProgressBar*>();
    auto* models = controller.findChild<AsrModelManager*>(QString(), Qt::FindDirectChildrenOnly);
    auto* engine = controller.findChild<AsrEngine*>();
    auto* tap = controller.findChild<AsrAudioTap*>();
    QPushButton* enable = panel.enableButton();
    if (!status || !busy || !models || !engine || !tap || !enable) {
        expect(false, "controller fixture exposes its real UI and engine");
        return 1;
    }
    const QString standDownReason = status->text();
    expect(standDownReason.contains(QStringLiteral("off for this session")),
           "fixture starts with a genuine fault explanation");
    for (int attempt = 0; attempt < 2; ++attempt) {
        enable->click();
        app.processEvents();
        expect(!enable->isChecked() && !engine->isEnabled() && busy->isHidden(),
               "rejected enable stops loading and leaves decoding off");
        expect(status->text() == standDownReason,
               "repeated rejected enables retain the fault explanation");
    }
    const auto preparePendingEnable = [&] {
        // Set the pending UI/engine state without requesting a real model.
        const QSignalBlocker blocker(&panel);
        panel.setAsrEnabled(true);
        panel.setBusy(true);
        panel.setStatus(QStringLiteral("Preparing model…"));
        tap->setEnabled(true);
    };
    QSignalSpy toggles(&panel, &CopyAssistPanel::enableToggled);
    for (bool download : std::array{true, false}) {
        preparePendingEnable();
        toggles.clear();
        const QString reason = download ? QStringLiteral("Model download failed: fixture download failure")
                                        : QStringLiteral("Model load failed: fixture load failure");
        if (download) {
            emit models->failed(QStringLiteral("fixture download failure"));
        } else {
            emit engine->loadFailed(QStringLiteral("fixture load failure"));
        }
        app.processEvents();
        expect(status->text() == reason, "model failure explanation survives enable reset");
        expect(!enable->isChecked() && !tap->isEnabled() && !engine->isEnabled(),
               "model failure runs the real decoding shutdown");
        expect(busy->isHidden(), "model failure clears the busy indicator");
        expect(toggles.size() == 1 && !toggles.first().first().toBool(),
               "model failure preserves the disable signal");
    }
    panel.setBusy(true);
    emit models->failed(QStringLiteral("later failure"));
    app.processEvents();
    expect(status->text() == QStringLiteral("Model download failed: later failure")
               && busy->isHidden() && !enable->isChecked(),
           "a failure while already disabled still updates status and clears busy");
    preparePendingEnable();
    enable->click();
    app.processEvents();
    expect(status->text() == QStringLiteral("Disabled") && !engine->isEnabled()
               && busy->isHidden(), "manual disable retains its ordinary status and shutdown");
    return failures == 0 ? 0 : 1;
}
