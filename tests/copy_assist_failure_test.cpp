// Real controller failure signals; no radio, model download or inference.
#include "TestSettingsProfile.h"
#include "AetherBuildIdentity.h"
#include "asr/AsrCrashMarker.h"
#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/LivePreview.h"
#include "asr/AsrModelManager.h"
#include "core/AppSettings.h"
#include "gui/AsrAudioTap.h"
#include "gui/CopyAssistController.h"
#include "gui/CopyAssistPanel.h"
#include "gui/CopyAssistSettings.h"
#include "gui/CopyAssistSettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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

    // The "None" model: no backend, the live preview is the transcript. Last,
    // because switching models rebuilds the engine the pointers above name.
    auto* dialog = panel.findChild<CopyAssistSettingsDialog*>();
    auto* modelCombo =
        dialog ? dialog->findChild<QComboBox*>(QStringLiteral("CopyAssistModelCombo")) : nullptr;
    auto* previewBox =
        dialog ? dialog->findChild<QCheckBox*>(QStringLiteral("CopyAssistLivePreview")) : nullptr;
    const bool previewPossible = sherpaLivePreviewSupported() || appleLivePreviewSupported();
    const int noneIndex = modelCombo ? modelCombo->findData(QStringLiteral("none")) : -1;
    expect(modelCombo && previewBox && (noneIndex >= 0) == previewPossible,
           "the model list offers None exactly where a preview recognizer can run");
    auto* speakerBox =
        dialog ? dialog->findChild<QCheckBox*>(QStringLiteral("CopyAssistLabelSpeakers")) : nullptr;
    // Blocked so the controller never hears the toggle: it would fetch the
    // speaker model. The box itself still updates.
    const auto setSpeakerBoxQuietly = [&](bool on) {
        const QSignalBlocker quiet(dialog);
        dialog->setLabelSpeakers(on);
    };
    if (noneIndex >= 0 && speakerBox) {
        CopyAssistSettings::setValue(QStringLiteral("AsrLivePreview"), QStringLiteral("False"));
        dialog->setLivePreviewChecked(false);
        setSpeakerBoxQuietly(true);
        modelCombo->setCurrentIndex(noneIndex);
        app.processEvents();
        auto* noneEngine = controller.findChild<AsrEngine*>();
        // The recognizer itself starts on Enable, once its model is verified.
        expect(noneEngine && noneEngine->livePreviewTranscribes(),
               "None builds an engine whose preview is the transcript");
        expect(previewBox->isChecked() && !previewBox->isEnabled()
                   && !previewBox->accessibleDescription().isEmpty(),
               "None shows the preview setting on and locked, with the reason");
        expect(noneEngine && !noneEngine->isSpeakerLabelingEnabled() && speakerBox->isChecked()
                   && !speakerBox->isEnabled() && !speakerBox->accessibleDescription().isEmpty(),
               "None labels no speakers, greying the box with the reason but keeping its check");
        setSpeakerBoxQuietly(false); // so the next engine has no speaker model to load
        modelCombo->setCurrentIndex(modelCombo->findData(AsrModelCatalog::defaultTierId()));
        app.processEvents();
        auto* whisperEngine = controller.findChild<AsrEngine*>();
        expect(whisperEngine && !whisperEngine->livePreviewTranscribes()
                   && !whisperEngine->isLivePreviewEnabled() && !previewBox->isChecked()
                   && previewBox->isEnabled(),
               "a model again gives back the operator's preview choice (off here)");
        expect(speakerBox->isEnabled() && speakerBox->accessibleDescription().isEmpty(),
               "a model again makes speaker labels available");
    }
    return failures == 0 ? 0 : 1;
}
