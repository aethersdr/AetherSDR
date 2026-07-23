// Offline UI test for CopyAssistSettingsDialog (RFC #4333). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies the model-tier + compute-device
// selectors that moved out of CopyAssistPanel into the modeless settings dialog.

#include "gui/CopyAssistSettingsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QSignalSpy>

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
    QApplication app(argc, argv);

    CopyAssistSettingsDialog dlg;

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

    // ---- Compute-device selector: hidden by default, shows + emits --------
    {
        auto* gpuCombo = dlg.findChild<QComboBox*>(QStringLiteral("CopyAssistGpuCombo"));
        expect(gpuCombo != nullptr && !gpuCombo->isVisibleTo(&dlg),
               "GPU combo hidden until a device exists");

        dlg.addGpuDevice(0, QStringLiteral("GPU0"));
        dlg.addGpuDevice(-1, QStringLiteral("CPU"));
        dlg.setGpuSelectorVisible(true);

        QSignalSpy gpuSpy(&dlg, &CopyAssistSettingsDialog::gpuChanged);
        dlg.setCurrentGpu(-1);
        expect(dlg.currentGpu() == -1, "setCurrentGpu selects the CPU sentinel");
        expect(!gpuSpy.isEmpty() && gpuSpy.last().at(0).toInt() == -1,
               "gpuChanged carries the device index");
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

    std::printf(g_failures == 0 ? "\nCopy Assist settings dialog: ALL PASS\n"
                                : "\nCopy Assist settings dialog: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
