// MainWindow_Session.cpp — radio-session wiring for MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 2c). Holds the three
// constructor wiring blocks that constitute "a connected radio":
//
//   • wireDiscovery(): LAN discovery, heartbeat / TCP-ping disconnect
//     detection, SmartLink (WAN)
//   • wireRadioModel(): RadioModel connection-state + status routing,
//     TX audio stream IDs for the DAX TX path
//   • wirePanLifecycle(): panadapter stream → spectrum widgets, S-history
//     markers, multi-pan create/destroy/rearrange lifecycle
//
// THIS FILE IS THE SEED OF RadioSession. The #3351 / #3445 (multi-radio)
// plan extracts a RadioSession aggregate owning {RadioModel, TciServer,
// CatPort[], connection lifecycle}; the methods here are the wiring that
// moves onto that class. Keep additions session-scoped — window-chrome
// wiring belongs elsewhere.
//
// Extracted verbatim from the constructor; each method is called at its
// original constructor position, so construction order is unchanged.

#include "MainWindow.h"

#include "AetherialAudioStrip.h"
#include "AppletPanel.h"
#include "ConnectedStationsDialog.h"
#include "ConnectionPanel.h"
#include "PhoneCwApplet.h"
#include "SpectrumOverlayMenu.h"
#include "core/CwSidetoneGenerator.h"
#include "core/CwTrace.h"
#include "core/CwxLocalKeyer.h"
#include "core/IambicKeyer.h"
#include "core/PerfTelemetry.h"
#if defined(Q_OS_MAC)
#include "core/VirtualAudioBridge.h"
#elif defined(HAVE_PIPEWIRE)
#include "core/PipeWireAudioBridge.h"
#endif
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#endif
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "CatControlApplet.h"
#include "ClientChainApplet.h"
#include "DaxApplet.h"
#include "DaxIqApplet.h"
#include "TciApplet.h"
#include "PanadapterStack.h"
#include "SMeterWidget.h"
#include "core/ThemeManager.h"
#include "SpectrumWidget.h"
#include "TitleBar.h"
#include "core/AppSettings.h"
#include "core/AutomationBridgeSettings.h"
#include "core/AutomationServer.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QMessageBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
QString defaultPanLayoutForCount(int panCount)
{
    static const QMap<int, QString> kDefaultLayouts = {
        {1, QStringLiteral("1")},
        {2, QStringLiteral("2v")},
        {3, QStringLiteral("2h1")},
        {4, QStringLiteral("2x2")},
        {5, QStringLiteral("3h2")},
        {6, QStringLiteral("2x3")},
        {7, QStringLiteral("4h3")},
        {8, QStringLiteral("2x4")}
    };
    return kDefaultLayouts.value(panCount, QStringLiteral("1"));
}
constexpr qint64 kXvtrWaterfallDecisionLogIntervalMs = 20000;
constexpr int kProfileLoadMinRenderableFrameBins = 128;

bool profileLoadFrameLooksRenderable(const SpectrumWidget* spectrum, int binCount)
{
    if (binCount <= 0 || !panPixelDimensionsReady(spectrum)) {
        return false;
    }

    // This is only a coarse sanity floor for profile-load release. Vertical
    // FFT scale agreement is enforced separately via radio-reported y_pixels;
    // horizontal bin counts can be reprojected by SpectrumWidget.
    return binCount >= kProfileLoadMinRenderableFrameBins;
}

void logXvtrWaterfallDecision(quint32 streamId,
                              const QString& panId,
                              double panCenterMhz,
                              double originalLowMhz,
                              double originalHighMhz,
                              const XvtrPolicy::WaterfallTileRange& mapped,
                              const XvtrPolicy::WaterfallTileMatch& match,
                              bool hasXvtrSliceAntenna,
                              const QVector<XvtrPolicy::Transverter>& xvtrs)
{
    if (!lcConnection().isDebugEnabled())
        return;

    const QString reason = mapped.shifted
        ? (match.matched ? QStringLiteral("matched_xvtr_offset")
                         : QStringLiteral("xvt_slice_antenna_fallback"))
        : QStringLiteral("no_xvtr_evidence");
    const QString key = QStringLiteral("%1:%2").arg(streamId).arg(panId);
    const QString signature = QStringLiteral("%1:%2:%3:%4")
        .arg(reason)
        .arg(mapped.shifted ? QStringLiteral("shifted") : QStringLiteral("unchanged"))
        .arg(match.matched ? match.name : QStringLiteral("(none)"))
        .arg(hasXvtrSliceAntenna ? QStringLiteral("xvt_ant") : QStringLiteral("no_xvt_ant"));

    struct LogState {
        QString signature;
        qint64 lastLoggedMs{0};
    };
    static QHash<QString, LogState> logStateByStream;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto& state = logStateByStream[key];
    if (state.signature == signature &&
        state.lastLoggedMs > 0 &&
        now - state.lastLoggedMs < kXvtrWaterfallDecisionLogIntervalMs) {
        return;
    }
    state.signature = signature;
    state.lastLoggedMs = now;

    qCDebug(lcConnection).noquote().nospace()
        << "WaterfallXVTR: stream=0x" << QString::number(streamId, 16)
        << " pan=" << panId
        << " reason=" << reason
        << " shifted=" << mapped.shifted
        << " pan_center_mhz=" << QString::number(panCenterMhz, 'f', 6)
        << " tile_mhz=" << QString::number(originalLowMhz, 'f', 6)
        << ".." << QString::number(originalHighMhz, 'f', 6)
        << " mapped_mhz=" << QString::number(mapped.lowMhz, 'f', 6)
        << ".." << QString::number(mapped.highMhz, 'f', 6)
        << " observed_offset_mhz=" << QString::number(match.observedOffsetMhz, 'f', 6)
        << " expected_offset_mhz=" << (match.matched
               ? QString::number(match.expectedOffsetMhz, 'f', 6)
               : QStringLiteral("n/a"))
        << " tolerance_mhz=" << QString::number(match.toleranceMhz, 'f', 6)
        << " matched_xvtr=" << (match.matched
               ? QStringLiteral("%1[idx=%2 order=%3]")
                     .arg(match.name.isEmpty() ? QStringLiteral("(unnamed)") : match.name)
                     .arg(match.index)
                     .arg(match.order)
               : QStringLiteral("(none)"))
        << " has_xvt_slice_antenna=" << hasXvtrSliceAntenna
        << " candidates=" << xvtrListSummary(xvtrs);
}

} // namespace

void MainWindow::wireDiscovery()
{
    // ── Wire up discovery ──────────────────────────────────────────────────
    connect(&m_discovery, &RadioDiscovery::radioDiscovered,
            m_connPanel, &ConnectionPanel::onRadioDiscovered);
    connect(&m_discovery, &RadioDiscovery::radioUpdated,
            m_connPanel, &ConnectionPanel::onRadioUpdated);
    connect(&m_discovery, &RadioDiscovery::radioUpdated,
            this, [this](const RadioInfo& info) {
        if (!m_radioModel.isConnected() || m_radioModel.serial() != info.serial)
            return;

        m_radioModel.mergeKnownGuiClients(info.guiClientHandles,
                                          info.guiClientPrograms,
                                          info.guiClientStations,
                                          info.guiClientIps,
                                          info.guiClientHosts);
    });
    connect(&m_discovery, &RadioDiscovery::radioLost,
            m_connPanel, &ConnectionPanel::onRadioLost);
    connect(m_connPanel, &ConnectionPanel::retryDiscoveryRequested, this, [this] {
        m_connPanel->setStatusText("Searching your local network…");
        if (m_titleBar) m_titleBar->setDiscovering(true);
        m_discovery.stopListening();
        m_discovery.startListening();
    });
    connect(m_connPanel, &ConnectionPanel::networkDiagnosticsRequested,
            this, &MainWindow::showNetworkDiagnosticsDialog);

    // ── Heartbeat indicator + disconnect detection via TCP ping ─────────
    m_heartbeatMissTimer = new QTimer(this);
    m_heartbeatMissTimer->setInterval(1500);
    connect(m_heartbeatMissTimer, &QTimer::timeout, this, [this]() {
        if (m_titleBar) m_titleBar->onHeartbeatLost();
    });

    // Ping-based heartbeat — covers local, routed, and SmartLink connections
    connect(&m_radioModel, &RadioModel::pingReceived, this, [this]() {
        if (m_titleBar) {
            m_titleBar->onHeartbeat();
            m_heartbeatMissTimer->start(); // reset miss timer
        }
    });

    connect(m_connPanel, &ConnectionPanel::connectRequested,
            this, [this](const RadioInfo& info){
        QList<quint32> disconnectHandles;
        if (!confirmClientSlotAvailability(info, &disconnectHandles)) {
            m_connPanel->setStatusText("Connection canceled");
            setPanadapterConnectionAnimation(false);
            return;
        }
        m_radioModel.setPendingClientDisconnects(disconnectHandles);
        m_connPanel->setStatusText("Connecting…");
        m_userDisconnected = false;
        setPanadapterConnectionAnimation(true, "Connecting to radio…");
        m_radioModel.connectToRadio(info);
        auto& s = AppSettings::instance();
        s.setValue("LastConnectedRadioSerial", info.serial);
        if (info.isRouted) {
            s.setValue("LastRoutedRadioIp", info.address.toString());
        } else {
            s.remove("LastRoutedRadioIp");
        }
        s.save();
    });

    // Start the AetherModem KISS TNC headlessly at launch if the user enabled
    // "Start TNC on Startup" — constructs the (hidden, persistent) AetherModem
    // window so the TCP server runs without the window being opened. Deferred so
    // the audio engine and main window are fully up first.
    QTimer::singleShot(0, this, [this] { startKissTncOnStartupIfConfigured(); });

    // Auto-connect: when a radio is discovered, check if it matches the last one
    connect(&m_discovery, &RadioDiscovery::radioDiscovered,
            this, [this](const RadioInfo& info) {
        if (m_userDisconnected) return;
        if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_NO_AUTOCONNECT")) {
            return;
        }
        if (AppSettings::instance().value("AutoConnectToLastRadio", "True").toString() != "True")
            return;
        const QString lastSerial = AppSettings::instance()
            .value("LastConnectedRadioSerial").toString();
        if (!lastSerial.isEmpty() && info.serial == lastSerial
            && !m_radioModel.isConnected()) {
            QList<quint32> disconnectHandles;
            if (!confirmClientSlotAvailability(info, &disconnectHandles)) {
                m_userDisconnected = true;
                m_connPanel->setStatusText("Connection canceled");
                setPanadapterConnectionAnimation(false);
                return;
            }
            m_radioModel.setPendingClientDisconnects(disconnectHandles);
            qDebug() << "Auto-connecting to" << info.displayName();
            m_connPanel->setStatusText("Auto-connecting…");
            setPanadapterConnectionAnimation(true, "Connecting to radio…");
            m_radioModel.connectToRadio(info);
        }
    });
    connect(m_connPanel, &ConnectionPanel::disconnectRequested,
            this, [this]{
        m_userDisconnected = true;
        m_wanReconnectTimer.stop();
        m_wanReconnectAttemptInProgress = false;
        setPanadapterConnectionAnimation(false);
        auto& s = AppSettings::instance();
        s.remove("LastConnectedRadioSerial");
        s.remove("LastRoutedRadioIp");
        s.save();
        m_radioModel.disconnectFromRadio();
    });

    // ── SmartLink ──────────────────────────────────────────────────────────
    m_connPanel->setSmartLinkClient(&m_smartLink);
    m_wanReconnectTimer.setInterval(5000);
    m_wanReconnectTimer.setSingleShot(true);
    connect(&m_wanReconnectTimer, &QTimer::timeout,
            this, &MainWindow::requestWanReconnect);
    connect(&m_smartLink, &SmartLinkClient::authFailed,
            this, [this](const QString& err) {
        if (!m_wanReconnectTimer.isActive() || m_pendingWanRadio.serial.isEmpty()
                || m_radioModel.isConnected()) {
            return;
        }

        m_wanReconnectTimer.stop();
        m_wanReconnectAttemptInProgress = false;
        m_connPanel->setStatusText("SmartLink sign-in required");
        statusBar()->showMessage("SmartLink reconnect stopped: " + err, 5000);
        setPanadapterConnectionAnimation(false);
        if (m_reconnectDlg) {
            QDialog* reconnectDialog = m_reconnectDlg;
            m_reconnectDlg = nullptr;
            reconnectDialog->close();
            reconnectDialog->deleteLater();
        }
        m_connPanel->show();
    });
    connect(&m_smartLink, &SmartLinkClient::serverConnected,
            this, [this] {
        m_wanReconnectAttemptInProgress = false;
    });
    connect(&m_smartLink, &SmartLinkClient::serverDisconnected,
            this, [this] {
        m_wanReconnectAttemptInProgress = false;
    });

    connect(m_connPanel, &ConnectionPanel::smartLinkLoginRequested,
            this, [this](const QString& email, const QString& pass) {
        m_smartLink.login(email, pass);
    });

    // WAN radio connect: ask SmartLink server for a handle, then TLS to radio
    connect(m_connPanel, &ConnectionPanel::wanConnectRequested,
            this, [this](const WanRadioInfo& info) {
        startWanRadioConnect(info);
    });
    connect(m_connPanel, &ConnectionPanel::wanDisconnectClientsRequested,
            this, [this](const WanRadioInfo& info) {
        disconnectWanRadioClients(info);
    });
    connect(&m_smartLink, &SmartLinkClient::radioListReceived,
            this, [this](const QList<WanRadioInfo>& radios) {
        if (!m_radioModel.isConnected() || !m_radioModel.isWan())
            return;

        for (const auto& info : radios) {
            if (info.serial != m_pendingWanRadio.serial)
                continue;

            m_radioModel.mergeKnownGuiClients(splitClientField(info.guiClientHandles),
                                              splitClientField(info.guiClientPrograms),
                                              splitClientField(info.guiClientStations),
                                              splitClientField(info.guiClientIps),
                                              splitClientField(info.guiClientHosts));
            break;
        }
    });

    // SmartLink server says radio is ready — connect via TLS
    connect(&m_smartLink, &SmartLinkClient::connectReady,
            this, [this](const QString& handle, const QString& serial) {
        if (serial != m_pendingWanRadio.serial) return;
        m_wanReconnectAttemptInProgress = false;
        m_connPanel->setStatusText("TLS connecting to radio…");
        setPanadapterConnectionAnimation(true, "Connecting to remote radio…");
        m_wanConnection.connectToRadio(
            m_pendingWanRadio.publicIp,
            static_cast<quint16>(m_pendingWanRadio.publicTlsPort),
            handle);
    });

    // WAN connection established — wire to RadioModel
    // TODO: RadioModel needs to accept WanConnection as an alternative
    // to RadioConnection. For now, log the event.
    connect(&m_wanConnection, &WanConnection::connected, this, [this] {
        qDebug() << "MainWindow: WAN connection established!";
        m_wanReconnectTimer.stop();
        m_wanReconnectAttemptInProgress = false;
        m_connPanel->setStatusText("Connected via SmartLink");
        m_connPanel->setConnected(true);

        // Wire WanConnection to RadioModel for full operation
        m_radioModel.connectViaWan(&m_wanConnection,
            m_pendingWanRadio.publicIp,
            static_cast<quint16>(m_pendingWanRadio.publicUdpPort > 0
                ? m_pendingWanRadio.publicUdpPort : 4993));
    });
    connect(&m_wanConnection, &WanConnection::disconnected, this, [this] {
        qDebug() << "MainWindow: WAN connection lost";
        m_wanReconnectAttemptInProgress = false;
        m_connPanel->setStatusText("SmartLink disconnected");
        m_connPanel->setConnected(false);
        if (m_userDisconnected) {
            setPanadapterConnectionAnimation(false);
        }
    });
    connect(&m_wanConnection, &WanConnection::errorOccurred, this, [this](const QString& err) {
        m_connPanel->setStatusText("SmartLink error: " + err);
        if (!m_reconnectDlg)
            setPanadapterConnectionAnimation(false);
    });

}

void MainWindow::wireRadioModel()
{
    // ── Wire up radio model ────────────────────────────────────────────────
    connect(&m_radioModel, &RadioModel::connectionStateChanged,
            this, &MainWindow::onConnectionStateChanged);
    connect(&m_radioModel, &RadioModel::connectionError,
            this, &MainWindow::onConnectionError);
    connect(&m_radioModel, &RadioModel::certFingerprintMismatch,
            this, &MainWindow::onWanCertFingerprintMismatch);
    connect(&m_radioModel, &RadioModel::forcedDisconnectRequested,
            this, [this] {
        const bool wasWan = m_radioModel.isWan();
        const RadioInfo radioInfo = m_radioModel.lastRadioInfo();
        const WanRadioInfo wanInfo = m_pendingWanRadio;

        m_userDisconnected = true;
        m_connPanel->setStatusText("Disconnected by another client");
        setPanadapterConnectionAnimation(false);
        showForcedDisconnectDialog(wasWan, radioInfo, wanInfo);
    });
    connect(&m_radioModel, &RadioModel::multiFlexConflictDetected, this, [this] {
        ConnectedStationsDialog::RadioMeta meta;
        meta.model    = m_radioModel.model();
        meta.nickname = m_radioModel.nickname();
        meta.callsign = m_radioModel.callsign();

        QList<ConnectedStationsDialog::Client> sdClients;
        const quint32 ours = m_radioModel.ourClientHandle();
        for (auto it = m_radioModel.clientInfoMap().cbegin();
             it != m_radioModel.clientInfoMap().cend(); ++it) {
            if (it.key() == ours)
                continue;
            ConnectedStationsDialog::Client c;
            c.handle  = it.key();
            c.program = it->program;
            c.station = it->station;
            sdClients.append(c);
        }

        ConnectedStationsDialog* dlg = new ConnectedStationsDialog(meta, sdClients, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [this, dlg] {
            const quint32 handle = dlg->selectedHandle();
            if (handle != 0)
                m_radioModel.resolveMultiFlexConflict(handle);
            else
                m_radioModel.cancelMultiFlexConflict();
        });
        connect(dlg, &QDialog::rejected, this, [this] {
            m_userDisconnected = true;
            m_connPanel->setStatusText("Connection canceled");
            setPanadapterConnectionAnimation(false);
            m_radioModel.cancelMultiFlexConflict();
        });
        dlg->show();
    });
    connect(&m_radioModel, &RadioModel::sliceAdded,
            this, &MainWindow::onSliceAdded);
    connect(&m_radioModel, &RadioModel::sliceRemoved,
            this, &MainWindow::onSliceRemoved);
    connect(&m_radioModel, &RadioModel::memoryChanged,
            this, &MainWindow::syncMemorySpot);
    connect(&m_radioModel, &RadioModel::memoryRemoved,
            this, &MainWindow::removeMemorySpot);
    connect(&m_radioModel, &RadioModel::memoriesCleared,
            this, &MainWindow::clearMemorySpotFeed);
    connect(&m_radioModel, &RadioModel::memoryChanged,
            this, [this](int) { refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::memoryRemoved,
            this, [this](int) { refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::memoriesCleared,
            this, [this]() { refreshMemoryBrowsePanel(); });
    // Keep the MEM button target-slice badge in sync with slice topology
    // changes so the displayed letter always matches which slice a
    // save/recall will route to (#1781).  Active-slice changes are
    // handled inside setActiveSlice() itself since RadioModel has no
    // activeSliceChanged signal.
    connect(&m_radioModel, &RadioModel::sliceAdded,
            this, [this](SliceModel*) { refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::sliceRemoved,
            this, [this](int) { refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::panadapterLimitReached,
            this, [this](int limit, const QString& model) {
        statusBar()->showMessage(
            QString("%1 supports a maximum of %2 panadapters")
                .arg(model).arg(limit), 4000);
    });
    connect(&m_radioModel, &RadioModel::sliceCreateFailed,
            this, [this](int limit, const QString& model) {
        statusBar()->showMessage(
            QString("%1 supports a maximum of %2 slices across all connected clients")
                .arg(model).arg(limit), 4000);
    });
    connect(&m_radioModel, &RadioModel::radioMessageReceived,
            this, &MainWindow::onRadioMessage);
    connect(&m_radioModel, &RadioModel::profileLoadStarted,
            this, &MainWindow::beginProfileLoadRadioStateWriteHold);
    connect(&m_radioModel, &RadioModel::profileLoadCompleted,
            this, &MainWindow::scheduleProfileLoadRecovery);
    connect(&m_radioModel.spotModel(), &SpotModel::spotsCleared,
            this, &MainWindow::rebuildMemorySpotFeed);

    // ── TX audio stream: set stream ID for DAX TX path ──────────────────
    // DAX TX audio is sent via PanadapterStream::sendToRadio() (the
    // registered VITA-49 socket).  We do NOT start a separate mic TX
    // stream — that would open a QAudioSource and an unregistered UDP
    // socket, wasting resources and corrupting the shared packet counter.
    // Route TX VITA-49 packets through the registered UDP socket
    connect(m_audio, &AudioEngine::txPacketReady,
            m_radioModel.panStream(), &PanadapterStream::sendToRadio);

    connect(&m_radioModel, &RadioModel::txAudioStreamReady,
            this, [this](quint32 streamId) {
        m_audio->setTxStreamId(streamId);
        // TX audio on remote_audio_tx always requires Opus (radio enforces compression=OPUS)
        m_audio->setOpusTxEnabled(true);
        qDebug() << "MainWindow: DAX TX stream ID set to" << Qt::hex << streamId;
        // Start PC audio TX if mic_selection is PC
        if (m_radioModel.transmitModel().micSelection() == "PC") {
            audioStartTx(m_radioModel.radioAddress(), 4991);
        }
    });
    connect(&m_radioModel, &RadioModel::remoteTxStreamReady,
            this, [this](quint32 streamId) {
        m_audio->setRemoteTxStreamId(streamId);
        // Radio always forces Opus for remote_audio_tx (confirmed v1.4.0.0)
        m_audio->setOpusTxEnabled(true);
        // Only the PC mic path needs local audio capture. For radio-side mic
        // selections, remote_audio_tx still exists for VOX/met_in_rx, but the
        // radio owns the actual input path. Starting QAudioSource here on
        // macOS pins Bluetooth output in telephony mode.
        if (m_radioModel.transmitModel().micSelection() == "PC") {
            // Restore PC mic gain from client-side settings (radio has no
            // hardware gain stage for PC input — client-authoritative)
            int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
            m_audio->setPcMicGain(gain);
            if (!m_audio->isTxStreaming()) {
                audioStartTx(m_radioModel.radioAddress(), 4991);
            }
        } else if (m_audio->isTxStreaming()) {
            audioStopTx();
        }
        qDebug() << "MainWindow: remote audio TX stream ID set to" << Qt::hex << streamId;
    });
    // Start/stop PC audio TX when mic_selection changes
    connect(&m_radioModel.transmitModel(), &TransmitModel::micStateChanged,
            this, [this]() {
#ifdef Q_OS_MAC
        const bool allowBluetoothTelephonyOutput =
            m_radioModel.transmitModel().micSelection() == "PC";
        QMetaObject::invokeMethod(m_audio, [this, allowBluetoothTelephonyOutput]() {
            m_audio->setAllowBluetoothTelephonyOutput(allowBluetoothTelephonyOutput);
        }, Qt::QueuedConnection);
#endif
        if (m_radioModel.transmitModel().micSelection() == "PC") {
            // Restore PC mic gain from client-side settings
            int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
            m_audio->setPcMicGain(gain);
            // Only start if a TX stream is already assigned (avoid streamId=0).
            // Voice TX (USB/LSB/AM/FM) flows over remote_audio_tx, NOT dax_tx —
            // gating solely on txStreamId() (the dax_tx id) meant that when no
            // DAX bridge was running, switching mic_selection to PC for plain
            // SSB never started mic capture, so onTxAudioReady never fired and
            // there was no modulating audio. Accept either stream.
            if (!m_audio->isTxStreaming() && m_audio->hasAnyTxStream()) {
                audioStartTx(m_radioModel.radioAddress(), 4991);
            }
        } else {
            // Reset to full gain — radio handles hardware mic gain
            m_audio->setPcMicGain(100);
            audioStopTx();
        }
        // PooDoo Audio readiness indicator — turn the chain widget's
        // MIC endpoint green when the TX input is actually flowing
        // through the client DSP chain: mic source = PC AND radio
        // DAX TX is off.  Any other combination routes audio around
        // our DSP, so the green cue would mislead.  The TX pulse
        // is gated on the same readiness so it only fires when
        // PooDoo is actually doing something during transmit.
        if (m_appletPanel && m_appletPanel->clientChainApplet()) {
            const auto& tx = m_radioModel.transmitModel();
            const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
            m_appletPanel->clientChainApplet()->setMicInputReady(ready);
            m_appletPanel->clientChainApplet()->setTxActive(
                ready && tx.isTransmitting());
            if (m_aetherialStrip) {
                m_aetherialStrip->setMicInputReady(ready);
                m_aetherialStrip->setTxActive(ready && tx.isTransmitting());
            }

            // If the user pulls the plug on readiness mid-recording
            // (mic source away from PC, or DAX back on), stop the
            // recording — auto-play kicks in via recordingStopped.
            if (!ready && m_finalMonitor && m_finalMonitor->isRecording()) {
                m_finalMonitor->stopRecording();
            }
        }
    });
#ifdef Q_OS_MAC
    const bool allowBluetoothTelephonyOutput =
        m_radioModel.transmitModel().micSelection() == "PC";
    QMetaObject::invokeMethod(m_audio, [this, allowBluetoothTelephonyOutput]() {
        m_audio->setAllowBluetoothTelephonyOutput(allowBluetoothTelephonyOutput);
    }, Qt::QueuedConnection);
#endif
    // Sync PC mic gain directly from slider. In RADE mode, the radio's mic input
    // is unused — the slider controls client-side gain regardless of mic_selection.
    connect(m_appletPanel->phoneCwApplet(), &PhoneCwApplet::micLevelChanged,
            this, [this](int level) {
        if (m_radioModel.transmitModel().micSelection() == "PC" || m_audio->isRadeMode()) {
            m_audio->setPcMicGain(level);
            auto& s = AppSettings::instance();
            s.setValue("PcMicGain", level);
            s.save();
        }
    });

    // Local CW sidetone — wire UI controls to the AudioEngine generator.
    // Initial state is loaded into the generator from AppSettings on first
    // connect; UI signals push subsequent changes live (atomic in DSP).
    {
        auto* pca = m_appletPanel->phoneCwApplet();
        // The single Sidetone toggle drives both engines at once.
        connect(pca, &PhoneCwApplet::sidetoneEnabledChanged,
                this, [this](bool on) {
            if (m_audio && m_audio->cwSidetone())
                m_audio->cwSidetone()->setEnabled(on);
        });
        connect(pca, &PhoneCwApplet::sidetoneVolumeChanged,
                this, [this](int pct) {
            if (m_audio && m_audio->cwSidetone())
                m_audio->cwSidetone()->setVolume(pct / 100.0f);
        });
        // Mirror the radio's iambic state into our local keyer — when the
        // operator toggles the existing "Iambic" button, we run the local
        // state machine for sub-5 ms sidetone latency.  The radio still
        // produces the on-air signal; we just drive the sidetone gate
        // ahead of the round trip.
        auto syncLocalKeyerToRadio = [this]() {
            if (!m_iambicKeyer) return;
            auto& tx = m_radioModel.transmitModel();
            const bool wantOn = tx.cwIambic();
            m_iambicKeyer->setMode(tx.cwIambicMode() == 0
                                       ? IambicKeyer::Mode::IambicA
                                       : IambicKeyer::Mode::IambicB);
            m_iambicKeyer->setWpm(tx.cwSpeed());
            if (wantOn && !m_iambicKeyer->isRunning()) {
                m_iambicKeyer->start();
            } else if (!wantOn && m_iambicKeyer->isRunning()) {
                m_iambicKeyer->stop();
            }
        };
        connect(&m_radioModel.transmitModel(), &TransmitModel::phoneStateChanged,
                this, syncLocalKeyerToRadio);

        // Mirror the radio's CW state into the local sidetone generator —
        // sidetone enable, volume (mon_gain_cw), and pitch all follow the
        // radio.  The radio is authoritative; we just stay in lockstep.
        auto syncLocalSidetoneToRadio = [this]() {
            if (!m_audio || !m_audio->cwSidetone()) return;
            auto& tx = m_radioModel.transmitModel();
            auto* gen = m_audio->cwSidetone();
            gen->setEnabled(tx.cwSidetone());
            gen->setVolume(tx.monGainCw() / 100.0f);
            gen->setPitchHz(static_cast<float>(tx.cwPitch()));
            gen->setPan(tx.monPanCw() / 100.0f);
        };
        connect(&m_radioModel.transmitModel(), &TransmitModel::phoneStateChanged,
                this, syncLocalSidetoneToRadio);
        syncLocalSidetoneToRadio();

        // CWX local keyer — when the user fires text/macros via CWX, this
        // generates a matching dit-dah pattern locally and routes it
        // through the same sidetone generator.  Keeps in sync with the
        // radio's keyer because both run at the configured WPM.  Drift
        // tolerance is high — we're not transmitting, just providing the
        // operator with audible feedback of what they sent.
        // The keyer runs its element schedule on its own worker thread, timed
        // against steady_clock rather than a QTimer, so panadapter paint /
        // VITA-49 burst pressure on the GUI event loop can't gap CW elements
        // (#3623) — same reason the iambic keyer below avoids QTimer.
        m_cwxLocalKeyer = std::make_unique<CwxLocalKeyer>();
        m_cwxLocalKeyer->setOnKeyDownChange([this](bool down) {
            // Lock-free atomic gate; safe to call directly from the keyer
            // thread, matching the iambic keyer's gate path below.
            if (m_audio)
                m_audio->setCwKeyDown(down);   // keys audible + recorder sidetone
        });
        connect(&m_radioModel.cwxModel(), &CwxModel::transmissionRequested,
                this, [this](const QString& text, int wpm) {
            if (m_cwxLocalKeyer) m_cwxLocalKeyer->start(text, wpm);
        });
        connect(&m_radioModel.cwxModel(), &CwxModel::transmissionCancelled,
                this, [this]() {
            if (m_cwxLocalKeyer) m_cwxLocalKeyer->stop();
        });

        // Local iambic keyer — when the radio's iambic mode is on, this
        // state machine runs in parallel and drives the local sidetone gate
        // at sub-5 ms latency (the radio's keyed-back signal carries 50–200
        // ms of round-trip jitter that's painful for paddle ops).  The radio
        // still produces the on-air signal; we forward paddle states to it,
        // and both engines run at the same WPM to stay phase-aligned.
        m_iambicKeyer = std::make_unique<IambicKeyer>();
        m_iambicKeyer->setOnKeyDownChange([this](bool down) {
            // Drive the local sidetone gate (lock-free atomic on the audio
            // thread) and the radio's per-element key edge in parallel.
            // The radio sees `cw key 1` / `cw key 0` matching our element
            // timing — same RF pattern the radio's own iambic engine
            // would have produced from a hardware paddle.
            if (m_audio)
                m_audio->setCwKeyDown(down);   // keys audible + recorder sidetone
            const quint64 traceId = m_lastCwPaddleTraceId.load(std::memory_order_relaxed);
            const quint64 sourceMs = m_lastCwPaddleSourceMs.load(std::memory_order_relaxed);
            if (lcCw().isDebugEnabled()) {
                const quint64 now = cwTraceNowMs();
                qCDebug(lcCw).noquote().nospace()
                    << "CW iambic key-edge trace=" << traceId
                    << " t=" << now << "ms"
                    << " sinceSourceMs=" << (sourceMs ? static_cast<qint64>(now - sourceMs) : -1)
                    << " down=" << down;
            }
            QMetaObject::invokeMethod(this, [this, down]() {
                const quint64 traceId = m_lastCwPaddleTraceId.load(std::memory_order_relaxed);
                const quint64 sourceMs = m_lastCwPaddleSourceMs.load(std::memory_order_relaxed);
                m_radioModel.sendCwKeyEdge(down, QStringLiteral("cw:iambic-keyer"),
                                           traceId, sourceMs);
            }, Qt::QueuedConnection);
        });
        m_iambicKeyer->setOnPaddleEvent([this](bool dit, bool dah) {
            // The radio's break-in setting decides whether key edges produce
            // RF. With break_in=1 (QSK), `cw key 1` from setOnKeyDownChange
            // triggers TX and break_in_delay holds the relay between
            // elements. With break_in=0, the operator engages PTT manually
            // (Space PTT, MOX, hardware PTT) before keying. Either way, the
            // iambic keyer should not auto-PTT — doing so would override
            // break-in OFF and force-drop the QSK hang on release.
            const bool active = dit || dah;
            const quint64 traceId = m_lastCwPaddleTraceId.load(std::memory_order_relaxed);
            const quint64 sourceMs = m_lastCwPaddleSourceMs.load(std::memory_order_relaxed);
            if (lcCw().isDebugEnabled()) {
                const quint64 now = cwTraceNowMs();
                qCDebug(lcCw).noquote().nospace()
                    << "CW iambic paddle-event trace=" << traceId
                    << " t=" << now << "ms"
                    << " sinceSourceMs=" << (sourceMs ? static_cast<qint64>(now - sourceMs) : -1)
                    << " dit=" << dit
                    << " dah=" << dah
                    << " active=" << active;
            }
        });
        // Initial sync after callbacks are installed. Without this, the
        // default/radio-reported iambic-on state may never emit a change.
        syncLocalKeyerToRadio();
    }

    // TX/RX transition → audio source switching
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        // Keep TX audio source strictly aligned with the local MOX edge for all
        // modes (SSB + DAX). Waiting for interlock introduces audible lag.
        if (m_audio) {
#ifdef HAVE_RADE
            // In RADE mode, defer setTransmitting(false) to the interlock
            // txAudioGateChanged fallback so the PTT gate stays open until
            // the EOO frame clears the AudioEngine queue.
            bool radeDefer = !tx && m_radeSliceId >= 0 && m_radeEngine && m_radeEngine->isActive();
            qCDebug(lcRade) << "MainWindow: moxChanged(" << tx << ")"
                            << "radeActive=" << (m_radeSliceId >= 0 && m_radeEngine && m_radeEngine->isActive())
                            << "deferringSetTransmitting=" << radeDefer;
            if (!radeDefer)
                m_audio->setTransmitting(tx);
#else
            m_audio->setTransmitting(tx);
#endif
        }
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
        if (m_daxBridge)
            m_daxBridge->setTransmitting(tx);
#endif
#ifdef HAVE_SERIALPORT
        QMetaObject::invokeMethod(m_serialPort, [this, tx] { m_serialPort->setTransmitting(tx); });
#endif
    });

    // Interlock fallback gate:
    // we only consume TX-off here, as a safety net if local edge updates
    // are missed while interlock transitions.
    connect(&m_radioModel, &RadioModel::txAudioGateChanged,
            this, [this](bool tx) {
        if (!tx) {
            if (m_audio) {
#ifdef HAVE_RADE
                // In RADE mode the EOO frame is still in the AudioEngine queue
                // when this fires (RadioModel emits it synchronously with moxChanged).
                // Suppress now; eooFinished posts setTransmitting(false) to the
                // AudioEngine queue after the EOO packets.
                if (m_radeSliceId >= 0 && m_radeEngine && m_radeEngine->isActive()) {
                    qCDebug(lcRade) << "MainWindow: txAudioGateChanged(false) suppressed — RADE EOO pending";
                } else {
                    m_audio->setTransmitting(false);
                }
#else
                m_audio->setTransmitting(false);
#endif
            }
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
            if (m_daxBridge)
                m_daxBridge->setTransmitting(false);
#endif
        }
    });

    // Raw radio TX state: fired for every interlock state=TRANSMITTING regardless
    // of TX ownership. Used for DAX passthrough (#752) and the TX status bar
    // indicator — moxChanged is ownership-gated so it misses external PTT and
    // Multi-Flex TX from other clients.
    connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
            this, [this](bool tx) {
        if (m_audio) {
            m_audio->setRadioTransmitting(tx);
        }
        // Waterfall freeze/unfreeze: gate on the actual interlock TRANSMITTING
        // state, not the MOX edge. moxChanged fires the instant the user releases
        // PTT, but the radio keeps streaming TX-contaminated tiles/FFT for the
        // UNKEY_REQUESTED window — those rows then take 10–23s to scroll off
        // the visible waterfall (#1927). Driving from radioTransmittingChanged
        // also fixes Multi-Flex: the freeze now triggers when any client TXes,
        // not just when this client owns MOX.
        for (auto* pan : m_radioModel.panadapters()) {
            if (auto* sw = m_panStack ? m_panStack->spectrum(pan->panId()) : nullptr)
                sw->setTransmitting(tx);
        }
        if (!m_panStack && m_panApplet)
            m_panApplet->spectrumWidget()->setTransmitting(tx);
        // S-Meter: use raw interlock state so Level/Compression modes work
        // during VOX/hardware CW without the effectiveTx power threshold (#877)
        m_appletPanel->sMeterWidget()->setTransmitting(tx);
        if (!tx) {
            m_appletPanel->phoneCwApplet()->updateCompression(0.0f);
            m_appletPanel->phoneCwApplet()->updateAlc(-20.0f);
        }
        if (tx) {
            AetherSDR::ThemeManager::instance().applyStyleSheet(m_txIndicator, "QLabel { color: white; background: {{color.accent.danger}}; font-weight: bold; "
                "font-size: 21px; border-radius: 4px; padding: 0px 1px; }");
        } else {
            m_txIndicator->setStyleSheet(
                "QLabel { color: rgba(255,255,255,128); font-weight: bold; "
                "font-size: 21px; }");
        }
    });

    // Sync show-TX-in-waterfall setting to all spectrum widgets
    auto syncShowTxWf = [this]() {
        bool show = m_radioModel.transmitModel().showTxInWaterfall();
        for (auto* pan : m_radioModel.panadapters()) {
            if (auto* sw = m_panStack ? m_panStack->spectrum(pan->panId()) : nullptr)
                sw->setShowTxInWaterfall(show);
        }
        if (!m_panStack && m_panApplet)
            m_panApplet->spectrumWidget()->setShowTxInWaterfall(show);
    };
    connect(&m_radioModel.transmitModel(), &TransmitModel::stateChanged,
            this, syncShowTxWf);

}

void MainWindow::wirePanLifecycle()
{
    // ── Panadapter stream → spectrum widget ───────────────────────────────
    // Route FFT/waterfall data to the correct SpectrumWidget by stream ID
    auto profileLoadFrameReady = [this](const QString& panId,
                                        SpectrumWidget* sw,
                                        int binCount) {
        if (!profileLoadRadioStateWritesHeld()
            && !profileLoadPanDisplaySettling(panId)) {
            return true;
        }

        if (m_pendingProfileLoadPanDimensions.contains(panId)) {
            if (!profileLoadRadioStateWritesHeld()) {
                flushPendingProfileLoadPanDimensions();
            }
            return false;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (auto expectedIt = m_profileLoadPendingFftYpixels.find(panId);
            expectedIt != m_profileLoadPendingFftYpixels.end()) {
            auto deadlineIt = m_profileLoadPanDimensionsSettlingUntilMs.find(panId);
            if (deadlineIt == m_profileLoadPanDimensionsSettlingUntilMs.end()
                || nowMs < deadlineIt.value()) {
                return false;
            }
            if (!profileLoadPanDimensionsMatchExpected(panId, sw)) {
                retryProfileLoadPanDimensions(panId, sw);
                return false;
            }
            releaseProfileLoadPanDisplayHold(panId, sw);
        } else if (auto it = m_profileLoadPanDimensionsSettlingUntilMs.find(panId);
            it != m_profileLoadPanDimensionsSettlingUntilMs.end()) {
            if (nowMs < it.value()) {
                return false;
            }
            releaseProfileLoadPanDisplayHold(panId, sw);
        }

        return profileLoadFrameLooksRenderable(sw, binCount);
    };

    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, [this, profileLoadFrameReady](quint32 streamId,
                                                const QVector<float>& bins,
                                                qint64 emittedNs) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        if (emittedNs > 0) {
            PerfTelemetry::instance().recordFrameAge(
                PerfTelemetry::FrameKind::Panadapter,
                static_cast<double>(PerfTelemetry::nowNs() - emittedNs) / 1000000.0);
        }
        deferReceivePresentation(
            ReceivePresentationSource::Flex,
            ReceivePresentationSurface::Spectrum,
            [this, profileLoadFrameReady, streamId, bins]() {
                if (m_shuttingDown || !m_panStack) {
                    return;
                }
                for (auto* pan : m_radioModel.panadapters()) {
                    if (pan->panStreamId() == streamId) {
                        if (auto* sw = m_panStack->spectrum(pan->panId())) {
                            if (!profileLoadFrameReady(pan->panId(), sw,
                                                       bins.size())) {
                                return;
                            }
                            sw->updateSpectrum(bins);
                            finishPanadapterConnectionAnimation();
                        }
                        return;
                    }
                }
                // Fallback: active spectrum only before any radio-owned pan has
                // been claimed. During profile loads/removals, queued frames
                // from streams that were just removed can arrive after their
                // model is gone; drawing them into the current active pane
                // produces a brief bogus trace.
                if (m_radioModel.panadapters().isEmpty()
                    && !profileLoadRadioStateWritesHeld()) {
                    if (auto* sw = spectrum()) {
                        sw->updateSpectrum(bins);
                        finishPanadapterConnectionAnimation();
                    }
                } else {
                    qCDebug(lcProtocol)
                        << "MainWindow: dropped unmatched FFT stream"
                        << QStringLiteral("0x%1").arg(streamId, 0, 16);
                }
            },
            QString::number(streamId));
    });
    // ── S History Markers — tap into FFT frames for voice signal detection ──
    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, &MainWindow::onSpectrumReadyForSHistory);

    // ── Adaptive RX filter — drive the fit engine off the same FFT frames (RFC #3878)
    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, &MainWindow::onSpectrumReadyForAdaptiveFilter);

    connect(m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
            this, [this, profileLoadFrameReady](quint32 streamId,
                                                const QVector<float>& bins,
                                                double low,
                                                double high,
                                                quint32 tc,
                                                qint64 emittedNs) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        if (emittedNs > 0) {
            PerfTelemetry::instance().recordFrameAge(
                PerfTelemetry::FrameKind::Waterfall,
                static_cast<double>(PerfTelemetry::nowNs() - emittedNs) / 1000000.0);
        }
        deferReceivePresentation(
            ReceivePresentationSource::Flex,
            ReceivePresentationSurface::Waterfall,
            [this, profileLoadFrameReady, streamId, bins, low, high, tc]() {
                if (m_shuttingDown || !m_panStack) {
                    return;
                }
                for (auto* pan : m_radioModel.panadapters()) {
                    if (pan->wfStreamId() == streamId) {
                        double rowLow = low;
                        double rowHigh = high;
                        const double panCenter = pan->centerMhz();
                        if (XvtrPolicy::isWaterfallTileOutsidePan(rowLow, rowHigh,
                                                                  panCenter)) {
                            // Only reinterpret non-overlapping tile ranges for
                            // real XVTR IF/RF translation. Ordinary HF pans can
                            // briefly see stale tile centers while dragging;
                            // shifting those corrupts rows.
                            const auto xvtrs =
                                xvtrPolicyBandsFrom(m_radioModel.xvtrList());
                            const auto match =
                                XvtrPolicy::matchWaterfallTileTransverterOffset(
                                    rowLow, rowHigh, panCenter, xvtrs);
                            bool hasXvtrSliceAntenna = false;
                            if (!match.matched) {
                                for (auto* slice : m_radioModel.slices()) {
                                    if (!slice || slice->panId() != pan->panId()) {
                                        continue;
                                    }
                                    if (slice->rxAntenna().startsWith(
                                            QStringLiteral("XVT"),
                                            Qt::CaseInsensitive)) {
                                        hasXvtrSliceAntenna = true;
                                        break;
                                    }
                                }
                            }
                            const auto mapped = XvtrPolicy::mapWaterfallTileRange(
                                rowLow, rowHigh, panCenter, xvtrs,
                                hasXvtrSliceAntenna);
                            logXvtrWaterfallDecision(
                                streamId, pan->panId(), panCenter, rowLow, rowHigh,
                                mapped, match, hasXvtrSliceAntenna, xvtrs);
                            rowLow = mapped.lowMhz;
                            rowHigh = mapped.highMhz;
                        }
                        if (auto* sw = m_panStack->spectrum(pan->panId())) {
                            if (!profileLoadFrameReady(pan->panId(), sw,
                                                       bins.size())) {
                                return;
                            }
                            sw->updateWaterfallRow(bins, rowLow, rowHigh, tc);
                            finishPanadapterConnectionAnimation();
                        }
                        return;
                    }
                }
                if (m_radioModel.panadapters().isEmpty()
                    && !profileLoadRadioStateWritesHeld()) {
                    if (auto* sw = spectrum()) {
                        sw->updateWaterfallRow(bins, low, high, tc);
                        finishPanadapterConnectionAnimation();
                    }
                } else {
                    qCDebug(lcProtocol)
                        << "MainWindow: dropped unmatched waterfall stream"
                        << QStringLiteral("0x%1").arg(streamId, 0, 16);
                }
            },
            QString::number(streamId));
    });
    connect(m_radioModel.panStream(), &PanadapterStream::waterfallAutoBlackLevel,
            this, [this](quint32 streamId, quint32 autoBlack) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        for (auto* pan : m_radioModel.panadapters()) {
            if (pan->wfStreamId() == streamId) {
                if (auto* sw = m_panStack->spectrum(pan->panId())) {
                    if (sw->wfAutoBlack() && sw->wfAutoBlackRadioSide()) {
                        // Feed the radio's per-tile auto-black level straight to
                        // the renderer only when the user selected radio-side
                        // auto-black (radio-authoritative low/black point).
                        sw->setRadioAutoBlackLevel(autoBlack);
                    }
                }
                return;
            }
        }
    });
    // Legacy panadapterInfoChanged — only used for initial display settings push.
    // Per-pan frequency/level tracking is done via PanadapterModel signals in panadapterAdded.
    connect(&m_radioModel, &RadioModel::panadapterInfoChanged,
            this, [this]() {
        if (!m_displaySettingsPushed) {
            auto* sw = spectrum();
            if (!sw) return;  // pan not yet available
            m_displaySettingsPushed = true;
            m_radioModel.setPanAverage(sw->fftAverage());
            if (!m_adaptiveThrottleActive)
                m_radioModel.setPanFps(sw->fftFps());
            m_radioModel.setPanWeightedAverage(sw->fftWeightedAvg());
            m_radioModel.setWaterfallColorGain(sw->wfColorGain());
            m_radioModel.setWaterfallBlackLevel(sw->wfBlackLevel());
            m_radioModel.setWaterfallAutoBlack(sw->wfAutoBlack());
            m_radioModel.setWaterfallAutoBlackSource(sw->wfAutoBlackRadioSide());
            int rate = sw->wfLineDuration();
            if (!m_adaptiveThrottleActive)
                m_radioModel.setWaterfallLineDuration(rate);
            // Restore saved WNB and RF gain
            auto& s = AppSettings::instance();
            bool wnbOn = s.value(sw->settingsKey("DisplayWnbEnabled"), "False").toString() == "True";
            int wnbLevel = s.value(sw->settingsKey("DisplayWnbLevel"), "50").toInt();
            int rfGain = s.value(sw->settingsKey("DisplayRfGain"), "0").toInt();
            m_radioModel.setPanWnb(wnbOn);
            m_radioModel.setPanWnbLevel(wnbLevel);
            m_radioModel.setPanRfGain(rfGain);
            sw->setWnbActive(wnbOn);
            sw->setRfGain(rfGain);
            sw->overlayMenu()->setWnbState(wnbOn, wnbLevel);
            sw->overlayMenu()->setRfGain(rfGain);
            QString bgPath = s.value(sw->settingsKey("BackgroundImage")).toString();
            if (!bgPath.isEmpty() && bgPath != "none")
                sw->setBackgroundImage(bgPath);
            int bgOpacity = s.value(sw->settingsKey("BackgroundOpacity"), "80").toInt();
            sw->setBackgroundOpacity(bgOpacity);
            QColor bgFill(s.value(sw->settingsKey("BackgroundFillColor"),
                                  "#0a0a14").toString());
            if (bgFill.isValid())
                sw->setBackgroundFillColor(bgFill);
            // Restore the spectrum render mode per pan. Source-specific trace
            // positions/depths are loaded by SpectrumWidget from
            // DisplaySourceTraceSettings; do not reapply legacy flat keys here.
            sw->setSpectrumRenderMode(
                s.value(sw->settingsKey("DisplaySpectrumRenderMode"), "0").toInt());
            sw->setDssGain(
                s.value(sw->settingsKey("Display3DGain"), "70").toInt());
            // Nudge rate to force waterfall tile re-sync
            if (!m_adaptiveThrottleActive) {
                QTimer::singleShot(500, this, [this, rate]() {
                    const int nudgeRate = (rate < 100) ? rate + 1 : rate - 1;
                    m_radioModel.setWaterfallLineDuration(nudgeRate);
                    m_radioModel.setWaterfallLineDuration(rate);
                });
            }
        }
    });
    // NOTE: panadapterLevelChanged → spectrum()::setDbmRange has been removed.
    // Level updates are routed per-pan via PanadapterModel::levelChanged in
    // wirePanadapter() so that PanadapterModel's change-guard prevents stale
    // echo-backs from overwriting in-flight user changes.
    // ── Multi-panadapter lifecycle ──────────────────────────────────────────
    connect(&m_radioModel, &RadioModel::panadapterAdded,
            this, [this](PanadapterModel* pan) {
        if (m_shuttingDown || !m_panStack || !pan) {
            return;
        }
        // During layout application, applyLayout/createPansSequentially handles
        // applet creation and wiring — don't duplicate here.
        if (m_applyingLayout) return;

        // Skip if this pan already has an applet
        if (m_panStack->panadapter(pan->panId())) {
            if (auto* sw = m_panStack->spectrum(pan->panId())) {
                auto* menu = sw->overlayMenu();
                menu->setPanId(pan->panId());
                menu->setRadioModel(&m_radioModel);
                menu->setRadioCapabilities(m_radioModel.capabilities());
                connect(pan, &PanadapterModel::infoChanged,
                        sw, &SpectrumWidget::setFrequencyRange);
                connect(pan, &PanadapterModel::infoChanged,
                        this, [this, panId = pan->panId()](double, double) {
                    if (!profileLoadRadioStateWritesHeld()) {
                        recenterCenterLockForPan(panId);
                    }
                });
                connect(pan, &PanadapterModel::levelChanged,
                        sw, [sw](float minDbm, float maxDbm) {
                    if (sw->isDraggingDbmScale()) {
                        return;
                    }
                    sw->setDbmRange(minDbm, maxDbm);
                });
                connect(pan, &PanadapterModel::wideChanged,
                        sw, &SpectrumWidget::setWideActive);
                sw->setWideActive(pan->wideActive());
                connect(pan, &PanadapterModel::wnbStateChanged,
                        sw, &SpectrumWidget::syncWnbState,
                        Qt::UniqueConnection);
                connect(pan, &PanadapterModel::wnbStateChanged,
                        sw->overlayMenu(), &SpectrumOverlayMenu::syncWnbState,
                        Qt::UniqueConnection);
                sw->syncWnbState(pan->wnbActive(), pan->wnbLevel(),
                                 pan->wnbUpdating());
                sw->overlayMenu()->syncWnbState(pan->wnbActive(),
                                                pan->wnbLevel(),
                                                pan->wnbUpdating());
                // Prime the spectrum widget with the pan's current dBm range on
                // reconnect so the noise-floor auto-adjust starts from the correct
                // position. (#3034)
                sw->setDbmRange(pan->minDbm(), pan->maxDbm());
            }
            for (SliceModel* slice : m_radioModel.slices()) {
                if (slice && slice->panId() == pan->panId()) {
                    reattachSliceVisualsToPanadapter(slice);
                }
            }
            return;
        }

        PanadapterApplet* applet = nullptr;

        // If applyLayout already created this applet, just wire signals
        if (m_panStack->panadapter(pan->panId())) {
            applet = m_panStack->panadapter(pan->panId());
        }
        // Reuse the "default" placeholder for the first real pan
        else if (m_panStack->panadapter("default")) {
            applet = m_panStack->panadapter("default");
            applet->setPanId(pan->panId());
            m_panStack->rekey("default", pan->panId());
        } else {
            applet = m_panStack->addPanadapter(pan->panId());
        }
        setActivePanApplet(applet);
        wirePanadapter(applet);
        if (m_panadapterConnectionAnimationVisible) {
            applet->spectrumWidget()->setConnectionAnimationVisible(
                true, m_panadapterConnectionAnimationLabel);
        }
        connect(pan, &PanadapterModel::infoChanged,
                applet->spectrumWidget(), &SpectrumWidget::setFrequencyRange);
        connect(pan, &PanadapterModel::infoChanged,
                this, [this, panId = pan->panId()](double, double) {
            if (!profileLoadRadioStateWritesHeld()) {
                recenterCenterLockForPan(panId);
            }
        });
        // NOTE: levelChanged → setDbmRange is wired in wirePanadapter() above;
        // don't connect it here again or setDbmRange fires twice per level change.
        connect(pan, &PanadapterModel::rfGainInfoChanged,
                applet->spectrumWidget()->overlayMenu(),
                &SpectrumOverlayMenu::setRfGainRange);
        connect(pan, &PanadapterModel::rfGainChanged,
                this, [applet](int gain) {
            applet->spectrumWidget()->setRfGain(gain);
            applet->spectrumWidget()->overlayMenu()->setRfGain(gain);
        });

        // Push display dimensions to the radio so it sends full-size FFT bins.
        // Without this, the radio uses xpixels=50 ypixels=20 (default) and
        // FFT data is essentially empty/unusable. Use widget width and the
        // actual FFT pane height for 1:1 bin-to-pixel mapping.
        auto* sw = applet->spectrumWidget();
        requestPanDimensionsForRadio(pan->panId(), sw, true);

        qDebug() << "MainWindow: added panadapter applet for" << pan->panId();
        for (SliceModel* slice : m_radioModel.slices()) {
            if (slice && slice->panId() == pan->panId()) {
                reattachSliceVisualsToPanadapter(slice);
            }
        }

        // Debounced layout restore: after all pans are added on connect,
        // rearrange to the saved layout (e.g. 2h instead of default vertical).
        if (!m_layoutRestoreTimer) {
            m_layoutRestoreTimer = new QTimer(this);
            m_layoutRestoreTimer->setSingleShot(true);
            m_layoutRestoreTimer->setInterval(1000);
            connect(m_layoutRestoreTimer, &QTimer::timeout, this, [this]() {
                if (m_shuttingDown || !m_panStack) {
                    return;
                }
                // The radio restores pans from the GUIClientID session.
                // Accept whatever the radio gives and arrange based on count.
                const int panCount = m_panStack->count();
                if (!m_suppressStartupPanLayoutRearrange && panCount > 1) {
                    // Pick a layout based on the number of pans the radio restored
                    const QString saved = AppSettings::instance()
                        .value("PanadapterLayout", "1").toString();
                    const QString layoutId = panCountForLayoutId(saved) == panCount
                        ? saved
                        : defaultPanLayoutForCount(panCount);
                    const QString floatingPanIds = AppSettings::instance()
                        .value("FloatingPanIds", "").toString();
                    m_panStack->rearrangeLayout(layoutId);
                    AppSettings::instance().setValue("FloatingPanIds", floatingPanIds);

                    // Defensive re-push xpixels for all pans after layout settles.
                    // Covers race where radio hadn't finished pan init when first push arrived.
                    QTimer::singleShot(500, this, [this]() {
                        if (m_shuttingDown || !m_panStack) {
                            return;
                        }
                        for (auto* applet : m_panStack->allApplets()) {
                            auto* sw = applet->spectrumWidget();
                            auto* pan = m_radioModel.panadapter(applet->panId());
                            if (!sw || !pan) continue;
                            requestPanDimensionsForRadio(pan->panId(), sw);
                        }
                    });
                }

                // Restore floating-pan state saved from the previous session.
                // Runs for any pan count so a single floated pan is also restored.
                m_panStack->restoreFloatingState();
            });
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_layoutRestoreUntilMs == kPanLayoutRestoreWaitingForFirstPan) {
            m_layoutRestoreUntilMs = nowMs + kPanLayoutRestoreWindowMs;
        }
        if (nowMs <= m_layoutRestoreUntilMs) {
            m_layoutRestoreTimer->start();
        }
    });
    // A reclaimed (previous-session) pan keeps its applet and all the
    // model→widget wiring from its original panadapterAdded, so the full add
    // path must not run again (it would duplicate connections). But the
    // disconnect path tears down the per-pan FPS / waterfall-line-duration
    // reconcilers, so those need re-wiring here.
    connect(&m_radioModel, &RadioModel::panadapterReclaimed,
            this, [this](PanadapterModel* pan) {
        if (m_shuttingDown || !m_panStack || !pan) {
            return;
        }
        auto* applet = m_panStack->panadapter(pan->panId());
        if (!applet) {
            return;
        }
        wirePanReconcilers(applet, pan);
        for (SliceModel* slice : m_radioModel.slices()) {
            if (slice && slice->panId() == pan->panId()) {
                reattachSliceVisualsToPanadapter(slice);
            }
        }
    });
    // Re-push xpixels/ypixels when the radio requests it (profile change, reconnect, etc.)
    connect(&m_radioModel, &RadioModel::panDimensionsNeeded,
            this, [this](const QString& panId) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        auto* applet = m_panStack->panadapter(panId);
        if (!applet) return;
        auto* sw = applet->spectrumWidget();
        auto* pan = m_radioModel.panadapter(panId);
        if (!sw || !pan) return;
        requestPanDimensionsForRadio(panId, sw);
    });
    connect(&m_radioModel, &RadioModel::panadapterFftScaleChanged,
            this, [this](const QString& panId, int yPixels) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        markProfileLoadPanDimensionsReady(panId, yPixels);
        if (auto* sw = m_panStack->spectrum(panId)) {
            sw->prepareForFftScaleChange();
        }
    });

#ifdef Q_OS_MAC
    auto repushPanDimensions = [this]() {
        QTimer::singleShot(200, this, [this]() {
            if (m_shuttingDown || !m_panStack) {
                return;
            }
            for (auto* applet : m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                auto* pan = m_radioModel.panadapter(applet->panId());
                if (!sw || !pan) {
                    continue;
                }
                if (!panPixelDimensionsReady(sw)) {
                    continue;
                }
                requestPanDimensionsForRadio(pan->panId(), sw);
            }
        });
    };
    connect(m_panStack, &PanadapterStack::panFloated,
            this, [repushPanDimensions](const QString&) { repushPanDimensions(); });
    connect(m_panStack, &PanadapterStack::panDocked,
            this, [repushPanDimensions](const QString&) { repushPanDimensions(); });
#endif

    connect(&m_radioModel, &RadioModel::panadapterRemoved,
            this, [this](const QString& panId) {
        clearKiwiSdrPanDisplaySourceOverride(panId);
        clearCenterLockForPan(panId);
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        if (auto it = m_panFpsReconcileConnections.find(panId);
            it != m_panFpsReconcileConnections.end()) {
            QObject::disconnect(it.value());
            m_panFpsReconcileConnections.erase(it);
        }
        if (auto it = m_wfLineDurationReconcileConnections.find(panId);
            it != m_wfLineDurationReconcileConnections.end()) {
            QObject::disconnect(it.value());
            m_wfLineDurationReconcileConnections.erase(it);
        }
        if (auto it = m_panFpsReconcile.find(panId);
            it != m_panFpsReconcile.end()) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
            m_panFpsReconcile.erase(it);
        }
        if (auto it = m_wfLineDurationReconcile.find(panId);
            it != m_wfLineDurationReconcile.end()) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
            m_wfLineDurationReconcile.erase(it);
        }
        if (auto it = m_panAverageReconcileConnections.find(panId);
            it != m_panAverageReconcileConnections.end()) {
            QObject::disconnect(it.value());
            m_panAverageReconcileConnections.erase(it);
        }
        if (auto it = m_panWeightedAvgReconcileConnections.find(panId);
            it != m_panWeightedAvgReconcileConnections.end()) {
            QObject::disconnect(it.value());
            m_panWeightedAvgReconcileConnections.erase(it);
        }
        if (auto it = m_panAverageReconcile.find(panId);
            it != m_panAverageReconcile.end()) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
            m_panAverageReconcile.erase(it);
        }
        if (auto it = m_panWeightedAvgReconcile.find(panId);
            it != m_panWeightedAvgReconcile.end()) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
            m_panWeightedAvgReconcile.erase(it);
        }

        // Disconnect all signals from the dying applet's widgets to prevent
        // dangling pointer crashes in wirePanadapter lambdas (#242)
        if (auto* applet = m_panStack->panadapter(panId)) {
            if (auto* sw = applet->spectrumWidget()) {
                sw->disconnect(this);
                if (auto* menu = sw->overlayMenu())
                    menu->disconnect(this);
            }
        }
        m_panStack->removePanadapter(panId);
        m_sHistoryData.remove(panId);
        m_sHistoryPanState.remove(panId);
        m_spectrogramBuffers.remove(panId);
        qDebug() << "MainWindow: removed panadapter applet for" << panId;

        // Rearrange remaining pans to a sensible layout. Do not persist this
        // fallback: a temporary resource-shortage session must not overwrite
        // the user's saved multi-pan layout. This is local splitter cleanup,
        // not a radio-state write, so it must still run during profile loads
        // to avoid leaving empty nested splitter cells behind.
        int remaining = m_panStack->count();
        if (remaining > 1) {
            m_panStack->rearrangeLayout(defaultPanLayoutForCount(remaining));
        }
    });

}

void MainWindow::wireCatPorts()
{
    // ── Unified CAT ports (kCatPorts slots, configured from settings) ───────────
    // Migrate old dual-server settings to the new per-port schema on first run.
    migrateCatSettings();
    for (int i = 0; i < kCatPorts; ++i) {
        // Owned by the session — no QObject parent (parent-based deletion
        // would run after member destruction and recreate #2385).
        m_session->setCatPort(i, new CatPort(&m_radioModel, nullptr));
        // Per-user symlink path (GHSA-qxhr-cwrc-pvrm — matches RigctlPty fix).
        catPort(i)->setSymlinkPath(CatPort::defaultSymlinkPath(i));
        // Load persisted dialect and VFO config; port and enabled are read
        // in applyCatPortCount() just before starting.
        const QString prefix = QString("CatPort_%1_").arg(i);
        auto& s = AppSettings::instance();
        QString d = s.value(prefix + "Dialect", "Rigctld").toString();
        CatDialect dial = (d == "FlexCAT") ? CatDialect::FlexCAT
                        : (d == "TS2000")  ? CatDialect::TS2000
                        : CatDialect::Rigctld;
        catPort(i)->setDialect(dial);
        catPort(i)->setVfoA(s.value(prefix + "VfoA", "0").toInt());
        catPort(i)->setVfoB(s.value(prefix + "VfoB", "-1").toInt());
    }

    // Wire the applet to the port objects
    m_appletPanel->catControlApplet()->setPorts(m_session->catPortsArray(), kCatPorts);
    m_appletPanel->catControlApplet()->setMaxSlices(catPortTargetCount());

    // Wire master enable toggle from the docked applet
    connect(m_appletPanel->catControlApplet(), &CatControlApplet::enableChanged,
            this, [this](bool) { applyCatPortCount(); });

    // Per-port config changes in the floating table → re-apply port states
    connect(m_appletPanel->catControlApplet(), &CatControlApplet::configChanged,
            this, [this]() { applyCatPortCount(); });

    // Auto-start based on saved master enable
    applyCatPortCount();
    m_appletPanel->daxApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->daxIqApplet()->setRadioModel(&m_radioModel);
#ifdef HAVE_WEBSOCKETS
    // Owned by the session — no QObject parent (see RadioSession docs, #2385).
    m_session->setTciServer(new TciServer(&m_radioModel, nullptr));
    tciServer()->setAudioEngine(m_audio);
    m_appletPanel->tciApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->tciApplet()->setTciServer(tciServer());

    // TCI applet sliders → TciServer gain setters
    connect(m_appletPanel->tciApplet(), &TciApplet::tciRxGainChanged,
            tciServer(), &TciServer::setRxChannelGain);
    connect(m_appletPanel->tciApplet(), &TciApplet::tciTxGainChanged,
            tciServer(), &TciServer::setTxGain);
    connect(m_appletPanel->tciApplet(), &TciApplet::tciTxOverflowModeChanged,
            tciServer(), &TciServer::setOverflowMode);

    // TciServer level signals → TCI applet meters
    connect(tciServer(), &TciServer::rxLevel,
            m_appletPanel->tciApplet(), &TciApplet::setTciRxLevel);
    connect(tciServer(), &TciServer::txLevel,
            m_appletPanel->tciApplet(), &TciApplet::setTciTxLevel);

    // TCI `volume:N;` master-volume SET → mirror on the title bar slider
    // and route through the same applyMasterVolume() slot the slider uses
    // (audio path + persistence + broadcast back to other TCI clients).
    // See issue #1764 — no master-volume TCI hook existed before.
    connect(tciServer(), &TciServer::masterVolumeRequested,
            this, [this](int pct) {
        if (m_titleBar) m_titleBar->setMasterVolume(pct);
        applyMasterVolume(pct);
    });

    // Wire slice state changes -> TCI broadcasts. TCI receivers are contiguous
    // indexes within our owned slice list; Flex slice ids can be non-zero when
    // another client owns lower-numbered slices.
    auto wireTciSlice = [this](SliceModel* s) {
        if (!tciServer() || !s)
            return;
        const int trx = m_radioModel.slices().indexOf(s);
        tciServer()->wireSlice(trx >= 0 ? trx : s->sliceId(), s);
    };
    connect(&m_radioModel, &RadioModel::sliceAdded, this, [wireTciSlice](SliceModel* s) {
        wireTciSlice(s);
    });
    // Wire existing slices (radio may already be connected with slices)
    for (auto* s : m_radioModel.slices())
        wireTciSlice(s);
    tciServer()->wireSpotModel();

    // Wire RX audio from PanadapterStream → TCI server for audio streaming.
    // TCI audio feeds exclusively from DAX (not audioDataReady) so that
    // audio_mute doesn't kill TCI audio (#1331).
    if (m_radioModel.panStream()) {
        connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                tciServer(), &TciServer::onDaxAudioReady);
        connect(m_radioModel.panStream(), &PanadapterStream::iqDataReady,
                tciServer(), &TciServer::onIqDataReady);
        connect(m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
                tciServer(), &TciServer::onWaterfallRowReady);
    }

    // TCI client count changes no longer auto-create/remove the audio stream.
    // Control-only TCI clients (StreamDeck) don't need audio, and auto-creating
    // the stream overrode the user's explicit PC Audio toggle. Users who need
    // TCI audio (WSJT-X) should enable PC Audio manually. (#1071)
#endif

}

void MainWindow::wireDaxIq()
{
    // ── DAX IQ wiring (all platforms, established once at construction) ──
    //
    // The DAX-IQ data path is independent of the DAX *audio* bridge: it needs
    // only PanadapterStream (VITA-49 IQ routing) and the applet, both of which
    // exist at construction on every platform. Wiring it here — instead of
    // lazily inside startDax() on Mac/PipeWire — means a DAX-IQ channel works
    // without first enabling DAX audio, and the enable/disable/rate connections
    // survive a DAX-audio stop (they were never tied to the bridge's lifetime,
    // so stopDax() can't tear them down and leave a half-wired path that
    // double-creates streams). This is the single source of IQ-side wiring;
    // startDax() wires only the audio-bridge connections. (Same stream-status
    // registration need as #1820 / RADE RX on Windows.)
    if (m_appletPanel && m_appletPanel->daxIqApplet() && m_radioModel.panStream()) {
        connect(&m_radioModel, &RadioModel::statusReceived,
                this, [this](const QString& obj, const QMap<QString,QString>& kvs) {
            if (!obj.startsWith("stream ")) return;
            const QStringList parts = obj.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() < 2) return;
            bool ok = false;
            quint32 streamId = parts[1].toUInt(&ok, 0);
            if (!ok) return;
            const bool removed = parts.contains(QStringLiteral("removed"))
                              || kvs.contains(QStringLiteral("removed"));
            if (removed) {
                m_radioModel.panStream()->unregisterIqStream(streamId);
                m_radioModel.daxIqModel().handleStreamRemoved(streamId);
                qCDebug(lcDax) << "MainWindow: unregistered removed DAX IQ stream"
                               << "0x" + QString::number(streamId, 16);
                return;
            }
            if (kvs.value("type") != "dax_iq") return;
            if (!streamStatusBelongsToUs(kvs, m_radioModel.ourClientHandle())) {
                qCDebug(lcDax) << "MainWindow: ignoring DAX IQ stream for another client"
                                << "stream=0x" + QString::number(streamId, 16)
                                << "owner=" << kvs.value("client_handle");
                return;
            }
            qCDebug(lcDax) << "MainWindow: DAX IQ stream status" << obj
                           << "keys=" << kvs.keys()
                           << "ch=" << kvs.value("daxiq_channel")
                           << "ip=" << kvs.value("ip");
            m_radioModel.daxIqModel().applyStreamStatus(streamId, kvs);
            int ch = kvs.value("daxiq_channel").toInt();
            if (streamId && ch >= 1 && ch <= 4)
                m_radioModel.panStream()->registerIqStream(streamId, ch);
        });

        connect(m_radioModel.panStream(), &PanadapterStream::iqDataReady,
                &m_radioModel.daxIqModel(), &DaxIqModel::feedRawIqPacket);
        connect(&m_radioModel.daxIqModel(), &DaxIqModel::iqLevelReady,
                m_appletPanel->daxIqApplet(), &DaxIqApplet::setDaxIqLevel);
        connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqEnableRequested,
                &m_radioModel.daxIqModel(), &DaxIqModel::createStream);
        connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqDisableRequested,
                &m_radioModel.daxIqModel(), &DaxIqModel::removeStream);
        connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqRateChanged,
                &m_radioModel.daxIqModel(), &DaxIqModel::setSampleRate);
    }

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    // DAX enable button in DaxApplet → start/stop DAX bridge
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxToggled,
            this, [this](bool on) {
        if (on) {
            if (!startDax() && m_appletPanel && m_appletPanel->daxApplet())
                m_appletPanel->daxApplet()->setDaxEnabled(false);
        } else {
            stopDax();
        }
    });
#endif

}

// ── Agent automation bridge (#3646) lifecycle ────────────────────────────────
// Kept out of the MainWindow.cpp monolith. The bridge is the endpoint MCP
// clients connect to; MainWindow owns the server instance for its lifetime.

bool MainWindow::startAutomationBridge(const QString& sockName)
{
    if (m_automation && m_automation->isRunning())
        return true;  // idempotent

    // AETHER_AUTOMATION_SOCKET (or the caller's sockName) pins an explicit
    // endpoint; otherwise the default is PID-suffixed so two instances don't
    // steal each other's socket. Drivers find the right one via the discovery
    // file the server drops in the temp dir.
    QString name = sockName;
    if (name.isEmpty())
        name = qEnvironmentVariableIsSet("AETHER_AUTOMATION_SOCKET")
                   ? qEnvironmentVariable("AETHER_AUTOMATION_SOCKET")
                   : QStringLiteral("aethersdr-automation-%1")
                         .arg(QCoreApplication::applicationPid());

    if (!m_automation)
        m_automation = std::make_unique<AutomationServer>();

    m_automation->setRadioModel(&radioModel());  // for the get() verb
    m_automation->setAudioEngine(audioEngine());
    m_automation->setQsoRecorder(qsoRecorder());  // for the record() verb
    m_automation->setConnectionDialogHost(this);
    m_automation->setConnectionAutomation(
        findChild<AetherSDR::ConnectionPanel*>(QStringLiteral("connectionPanel")));
    m_automation->setSliceReceiveSourceHandler(
        [this](const QString& arg) { return automationSetSliceReceiveSource(arg); });
    m_automation->setSliceCenterLockHandler(
        [this](int sliceId, bool enabled) { return automationSetCenterLock(sliceId, enabled); });
    m_automation->setTuneHandler(
        [this](double mhz) { return automationTune(mhz); });
    m_automation->setReceiveSyncSnapshotHandler(
        [this]() { return automationReceiveSyncSnapshot(); });
    m_automation->setKiwiSdrSnapshotHandler(
        [this]() { return automationKiwiSdrSnapshot(); });
    m_automation->setTxTimerSnapshotHandler(
        [this]() { return automationTxTimerSnapshot(); });

    // The access token lives in the OS secret store (QtKeychain), which reads
    // ASYNCHRONOUSLY. Defer start()/listen() into the token callback rather
    // than opening a brief tokenless window on the socket. A QPointer guards
    // against the bridge being stopped/replaced before the read lands.
    QPointer<AutomationServer> guard(m_automation.get());
    const QString startName = name;
    AutomationBridgeSettings::loadToken(this, [this, guard, startName](const QString& tok) {
        if (!guard || m_automation.get() != guard)
            return;  // toggled off or restarted before the token arrived
        guard->setAuthToken(tok);
        if (tok.isEmpty()) {
            qWarning().noquote()
                << "Automation bridge starting UNAUTHENTICATED — any same-user "
                   "process can drive the radio. Set an access token in Radio "
                   "Setup → Network, or the AETHER_MCP_TOKEN environment variable.";
        }
        if (!guard->start(startName)) {
            qWarning() << "Automation bridge failed to start (socket in use?)";
            m_automation.reset();
            return;
        }
        // TX-automation gate — set AFTER start(), which reads
        // AETHER_AUTOMATION_ALLOW_TX into m_txAllowed and would otherwise
        // clobber a pre-start value. Fold in the persisted operator opt-in so
        // a GUI enable survives restart; setTxAllowed arms the watchdog.
        guard->setTxAllowed(
            qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX")
            || AutomationBridgeSettings::txAllowed());
    });
    return true;  // start initiated; the socket begins listening once the token resolves
}

void MainWindow::stopAutomationBridge()
{
    if (m_automation) {
        m_automation->stop();
        m_automation.reset();
    }
}

void MainWindow::setAutomationBridgeToken(const QString& token)
{
    // Persist to the OS secret store (survives restart; read by
    // startAutomationBridge). No plaintext copy in the settings file.
    AutomationBridgeSettings::saveToken(token);
    // Push live so a rotate takes effect immediately on the running bridge —
    // any client still using the old token is locked out on its next request.
    if (m_automation)
        m_automation->setAuthToken(token);
}

void MainWindow::setAutomationTxAllowed(bool allowed)
{
    // Persist the operator opt-in (nested config) so it survives restart.
    AutomationBridgeSettings::setTxAllowed(allowed);
    // Push live so toggling takes effect on a running bridge immediately —
    // disabling force-unkeys the radio; enabling arms the TX watchdog.
    if (m_automation)
        m_automation->setTxAllowed(allowed);
}

} // namespace AetherSDR
