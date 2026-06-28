// MainWindow_Menus.cpp — menu-bar construction for MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1b). Holds buildMenuBar():
// every QMenu/QAction in the menu bar, their enable/disable wiring, and the
// inline lambdas they trigger (~70 connects).
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "DaxApplet.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "RadioSetupDialog.h"
#include "TciApplet.h"
#include "ClientChainApplet.h"
#include "DxClusterDialog.h"
#include "HelpDialog.h"
#include "MainWindowHelpers.h"
#include "MainWindowShortcutState.h"
#include "PersistentDialog.h"
#include "RC28MappingDialog.h"
#include "ShortcutDialog.h"
#include "SliceTroubleshootingDialog.h"
#include "SpectrumWidget.h"
#include "SupportDialog.h"
#include "TitleBar.h"
#include "MidiMappingDialog.h"
#include "ProfileImportExportDialog.h"
#include "ProfileManagerDialog.h"
#include "ThemeEditorDialog.h"
#include "TxBandDialog.h"
#include "UlanziDialMapperDialog.h"
#include "WaveformsDialog.h"
#include "WhatsNewDialog.h"
#include "core/UpdateChecker.h"
#include "core/AppSettings.h"
#include "core/SpotModeResolver.h"
#include "core/ThemeManager.h"
#include "models/BandPlanManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QActionGroup>
#include <QCoreApplication>
#include <QCheckBox>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <cmath>

namespace AetherSDR {

void MainWindow::buildMenuBar()
{
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* waveformsAct = fileMenu->addAction("Waveforms...");
    waveformsAct->setMenuRole(QAction::NoRole);
    connect(waveformsAct, &QAction::triggered, this, [this] {
        showOrRaisePersistent(m_waveformsDialog, &m_radioModel);
    });

    fileMenu->addSeparator();
    auto* quitAct = fileMenu->addAction("&Quit");
    quitAct->setShortcut(QKeySequence::Quit);
    quitAct->setMenuRole(QAction::QuitRole);
    connect(quitAct, &QAction::triggered, this, [this]() {
        if (m_panStack) {
            m_panStack->setShuttingDown(true);
        }
        close();
    });

    // ── Settings menu ──────────────────────────────────────────────────────
    auto* settingsMenu = menuBar()->addMenu("&Settings");

    auto* radioSetup = settingsMenu->addAction("Radio Setup...");
    radioSetup->setMenuRole(QAction::PreferencesRole);  // macOS: appears in app menu as Preferences (#883, #1013)
    connect(radioSetup, &QAction::triggered, this, [this] {
        // Snapshot compression setting before dialog opens — used by the
        // finished handler to detect a change and recreate the RX audio stream.
        const QString prevComp = m_radioModel.audioCompressionParam();
        const bool wasFresh = !m_radioSetupDialog;
        showOrRaisePersistent(m_radioSetupDialog,
                              &m_radioModel, m_audio,
                              &m_tgxlConn, &m_pgxlConn, &m_antennaGenius,
                              m_kiwiSdrManager);
        if (wasFresh && m_radioSetupDialog)
            wireRadioSetupDialogSignals(m_radioSetupDialog, prevComp);
    });

    auto* chooseRadio = settingsMenu->addAction("Connect to Radio...");
    chooseRadio->setMenuRole(QAction::NoRole);      // prevent macOS auto-reparenting (#883)
    connect(chooseRadio, &QAction::triggered, this, [this] {
        toggleConnectionDialog();
    });

    auto* flexControlAction = settingsMenu->addAction("AetherControl...");
    flexControlAction->setMenuRole(QAction::NoRole);
    connect(flexControlAction, &QAction::triggered,
            this, &MainWindow::showFlexControlDialog);

    auto* networkAction = settingsMenu->addAction("Network...");
    connect(networkAction, &QAction::triggered, this, [this] {
        showNetworkDiagnosticsDialog();
    });

    auto* receiveSyncMenu = settingsMenu->addMenu("Receive Sync");
    auto* receiveSyncEnabledAct =
        receiveSyncMenu->addAction("Sync Kiwi Audio && Display");
    receiveSyncEnabledAct->setCheckable(true);
    receiveSyncEnabledAct->setChecked(receivePresentationSettings().enabled);

    auto* receiveSyncModeGroup = new QActionGroup(receiveSyncMenu);
    receiveSyncModeGroup->setExclusive(true);
    auto* receiveSyncManualAct = receiveSyncMenu->addAction("Manual Offset");
    receiveSyncManualAct->setCheckable(true);
    receiveSyncModeGroup->addAction(receiveSyncManualAct);
    auto* receiveSyncAutoAct = receiveSyncMenu->addAction("Auto Assist");
    receiveSyncAutoAct->setCheckable(true);
    receiveSyncModeGroup->addAction(receiveSyncAutoAct);
    receiveSyncMenu->addSeparator();

    auto* receiveSyncOffsetLabel = receiveSyncMenu->addAction(QString());
    receiveSyncOffsetLabel->setEnabled(false);
    auto* receiveSyncOffsetMinus =
        receiveSyncMenu->addAction("Delay KiwiSDR 50 ms");
    auto* receiveSyncOffsetPlus =
        receiveSyncMenu->addAction("Delay Flex 50 ms");
    auto* receiveSyncOffsetReset = receiveSyncMenu->addAction("Reset Offset");
    receiveSyncMenu->addSeparator();

    auto* receiveSyncLatencyMenu = receiveSyncMenu->addMenu("Latency");
    auto* receiveSyncLatencyGroup = new QActionGroup(receiveSyncLatencyMenu);
    receiveSyncLatencyGroup->setExclusive(true);
    auto* receiveSyncLatencyNormal =
        receiveSyncLatencyMenu->addAction("Normal (360 ms)");
    receiveSyncLatencyNormal->setCheckable(true);
    receiveSyncLatencyGroup->addAction(receiveSyncLatencyNormal);
    auto* receiveSyncLatencyStable =
        receiveSyncLatencyMenu->addAction("More Stable (520 ms)");
    receiveSyncLatencyStable->setCheckable(true);
    receiveSyncLatencyGroup->addAction(receiveSyncLatencyStable);
    auto* receiveSyncLatencyHigh =
        receiveSyncLatencyMenu->addAction("High Jitter (1000 ms)");
    receiveSyncLatencyHigh->setCheckable(true);
    receiveSyncLatencyGroup->addAction(receiveSyncLatencyHigh);
    receiveSyncMenu->addSeparator();
    auto* receiveSyncStatusLabel = receiveSyncMenu->addAction(QString());
    receiveSyncStatusLabel->setEnabled(false);

    auto refreshReceiveSyncMenu = [this, receiveSyncEnabledAct,
                                   receiveSyncManualAct, receiveSyncAutoAct,
                                   receiveSyncOffsetLabel,
                                   receiveSyncLatencyNormal,
                                   receiveSyncLatencyStable,
                                   receiveSyncLatencyHigh,
                                   receiveSyncStatusLabel]() {
        const ReceivePresentationSettings settings =
            receivePresentationSettings();
        const ReceiveDelayBreakdown delays =
            receivePresentationDelayBreakdown();
        receiveSyncEnabledAct->setChecked(settings.enabled);
        receiveSyncManualAct->setChecked(settings.mode == ReceiveSyncMode::Manual);
        receiveSyncAutoAct->setChecked(settings.mode == ReceiveSyncMode::AutoAssist);
        receiveSyncOffsetLabel->setText(
            QStringLiteral("Offset: %1%2 ms")
                .arg(settings.manualOffsetMs >= 0 ? QStringLiteral("+")
                                                  : QString())
                .arg(settings.manualOffsetMs));
        receiveSyncLatencyNormal->setChecked(settings.baseLatencyMs == 360);
        receiveSyncLatencyStable->setChecked(settings.baseLatencyMs == 520);
        receiveSyncLatencyHigh->setChecked(settings.baseLatencyMs == 1000);
        QString status = QStringLiteral("Off");
        switch (delays.status) {
        case ReceiveSyncStatus::Off:
            status = QStringLiteral("Off");
            break;
        case ReceiveSyncStatus::Manual:
            status = QStringLiteral("Manual");
            break;
        case ReceiveSyncStatus::Searching:
            status = QStringLiteral("Searching");
            break;
        case ReceiveSyncStatus::Holding:
            status = QStringLiteral("Coasting");
            break;
        case ReceiveSyncStatus::Locked:
            status = QStringLiteral("Locked");
            break;
        case ReceiveSyncStatus::LowConfidence:
            status = QStringLiteral("Low confidence");
            break;
        }
        QString statusText =
            QStringLiteral("Status: %1, Flex %2 ms, KiwiSDR %3 ms")
                .arg(status)
                .arg(delays.flexDelayMs)
                .arg(delays.kiwiDelayMs);
        if (settings.mode == ReceiveSyncMode::AutoAssist
            && settings.autoEstimate.valid) {
            statusText += QStringLiteral(", est %1%2 ms, conf %3%, drift %4%5 ppm")
                              .arg(settings.autoEstimate.offsetMs >= 0
                                       ? QStringLiteral("+")
                                       : QString())
                              .arg(settings.autoEstimate.offsetMs)
                              .arg(static_cast<int>(std::lround(
                                  settings.autoEstimate.confidence * 100.0f)))
                              .arg(settings.autoEstimate.driftPpm >= 0
                                       ? QStringLiteral("+")
                                       : QString())
                              .arg(settings.autoEstimate.driftPpm);
        }
        receiveSyncStatusLabel->setText(statusText);
    };
    refreshReceiveSyncMenu();
    connect(receiveSyncMenu, &QMenu::aboutToShow, this, refreshReceiveSyncMenu);

    connect(receiveSyncEnabledAct, &QAction::toggled, this,
            [this, refreshReceiveSyncMenu](bool enabled) {
        setReceivePresentationSyncEnabled(enabled);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncManualAct, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        setReceivePresentationSyncMode(ReceiveSyncMode::Manual);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncAutoAct, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        setReceivePresentationSyncMode(ReceiveSyncMode::AutoAssist);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncOffsetMinus, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        adjustReceivePresentationManualOffsetMs(-50);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncOffsetPlus, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        adjustReceivePresentationManualOffsetMs(50);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncOffsetReset, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        resetReceivePresentationManualOffset();
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncLatencyNormal, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        setReceivePresentationLatencyMs(360);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncLatencyStable, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        setReceivePresentationLatencyMs(520);
        refreshReceiveSyncMenu();
    });
    connect(receiveSyncLatencyHigh, &QAction::triggered, this,
            [this, refreshReceiveSyncMenu]() {
        setReceivePresentationLatencyMs(1000);
        refreshReceiveSyncMenu();
    });
#ifdef HAVE_MQTT
    auto* mqttAction = settingsMenu->addAction("MQTT...");
    mqttAction->setMenuRole(QAction::NoRole);
    connect(mqttAction, &QAction::triggered,
            this, &MainWindow::showMqttSettingsDialog);
#endif
    auto* memoryAction = settingsMenu->addAction("Memory...");
    connect(memoryAction, &QAction::triggered, this, [this] {
        showMemoryDialog();
    });
    auto* netSchedulerAction = settingsMenu->addAction("Net Scheduler...");
    connect(netSchedulerAction, &QAction::triggered, this, [this] {
        showNetSchedulerDialog();
    });
    auto* usbCablesAction = settingsMenu->addAction("USB Cables...");
    connect(usbCablesAction, &QAction::triggered, this, [this] {
        const QString prevComp = m_radioModel.audioCompressionParam();
        const bool wasFresh = !m_radioSetupDialog;
        showOrRaisePersistent(m_radioSetupDialog,
                              &m_radioModel, m_audio,
                              &m_tgxlConn, &m_pgxlConn, &m_antennaGenius,
                              m_kiwiSdrManager);
        if (wasFresh && m_radioSetupDialog)
            wireRadioSetupDialogSignals(m_radioSetupDialog, prevComp);
        if (m_radioSetupDialog)
            m_radioSetupDialog->selectTab(QStringLiteral("USB Cables"));
    });
#ifdef HAVE_MIDI
    auto* midiAction = settingsMenu->addAction("MIDI Mapping...");
    connect(midiAction, &QAction::triggered, this, [this] {
        showOrRaisePersistent(m_midiDialog, m_midiControl);
    });
#endif
#ifdef HAVE_HIDAPI
    auto* rc28Action = settingsMenu->addAction("Icom RC-28 Remote Encoder...");
    connect(rc28Action, &QAction::triggered, this, [this] {
        const bool fresh = !m_rc28MappingDialog;
        showOrRaisePersistent(m_rc28MappingDialog, m_hidEncoder);
        if (fresh && m_rc28MappingDialog)
            connect(m_rc28MappingDialog, &RC28MappingDialog::mappingFieldChanged,
                    this, [this](const QString& field, const QString& value) {
                if (field == "f1Hold" || field == "f2Hold") {
                    m_hidFastTune = false;
                    m_hidFineTune = false;
                    updateRC28Leds();
                } else if (field == "sensitivity") {
                    m_hidSensitivity = value.toInt();
                    if (m_hidSensitivity < 1) m_hidSensitivity = 1;
                    m_hidPulseAccum = 0;
                } else if (field == "autoSnap") {
                    m_hidAutoSnap = (value == "True");
                    if (!m_hidAutoSnap && m_hidSnapTimer)
                        m_hidSnapTimer->stop();
                }
            });
    });
#endif
    auto* ulanziAction = settingsMenu->addAction("Ulanzi Dial Mapping...");
    connect(ulanziAction, &QAction::triggered, this, [this] {
#ifdef HAVE_MIDI
        MidiControlManager* midi = m_midiControl;
#else
        MidiControlManager* midi = nullptr;
#endif
        showOrRaisePersistent(m_ulanziMapperDialog, m_dialBackend,
                              &m_shortcutManager, midi);
    });
    auto* spotsAction = settingsMenu->addAction("SpotHub...");
    connect(spotsAction, &QAction::triggered, this, [this] {
        const bool wasFresh = !m_spotHubDialog;
        showOrRaisePersistent(m_spotHubDialog, m_dxCluster, m_rbnClient, m_wsjtxClient,
                              m_spotCollectorClient, m_potaClient,
#ifdef HAVE_WEBSOCKETS
                              m_freedvClient,
#endif
                              &m_radioModel, &m_dxccProvider);
        if (!wasFresh || !m_spotHubDialog) return;
        auto* dlg = m_spotHubDialog.data();
        dlg->setTotalSpots(m_radioModel.spotModel().spots().size());
        // Live preview: refresh spots on every display settings change
        auto refreshSpots = [this]() {
            auto& s = AppSettings::instance();
            bool on       = s.value("IsSpotsEnabled", "True").toString() == "True";
            int fontSize  = s.value("SpotFontSize", "16").toInt();
            int levels    = s.value("SpotsMaxLevel", "3").toInt();
            int position  = s.value("SpotsStartingHeightPercentage", "50").toInt();
            bool override = s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True";
            QColor spotColor(s.value("SpotsOverrideColor", "#FFFF00").toString());
            QColor bgColor(s.value("SpotsOverrideBgColor", "#000000").toString());
            int bgOpacity = s.value("SpotsBackgroundOpacity", 48).toInt();
            for (auto* a : m_panStack->allApplets()) {
                auto* sw = a->spectrumWidget();
                sw->setShowSpots(on);
                sw->setSpotFontSize(fontSize);
                sw->setSpotMaxLevels(levels);
                sw->setSpotStartPct(position);
                sw->setSpotOverrideColors(override);
                sw->setSpotOverrideBg(s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True");
                sw->setSpotColor(spotColor);
                sw->setSpotBgColor(bgColor);
                sw->setSpotBgOpacity(bgOpacity);
                sw->setSpotShowLines(s.value("IsSpotsLinesEnabled", "True").toString() == "True");
                sw->setSHistorySnapToStep(
                    s.value("SHistorySnapToStep", "False").toString() == "True");
            }
            // Rebuild markers so source-level visibility changes, such as the
            // Memories feed toggle, apply immediately without mutating the cache.
            m_radioModel.spotModel().refresh();
        };
        connect(dlg, &DxClusterDialog::settingsChanged, this, refreshSpots);
        // Signal/QRM History Markers live exclusively on the SpotHub
        // Display tab (no View-menu duplicate, by design — a single UI
        // surface with no risk of state drift).
        connect(dlg, &DxClusterDialog::sHistoryEnabledToggled, this,
                &MainWindow::applySHistoryEnabled);
        connect(dlg, &DxClusterDialog::sHistoryQrmToggled, this,
                &MainWindow::applySHistoryQrmEnabled);
        connect(dlg, &DxClusterDialog::smartSpotOpacityChanged, this,
                [this](int pct) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setSmartSpotFilterOpacity(pct);
        });
        connect(dlg, &DxClusterDialog::smartSpotDelayChanged, this,
                [this](int seconds) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setSmartSpotFilterDelayS(seconds);
        });
        connect(dlg, &DxClusterDialog::smartSpotMatchHzChanged, this,
                [this](int hz) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setSmartSpotFilterMatchHz(hz);
        });
        connect(dlg, &DxClusterDialog::connectRequested,
                this, [this](const QString& host, quint16 port, const QString& call) {
            QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->connectToCluster(host, port, call); });
        });
        connect(dlg, &DxClusterDialog::disconnectRequested,
                this, [this] { QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->disconnect(); }); });
        connect(dlg, &DxClusterDialog::rbnConnectRequested,
                this, [this](const QString& host, quint16 port, const QString& call) {
            QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->connectToCluster(host, port, call); });
        });
        connect(dlg, &DxClusterDialog::rbnDisconnectRequested,
                this, [this] { QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->disconnect(); }); });
        connect(dlg, &DxClusterDialog::wsjtxStartRequested,
                this, [this](const QString& addr, quint16 port) {
            QMetaObject::invokeMethod(m_wsjtxClient, [=] { m_wsjtxClient->startListening(addr, port); });
        });
        connect(dlg, &DxClusterDialog::wsjtxStopRequested,
                this, [this] { QMetaObject::invokeMethod(m_wsjtxClient, [=] { m_wsjtxClient->stopListening(); }); });
        connect(dlg, &DxClusterDialog::spotCollectorStartRequested,
                this, [this](quint16 port) {
            QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->startListening(port); });
        });
        connect(dlg, &DxClusterDialog::spotCollectorStopRequested,
                this, [this] { QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->stopListening(); }); });
        connect(dlg, &DxClusterDialog::potaStartRequested,
                this, [this](int interval) {
            QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->startPolling(interval); });
        });
        connect(dlg, &DxClusterDialog::potaStopRequested,
                this, [this] { QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->stopPolling(); }); });
#ifdef HAVE_WEBSOCKETS
        connect(dlg, &DxClusterDialog::freedvStartRequested,
                this, [this] { QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->startConnection(); }); });
        connect(dlg, &DxClusterDialog::freedvStopRequested,
                this, [this] { QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->stopConnection(); }); });
        connect(dlg, &DxClusterDialog::freedvMessageChanged,
                this, [this](const QString& msg) {
            QMetaObject::invokeMethod(m_freedvClient, [this, msg] { m_freedvClient->updateMessage(msg); });
        });
#ifdef HAVE_RADE
        connect(dlg, &DxClusterDialog::freedvReportingToggled,
                this, [this](bool on) {
                    if (on) {
                        if (m_radeEngine)
                            startFreeDvReporting(m_radeSliceId);
                    } else {
                        stopFreeDvReporting(m_radeSliceId);
                    }
                });
#endif
#endif
        connect(dlg, &DxClusterDialog::spotsClearedAll,
                this, [this] {
            m_spotDedup.clear();
            m_radioModel.spotModel().clear();
            // Also wipe Signal History + QRM marker state so "Clear All
            // Spots" really does clear every marker shape on the
            // spectrum, not just the DX cluster spots.
            m_sHistoryData.clear();
            m_sHistoryPanState.clear();
            for (auto* a : m_panStack->allApplets()) {
                a->spectrumWidget()->setSHistoryMarkers({});
            }
        });
        connect(dlg, &DxClusterDialog::tuneRequested,
                this, [this](double freqMhz, const QString& spotMode, const QString& comment) {
            auto* sl = activeSlice();
            if (!sl) return;
            applyTuneRequest(sl, freqMhz, TuneIntent::AbsoluteJump, "dx-cluster");
            // #2298: also auto-switch mode (e.g. SSB→CW) the same way panadapter
            // spot clicks already do, gated by SpotAutoSwitchMode.
            if (AppSettings::instance().value("SpotAutoSwitchMode", "True").toString() != "True")
                return;
            const QString radioMode = SpotModeResolver::resolveSpotRadioMode(
                spotMode, comment, freqMhz);
            if (!radioMode.isEmpty() && radioMode != sl->mode())
                sl->setMode(radioMode);
        });
        connect(dlg, &QDialog::finished, this, refreshSpots);  // refresh on close
    });
    auto* multiFlexAction = settingsMenu->addAction("multiFLEX...");
    connect(multiFlexAction, &QAction::triggered,
            this, &MainWindow::showMultiFlexDialog);
    // m_titleBar connect deferred — see after TitleBar creation (~line 2530)
    m_txBandAction = settingsMenu->addAction("TX Band Settings...");
    m_txBandAction->setMenuRole(QAction::NoRole);   // prevent macOS auto-reparenting (#883)
    auto* txBandAct = m_txBandAction;
    connect(txBandAct, &QAction::triggered, this, [this] {
        if (!m_radioModel.isConnected()) {
            statusBar()->showMessage("Not connected to radio", 3000);
            return;
        }
        showOrRaisePersistent(m_txBandDialog, &m_radioModel);
    });

    // Inhibit during TUNE submenu — user selects which TX outputs to suppress.
    // Uses QWidgetAction with QCheckBox so the menu stays open for multi-select.
    auto* tuneInhibitMenu = settingsMenu->addMenu("Inhibit during TUNE");

    auto& settings = AppSettings::instance();
    struct InhibitDef { const char* label; const char* key; };
    static const InhibitDef inhibitDefs[] = {
        {"None",   "TuneInhibitNone"},
        {"ACC TX", "TuneInhibitAccTx"},
        {"TX1",    "TuneInhibitTx1"},
        {"TX2",    "TuneInhibitTx2"},
        {"TX3",    "TuneInhibitTx3"},
    };

    QCheckBox* noneCb = nullptr;
    QVector<QCheckBox*> outputCbs;

    for (const auto& def : inhibitDefs) {
        auto* cb = new QCheckBox(def.label);
        AetherSDR::ThemeManager::instance().applyStyleSheet(cb, "QCheckBox { color: {{color.text.primary}}; padding: 4px 12px; }"
            "QCheckBox::indicator { width: 14px; height: 14px; }"
            "QCheckBox::indicator:unchecked { border: 1px solid {{color.background.3}}; background: {{color.background.1}}; border-radius: 2px; }"
            "QCheckBox::indicator:checked { border: 1px solid {{color.accent}}; background: {{color.accent}}; border-radius: 2px; }");
        bool on = settings.value(def.key, "False").toString() == "True";
        cb->setChecked(on);

        auto* wa = new QWidgetAction(tuneInhibitMenu);
        wa->setDefaultWidget(cb);
        tuneInhibitMenu->addAction(wa);

        if (QString(def.label) == "None")
            noneCb = cb;
        else
            outputCbs.append(cb);
    }

    // Migrate old TuneInhibitAmp → TuneInhibitAccTx
    if (settings.value("TuneInhibitAmp", "").toString() == "True"
        && settings.value("TuneInhibitAccTx", "").toString().isEmpty()) {
        settings.setValue("TuneInhibitAccTx", "True");
        settings.setValue("TuneInhibitNone", "False");
        outputCbs[0]->setChecked(true);  // ACC TX
        if (noneCb) noneCb->setChecked(false);
        settings.save();
    }

    // If no outputs selected, check None
    bool anyOutput = false;
    for (auto* cb : outputCbs) anyOutput |= cb->isChecked();
    if (noneCb && !anyOutput) noneCb->setChecked(true);

    auto syncNone = [noneCb, outputCbs]() {
        bool anyOn = false;
        for (auto* cb : outputCbs) anyOn |= cb->isChecked();
        if (noneCb) {
            QSignalBlocker b(noneCb);
            noneCb->setChecked(!anyOn);
        }
    };

    // "None" unchecks all outputs
    connect(noneCb, &QCheckBox::toggled, this, [noneCb, outputCbs, &settings](bool on) {
        if (on) {
            for (auto* cb : outputCbs) {
                QSignalBlocker b(cb);
                cb->setChecked(false);
            }
            settings.setValue("TuneInhibitAccTx", "False");
            settings.setValue("TuneInhibitTx1", "False");
            settings.setValue("TuneInhibitTx2", "False");
            settings.setValue("TuneInhibitTx3", "False");
            settings.setValue("TuneInhibitNone", "True");
            settings.save();
        } else {
            QSignalBlocker b(noneCb);
            bool anyOn = false;
            for (auto* cb : outputCbs) anyOn |= cb->isChecked();
            if (!anyOn) noneCb->setChecked(true);
        }
    });

    // Each output toggle saves and syncs None
    for (int i = 0; i < outputCbs.size(); ++i) {
        connect(outputCbs[i], &QCheckBox::toggled, this,
                [i, syncNone, &settings](bool on) {
            static const char* keys[] = {"TuneInhibitAccTx", "TuneInhibitTx1",
                                         "TuneInhibitTx2", "TuneInhibitTx3"};
            settings.setValue(keys[i], on ? "True" : "False");
            if (on)
                settings.setValue("TuneInhibitNone", "False");
            syncNone();
            settings.save();
        });
    }

    auto* dspAction = settingsMenu->addAction("AetherDSP Settings...");
    dspAction->setMenuRole(QAction::NoRole);        // prevent macOS auto-reparenting (#883)
    connect(dspAction, &QAction::triggered, this, [this] {
        ensureAetherDspDialog();
    });
    // RX chain DSP tile double-click also opens the full AetherDSP
    // Settings dialog — same entry point as the Settings menu action.
    if (m_appletPanel && m_appletPanel->clientChainApplet()) {
        connect(m_appletPanel->clientChainApplet(),
                &ClientChainApplet::rxDspEditRequested,
                this, [dspAction]() { dspAction->trigger(); });
        // Single-click re-enable of NR2 from LastClientNr also runs
        // through enableNr2WithWisdom (#2275 — direct enable can crash).
        connect(m_appletPanel->clientChainApplet(),
                &ClientChainApplet::rxNr2EnableWithWisdomRequested,
                this, &MainWindow::enableNr2WithWisdom);
    }

    settingsMenu->addSeparator();

    // CAT: unified port manager (rigctld / TS-2000 / FlexCAT per port)
    auto* autoCatAction = settingsMenu->addAction("Autostart CAT with AetherSDR");
    autoCatAction->setCheckable(true);
    autoCatAction->setChecked(
        AppSettings::instance().value("CatEnabled", "False").toString() == "True");
    connect(autoCatAction, &QAction::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("CatEnabled", on ? "True" : "False");
        s.save();
        applyCatPortCount();
    });

    auto* autoTciAction = settingsMenu->addAction("Autostart TCI with AetherSDR");
    autoTciAction->setCheckable(true);
    autoTciAction->setChecked(
        AppSettings::instance().value("AutoStartTCI", "False").toString() == "True");
    connect(autoTciAction, &QAction::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AutoStartTCI", on ? "True" : "False");
        s.save();
#ifdef HAVE_WEBSOCKETS
        if (tciServer()) {
            if (on && !tciServer()->isRunning()) {
                int port = s.value("TciPort", "50001").toInt();
                tciServer()->start(static_cast<quint16>(port));
            } else if (!on && tciServer()->isRunning()) {
                tciServer()->stop();
            }
            if (m_appletPanel && m_appletPanel->tciApplet())
                m_appletPanel->tciApplet()->setTciEnabled(on);
        }
#endif
    });

#if !defined(Q_OS_MAC) && !defined(HAVE_PIPEWIRE)
    // DAX audio bridge requires macOS CoreAudio or Linux with PipeWire.
    // Force off and omit the menu entry on platforms without a bridge (#1556).
    {
        auto& s = AppSettings::instance();
        if (s.value("AutoStartDAX", "False").toString() != "False") {
            s.setValue("AutoStartDAX", "False");
            s.save();
        }
    }
#else
    auto* autoDaxAction = settingsMenu->addAction("Autostart DAX with AetherSDR");
    autoDaxAction->setCheckable(true);
    autoDaxAction->setChecked(
        AppSettings::instance().value("AutoStartDAX", "False").toString() == "True");
    connect(autoDaxAction, &QAction::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AutoStartDAX", on ? "True" : "False");
        s.save();
        if (m_radioModel.isConnected()) {
            if (on) {
                if (startDax() && m_appletPanel && m_appletPanel->daxApplet())
                    m_appletPanel->daxApplet()->setDaxEnabled(true);
            } else {
                stopDax();
                if (m_appletPanel && m_appletPanel->daxApplet())
                    m_appletPanel->daxApplet()->setDaxEnabled(false);
            }
        }
    });
#endif

    // "Low-Latency DAX (FreeDV)" menu retired in v0.8.19 — the toggle
    // it used to drive is now applied automatically by RADE mode, since
    // RADE was the only consumer that ever actually wanted that route.
    // See AudioEngine::setRadeMode().

    // Connect placeholder items to show "not implemented" message
    for (auto* action : settingsMenu->actions()) {
        if (!action->isSeparator() && action != radioSetup && action != chooseRadio
            && action != networkAction && action != memoryAction && action != spotsAction
            && action != usbCablesAction
#ifdef HAVE_SERIALPORT
            && action != flexControlAction
#endif
#ifdef HAVE_MIDI
            && action != midiAction
#endif
            && action != multiFlexAction
            && action != autoCatAction
            && action != autoTciAction
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
            && action != autoDaxAction
#endif
            ) {
            connect(action, &QAction::triggered, this, [this, action] {
                statusBar()->showMessage(action->text().remove("...") + " — not yet implemented", 3000);
            });
        }
    }

    // ── Profiles menu ──────────────────────────────────────────────────────
    m_profilesMenu = menuBar()->addMenu("&Profiles");
    auto* profileMgrAct = m_profilesMenu->addAction("Profile Manager...");
    connect(profileMgrAct, &QAction::triggered, this, [this] {
        showOrRaisePersistent(m_profileManagerDialog, &m_radioModel);
    });
    auto* profileImportExportAct = m_profilesMenu->addAction("Import/Export Profiles...");
    connect(profileImportExportAct, &QAction::triggered, this, [this] {
        showOrRaisePersistent(m_profileImportExportDialog, &m_radioModel);
    });
    m_profilesMenu->addSeparator();

    // Global profile list (populated on connect)
    connect(&m_radioModel, &RadioModel::globalProfilesChanged, this, [this] {
        // Remove old profile actions (after the separator)
        const auto actions = m_profilesMenu->actions();
        for (int i = 3; i < actions.size(); ++i)  // skip Manager, Import/Export, separator
            m_profilesMenu->removeAction(actions[i]);

        // Add current global profiles
        const auto profiles = m_radioModel.globalProfiles();
        const auto active = m_radioModel.activeGlobalProfile();
        for (const auto& name : profiles) {
            auto* act = m_profilesMenu->addAction(name);
            act->setCheckable(true);
            act->setChecked(name == active);
            connect(act, &QAction::triggered, this, [this, name] {
                m_radioModel.loadGlobalProfile(name);
            });
        }
    });

    auto* viewMenu = menuBar()->addMenu("&View");

    // Applet-panel show/hide and pop-out are now driven entirely from the
    // title-bar dock icons (#1713 Phase 6).  Ctrl+Shift+S retained here as
    // a window-scoped QShortcut so the keystroke survives the View-menu
    // entries being removed.
    auto* popOutShortcut = new QShortcut(QKeySequence("Ctrl+Shift+S"), this);
    connect(popOutShortcut, &QShortcut::activated, this, [this]() {
        toggleAppletPanelFloating(m_appletPanelFloatWindow == nullptr);
    });

    // Restore floating state at startup if the user had it floating last
    // time.  Delayed to the next event-loop turn so the splitter has
    // finished its initial layout before we yank the panel out.
    if (AppSettings::instance().value("AppletPanelFloating", "False").toString() == "True") {
        QTimer::singleShot(0, this, [this]() {
            toggleAppletPanelFloating(true);
        });
    }

    // Band Plan submenu — Off / Small / Medium / Large / Huge
    auto* bandPlanMenu = viewMenu->addMenu("Band Plan");
    // Use contains() to distinguish an explicit "Off" (0) from an absent key.
    // Without it, .toInt() returns 0 for both cases and the legacy migration
    // always promotes a saved "Off" back to Small (6) on next launch. (#3358)
    int savedBpSize;
    if (!AppSettings::instance().contains("BandPlanFontSize")) {
        savedBpSize = AppSettings::instance().value("ShowBandPlan", "True").toString() == "True"
                      ? 6 : 0;  // migrate old boolean → default Small
    } else {
        savedBpSize = AppSettings::instance().value("BandPlanFontSize").toInt();
    }
    auto* bpGroup = new QActionGroup(bandPlanMenu);
    struct BpOption { const char* label; int pt; };
    for (auto [label, pt] : {BpOption{"Off", 0}, {"Small", 6}, {"Medium", 10}, {"Large", 12}, {"Huge", 16}}) {
        auto* act = bandPlanMenu->addAction(label);
        act->setCheckable(true);
        act->setChecked(pt == savedBpSize);
        bpGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, pt] {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setBandPlanFontSize(pt);
            AppSettings::instance().setValue("BandPlanFontSize", QString::number(pt));
            AppSettings::instance().save();
        });
    }

    // Spot-marker toggle (#3339) — declutter the strip on dense band plans
    bandPlanMenu->addSeparator();
    const bool bpShowSpots =
        AppSettings::instance().value("BandPlanShowSpots", "True").toString() == "True";
    auto* spotsAct = bandPlanMenu->addAction("Show Spots");
    spotsAct->setCheckable(true);
    spotsAct->setChecked(bpShowSpots);
    connect(spotsAct, &QAction::toggled, this, [this](bool on) {
        for (auto* a : m_panStack->allApplets())
            a->spectrumWidget()->setBandPlanShowSpots(on);
        AppSettings::instance().setValue("BandPlanShowSpots", on ? "True" : "False");
        AppSettings::instance().save();
    });

    // Band plan region selector (#425)
    bandPlanMenu->addSeparator();
    auto* planGroup = new QActionGroup(bandPlanMenu);
    const QString activePlan = m_bandPlanMgr->activePlanName();
    for (const auto& name : m_bandPlanMgr->availablePlans()) {
        auto* act = bandPlanMenu->addAction(name);
        act->setCheckable(true);
        act->setChecked(name == activePlan);
        planGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, name] {
            m_bandPlanMgr->setActivePlan(name);
        });
    }

    // Theme submenu — list every theme ThemeManager discovered in
    // :/themes/ (built-ins) + ~/.config/AetherSDR/themes/ (user themes).
    // setActiveTheme() handles persistence + emits themeChanged so every
    // widget registered through applyStyleSheet re-themes on the next
    // paint, no app restart required.
    auto* themeMenu = viewMenu->addMenu("Theme");
    auto* themeGroup = new QActionGroup(themeMenu);
    auto rebuildThemeMenu = [themeMenu, themeGroup]() {
        themeMenu->clear();
        for (auto* a : themeGroup->actions())
            themeGroup->removeAction(a);
        auto& tm = ThemeManager::instance();
        const QString active = tm.activeTheme();
        for (const QString& name : tm.availableThemes()) {
            auto* act = themeMenu->addAction(name);
            act->setCheckable(true);
            act->setChecked(name == active);
            themeGroup->addAction(act);
            QObject::connect(act, &QAction::triggered, themeMenu, [name] {
                ThemeManager::instance().setActiveTheme(name);
            });
        }
    };
    rebuildThemeMenu();
    // Rebuild whenever the active theme changes (covers in-app theme
    // switches re-checking the right entry, and Phase-5 user themes
    // saved from the editor below appearing in the list immediately).
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                     themeMenu, rebuildThemeMenu);

    // Theme Editor (Phase 5 PR 1) — modeless dialog for live-editing
    // the active theme's colour tokens.  Sits as a sibling of the
    // Theme submenu above (which switches between saved themes);
    // the editor is for authoring a new one.  Open-on-demand; only
    // one instance at a time, cleaned up via WA_DeleteOnClose.
    auto* themeEditorAct = viewMenu->addAction("Theme Editor…");
    connect(themeEditorAct, &QAction::triggered, this, [this] {
        showOrRaisePersistent(m_themeEditorDialog);
    });

    auto* singleClickTuneAct = viewMenu->addAction("Single-Click to Tune");
    singleClickTuneAct->setCheckable(true);
    singleClickTuneAct->setChecked(
        AppSettings::instance().value("SingleClickTune", "False").toString() == "True");
    connect(singleClickTuneAct, &QAction::toggled, this, [this](bool on) {
        for (auto* a : m_panStack->allApplets())
            a->spectrumWidget()->setSingleClickTune(on);
        AppSettings::instance().setValue("SingleClickTune", on ? "True" : "False");
        AppSettings::instance().save();
    });

    auto* panFollowVfoAct = viewMenu->addAction("Pan Follows VFO");
    panFollowVfoAct->setCheckable(true);
    panFollowVfoAct->setChecked(
        AppSettings::instance().value("PanFollowVfo", "True").toString() == "True");
    connect(panFollowVfoAct, &QAction::toggled, this, [](bool on) {
        AppSettings::instance().setValue("PanFollowVfo", on ? "True" : "False");
        AppSettings::instance().save();
    });

    // Signal/QRM History Markers live exclusively on the SpotHub Display
    // tab — no View-menu duplicate.  Boot-time state is read here; Display
    // tab toggle signals call applySHistoryEnabled / applySHistoryQrmEnabled
    // (defined in this file) for the live apply + persistence path.
    m_sHistoryEnabled =
        AppSettings::instance().value("SHistoryMarkersEnabled", "False").toString() == "True";
    m_sHistoryQrmEnabled =
        AppSettings::instance().value("SHistoryQrmEnabled", "False").toString() == "True";

    // UI Scale submenu — sets QT_SCALE_FACTOR, applies on restart
    auto* scaleMenu = viewMenu->addMenu("UI Scale");
    int savedScale = AppSettings::instance().value("UiScalePercent", "100").toInt();
    auto* scaleGroup = new QActionGroup(scaleMenu);
    for (int pct : {75, 85, 100, 110, 125, 150, 175, 200}) {
        auto* act = scaleMenu->addAction(QString("%1%").arg(pct));
        act->setCheckable(true);
        act->setChecked(pct == savedScale);
        scaleGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, pct] {
            applyUiScale(pct);
        });
    }
    scaleMenu->addSeparator();
    auto* zoomInAct = scaleMenu->addAction("Zoom In");
    zoomInAct->setShortcut(QKeySequence("Ctrl+="));
    connect(zoomInAct, &QAction::triggered, this, [this] { stepUiScale(+1); });
    auto* zoomOutAct = scaleMenu->addAction("Zoom Out");
    zoomOutAct->setShortcut(QKeySequence("Ctrl+-"));
    connect(zoomOutAct, &QAction::triggered, this, [this] { stepUiScale(-1); });
    auto* zoomResetAct = scaleMenu->addAction("Reset (100%)");
    zoomResetAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(zoomResetAct, &QAction::triggered, this, [this] { applyUiScale(100); });

    auto* resetOrderAct = viewMenu->addAction("Reset Applet Order");
    connect(resetOrderAct, &QAction::triggered, this, [this] {
        m_appletPanel->resetOrder();
    });

    auto* pskMapAction = viewMenu->addAction("PSK Reporter...");
    pskMapAction->setMenuRole(QAction::NoRole);
    connect(pskMapAction, &QAction::triggered,
            this, &MainWindow::showPskReporterMapDialog);

#ifdef HAVE_WEBSOCKETS
    {
        auto* fdvReporterAct = viewMenu->addAction(tr("FreeDV Reporter..."));
        connect(fdvReporterAct, &QAction::triggered, this, &MainWindow::showFreeDvReporter);
    }
#endif

    viewMenu->addSeparator();
    m_minimalModeAction = viewMenu->addAction("Minimal Mode\tCtrl+M");
    m_minimalModeAction->setCheckable(true);
    m_minimalModeAction->setChecked(
        AppSettings::instance().value("MinimalModeEnabled", "False").toString() == "True");
    connect(m_minimalModeAction, &QAction::toggled, this, [this](bool on) {
        toggleMinimalMode(on);
    });

    auto* framelessAct = viewMenu->addAction("Frameless Window");
    framelessAct->setCheckable(true);
    framelessAct->setShortcut(QKeySequence("Ctrl+Shift+F"));
    framelessAct->setToolTip(
        "Hide the OS title bar.  Drag the AetherSDR title bar to move,\n"
        "double-click to maximize, or use the min/max/close buttons on\n"
        "the right.  Toggle off if your compositor mishandles it.");
    framelessAct->setChecked(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    connect(framelessAct, &QAction::toggled, this, [this](bool on) {
        setFramelessWindow(on);
    });

    auto* propForecastAct = viewMenu->addAction("Propagation Conditions");
    propForecastAct->setCheckable(true);
    propForecastAct->setChecked(
        AppSettings::instance().value("PropForecastEnabled", "False").toString() == "True");
    connect(propForecastAct, &QAction::toggled, this, [this](bool on) {
        AppSettings::instance().setValue("PropForecastEnabled", on ? "True" : "False");
        AppSettings::instance().save();
        // Enable/disable the client (timer only runs when on)
        m_propForecast->setEnabled(on);
        // Show/hide the overlay on all panadapters immediately
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            applet->spectrumWidget()->setPropForecastVisible(on);
        }
        // If turning off, clear the stale values so they don't reappear
        if (!on) {
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                applet->spectrumWidget()->setPropForecast(-1, -1, -1);
            }
        }
    });

    auto* packetDecoderAction = viewMenu->addAction("AetherModem...");
    packetDecoderAction->setMenuRole(QAction::NoRole);
    connect(packetDecoderAction, &QAction::triggered,
            this, &MainWindow::showAx25HfPacketDecodeDialog);

    auto* smartSpotAct = viewMenu->addAction("Smart Spot Filtering");
    smartSpotAct->setCheckable(true);
    smartSpotAct->setToolTip(
        "Dim SSB spots that have no detected voice signal within ±1 kHz.\n"
        "Spots on active frequencies remain at full brightness;\n"
        "unoccupied spots fade to 20% opacity (default, adjustable in\n"
        "SpotHub → Display → Signal History).  CW and digital spots\n"
        "are unaffected.  Requires Signal History to be enabled.");
    m_smartSpotFilterEnabled =
        AppSettings::instance().value("SmartSpotFilterEnabled", "False").toString() == "True";
    smartSpotAct->setChecked(m_smartSpotFilterEnabled);
    connect(smartSpotAct, &QAction::toggled, this, [this](bool on) {
        m_smartSpotFilterEnabled = on;
        if (on) m_smartSpotFilterEnabledMs = QDateTime::currentMSecsSinceEpoch();
        for (auto* a : m_panStack->allApplets())
            a->spectrumWidget()->setSmartSpotFilter(on, m_smartSpotFilterEnabledMs);
        AppSettings::instance().setValue("SmartSpotFilterEnabled", on ? "True" : "False");
        AppSettings::instance().save();
    });

    auto* fpsMetersAct = viewMenu->addAction("FPS Meters");
    fpsMetersAct->setCheckable(true);
    fpsMetersAct->setShortcut(QKeySequence("Ctrl+F"));
    fpsMetersAct->setChecked(
        AppSettings::instance().value("DisplayFpsMeters", "False").toString() == "True");
    connect(fpsMetersAct, &QAction::toggled, this, [this](bool on) {
        AppSettings::instance().setValue("DisplayFpsMeters", on ? "True" : "False");
        AppSettings::instance().save();
        if (!m_panStack)
            return;
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (auto* sw = applet->spectrumWidget())
                sw->setShowFpsMeters(on);
        }
    });

    m_keyboardShortcutsEnabled = AppSettings::instance()
        .value("KeyboardShortcutsEnabled", "False").toString() == "True";
    auto* kbAct = viewMenu->addAction("Keyboard Shortcuts");
    kbAct->setCheckable(true);
    kbAct->setChecked(m_keyboardShortcutsEnabled);
    connect(kbAct, &QAction::toggled, this, [this](bool on) {
        m_keyboardShortcutsEnabled = on;
        s_keyboardShortcutsEnabled = on;
        AppSettings::instance().setValue("KeyboardShortcutsEnabled", on ? "True" : "False");
        AppSettings::instance().save();
    });
    auto* configShortcutsAct = viewMenu->addAction("Configure Shortcuts...");
    configShortcutsAct->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    connect(configShortcutsAct, &QAction::triggered, this, [this] {
        ShortcutDialog dlg(&m_shortcutManager, this);
        dlg.exec();
        // Rebuild shortcuts in case bindings changed
        m_shortcutManager.rebuildShortcuts(this, shortcutGuard);
    });

    viewMenu->addSeparator();
    auto* heartbeatBlinkAct = viewMenu->addAction("Blink Status Indicator");
    heartbeatBlinkAct->setCheckable(true);
    heartbeatBlinkAct->setChecked(
        AppSettings::instance().value("HeartbeatBlinkEnabled", "True").toString() == "True");
    connect(heartbeatBlinkAct, &QAction::toggled, this, [this](bool on) {
        if (m_titleBar) m_titleBar->setBlinkEnabled(on);
    });
    // Keep the menu item in sync when the right-click on the indicator changes the setting
    if (m_titleBar) {
        connect(m_titleBar, &TitleBar::blinkEnabledChanged,
                heartbeatBlinkAct, &QAction::setChecked);
    }

    auto* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("What's New...", this, [this]() {
        if (m_whatsNewDialog) {
            m_whatsNewDialog->show();
            m_whatsNewDialog->raise();
            m_whatsNewDialog->activateWindow();
            return;
        }
        m_whatsNewDialog = WhatsNewDialog::showAll(this);
        m_whatsNewDialog->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        m_persistentDialogs.append(QPointer<PersistentDialog>(m_whatsNewDialog));
    });
    helpMenu->addSeparator();
    helpMenu->addAction("Getting Started...", this, [this]() {
        auto* dlg = new HelpDialog("Getting Started", ":/help/getting-started.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addAction("AetherSDR Help...", this, [this]() {
        auto* dlg = new HelpDialog("AetherSDR Help", ":/help/aethersdr-help.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addAction("Understanding Noise Cancellation...", this, [this]() {
        auto* dlg = new HelpDialog("Understanding Noise Cancellation", ":/help/understanding-noise-cancellation.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    auto* controlsHelpAction = helpMenu->addAction("Configuring AetherSDR Controls...", this, [this]() {
        auto* dlg = new HelpDialog("Configuring AetherSDR Controls", ":/help/configuring-aethersdr-controls.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    controlsHelpAction->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    auto* dataModesAction = helpMenu->addAction("Configuring Data Modes...", this, [this]() {
        auto* dlg = new HelpDialog("Configuring Data Modes", ":/help/understanding-data-modes.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    dataModesAction->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    helpMenu->addAction("Contributing to AetherSDR...", this, [this]() {
        auto* dlg = new HelpDialog("Contributing to AetherSDR", ":/help/contributing-to-aethersdr.md", this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addSeparator();
    helpMenu->addAction(QString::fromUtf8("Submit your idea... \xF0\x9F\x92\xA1"),
                        this, [this]() {
        if (m_titleBar) m_titleBar->showFeatureRequestDialog();
    });
    helpMenu->addAction("Support...", this, [this]() {
        auto* dlg = new SupportDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setRadioModel(&m_radioModel);
        dlg->show();
        dlg->raise();
    });
    helpMenu->addAction("Slice Troubleshooting...", this, [this]() {
        SliceTroubleshootingDialog dlg(&m_radioModel, m_audio, this,
                                       [this]() { return buildControlDevicesSnapshot(); },
                                       [this]() {
                                           QJsonObject renderer;
                                           renderer["available"] = true;
                                           renderer["description"] = spectrum()
                                               ? spectrum()->rendererDescription()
                                               : QStringLiteral("No active pan");
                                           return renderer;
                                       });
        dlg.exec();
    });
    helpMenu->addAction("Check for Updates...", this, [this]() {
        m_updateChecker->checkNow();
    });
    helpMenu->addSeparator();
    helpMenu->addAction("About AetherSDR", this, [this]{
        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle("About AetherSDR");
        dlg->setFixedWidth(380);
        AetherSDR::ThemeManager::instance().applyStyleSheet(dlg, "QDialog { background: {{color.background.0}}; }");

        auto* vbox = new QVBoxLayout(dlg);
        vbox->setSpacing(8);
        vbox->setContentsMargins(16, 16, 16, 16);

        // Icon
        auto* iconLbl = new QLabel;
        iconLbl->setPixmap(QPixmap(":/icon.png").scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        iconLbl->setAlignment(Qt::AlignCenter);
        vbox->addWidget(iconLbl);

        // Header
        // The git SHA captured at CMake configure time identifies the build —
        // useful when bug-reporting against a dev/test build that doesn't
        // correspond to a tagged release.  See CMakeLists.txt for the capture
        // and the file-top #define for the non-CMake-build fallback.
        const QString rendererDescription = [this]() {
            if (SpectrumWidget* sw = spectrum()) {
                return sw->rendererDescription();
            }
            return QStringLiteral("No active pan");
        }();
        auto* header = new QLabel(QString(
            "<div style='text-align:center;'>"
            "<h2 style='margin-bottom:2px; color:#c8d8e8;'>AetherSDR</h2>"
            "<p style='margin-top:0; color:#8aa8c0;'>v%1<br>"
            "<span style='font-size:10px; color:#6a8090;'>(%4)</span></p>"
            "<p style='margin-top:8px; color:#c8d8e8;'>Linux-native SmartSDR-compatible client<br>"
            "for FlexRadio transceivers.</p>"
            "<p style='font-size:11px; color:#6a8090;'>"
            "Built with Qt %2 &middot; C++20<br>"
            "Compiled: %3<br>"
            "Renderer: %5</p>"
            "</div>")
            .arg(QCoreApplication::applicationVersion(), qVersion(),
                 QStringLiteral(__DATE__),
                 QStringLiteral(AETHER_GIT_SHA),
                 rendererDescription.toHtmlEscaped()));
        header->setAlignment(Qt::AlignCenter);
        header->setWordWrap(true);
        // Tooltip explains the staleness possibility — the SHA is baked at
        // CMake configure time, so a dev who runs `cmake --build` after a
        // new commit without re-configuring sees the previous SHA here.
        // Re-running `cmake --fresh` (or deleting CMakeCache.txt) captures
        // the current HEAD. The renderer line comes from the active pan at
        // dialog-open time, after Qt has picked a real QRhi backend when the
        // GPU path is active.
        header->setToolTip(
            QStringLiteral("Build identity and active pan renderer. SHA is captured at CMake "
                           "configure time — re-run `cmake -B build` after "
                           "a new commit if you need the current value."));
        vbox->addWidget(header);

        // Separator
        auto* sep1 = new QFrame;
        sep1->setFrameShape(QFrame::HLine);
        AetherSDR::ThemeManager::instance().applyStyleSheet(sep1, "color: {{color.background.2}};");
        vbox->addWidget(sep1);

        // Contributors label
        auto* contribTitle = new QLabel("<b style='color:#c8d8e8;'>Contributors</b>");
        contribTitle->setAlignment(Qt::AlignCenter);
        vbox->addWidget(contribTitle);

        // Scrollable contributors list
        auto* contribLabel = new QLabel("Jeremy (KK7GWY)<br>Claude &middot; Anthropic<br>rfoust<br>Ian (M7HNF)<br>VE3NEM<br>jensenpat<br>chibondking<br>Dependabot");
        contribLabel->setAlignment(Qt::AlignCenter);
        AetherSDR::ThemeManager::instance().applyStyleSheet(contribLabel, "QLabel { color: {{color.text.primary}}; font-size: 11px; }");
        contribLabel->setWordWrap(true);

        auto* scroll = new QScrollArea;
        scroll->setWidget(contribLabel);
        scroll->setWidgetResizable(true);
        scroll->setFixedHeight(80);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        AetherSDR::ThemeManager::instance().applyStyleSheet(scroll, "QScrollArea { background: {{color.background.0}}; border: 1px solid {{color.background.1}}; border-radius: 4px; }"
            "QScrollBar:vertical { background: {{color.background.0}}; width: 6px; }"
            "QScrollBar::handle:vertical { background: {{color.background.2}}; border-radius: 3px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
        vbox->addWidget(scroll);

        // Separator
        auto* sep2 = new QFrame;
        sep2->setFrameShape(QFrame::HLine);
        AetherSDR::ThemeManager::instance().applyStyleSheet(sep2, "color: {{color.background.2}};");
        vbox->addWidget(sep2);

        // Footer
        auto* footer = new QLabel(
            "<div style='text-align:center;'>"
            "<p style='font-size:11px; color:#8aa8c0;'>"
            "&copy; 2026 AetherSDR Contributors<br>"
            "Licensed under "
            "<a href='https://www.gnu.org/licenses/gpl-3.0.html' style='color:#00b4d8;'>GPLv3</a></p>"
            "<p style='font-size:11px;'>"
            "<a href='https://github.com/aethersdr/AetherSDR' style='color:#00b4d8;'>"
            "github.com/aethersdr/AetherSDR</a></p>"
            "<p style='font-size:10px; color:#6a8090;'>"
            "SmartSDR protocol &copy; FlexRadio Systems</p>"
            "<p style='font-size:10px; color:#6a8090;'>"
            "HF propagation forecasts provided by "
            "<a href='https://www.hamqsl.com/' style='color:#8aa8c0;'>hamqsl.com</a></p>"
            "</div>");
        footer->setAlignment(Qt::AlignCenter);
        footer->setOpenExternalLinks(true);
        footer->setWordWrap(true);
        vbox->addWidget(footer);

        // OK button
        auto* okBtn = new QPushButton("OK");
        AetherSDR::ThemeManager::instance().applyStyleSheet(okBtn, "QPushButton { background: {{color.accent}}; color: {{color.background.0}}; font-weight: bold; "
            "border-radius: 4px; padding: 6px 24px; }"
            "QPushButton:hover { background: {{color.accent.bright}}; }");
        connect(okBtn, &QPushButton::clicked, dlg, &QDialog::close);
        vbox->addWidget(okBtn, 0, Qt::AlignCenter);

        dlg->show();

        // Fetch live contributor list from GitHub API
        auto* nam = new QNetworkAccessManager(dlg);
        auto* reply = nam->get(QNetworkRequest(
            QUrl("https://api.github.com/repos/aethersdr/AetherSDR/contributors")));
        connect(reply, &QNetworkReply::finished, dlg, [contribLabel, reply] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            auto doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) return;
            QStringList names;
            names << "Jeremy (KK7GWY)" << "Claude &middot; Anthropic";
            for (const auto& val : doc.array()) {
                auto obj = val.toObject();
                QString login = obj.value("login").toString();
                if (login.isEmpty() || login == "ten9876") continue;
                if (login.contains("[bot]"))
                    login = login.replace("[bot]", "");
                if (!names.contains(login))
                    names << login;
            }
            contribLabel->setText(names.join("<br>"));
        });
    });
}

} // namespace AetherSDR
