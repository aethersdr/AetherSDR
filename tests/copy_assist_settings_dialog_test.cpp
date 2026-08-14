// Offline UI test for CopyAssistSettingsDialog (RFC #4333). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies frameless-window behavior plus the
// selectors that moved out of CopyAssistPanel into the modeless settings dialog.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/CopyAssistSettingsDialog.h"
#include "gui/FramelessWindowTitleBar.h"

#include "asr/WhisperAsrBackend.h" // asrLanguageOrDefault (header-inline, whisper-free)
#include "gui/CopyAssistSettings.h" // foldLegacyKeys + value/setValue

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QSignalSpy>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-copy-assist-settings-test"));
    QApplication app(argc, argv);

    AppSettings::instance().load();

    // ---- CopyAssistSettings migration: flat Asr* keys -> nested "CopyAssist" ---
    // Exercises the stateful path (ensureMigrated + value/setValue round-trip
    // through AppSettings), complementing the pure foldLegacyKeys checks below.
    // Runs first so it is the first accessor call (the one-time migration).
    {
        auto& s = AppSettings::instance();
        // Seed a couple of legacy flat keys as an un-migrated profile would have.
        s.setValue(QStringLiteral("AsrLanguage"), QStringLiteral("es"));
        s.setValue(QStringLiteral("AsrPanelHeight"), QStringLiteral("240"));

        expect(CopyAssistSettings::value(QStringLiteral("AsrLanguage"),
                                         QStringLiteral("en")).toString()
                   == QStringLiteral("es"),
               "value() returns the migrated legacy value");
        expect(!s.contains(QStringLiteral("AsrLanguage")),
               "migration removes the flat AsrLanguage key");
        expect(!s.contains(QStringLiteral("AsrPanelHeight")),
               "migration folds every present flat key, not just the one read");

        const QJsonObject nested = QJsonDocument::fromJson(
            s.value(CopyAssistSettings::rootKey()).toString().toUtf8()).object();
        expect(nested.value(QStringLiteral("AsrLanguage")).toString()
                   == QStringLiteral("es"),
               "nested CopyAssist object holds the folded language");
        expect(nested.value(QStringLiteral("AsrPanelHeight")).toString()
                   == QStringLiteral("240"),
               "nested CopyAssist object holds every folded field");

        CopyAssistSettings::setValue(QStringLiteral("AsrLanguage"),
                                     QStringLiteral("fr"));
        expect(CopyAssistSettings::value(QStringLiteral("AsrLanguage")).toString()
                   == QStringLiteral("fr"),
               "setValue then value round-trips through the nested object");
    }

    // Frameless-window behavior (from #4414) shares this offscreen harness.
    AppSettings::instance().setValue(QStringLiteral("FramelessWindow"),
                                     QStringLiteral("True"));
    CopyAssistSettingsDialog dlg;
    dlg.show();
    app.processEvents();

    auto* titleBar = dlg.findChild<FramelessWindowTitleBar*>();
    expect((dlg.windowFlags() & Qt::FramelessWindowHint) != 0,
           "saved frameless setting applies on construction");
    expect(titleBar != nullptr && titleBar->isVisible(),
           "frameless title bar is visible");
    // The dialog is a modeless tool window (out of the taskbar, floats above the
    // app) — must survive the frameless toggle, not just the initial build (#4414).
    expect(dlg.windowType() == Qt::Tool,
           "settings dialog is a tool window on construction");

    dlg.setFramelessMode(false);
    app.processEvents();
    expect((dlg.windowFlags() & Qt::FramelessWindowHint) == 0,
           "runtime toggle restores native window chrome");
    expect(titleBar != nullptr && !titleBar->isVisible(),
           "frameless title bar hides in native mode");
    expect(dlg.windowType() == Qt::Tool,
           "tool window type is preserved in native mode");

    dlg.setFramelessMode(true);
    app.processEvents();
    expect((dlg.windowFlags() & Qt::FramelessWindowHint) != 0,
           "runtime toggle restores frameless window chrome");
    expect(titleBar != nullptr && titleBar->isVisible(),
           "frameless title bar returns in frameless mode");

    // ---- Tier selection emits the tier id ---------------------------------
    dlg.addTier(QStringLiteral("base"), QStringLiteral("Base"));
    dlg.addTier(QStringLiteral("small"), QStringLiteral("Small"));
    QSignalSpy tierSpy(&dlg, &CopyAssistSettingsDialog::tierChanged);
    dlg.setCurrentTier(QStringLiteral("small"));
    expect(dlg.currentTier() == QStringLiteral("small"), "setCurrentTier selects the tier");
    expect(!tierSpy.isEmpty() && tierSpy.last().at(0).toString() == QStringLiteral("small"),
           "tierChanged carries the tier id");

    // ---- Relabel a tier in place (used by the "Custom model…" flow) --------
    dlg.setTierLabel(QStringLiteral("base"), QStringLiteral("Custom: my.bin"));
    {
        auto* combo = dlg.findChild<QComboBox*>(QStringLiteral("CopyAssistModelCombo"));
        const int idx = combo != nullptr ? combo->findData(QStringLiteral("base")) : -1;
        expect(idx >= 0 && combo->itemText(idx) == QStringLiteral("Custom: my.bin"),
               "setTierLabel renames the entry, keeps its id");
    }

    // ---- Compute-device selector: pending state, discovery, selection -----
    {
        auto* gpuCombo = dlg.findChild<QComboBox*>(QStringLiteral("CopyAssistGpuCombo"));
        expect(gpuCombo != nullptr && gpuCombo->isVisibleTo(&dlg)
                   && !gpuCombo->isEnabled()
                   && gpuCombo->currentText() == QStringLiteral("Detecting…"),
               "GPU combo shows a disabled discovery placeholder");
        expect(dlg.currentGpu() == CopyAssistSettingsDialog::kGpuDiscoveryPending,
               "discovery placeholder has a distinct non-device sentinel");

        dlg.clearGpuDevices();
        dlg.addGpuDevice(0, QStringLiteral("GPU0"));
        dlg.addGpuDevice(-1, QStringLiteral("CPU"));
        dlg.setGpuSelectorVisible(true);
        dlg.setGpuSelectorEnabled(true);

        QSignalSpy gpuSpy(&dlg, &CopyAssistSettingsDialog::gpuChanged);
        dlg.setCurrentGpu(-1);
        expect(gpuCombo->isEnabled(), "GPU combo enables when discovery completes");
        expect(dlg.currentGpu() == -1, "setCurrentGpu selects the CPU sentinel");
        expect(!gpuSpy.isEmpty() && gpuSpy.last().at(0).toInt() == -1,
               "gpuChanged carries the device index");

        dlg.clearGpuDevices();
        dlg.setGpuSelectorVisible(false);
        expect(gpuCombo->count() == 0 && !gpuCombo->isVisibleTo(&dlg),
               "CPU-only resolution clears and hides the discovery placeholder");
    }

    // ---- Language selector: round-trip, signal, paired visibility ---------
    {
        dlg.addLanguage(QStringLiteral("en"), QStringLiteral("English"));
        dlg.addLanguage(QStringLiteral("es"), QStringLiteral("Spanish"));

        QSignalSpy langSpy(&dlg, &CopyAssistSettingsDialog::languageChanged);
        dlg.setCurrentLanguage(QStringLiteral("es"));
        expect(dlg.currentLanguage() == QStringLiteral("es"),
               "setCurrentLanguage selects by code, currentLanguage round-trips");
        expect(!langSpy.isEmpty() && langSpy.last().at(0).toString() == QStringLiteral("es"),
               "languageChanged carries the language code");

        // Unknown code is a no-op (the controller coerces to a supported code
        // before calling this), so the selection stays put.
        dlg.setCurrentLanguage(QStringLiteral("zz-nonesuch"));
        expect(dlg.currentLanguage() == QStringLiteral("es"),
               "setCurrentLanguage ignores an unsupported code");

        // The label + combo hide/show together (sherpa-onnx path hides the row).
        auto* langCombo = dlg.findChild<QComboBox*>(QStringLiteral("CopyAssistLanguageCombo"));
        auto* langLabel = dlg.findChild<QLabel*>(QStringLiteral("CopyAssistLanguageLabel"));
        dlg.setLanguageSelectorVisible(false);
        expect(langCombo != nullptr && !langCombo->isVisibleTo(&dlg)
                   && langLabel != nullptr && !langLabel->isVisibleTo(&dlg),
               "setLanguageSelectorVisible(false) hides label + combo together");
        dlg.setLanguageSelectorVisible(true);
        expect(langCombo != nullptr && langCombo->isVisibleTo(&dlg)
                   && langLabel != nullptr && langLabel->isVisibleTo(&dlg),
               "setLanguageSelectorVisible(true) shows label + combo together");
    }

    // ---- asrLanguageOrDefault: validate/migrate a saved language code -----
    {
        const std::vector<AsrLanguage> supported = {
            {QStringLiteral("en"), QStringLiteral("English")},
            {QStringLiteral("es"), QStringLiteral("Spanish")},
            {QStringLiteral("fr"), QStringLiteral("French")},
        };
        expect(asrLanguageOrDefault(QStringLiteral("fr"), supported) == QStringLiteral("fr"),
               "asrLanguageOrDefault keeps a supported code");
        expect(asrLanguageOrDefault(QStringLiteral("auto"), supported) == QStringLiteral("en"),
               "asrLanguageOrDefault migrates the retired \"auto\" sentinel to en");
        expect(asrLanguageOrDefault(QString(), supported) == QStringLiteral("en"),
               "asrLanguageOrDefault migrates an empty code to en");
        expect(asrLanguageOrDefault(QStringLiteral("zz-nonesuch"), supported) == QStringLiteral("en"),
               "asrLanguageOrDefault falls back to en for an unsupported code");
        expect(asrLanguageOrDefault(QStringLiteral("en"), {}) == QStringLiteral("en"),
               "asrLanguageOrDefault falls back to en when the list is empty");
    }

    // ---- CopyAssistSettings::foldLegacyKeys: flat -> nested migration -----
    {
        using namespace AetherSDR;
        QMap<QString, QString> present;
        present.insert(QStringLiteral("AsrLanguage"), QStringLiteral("es"));
        present.insert(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("True"));
        present.insert(QStringLiteral("AsrPanelHeight"), QStringLiteral("240"));

        // Fold into an object that already has a migrated field: existing wins.
        QJsonObject existing;
        existing.insert(QStringLiteral("AsrLanguage"), QStringLiteral("fr"));
        const QJsonObject merged =
            CopyAssistSettings::foldLegacyKeys(present, existing);

        expect(merged.value(QStringLiteral("AsrLanguage")).toString() == QStringLiteral("fr"),
               "foldLegacyKeys does not overwrite an already-nested field");
        expect(merged.value(QStringLiteral("AsrRemoteEnabled")).toString() == QStringLiteral("True"),
               "foldLegacyKeys folds a present flat bool verbatim");
        expect(merged.value(QStringLiteral("AsrPanelHeight")).toString() == QStringLiteral("240"),
               "foldLegacyKeys folds a present flat int verbatim");

        // Nothing present → object unchanged (fresh install / already migrated).
        expect(CopyAssistSettings::foldLegacyKeys({}, QJsonObject{}).isEmpty(),
               "foldLegacyKeys leaves an empty object empty when nothing is present");

        // Every migrated key is a real, unique entry in the legacy list.
        expect(CopyAssistSettings::legacyFlatKeys().size() == 21,
               "legacyFlatKeys enumerates all 21 migrated Asr keys");
    }

    // ---- Transcript file logging: state + toggle signal -------------------
    {
        QSignalSpy logSpy(&dlg, &CopyAssistSettingsDialog::logToFileToggled);
        dlg.setLogFilePath(QStringLiteral("/tmp/aether-transcript.txt"));
        expect(dlg.logFilePath() == QStringLiteral("/tmp/aether-transcript.txt"),
               "setLogFilePath round-trips");
        dlg.setLogToFile(true);
        expect(dlg.logToFile(), "setLogToFile reflects state");
        expect(!logSpy.isEmpty() && logSpy.last().at(0).toBool(),
               "logToFileToggled(true) emitted");
    }

    // ---- Silero VAD: state + toggle signal --------------------------------
    {
        QSignalSpy vadSpy(&dlg, &CopyAssistSettingsDialog::useSileroVadToggled);
        dlg.setVadModelPath(QStringLiteral("/tmp/silero_vad.onnx"));
        expect(dlg.vadModelPath() == QStringLiteral("/tmp/silero_vad.onnx"),
               "setVadModelPath round-trips");
        dlg.setUseSileroVad(true);
        expect(dlg.useSileroVad(), "setUseSileroVad reflects state");
        expect(!vadSpy.isEmpty() && vadSpy.last().at(0).toBool(),
               "useSileroVadToggled(true) emitted");
    }

    // ---- Speaker labeling: toggle + threshold slider ----------------------
    {
        QSignalSpy spkSpy(&dlg, &CopyAssistSettingsDialog::labelSpeakersToggled);
        dlg.setSpeakerModelPath(QStringLiteral("/tmp/spk.onnx"));
        dlg.setLabelSpeakers(true);
        expect(dlg.labelSpeakers() && dlg.speakerModelPath() == QStringLiteral("/tmp/spk.onnx"),
               "speaker toggle + path round-trip");
        expect(!spkSpy.isEmpty() && spkSpy.last().at(0).toBool(),
               "labelSpeakersToggled(true) emitted");

        QSignalSpy thrSpy(&dlg, &CopyAssistSettingsDialog::speakerThresholdChanged);
        dlg.setSpeakerThreshold(65);
        expect(dlg.speakerThreshold() == 65, "setSpeakerThreshold round-trips");
        expect(!thrSpy.isEmpty() && thrSpy.last().at(0).toInt() == 65,
               "speakerThresholdChanged emits percent");
    }

    // ---- Boundary overlap slider: round-trip, no echo, operator edit ------
    // Unlike setSpeakerThreshold() above, setBoundaryOverlapMs() must NOT emit:
    // the controller calls it to seed the widget from the store, and an echo
    // would round-trip straight back into saveInt("AsrBoundaryOverlapMs") and
    // re-write what it just read (RFC #4821). The QSignalBlocker in that setter
    // is the only thing preventing it, so pin both halves — the programmatic
    // set stays silent, a real operator edit still reaches the controller.
    {
        QSignalSpy ovSpy(&dlg, &CopyAssistSettingsDialog::boundaryOverlapChanged);
        dlg.setBoundaryOverlapMs(750);
        expect(dlg.boundaryOverlapMs() == 750, "setBoundaryOverlapMs round-trips");
        expect(ovSpy.isEmpty(),
               "a programmatic overlap set does not echo back as an operator edit");

        auto* ov = dlg.findChild<QSlider*>(QStringLiteral("CopyAssistOverlapSlider"));
        expect(ov != nullptr, "boundary overlap slider is findable by object name");
        if (ov != nullptr) {
            expect(ov->minimum() == 0 && ov->maximum() == 2000,
                   "boundary overlap slider spans 0-2000 ms");
            ov->setValue(1250); // a real operator edit
            expect(!ovSpy.isEmpty() && ovSpy.last().at(0).toInt() == 1250,
                   "boundaryOverlapChanged carries the operator's value in ms");
        }

        dlg.setBoundaryOverlapMs(0); // restore the default (off)
    }

    dlg.resize(520, 360);
    app.processEvents();
    dlg.close();
    expect(!AppSettings::instance()
                .value(QStringLiteral("CopyAssistSettingsDialogGeometry"))
                .toString()
                .isEmpty(),
           "dialog geometry is persisted on close");

    std::printf(g_failures == 0 ? "\nCopy Assist settings dialog: ALL PASS\n"
                                : "\nCopy Assist settings dialog: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
