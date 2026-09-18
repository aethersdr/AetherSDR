// Offline UI test for CopyAssistSettingsDialog (RFC #4333). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies frameless-window behavior plus the
// selectors that moved out of CopyAssistPanel into the modeless settings dialog.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/SettingsBootstrap.h"
#include "gui/CopyAssistSettingsDialog.h"
#include "gui/FramelessWindowTitleBar.h"

#include "asr/AsrCrashMarker.h"    // fault-record decision (header-inline, whisper-free)
#include "asr/WhisperAsrBackend.h" // asrLanguageOrDefault (header-inline, whisper-free)
#include "gui/CopyAssistSettings.h" // foldLegacyKeys + value/setValue

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>

#include <cstdio>
#include <optional>

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

    // ---- #5190: surviving an uncatchable ASR fault ---------------------------
    // The decision is pure, so every branch is pinned here without staging a
    // crash. All attempts below are CONSTRUCTED: they exercise the decision
    // table, and claim nothing about which faults occur in the field.
    {
        const QString ver = QStringLiteral("26.9.3+abc12345");
        AsrAttempt gpuLoad;
        gpuLoad.stage = QString::fromLatin1(kAsrStageLoad);
        gpuLoad.device = 1;
        gpuLoad.deviceName = QStringLiteral("GPU One");
        gpuLoad.tier = QStringLiteral("large-v3-turbo");
        gpuLoad.vramFreeMb = 1383;
        gpuLoad.vramTotalMb = 8151;
        gpuLoad.appVersion = ver;
        gpuLoad.startedUtc = QStringLiteral("2026-09-17T17:40:42Z");

        expect(asrFaultAction(AsrAttempt(), ver, std::nullopt) == AsrFaultAction::None,
               "no fault record -> run normally");
        expect(asrFaultAction(gpuLoad, ver, std::nullopt) == AsrFaultAction::AwaitDevices,
               "GPU load fault before discovery has run -> decided later, not guessed");
        expect(asrFaultAction(gpuLoad, ver, QStringLiteral("GPU One")) == AsrFaultAction::RetireGpu,
               "GPU load fault, same device at that index -> retire that GPU");
        expect(asrFaultAction(gpuLoad, ver, QStringLiteral("Other GPU")) == AsrFaultAction::Forget,
               "GPU load fault, different hardware at that index -> forget it");
        expect(asrFaultAction(gpuLoad, ver, QString()) == AsrFaultAction::Forget,
               "GPU load fault, no device at that index any more -> forget it");
        expect(asrFaultAction(gpuLoad, QStringLiteral("26.9.4+def67890"), QStringLiteral("GPU One"))
                   == AsrFaultAction::Forget,
               "fault recorded by another app version -> forget it (new whisper/ggml build)");

        AsrAttempt cpuLoad = gpuLoad;
        cpuLoad.device = -1;
        cpuLoad.deviceName.clear();
        expect(asrFaultAction(cpuLoad, ver, std::nullopt) == AsrFaultAction::DisableAsr,
               "load fault already on CPU -> local engine off (forcing CPU cannot help)");

        // #5190 review (NF0T): load() retries on CPU inside the same call after a
        // CAUGHT GPU failure. A death in that retry is a CPU death; the marker the
        // fallback hook persists must classify like one.
        {
            const AsrAttempt fellBack = asrAttemptOnCpuFallback(gpuLoad);
            expect(fellBack.device == -1 && fellBack.deviceName.isEmpty()
                       && fellBack.vramFreeMb == 0 && fellBack.vramTotalMb == 0,
                   "CPU fallback re-aims a GPU load marker at the CPU");
            expect(fellBack.stage == gpuLoad.stage && fellBack.tier == gpuLoad.tier
                       && fellBack.appVersion == gpuLoad.appVersion
                       && fellBack.startedUtc == gpuLoad.startedUtc,
                   "CPU fallback keeps what the attempt was (tier, version, start)");
            expect(asrFaultAction(fellBack, ver, std::nullopt) == AsrFaultAction::DisableAsr
                       && asrFaultAction(fellBack, ver, QStringLiteral("GPU One"))
                           == AsrFaultAction::DisableAsr,
                   "caught GPU failure then a death in the CPU retry -> local engine off, "
                   "not RetireGpu");
            expect(asrMarkerJsonOnCpuFallback(QString()).isEmpty()
                       && asrMarkerJsonOnCpuFallback(QStringLiteral("not json")).isEmpty(),
                   "CPU fallback with nothing armed leaves the field empty");
        }

        AsrAttempt discovery;
        discovery.stage = QString::fromLatin1(kAsrStageDiscovery);
        discovery.appVersion = ver;
        expect(asrFaultAction(discovery, ver, std::nullopt) == AsrFaultAction::DisableAsr,
               "discovery fault -> local engine off, never 'force CPU'");
        expect(asrFaultAction(discovery, QStringLiteral("other"), std::nullopt)
                   == AsrFaultAction::Forget,
               "discovery fault from another version -> forget it");

        // Round trip, and the rule that a damaged field never stands anything down.
        const AsrAttempt back = asrAttemptFromJson(asrAttemptToJson(gpuLoad));
        expect(back.isValid() && back.stage == gpuLoad.stage && back.device == 1
                   && back.deviceName == gpuLoad.deviceName && back.tier == gpuLoad.tier
                   && back.vramFreeMb == 1383 && back.vramTotalMb == 8151
                   && back.appVersion == ver && back.startedUtc == gpuLoad.startedUtc,
               "attempt survives a JSON round trip field for field");
        expect(!asrAttemptFromJson(QStringLiteral("{not json")).isValid()
                   && !asrAttemptFromJson(QStringLiteral("[]")).isValid()
                   && !asrAttemptFromJson(QStringLiteral("{\"stage\":\"decode\"}")).isValid()
                   && !asrAttemptFromJson(QString()).isValid(),
               "unparseable / unknown-stage / empty marker reads as no attempt");
        expect(asrAttemptToJson(AsrAttempt()).isEmpty(),
               "an invalid attempt serialises to nothing (never arms an empty marker)");

        // The record ACCUMULATES. Without the merge, two faulting GPUs hand the
        // decode back and forth forever: launch 3 would retire only the newest
        // one and walk straight back onto the first.
        {
            AsrAttempt gpu0 = gpuLoad;
            gpu0.device = 0;
            gpu0.deviceName = QStringLiteral("GPU Zero");
            const AsrAttempt afterFirst = asrMergeFault(AsrAttempt(), gpu0);
            expect(afterFirst.device == 0 && afterFirst.retired.isEmpty(),
                   "first fault: the record names that GPU, nothing carried yet");
            const AsrAttempt afterSecond = asrMergeFault(afterFirst, gpuLoad); // GPU One dies next
            expect(afterSecond.device == 1 && afterSecond.retired.size() == 1
                       && afterSecond.retired[0].device == 0
                       && afterSecond.retired[0].name == QStringLiteral("GPU Zero"),
                   "second fault on another GPU: the first GPU stays retired");
            const AsrAttempt back2 = asrAttemptFromJson(asrAttemptToJson(afterSecond));
            expect(back2.retired.size() == 1 && back2.retired[0].device == 0
                       && back2.retired[0].name == QStringLiteral("GPU Zero"),
                   "the retired set survives the JSON round trip");
            const AsrAttempt afterCpu = asrMergeFault(afterSecond, cpuLoad);
            expect(afterCpu.device == -1 && afterCpu.retired.size() == 2
                       && asrFaultAction(afterCpu, ver, std::nullopt) == AsrFaultAction::DisableAsr,
                   "third fault on CPU: both GPUs carried, and the ladder ends at engine-off");
            const AsrAttempt again = asrMergeFault(afterSecond, gpuLoad);
            expect(again.retired.size() == 1,
                   "the same GPU faulting again is not carried as its own predecessor");
            AsrAttempt otherVersion = gpu0;
            otherVersion.appVersion = QStringLiteral("26.9.4+def67890");
            expect(asrMergeFault(afterSecond, otherVersion).retired.isEmpty(),
                   "a fault under another app version starts a clean slate");
            expect(!asrMergeFault(afterSecond, AsrAttempt()).isValid(),
                   "merging nothing yields no record");
        }

        // Marker bookkeeping — the two rules a read of the controller got wrong
        // before they were pinned here.
        {
            AsrMarkerState st;
            expect(st.armDiscovery() && st.discoveryArmed(), "discovery arms its own slot");
            expect(st.armLoad(), "a load arms while discovery is still running (timed-out probe)");
            const bool loadCleared = st.loadSettled();
            expect(loadCleared && st.discoveryArmed(),
                   "the load settling clears ITS slot and leaves discovery armed");
            expect(st.discoveryFinished() && !st.discoveryArmed(),
                   "discovery finishing clears its slot");
            expect(!st.discoveryFinished(), "a second finish has nothing to clear");

            AsrMarkerState two;
            two.armLoad();
            two.armLoad(); // tier changed mid-load: a second load queues behind the first
            expect(!two.loadSettled(), "first of two queued loads settling does NOT clear");
            expect(two.loadSettled(), "the last outstanding load clears");
            expect(!two.loadSettled(), "a stray settle (remote/sherpa ready) clears nothing");

            AsrMarkerState torn;
            torn.armLoad();
            torn.armLoad();
            expect(torn.engineTornDown() && torn.loadsInFlight() == 0,
                   "engine teardown clears once and forgets the queued loads");
            expect(!torn.engineTornDown(), "teardown with nothing armed writes nothing");
        }

        // Both fields live inside the ONE CopyAssist document (Principle V) and
        // persist through the same accessor the controller uses.
        CopyAssistSettings::setValue(QStringLiteral("AsrInFlight"), asrAttemptToJson(gpuLoad));
        const QJsonObject doc =
            QJsonDocument::fromJson(AppSettings::instance()
                                        .value(CopyAssistSettings::rootKey())
                                        .toString()
                                        .toUtf8())
                .object();
        expect(doc.contains(QStringLiteral("AsrInFlight"))
                   && !AppSettings::instance().contains(QStringLiteral("AsrInFlight")),
               "the marker is a field of the CopyAssist document, not a flat key");
        expect(asrAttemptFromJson(
                   CopyAssistSettings::value(QStringLiteral("AsrInFlight")).toString())
                       .deviceName
                   == QStringLiteral("GPU One"),
               "the marker reads back through CopyAssistSettings");
        // The transform the controller's CPU-fallback hook installs, applied by
        // updateValue() to a GPU load marker in the real store (#5190 review).
        // This pins the store write, not the call sites inside load() — those
        // need a GPU load to fail and are covered on the bench.
        {
            CopyAssistSettings::setValue(QStringLiteral("AsrCpuFallbackProbe"), QStringLiteral("kept"));
            QString seenByUpdate;
            CopyAssistSettings::updateValue(QStringLiteral("AsrInFlight"),
                                            [&seenByUpdate](const QString& current) {
                                                seenByUpdate = current;
                                                return asrMarkerJsonOnCpuFallback(current);
                                            });
            expect(asrAttemptFromJson(seenByUpdate).device == 1,
                   "updateValue hands the update the field's current value");
            const AsrAttempt stored = asrAttemptFromJson(
                CopyAssistSettings::value(QStringLiteral("AsrInFlight")).toString());
            expect(stored.isValid() && stored.device == -1 && stored.tier == gpuLoad.tier,
                   "the persisted GPU marker is re-aimed at the CPU");
            expect(asrFaultAction(stored, ver, QStringLiteral("GPU One")) == AsrFaultAction::DisableAsr,
                   "a death after the CPU fallback reads back as DisableAsr, not RetireGpu");
            expect(CopyAssistSettings::value(QStringLiteral("AsrCpuFallbackProbe")).toString()
                       == QStringLiteral("kept"),
                   "updateValue leaves the document's other fields in place");
        }
        CopyAssistSettings::setValue(QStringLiteral("AsrInFlight"), QString());
        expect(!asrAttemptFromJson(
                    CopyAssistSettings::value(QStringLiteral("AsrInFlight")).toString())
                    .isValid(),
               "a cleared marker reads as no attempt");

        // Adopt through the production store seam, and inspect a separate
        // read-only database connection rather than the in-memory settings.
        {
            AsrAttempt older = gpuLoad;
            older.device = 0;
            older.deviceName = QStringLiteral("GPU Zero");
            CopyAssistSettings::setValue(QStringLiteral("AsrLastFault"), asrAttemptToJson(older));
            CopyAssistSettings::setValue(QStringLiteral("AsrInFlight"), asrAttemptToJson(gpuLoad));
            CopyAssistSettings::setValue(QStringLiteral("AsrInFlightDiscovery"), asrAttemptToJson(discovery));
            const auto diskDocument = [] {
                return QJsonDocument::fromJson(
                    SettingsBootstrap::readValue(CopyAssistSettings::rootKey()).toUtf8()).object();
            };
            const QJsonObject before = diskDocument();
            expect(!before.value(QStringLiteral("AsrInFlight")).toString().isEmpty(),
                   "the recovery input is durable before adoption");
            const AsrAttempt adopted = CopyAssistSettings::adoptSurvivingFault();
            const QJsonObject after = diskDocument();
            const AsrAttempt persisted = asrAttemptFromJson(
                after.value(QStringLiteral("AsrLastFault")).toString());
            expect(adopted.device == 1 && persisted.device == 1
                       && persisted.retired.size() == 1 && persisted.retired[0].device == 0,
                   "adoption durably merges the load fault with earlier retired GPUs");
            expect(after.value(QStringLiteral("AsrInFlight")).toString().isEmpty()
                       && after.value(QStringLiteral("AsrInFlightDiscovery")).toString().isEmpty(),
                   "the same adoption consumes both marker slots");
            expect(after.value(QStringLiteral("AsrCpuFallbackProbe"))
                       == before.value(QStringLiteral("AsrCpuFallbackProbe")),
                   "adoption preserves unrelated CopyAssist fields");
            expect(!CopyAssistSettings::adoptSurvivingFault().isValid()
                       && diskDocument() == after,
                   "repeated adoption does not erase or change the adopted fault");

            // A save refused during reset must leave the OLD durable document
            // intact, with its marker available for a subsequent process.
            CopyAssistSettings::setValue(QStringLiteral("AsrInFlightDiscovery"), asrAttemptToJson(discovery));
            const QJsonObject beforeRefusal = diskDocument();
            app.setProperty("AetherSettingsResetInProgress", true);
            CopyAssistSettings::adoptSurvivingFault();
            expect(diskDocument() == beforeRefusal,
                   "a refused adoption commit retains the complete durable marker document");
            app.setProperty("AetherSettingsResetInProgress", false);
            AppSettings::instance().save();
            expect(asrAttemptFromJson(diskDocument().value(QStringLiteral("AsrLastFault")).toString()).stage
                       == QLatin1String(kAsrStageDiscovery),
                   "retrying the pending save publishes the complete discovery fault");
            CopyAssistSettings::setValue(QStringLiteral("AsrLastFault"), QString());
        }

        // The undo control: hidden by default, shown with the reason, one click
        // asks for another attempt.
        expect(!dlg.faultStandDownVisible(), "fault row is hidden when nothing is stood down");
        auto* retry = dlg.findChild<QPushButton*>(QStringLiteral("CopyAssistFaultRetryButton"));
        auto* reason = dlg.findChild<QLabel*>(QStringLiteral("CopyAssistFaultReason"));
        expect(retry != nullptr && reason != nullptr, "fault row has its reason label and button");
        if (retry != nullptr && reason != nullptr) {
            expect(!retry->accessibleName().isEmpty(), "retry button has an accessible name");
            dlg.setFaultStandDown(QStringLiteral("GPU One stopped AetherSDR"));
            expect(dlg.faultStandDownVisible() && reason->text().contains(QStringLiteral("GPU One")),
                   "setFaultStandDown shows the row with the reason");
            QSignalSpy retrySpy(&dlg, &CopyAssistSettingsDialog::retryAfterFaultRequested);
            retry->click();
            expect(retrySpy.count() == 1, "clicking the button requests another attempt once");
            expect(retry->accessibleDescription().contains(QStringLiteral("GPU One")),
                   "the reason is on the button for a screen reader (a tooltip is never announced)");
            dlg.setFaultRetryPending();
            expect(dlg.faultStandDownVisible() && !retry->isEnabled()
                       && reason->text().contains(QStringLiteral("next time")),
                   "after a retry request the row stays, says next launch, and the button is off");
            dlg.clearFaultStandDown();
            expect(!dlg.faultStandDownVisible() && reason->text().isEmpty(),
                   "clearFaultStandDown hides the row and drops the reason");
        }
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
