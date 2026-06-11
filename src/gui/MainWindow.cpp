#include "MainWindow.h"

#include "MainWindowHelpers.h"

#include "CwDecodeSettings.h"
#include "DisplaySettings.h"
#ifdef HAVE_MQTT
#include "MqttApplet.h"
#include "MqttSettingsDialog.h"
#include "core/MqttAntennaAlias.h"
#include "core/MqttSettings.h"
#endif
#include "ConnectionPanel.h"
#include "Theme.h"
#include "ClientDisconnectDialog.h"
#include "ConnectedStationsDialog.h"
#include "TitleBar.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "PanLayoutDialog.h"
#include "core/CommandParser.h"
#include "core/LogManager.h"
#include "core/PerfTelemetry.h"
#include "core/PeripheralSettings.h"
#include "core/VoiceSignalDetector.h"
#include "core/MemoryRecallPolicy.h"
#include "core/StreamStatus.h"
#include "models/PanadapterModel.h"
#include "models/RadioStatusOwnership.h"
#include "SpectrumWidget.h"
#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#endif
#include "SpectrumOverlayMenu.h"
#include "VfoWidget.h"
#include "MeterSmoother.h"  // global lean-mode meter repaint throttle (#3283)
#include "AppletPanel.h"
#include "containers/ContainerManager.h"
#include "RxApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "TxApplet.h"
#include "PhoneCwApplet.h"
#include "PhoneApplet.h"
#include "EqApplet.h"
#include "WaveApplet.h"
#include "ClientEqApplet.h"
#include "ClientEqEditor.h"
#include "ClientCompApplet.h"
#include "ClientCompEditor.h"
#include "ClientGateApplet.h"
#include "ClientGateEditor.h"
#include "ClientDeEssApplet.h"
#include "ClientTubeApplet.h"
#include "ClientTubeEditor.h"
#include "ClientPuduApplet.h"
#include "ClientPuduEditor.h"
#include "ClientReverbApplet.h"
#include "AetherialAudioStrip.h"
#include "StripFinalOutputPanel.h"
#include "ClientChainApplet.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientDeEss.h"
#include "core/ClientTube.h"
#include "core/ClientPudu.h"
#include "core/ClientReverb.h"
#include "core/CwTrace.h"
#include "core/CwSidetoneGenerator.h"
#include "core/CwxLocalKeyer.h"
#include "core/IambicKeyer.h"
#include "CatControlApplet.h"
#include "DaxApplet.h"
#include "TciApplet.h"
#include "DaxIqApplet.h"
#include "AntennaGeniusApplet.h"
#include "ShackSwitchApplet.h"
#include "RadioSetupDialog.h"
#include "AgcCalibrationDialog.h"
#include "AudioDeviceChangeDialog.h"
#include "NetworkDiagnosticsDialog.h"
#include "PropDashboardDialog.h"
#include "MemoryCommands.h"
#include "MemoryDialog.h"
#include "SwrSweepLicenseDialog.h"
#include "DxClusterDialog.h"
#ifdef HAVE_WEBSOCKETS
#include "FreeDvReporterDialog.h"
#endif
#include "Ax25HfPacketDecodeDialog.h"
#include "FlexControlDialog.h"
#include "CwxPanel.h"
#include "DvkPanel.h"
#include "core/DvkWavTransfer.h"
#include "AmpApplet.h"
#include "MeterApplet.h"
#include "HealthApplet.h"
#include "PersistentDialog.h"
#include "ProfileManagerDialog.h"
#include "ProfileImportExportDialog.h"
#include "TxBandDialog.h"
#include "SupportDialog.h"
#include "SliceTroubleshootingDialog.h"
#include "ShortcutDialog.h"
#include "MultiFlexDialog.h"
#include "HelpDialog.h"
#include "ThemeEditorDialog.h"
#include "WhatsNewDialog.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include "models/BandDefs.h"
#include "models/BandPlanManager.h"
#include "models/XvtrPolicy.h"
#include "core/BandStackSettings.h"
#include "gui/BandStackPanel.h"
#include "models/TunerModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#ifdef HAVE_MIDI
#include "core/MidiSettings.h"
#include "MidiMappingDialog.h"
#endif
#ifdef HAVE_HIDAPI
#include "RC28MappingDialog.h"
#endif
#include "core/UlanziDialBackend.h"
#include "UlanziDialMapperDialog.h"
#include "AetherDspDialog.h"
#include "AetherDspWidget.h"
#include "WaveformsDialog.h"
#include "ClientRxDspApplet.h"
#include "DspParamPopup.h"
#include "GuardedSlider.h"
#include "MeterSlider.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <functional>
#include <QApplication>
#include <QAudioDevice>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QDateTime>
#include <QPropertyAnimation>
#include <QIcon>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QWindow>
#include <QPixmap>
#include <QImage>
#include <QBuffer>
#include <QFont>
#include <QWidgetAction>
#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMenuBar>
#include <QDialog>
#include <QGridLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QAbstractSlider>
#include <QLabel>
#include <QCloseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QScrollArea>
#include <QSizeGrip>
#include <QStatusBar>
#include <QFrame>
#include <QFileDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "core/VersionNumber.h"
#include "core/UpdateChecker.h"
#include <QDesktopServices>
#include <QPointer>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QProgressBar>
#include <QThread>
#include <QToolTip>
#include <QMediaDevices>
#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"
#include "core/SpotModeResolver.h"
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#include "RadeApplet.h"
#endif
#if defined(Q_OS_MAC)
#include "core/VirtualAudioBridge.h"
#include <QFileInfo>
#elif defined(HAVE_PIPEWIRE)
#include "core/PipeWireAudioBridge.h"
#endif
#include <QDebug>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#ifdef Q_OS_MAC
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#endif
#endif
#include <QLocale>
#include <QFile>
#include <QStandardPaths>
#include "core/ThemeManager.h"

// CMake captures the short git SHA at configure time and passes it as a
// preprocessor definition (see CMakeLists.txt).  Defaulted to "unknown" so
// non-CMake builds (e.g. raw clang invocations during local experiments)
// still compile.  See issue #2991 for the rationale on hoisting this to
// file scope rather than the inline definition inside buildMenuBar().
#ifndef AETHER_GIT_SHA
#define AETHER_GIT_SHA "unknown"
#endif

namespace AetherSDR {

namespace {

// Pan-follow edge-margin constants moved to MainWindow_Wiring.cpp (#3351 Phase 1d).
// kPanFollowAnimationDurationMs moved to MainWindow_Wiring.cpp (#3351 Phase 1d).
// kSliderShortcutLeaseMs moved to MainWindow_Shortcuts.cpp (#3351 Phase 1c).
constexpr int kPanadapterSliceCapacityStatusMs = 4000;
// Pan pixel-dimension constants + helpers moved to MainWindowHelpers
// (#3351 Phase 1d) — shared with MainWindow_Wiring.cpp.
constexpr qint64 kPanLayoutRestoreWaitingForFirstPan = -1;
constexpr int kPanLayoutRestoreWindowMs = 30000;
constexpr qint64 kXvtrWaterfallDecisionLogIntervalMs = 20000;
constexpr double kSwrSweepStepMhz = 0.020;
constexpr double kSwrSweepEdgeGuardMhz = 0.005;
constexpr double kSwrSweepPanPaddingMhz = 0.020;
constexpr int kSwrSweepPollMs = 50;
constexpr int kSwrSweepInitialSettleMs = 350;
constexpr int kSwrSweepStepSettleMs = 160;
constexpr int kSwrSweepMaxSettleMs = 900;
constexpr int kSwrSweepTgxlBypassTimeoutMs = 3500;
constexpr int kSwrSweepTgxlRelaySettleMs = 250;
constexpr int kSwrSweepTuneStopWaitMs = 350;
constexpr int kSwrSweepTuneStopTimeoutMs = 1800;
constexpr int kSwrSweepTgxlRestoreTimeoutMs = 3500;
constexpr int kSwrSweepMaxPoints = 260;
constexpr const char* kSuppressAudioDeviceNotificationsKey =
    "SuppressAudioDeviceNotifications";
constexpr int kTMate2DefaultUserInteractionTimeoutMs = 2000;

#ifdef HAVE_HIDAPI
QString tmate2EncoderDefaultAction(int encoderIndex)
{
    switch (encoderIndex) {
    case 0:  return QStringLiteral("WheelFrequency");
    case 1:  return QStringLiteral("WheelRit");
    case 2:  return QStringLiteral("WheelXit");
    default: return QStringLiteral("WheelFrequency");
    }
}

QString tmate2KeyDefaultAction(int keyIndex)
{
    switch (keyIndex) {
    case 0:  return QStringLiteral("ToggleMox");
    case 1:  return QStringLiteral("ToggleAgc");
    case 2:  return QStringLiteral("BandZoom");
    case 3:  return QStringLiteral("ToggleApf");
    case 4:  return QStringLiteral("ToggleMute");
    case 5:  return QStringLiteral("ToggleRit");
    default: return QStringLiteral("None");
    }
}

QString tmate2PushDefaultAction(int encoderIndex)
{
    switch (encoderIndex) {
    case 0:  return QStringLiteral("StepCycle");
    case 1:  return QStringLiteral("ToggleRit");
    case 2:  return QStringLiteral("ToggleXit");
    default: return QStringLiteral("None");
    }
}
#endif

bool isTransientAudioDeviceId(const QByteArray& id)
{
#ifdef Q_OS_LINUX
    // PipeWire/pulse-shim churns these constantly (monitor sources, per-app
    // loopbacks, fallback auto-null sink, echo-cancel/combine virtuals).
    // They are never useful as a PC mic or local speaker target; treating
    // them as "new devices" is what re-fires the dialog in #2864.
    if (id.contains(".monitor"))               return true;
    if (id.startsWith("pulse_input_loopback")) return true;
    if (id.contains("auto_null"))              return true;
    if (id.contains("echo-cancel"))            return true;
    if (id.contains("combined"))               return true;
#else
    Q_UNUSED(id);
#endif
    return false;
}

QList<QByteArray> audioDeviceIds(const QList<QAudioDevice>& devices)
{
    QList<QByteArray> ids;
    ids.reserve(devices.size());
    for (const QAudioDevice& device : devices) {
        if (isTransientAudioDeviceId(device.id()))
            continue;
        ids.append(device.id());
    }
    return ids;
}

bool containsAudioDeviceId(const QList<QByteArray>& ids, const QByteArray& id)
{
    return std::any_of(ids.cbegin(), ids.cend(),
                       [&id](const QByteArray& candidate) {
                           return candidate == id;
                       });
}

QList<QByteArray> newlyAddedAudioDeviceIds(const QList<QAudioDevice>& devices,
                                           const QList<QByteArray>& knownIds)
{
    QList<QByteArray> added;
    for (const QAudioDevice& device : devices) {
        if (isTransientAudioDeviceId(device.id()))
            continue;
        if (!containsAudioDeviceId(knownIds, device.id()))
            added.append(device.id());
    }
    return added;
}

QList<QByteArray> removedAudioDeviceIds(const QList<QByteArray>& knownIds,
                                        const QList<QByteArray>& currentIds)
{
    QList<QByteArray> removed;
    for (const QByteArray& id : knownIds) {
        if (!containsAudioDeviceId(currentIds, id))
            removed.append(id);
    }
    return removed;
}

bool audioDevicePresent(const QList<QAudioDevice>& devices,
                        const QAudioDevice& target)
{
    if (target.isNull())
        return true;

    return std::any_of(devices.cbegin(), devices.cend(),
                       [&target](const QAudioDevice& device) {
                           return device.id() == target.id();
                       });
}

bool sameAudioDeviceSelection(const QAudioDevice& lhs, const QAudioDevice& rhs)
{
    if (lhs.isNull() && rhs.isNull())
        return true;
    if (lhs.isNull() || rhs.isNull())
        return false;
    return lhs.id() == rhs.id();
}

// memoryRevealTargetMatches moved to MainWindow_Wiring.cpp (#3351 Phase 1d).

#ifdef Q_OS_WIN
bool mainWindowCustomFrameEnabled()
{
    return AppSettings::instance()
        .value("FramelessWindow", "True").toString() == "True";
}

int windowsResizeBorderThickness(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
        + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}
#endif

// flexWheelModeForAction / flexControlButtonAction moved to
// MainWindow_Controllers.cpp (#3351 Phase 1a) — only controller code calls them.


// panCountForLayoutId moved to MainWindowHelpers (#3351 Phase 1c).

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


// xvtrPolicyBandsFrom / xvtrListSummary / xvtrForBandSummary moved to
// MainWindowHelpers (#3351 Phase 1d) — shared with MainWindow_Wiring.cpp.

// parseStatusHandle / streamStatusBelongsToUs  → core/StreamStatus.h

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

// quantizeIncrementalFollowDelta moved to MainWindow_Wiring.cpp (#3351 Phase 1d).

}  // namespace

// Pure formatting / parsing helpers formerly defined here as file-scope
// statics now live in MainWindowHelpers.{h,cpp} (#3351 Phase 0). Only
// helpers coupled to the mutable shortcut-lease state below remain.

// ─── Shortcut guard (file-scope for use as std::function<bool()>) ───────────

static constexpr const char* kPaTempUnitSettingKey = "PaTempDisplayUnit";
// kCw*ActionId/Name constants moved to MainWindowHelpers.h (#3351 Phase 1a)
// — now shared with the MIDI/HID registries in MainWindow_Controllers.cpp.

// s_keyboardShortcutsEnabled / s_sliderShortcutLeaseActive definitions lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).

static bool isCwMomentaryActionId(const QString& id)
{
    return id == QLatin1String(kCwStraightKeyActionId)
        || id == QLatin1String(kCwLeftPaddleActionId)
        || id == QLatin1String(kCwRightPaddleActionId);
}

// Shortcut-state helpers (textInputCaptured/shortcutGuard/...) lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
bool MainWindow::confirmClientSlotAvailability(const RadioInfo& info,
                                               QList<quint32>* disconnectHandles)
{
    if (disconnectHandles)
        disconnectHandles->clear();

    const auto clients = buildDisconnectClients(info);

    // When multiFLEX is disabled, any connected client blocks us — show the
    // Connected Stations dialog so the user can disconnect them first.
    if (!info.multiFlexEnabled && !clients.isEmpty()) {
        ConnectedStationsDialog::RadioMeta meta;
        meta.model    = info.model;
        meta.nickname = info.nickname;
        meta.callsign = info.callsign;

        QList<ConnectedStationsDialog::Client> sdClients;
        for (const auto& c : clients) {
            ConnectedStationsDialog::Client sc;
            sc.handle  = c.handle;
            sc.program = c.program;
            sc.station = c.station;
            sdClients.append(sc);
        }

        ConnectedStationsDialog dialog(meta, sdClients, this);
        if (dialog.exec() != QDialog::Accepted)
            return false;

        const quint32 handle = dialog.selectedHandle();
        if (handle == 0)
            return false;

        if (disconnectHandles)
            *disconnectHandles = {handle};
        return true;
    }

    const int maxSlices = RadioModel::maxSlicesForModel(info.model);
    if (clients.isEmpty() || clients.size() < maxSlices)
        return true;

    ClientDisconnectDialog dialog(clients, maxSlices, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    if (disconnectHandles)
        *disconnectHandles = dialog.selectedHandles();
    return !dialog.selectedHandles().isEmpty();
}

bool MainWindow::confirmClientSlotAvailability(const WanRadioInfo& info,
                                               QList<quint32>* disconnectHandles)
{
    if (disconnectHandles)
        disconnectHandles->clear();

    const auto clients = buildDisconnectClients(info);

    // licensedClients == 1 means the radio's multiFLEX license allows only one
    // simultaneous client — effectively mf_enable=0 from the SmartLink perspective.
    // WanRadioInfo defaults to 1 when licensed_clients is absent from the SmartLink
    // response (older firmware, partial parse), so this gate is fail-safe: it blocks
    // rather than allows.  Log when we hit the default so field reports are diagnosable.
    if (info.licensedClients <= 1 && !clients.isEmpty()) {
        if (info.licensedClients == 1)
            qCWarning(lcGui) << "MainWindow: WAN licensedClients=1 (may be default) — "
                                "showing conflict dialog as a precaution";
        ConnectedStationsDialog::RadioMeta meta;
        meta.model    = info.model;
        meta.nickname = info.nickname;
        meta.callsign = info.callsign;

        QList<ConnectedStationsDialog::Client> sdClients;
        for (const auto& c : clients) {
            ConnectedStationsDialog::Client sc;
            sc.handle  = c.handle;
            sc.program = c.program;
            sc.station = c.station;
            sdClients.append(sc);
        }

        ConnectedStationsDialog dialog(meta, sdClients, this);
        if (dialog.exec() != QDialog::Accepted)
            return false;

        const quint32 handle = dialog.selectedHandle();
        if (handle == 0)
            return false;

        if (disconnectHandles)
            *disconnectHandles = {handle};
        return true;
    }

    const int maxSlices = RadioModel::maxSlicesForModel(info.model);
    if (clients.isEmpty() || clients.size() < maxSlices)
        return true;

    ClientDisconnectDialog dialog(clients, maxSlices, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    if (disconnectHandles)
        *disconnectHandles = dialog.selectedHandles();
    return !dialog.selectedHandles().isEmpty();
}

bool MainWindow::sendWanRadioClientDisconnects(const QString& serial,
                                               const QList<quint32>& handles)
{
    if (!m_smartLink.isConnected()) {
        m_connPanel->setStatusText("SmartLink is not connected");
        statusBar()->showMessage("SmartLink is not connected.", 4000);
        return false;
    }

    if (serial.trimmed().isEmpty()) {
        m_connPanel->setStatusText("SmartLink radio serial unavailable");
        return false;
    }

    if (handles.isEmpty()) {
        m_connPanel->setStatusText("No remote clients to disconnect");
        return false;
    }

    m_smartLink.disconnectRadioClients(serial, handles);
    return true;
}

void MainWindow::disconnectWanRadioClients(const WanRadioInfo& info)
{
    const auto clients = buildDisconnectClients(info);
    if (clients.isEmpty()) {
        m_connPanel->setStatusText("No remote clients to disconnect");
        statusBar()->showMessage("No remote clients are currently reported for that radio.", 4000);
        return;
    }

    ClientDisconnectDialog dialog(clients,
                                  RadioModel::maxSlicesForModel(info.model),
                                  this,
                                  ClientDisconnectDialog::Mode::RemoteClientDisconnect);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QList<quint32> handles = dialog.selectedHandles();
    if (handles.isEmpty())
        return;

    if (sendWanRadioClientDisconnects(info.serial, handles)) {
        m_connPanel->setStatusText("Disconnect request sent");
        statusBar()->showMessage("Remote client disconnect request sent through SmartLink.", 4000);
    }
}

void MainWindow::showMultiFlexDialog()
{
    MultiFlexDialog dlg(&m_radioModel, this);
    connect(&dlg, &MultiFlexDialog::disconnectClientRequested,
            this, &MainWindow::handleMultiFlexClientDisconnect);
    dlg.exec();
}

void MainWindow::handleMultiFlexClientDisconnect(quint32 handle, const QString& displayName)
{
    if (handle == 0 || handle == m_radioModel.ourClientHandle())
        return;

    QString name = cleanClientDisplayText(displayName);
    if (name.isEmpty())
        name = QString("client 0x%1").arg(handle, 8, 16, QChar('0')).toUpper();

    if (m_radioModel.isWan()) {
        const QString serial = !m_pendingWanRadio.serial.isEmpty()
            ? m_pendingWanRadio.serial
            : m_radioModel.serial();
        if (sendWanRadioClientDisconnects(serial, {handle})) {
            m_connPanel->setStatusText("Disconnect request sent");
            statusBar()->showMessage(
                QString("SmartLink disconnect request sent for %1.").arg(name), 4000);
        }
        return;
    }

    if (m_radioModel.disconnectClient(handle))
        statusBar()->showMessage(QString("Disconnect request sent for %1.").arg(name), 4000);
}

void MainWindow::startWanRadioConnect(const WanRadioInfo& info, bool promptForClientSlots)
{
    QList<quint32> disconnectHandles;
    if (promptForClientSlots && !confirmClientSlotAvailability(info, &disconnectHandles)) {
        m_connPanel->setStatusText("Connection canceled");
        setPanadapterConnectionAnimation(false);
        return;
    }

    m_userDisconnected = false;
    m_radioModel.setKnownGuiClients(splitClientField(info.guiClientHandles),
                                    splitClientField(info.guiClientPrograms),
                                    splitClientField(info.guiClientStations),
                                    splitClientField(info.guiClientIps),
                                    splitClientField(info.guiClientHosts));
    m_radioModel.setPendingClientDisconnects(disconnectHandles);
    m_connPanel->setStatusText("Requesting SmartLink connection…");
    setPanadapterConnectionAnimation(true, "Connecting to remote radio…");
    // Store WAN radio info for when connect_ready arrives
    m_pendingWanRadio = info;

    // Pre-bind UDP socket for VITA-49 reception BEFORE requesting
    // connection, so we can pass our port to the SmartLink server.
    // The server tells the radio our public IP:port for UDP streaming.
    quint16 udpPort = m_radioModel.panStream()->localPort();
    if (udpPort == 0) {
        // Not yet bound — start WAN early to get a port
        const quint16 radioUdpPort = static_cast<quint16>(
            info.publicUdpPort > 0 ? info.publicUdpPort : 4993);
        auto* ps = m_radioModel.panStream();
        QMetaObject::invokeMethod(ps, [ps, info, radioUdpPort]() {
            ps->startWan(QHostAddress(info.publicIp), radioUdpPort);
        }, Qt::BlockingQueuedConnection);
        udpPort = ps->localPort();
    }
    qDebug() << "MainWindow: pre-bound UDP port" << udpPort << "for WAN hole punch";
    auto requestSmartLinkConnect = [this, serial = info.serial, udpPort] {
        if (serial != m_pendingWanRadio.serial)
            return;
        m_smartLink.requestConnect(serial, udpPort);
    };
    if (!disconnectHandles.isEmpty()) {
        sendWanRadioClientDisconnects(info.serial, disconnectHandles);
        QTimer::singleShot(350, this, requestSmartLinkConnect);
    } else {
        requestSmartLinkConnect();
    }
}

void MainWindow::requestWanReconnect()
{
    if (m_userDisconnected || m_radioModel.isConnected()
            || m_pendingWanRadio.serial.isEmpty()) {
        m_wanReconnectTimer.stop();
        m_wanReconnectAttemptInProgress = false;
        return;
    }

    if (m_wanReconnectAttemptInProgress) {
        m_wanReconnectTimer.start();
        return;
    }

    m_connPanel->setStatusText("Reconnecting via SmartLink…");
    setPanadapterConnectionAnimation(true, "Reconnecting to remote radio…");
    m_wanReconnectAttemptInProgress = true;

    if (!m_smartLink.isConnected()) {
        m_smartLink.reconnect();
        m_wanReconnectTimer.start();
        return;
    }

    startWanRadioConnect(m_pendingWanRadio, false);
    m_wanReconnectTimer.start();
}

void MainWindow::showForcedDisconnectDialog(bool wasWan,
                                            const RadioInfo& radioInfo,
                                            const WanRadioInfo& wanInfo)
{
    if (m_reconnectDlg) {
        QDialog* reconnectDialog = m_reconnectDlg;
        m_reconnectDlg = nullptr;
        reconnectDialog->close();
        reconnectDialog->deleteLater();
    }

    auto* dialog = new QDialog(this);
    m_reconnectDlg = dialog;
    dialog->setWindowTitle(tr("Radio Disconnected"));
    dialog->setModal(true);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setWindowFlags(dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog->setFixedWidth(460);
    AetherSDR::ThemeManager::instance().applyStyleSheet(dialog, "QDialog { background: {{color.background.0}}; }"
        "QFrame#forcedDisconnectHeader {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #103626, stop:1 #10283a);"
        "  border-bottom: 1px solid {{color.accent}};"
        "}"
        "QLabel { color: {{color.text.primary}}; background: transparent; }"
        "QLabel#eyebrow { color: #40ff80; background: transparent; font-weight: bold; }"
        "QLabel#title { color: {{color.text.primary}}; background: transparent; font-size: 18px; font-weight: bold; }"
        "QLabel#body { color: {{color.text.primary}}; background: transparent; }"
        "QPushButton {"
        "  background: {{color.background.1}};"
        "  border: 1px solid {{color.background.2}};"
        "  border-radius: 3px;"
        "  color: {{color.text.primary}};"
        "  padding: 7px 16px;"
        "}"
        "QPushButton:hover { background: {{color.background.1}}; border-color: {{color.accent}}; }"
        "QPushButton#primaryButton {"
        "  background: {{color.accent}};"
        "  border-color: {{color.accent.bright}};"
        "  color: {{color.background.0}};"
        "  font-weight: bold;"
        "}"
        "QPushButton#primaryButton:hover { background: {{color.accent.bright}}; }");

    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QFrame(dialog);
    header->setObjectName("forcedDisconnectHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);
    headerLayout->setSpacing(4);

    auto* eyebrow = new QLabel(tr("CONNECTION ENDED"), header);
    eyebrow->setObjectName("eyebrow");
    headerLayout->addWidget(eyebrow);

    auto* title = new QLabel(tr("Disconnected by another client"), header);
    title->setObjectName("title");
    headerLayout->addWidget(title);
    outer->addWidget(header);

    auto* content = new QWidget(dialog);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(14);

    auto* body = new QLabel(
        tr("Another client requested this AetherSDR session to disconnect. "
           "Automatic reconnect has been paused so the other operator can use the radio safely."),
        content);
    body->setObjectName("body");
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);

    auto* quit = new QPushButton(tr("Quit"), content);
    quit->setCursor(Qt::PointingHandCursor);
    quit->setMinimumHeight(34);
    buttons->addWidget(quit);

    auto* reconnect = new QPushButton(tr("Reconnect"), content);
    reconnect->setObjectName("primaryButton");
    reconnect->setCursor(Qt::PointingHandCursor);
    reconnect->setMinimumHeight(34);
    buttons->addWidget(reconnect);
    layout->addLayout(buttons);
    outer->addWidget(content);

    connect(dialog, &QObject::destroyed, this, [this, dialog] {
        if (m_reconnectDlg == dialog)
            m_reconnectDlg = nullptr;
    });

    connect(quit, &QPushButton::clicked, this, [this, dialog] {
        m_userDisconnected = true;
        if (m_reconnectDlg == dialog)
            m_reconnectDlg = nullptr;
        dialog->close();
        dialog->deleteLater();
        QApplication::quit();
    });

    connect(reconnect, &QPushButton::clicked, this, [this, dialog, wasWan, radioInfo, wanInfo] {
        if (m_reconnectDlg == dialog)
            m_reconnectDlg = nullptr;
        dialog->close();
        dialog->deleteLater();

        m_userDisconnected = false;
        m_connPanel->setStatusText("Reconnecting…");
        setPanadapterConnectionAnimation(true, "Reconnecting to radio…");

        QTimer::singleShot(300, this, [this, wasWan, radioInfo, wanInfo] {
            if (wasWan && !wanInfo.serial.isEmpty()) {
                startWanRadioConnect(wanInfo);
                return;
            }

            if (radioInfo.address.isNull()) {
                setPanadapterConnectionAnimation(false);
                m_connPanel->setStatusText("Select a radio to reconnect");
                m_connPanel->show();
                return;
            }

            QList<quint32> disconnectHandles;
            if (!confirmClientSlotAvailability(radioInfo, &disconnectHandles)) {
                m_userDisconnected = true;
                m_connPanel->setStatusText("Connection canceled");
                setPanadapterConnectionAnimation(false);
                return;
            }

            m_radioModel.setPendingClientDisconnects(disconnectHandles);
            m_radioModel.connectToRadio(radioInfo);
        });
    });

    dialog->adjustSize();
    if (QScreen* screen = windowHandle() ? windowHandle()->screen() : QApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        dialog->move(area.center() - dialog->rect().center());
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // Status bar is the only top-level shell besides the spectrum / applet
    // rail / titlebar that the operator can directly retheme.  Declare its
    // container here — statusBar() lazy-creates the QStatusBar on first
    // call, so this is also the construction point.
    theme::setContainer(statusBar(), QStringLiteral("statusbar"));

    setWindowTitle(QString("AetherSDR v%1").arg(QCoreApplication::applicationVersion()));
    setWindowIcon(QIcon(":/icon.png"));
    setMinimumSize(1024, 400);
    resize(1400, 800);

    // Apply frameless flag before first show() so the window is created
    // without chrome from the start — avoids the flash + re-create that
    // setWindowFlags() after show would cause (#framleess via View menu).
    // Default ON: TitleBar provides drag, double-click-maximize, and the
    // min/max/close trio at the far right.  View → Frameless Window can
    // still toggle it off as an escape hatch.
    {
        auto& s = AppSettings::instance();
        // One-shot migration: existing installs have FramelessWindow=False
        // saved from when frameless was opt-in.  Force ON for the
        // transition so users see the new chrome by default; the View
        // menu toggle still lets them flip it off afterwards.
        if (!s.contains("FramelessMigratedV0823")) {
            s.setValue("FramelessWindow", "True");
            s.setValue("FramelessMigratedV0823", "True");
            s.save();
        }
        if (s.value("FramelessWindow", "True").toString() == "True") {
#ifndef Q_OS_WIN
            setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
#endif
        }

        // Theming layer-0 backdrop (Phase 5 PR 3 — "fade to desktop"
        // experiment).  Disabling the default opaque window background
        // lets MainWindow::paintEvent() honour color.background.app's
        // alpha.  Today's installs see no visual change because the
        // bundled themes ship the token fully opaque (#0f0f1a / #f5f5f8) —
        // the architectural hook just lets operators dial alpha down
        // through the Theme Editor to A/B test which applets/docks still
        // need their own opaque backgrounds.
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAutoFillBackground(false);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, qOverload<>(&QWidget::update));

        // 8-axis edge resize for frameless mode — same install pattern
        // as the floating dialogs (SpotHub, RadioSetup, MemoryDialog).
        // Filter sits on the native QWindow so it doesn't compete with
        // the TitleBar's drag-to-move handler (the 6 px resize margin
        // is well clear of the 18+ px title-bar height).  Stays
        // installed across frameless toggles — when the system frame is
        // back on, the platform owns resize and our filter no-ops.
        FramelessResizer::install(this);

        // One-shot migration: collapse the legacy "CwDecodeOverlay" flat
        // key into the nested AppSettings["CwDecoder"] blob (#2417).  The
        // legacy key only encoded RX-side decode; the new blob also holds
        // the independent TX-side toggle.
        CwDecodeSettings::migrateLegacy();

        // One-shot migration: remove persisted per-slice audio mute state.
        // The radio does not persist audio_mute, so restoring it from
        // AppSettings caused Slice A to start muted on every reconnect.
        if (!s.contains("SliceAudioMutedMigratedV0999")) {
            // Cover A-H for FLEX-6700/M owners — the now-removed setter
            // computed QChar('A' + sliceId) for sliceId 0..7, so 8-slice
            // radios could have left up to eight orphan keys behind.
            for (const QChar letter : {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'})
                s.remove(QString("SliceAudioMuted_%1").arg(letter));
            s.setValue("SliceAudioMutedMigratedV0999", "True");
            s.save();
        }
    }

    applyDarkTheme();

    m_perfHeartbeatTimer.setInterval(50);
    connect(&m_perfHeartbeatTimer, &QTimer::timeout, this, [] {
        PerfTelemetry::instance().recordUiHeartbeat();
    });
    m_perfHeartbeatTimer.start();

    // Audio worker thread (#502) — AudioEngine runs on its own thread so
    // audio processing never competes with paintEvent for main thread CPU.
    m_audioThread = new QThread(this);
    // Lean render mode (#3283): read persisted state early so panadapters
    // created during startup seed their Lean button/widget correctly, then
    // apply once after construction to cover VFOs + the WAVE applet.
    // Persistence is the nested "Display" blob (Principle V); the legacy
    // flat "LeanMode" key is migrated into it on first read.
    DisplaySettings::migrateLegacy();
    m_leanMode = DisplaySettings::leanMode();
    if (m_leanMode) {
        QTimer::singleShot(0, this, [this]() { applyLeanMode(true); });
    }

    m_audioThread->setObjectName("AudioEngine");
    m_audio = new AudioEngine;  // no parent — will be moved to thread
    m_audio->setRxBoost(
        AppSettings::instance().value("AudioBoost", "False").toString() == "True");
    m_audio->setRxOutputTrimDb(
        AppSettings::instance().value("RxOutputTrimDb", "0.0").toFloat());
    m_audio->setRxBufferCapMs(
        AppSettings::instance().value("AudioBufferMs", "200").toInt());
    m_audio->moveToThread(m_audioThread);
    m_audioThread->start();
    setupAudioDeviceChangeMonitor();

    // QSO audio recorder (#1297) — lives on main thread, audio feeds are thread-safe
    m_qsoRecorder = new QsoRecorder(this);
    // During playback, block live RX audio from entering the buffer
    connect(m_qsoRecorder, &QsoRecorder::muteRxRequested, this, [this](bool mute) {
        if (mute) {
            disconnect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                    m_audio, &AudioEngine::feedAudioData);
        }
    });

    // PUDU TX monitor — captures post-PooDoo TX int16 audio, plays
    // back through the RX sink so the user can hear what their chain
    // is producing without keying the radio.  Registered with
    // AudioEngine so its tap hook picks it up on the audio thread.
    // TX recording monitor — tapped post-final-limiter so a recording
    // captures EXACTLY what the radio is told to transmit (the int16
    // bytes that get packetised into VITA-49).  Drives the chain row's
    // ⏺ / ▶ buttons in both the docked Chain applet and the channel
    // strip.
    m_finalMonitor = new ClientPuduMonitor(this);
    m_audio->setTxFinalMonitor(m_finalMonitor);
    // Route monitor playback through the user-selected output device
    // rather than the system default — without this, the post-DSP TX
    // capture plays out of whichever device Windows currently considers
    // the default, which is rarely the device the user picked in Radio
    // Settings > Audio.  Seed once and re-seed whenever the user changes
    // their output selection (#3361).
    m_finalMonitor->setOutputDevice(m_audio->outputDevice());
    m_qsoRecorder->setOutputDevice(m_audio->outputDevice());
    connect(m_audio, &AudioEngine::outputDeviceChanged, this, [this]() {
        m_finalMonitor->setOutputDevice(m_audio->outputDevice());
        m_qsoRecorder->setOutputDevice(m_audio->outputDevice());
    }, Qt::QueuedConnection);

    // Wire the Quindar tone coordinator (#2262).  TransmitModel needs
    // the DSP module (to drive intro/outro phases) and a TX-mode
    // getter for the phone-mode gate.  The getter indirection keeps
    // TransmitModel decoupled from RadioModel for test-build purposes.
    m_radioModel.transmitModel().setQuindarTone(m_audio->clientQuindarTone());
    m_radioModel.transmitModel().setTxModeGetter([this]() -> QString {
        for (auto* s : m_radioModel.slices()) {
            if (s && s->isTxSlice()) return s->mode();
        }
        return QString();
    });
    // QUIN chip flash via signal hop — see TransmitModel::quindarActiveChanged.
    // Strip is created lazily; the lambda checks for existence each fire so
    // we don't need to re-connect when the strip pops up.
    connect(&m_radioModel.transmitModel(),
            &TransmitModel::quindarActiveChanged,
            this, [this](bool active) {
        if (m_aetherialStrip) {
            if (auto* p = m_aetherialStrip->finalOutputPanel())
                p->setQuindarActive(active);
        }
    });
    connect(&m_radioModel, &RadioModel::interlockNotificationRequested,
            this, &MainWindow::showPanadapterInterlockNotification);

    m_networkDiagnosticsHistory = new NetworkDiagnosticsHistory(&m_radioModel, m_audio, this);

    // Local CW sidetone — every key source (serial, MIDI, TCI, CWX, HID)
    // funnels through RadioModel::sendCwKey/sendCwPaddle, which emits
    // cwKeyDownChanged.  Auto-queued connection so the audio thread sees
    // the state change via atomic without any blocking.
    connect(&m_radioModel, &RadioModel::cwKeyDownChanged,
            this, [this](bool down) {
        if (m_audio && m_audio->cwSidetone())
            m_audio->cwSidetone()->setKeyDown(down);
    });
    // Monitor owns a dedicated QAudioSink in pull mode — no
    // feedDecodedSpeech routing, no timer pacing.  Keeps playback
    // glitch-free on macOS/Windows where QTimer jitter was starving
    // the shared RX sink.
    // Disconnect live RX audio while the monitor is recording or
    // playing so the user hears ONLY the captured PooDoo audio.
    // Mirrors QsoRecorder's muteRxRequested handling — merely setting
    // the sink volume to 0 would mute our playback too.
    connect(m_finalMonitor, &ClientPuduMonitor::muteRxRequested,
            this, [this](bool mute) {
        if (mute) {
            disconnect(m_radioModel.panStream(),
                       &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            connect(m_radioModel.panStream(),
                    &PanadapterStream::audioDataReady,
                    m_audio, &AudioEngine::feedAudioData);
        }
    });

    // Band plan manager — must be created before buildMenuBar() which references it
    m_bandPlanMgr = new BandPlanManager(this);
    m_bandPlanMgr->loadPlans();

    // UpdateChecker — must be created before buildMenuBar() which references it
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::updateAvailable, this, [this](const QString& ver) {
        const QString current = QCoreApplication::applicationVersion();
        QMessageBox box(this);
        box.setWindowTitle("AetherSDR Update Available");
        box.setIcon(QMessageBox::Information);
        box.setText(QString("AetherSDR v%1 is available.").arg(ver));
        box.setInformativeText(QString("You are running v%1.").arg(current));
        QPushButton* viewBtn = box.addButton("View Latest Release", QMessageBox::ActionRole);
        viewBtn->setAutoDefault(false);
        QPushButton* closeBtn = box.addButton(QMessageBox::Close);
        closeBtn->setAutoDefault(false);
        // Force minimum width via layout spacer — setMinimumWidth() is ignored by QMessageBox
        if (auto* grid = qobject_cast<QGridLayout*>(box.layout()))
            grid->addItem(new QSpacerItem(480, 0, QSizePolicy::Minimum, QSizePolicy::Fixed),
                          grid->rowCount(), 0, 1, grid->columnCount());
        box.exec();
        if (box.clickedButton() == viewBtn)
            QDesktopServices::openUrl(QUrl(UpdateChecker::kReleasesPageUrl));
    });
    connect(m_updateChecker, &UpdateChecker::upToDate, this, [this](const QString& ver) {
        QMessageBox::information(this, "Check for Updates",
            QString("AetherSDR is up to date (v%1).").arg(ver));
    });
    connect(m_updateChecker, &UpdateChecker::checkFailed, this, [this]() {
        QMessageBox::warning(this, "Check for Updates",
            "Could not reach GitHub. Check your connection and try again.");
    });

    buildMenuBar();
    buildUI();
#ifdef Q_OS_WIN
    applyWindowsCustomFrame();
#endif
    registerShortcutActions();

    m_swrSweepTimer.setTimerType(Qt::PreciseTimer);
    m_swrSweepTimer.setInterval(kSwrSweepPollMs);
    connect(&m_swrSweepTimer, &QTimer::timeout,
            this, &MainWindow::advanceSwrSweep);

    if (auto* wave = m_appletPanel ? m_appletPanel->waveApplet() : nullptr) {
        // Use the dedicated high-rate post-chain feeds (8 ms throttle,
        // one emit per audio callback) instead of the shared 25 ms
        // scopeSamplesReady so the applet's scroll tracks wall clock at
        // short time-window settings.  Each side-specific signal lacks
        // the tx flag, so the lambdas reintroduce it.
        connect(m_audio, &AudioEngine::txPostChainScopeReady,
                wave, [wave](const QByteArray& mono, int sr) {
            wave->appendScopeSamples(mono, sr, /*tx=*/true);
        }, Qt::QueuedConnection);
        connect(m_audio, &AudioEngine::rxPostChainScopeReady,
                wave, [wave](const QByteArray& mono, int sr) {
            wave->appendScopeSamples(mono, sr, /*tx=*/false);
        }, Qt::QueuedConnection);
        connect(m_audio, &AudioEngine::radioTransmittingChanged,
                wave, &WaveApplet::setTransmitting,
                Qt::QueuedConnection);
    }

    m_paTempUseFahrenheit =
        AppSettings::instance().value(kPaTempUnitSettingKey, "Fahrenheit").toString() != "Celsius";
    updatePaTempLabel();

    // DXCC spot coloring (#330)
    m_dxccProvider.loadCtyDat(":/cty.dat");
    {
        auto& s = AppSettings::instance();
        m_dxccProvider.setEnabled(s.value("IsDxccColoringEnabled", "False").toString() == "True");
        m_dxccProvider.colorNewDxcc = QColor(s.value("DxccColorNewEntity", "#FF3030").toString());
        m_dxccProvider.colorNewBand = QColor(s.value("DxccColorNewBand", "#FF8C00").toString());
        m_dxccProvider.colorNewMode = QColor(s.value("DxccColorNewMode", "#FFD700").toString());
        m_dxccProvider.colorWorked  = QColor(s.value("DxccColorWorked", "#606060").toString());
        const QString adifPath = s.value("DxccAdifFilePath", "").toString();
        if (!adifPath.isEmpty()) {
            m_dxccProvider.importAdifFile(adifPath);
            // Always watch the file so spot colours update automatically when
            // the user exports a new log — no manual reload step needed.
            m_dxccProvider.setAutoReload(true, adifPath);
        }
    }
    connect(&m_dxccProvider, &DxccColorProvider::importFinished,
            this, [this](int, int) { m_radioModel.spotModel().refresh(); });

    // Install event filter on the application to intercept Space PTT
    // before child widgets (buttons, combos) consume the key event.
    qApp->installEventFilter(this);

    // Ctrl+M toggle — keep this as the single real shortcut owner.  Registering
    // the same chord on the menu action can make Qt report an ambiguous
    // shortcut on Windows, and the menu bar is hidden in minimal mode anyway.
    auto* minimalShortcut = new QShortcut(QKeySequence("Ctrl+M"), this);
    minimalShortcut->setContext(Qt::ApplicationShortcut);
    minimalShortcut->setAutoRepeat(false);
    const auto toggleMinimalModeShortcut = [this]() {
        bool next = !m_minimalMode;
        // Sync the menu action (with blocker to avoid double-toggle)
        if (m_minimalModeAction) {
            QSignalBlocker b(m_minimalModeAction);
            m_minimalModeAction->setChecked(next);
        }
        toggleMinimalMode(next);
    };
    connect(minimalShortcut, &QShortcut::activated, this, toggleMinimalModeShortcut);
    connect(minimalShortcut, &QShortcut::activatedAmbiguously, this, toggleMinimalModeShortcut);

    // Ctrl+Shift+A — starstruck easter egg: toggles pan-drag sound
    auto* starstruckShortcut = new QShortcut(QKeySequence("Ctrl+Shift+A"), this);
    connect(starstruckShortcut, &QShortcut::activated, this, []() {
        SpectrumWidget::toggleStarstruckMode();
    });

    // Minimal-mode auto-enter and Aetherial Strip restore moved down to
    // run AFTER the saved geometry restore at the bottom of the
    // constructor — calling toggleMinimalMode(true) at this point in
    // startup made it snapshot the default initial geometry into
    // FullModeGeometry (corrupting the previous session's saved rect)
    // and restoreGeometry(MinimalModeGeometry) against a window that
    // hadn't been placed on the correct screen yet.  See the geometry
    // restore block lower in the constructor. (#2483)

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

    // ── DX Cluster — forward parsed spots to radio ──────────────────────
    // ── Spot clients on worker thread ─────────────────────────────────────
    m_dxCluster = new DxClusterClient;
    m_rbnClient = new DxClusterClient;
    m_rbnClient->setLogFileName("rbn.log");
    m_rbnClient->setStartupCommandsKey("RbnStartupCommands");
    m_wsjtxClient = new WsjtxClient;
    m_spotCollectorClient = new SpotCollectorClient;
    m_potaClient = new PotaClient;
#ifdef HAVE_WEBSOCKETS
    m_freedvClient = new FreeDvClient;
#endif
#ifdef HAVE_MQTT
    m_mqttClient = new MqttClient(this);
    m_appletPanel->mqttApplet()->setMqttClient(m_mqttClient);

    connect(m_appletPanel->mqttApplet(), &MqttApplet::connectRequested,
            this, [this](const QString& host, quint16 port,
                         const QString& user, const QString& pass,
                         const QStringList& topics,
                         bool useTls, const QString& caFile) {
        m_mqttClient->setSubscriptions(mqttSubscriptionTopics(topics));
        m_mqttClient->connectToBroker(host, port, user, pass, useTls, caFile);
    });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::disconnectRequested,
            this, [this] { m_mqttClient->disconnect(); });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::settingsRequested,
            this, &MainWindow::showMqttSettingsDialog);
    m_appletPanel->mqttApplet()->restoreConnectionState();

    // CW decode → MQTT topic "aethersdr/cw/decode".
    // Publishes {"text":"K","freq":14.025,"rx":true} per decoded character.
    // RX decoder uses rx:true; TX sidetone decoder uses rx:false.
    // Any MQTT subscriber (e.g. a contest logger) receives the stream
    // without additional AetherSDR interfaces.
    connect(&m_cwDecoder,   &CwDecoder::textDecoded, this,
            [this](const QString& t, float cost) { publishCwDecodeMqtt(t, cost, true);  });
    connect(&m_cwDecoderTx, &CwDecoder::textDecoded, this,
            [this](const QString& t, float cost) { publishCwDecodeMqtt(t, cost, false); });

    // aethersdr/radio/state — publish on PTT transitions.
    // Slice freq/mode changes are wired per-slice in setActiveSliceInternal().
    m_radioStateCoalesceTimer.setSingleShot(true);
    m_radioStateCoalesceTimer.setInterval(150);
    connect(&m_radioStateCoalesceTimer, &QTimer::timeout,
            this, &MainWindow::publishRadioStateMqtt);
    connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
            this, [this](bool) { publishRadioStateMqtt(); });
    // Debounce timer for end-of-CWX detection (queueEmpty unreliable with sync_cwx=0).
    // Fires 1 s after the last tx:false with no intervening tx:true = transmission done.
    m_cwxTxEndTimer.setSingleShot(true);
    connect(&m_cwxTxEndTimer, &QTimer::timeout, this, [this]() {
        if (m_cwxSavedWpm > 0 && m_radioModel.cwxModel().speed() == m_cwxSentWpm)
            m_radioModel.cwxModel().setSpeed(m_cwxSavedWpm);
        if (m_cwxSavedHz  > 0 && m_radioModel.transmitModel().cwPitch() == m_cwxSentHz)
            m_radioModel.transmitModel().setCwPitch(m_cwxSavedHz);
        m_cwxSavedWpm = 0;
        m_cwxSavedHz  = 0;
        m_cwxSentWpm  = 0;
        m_cwxSentHz   = 0;
        m_cwxTransmitting = false;
        m_cwxPublishedTxTrue = false;
        publishRadioStateMqtt();
    });

    // aethersdr/cw/transmit → CWX keyer.
    // Payload: {"text":"de k5ptb","speed_wpm":28,"pitch_hz":600}
    // speed_wpm and pitch_hz are optional; absent = use current radio settings.
    connect(m_mqttClient, &MqttClient::messageReceived,
            this, [this](const QString& topic, const QByteArray& payload) {
        if (topic != QString::fromLatin1(kCwTransmitTopic)) return;
        if (!isMqttTopicEnabled(QString::fromLatin1(kCwTransmitTopic))) return;
        const QJsonObject obj =
            QJsonDocument::fromJson(payload).object();
        const QString text = obj.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) return;
        auto& tx = m_radioModel.transmitModel();
        const int wpm = obj.value(QStringLiteral("speed_wpm")).toInt(0);
        const int hz  = obj.value(QStringLiteral("pitch_hz")).toInt(0);
        const bool changeWpm = (wpm >= 5 && wpm <= 100);
        const bool changeHz  = (hz >= 100 && hz <= 6000);
        if (!m_cwxTransmitting) {
            m_cwxSavedWpm = changeWpm ? m_radioModel.cwxModel().speed() : 0;
            m_cwxSavedHz  = changeHz  ? tx.cwPitch() : 0;
        }
        if (changeWpm) { m_radioModel.cwxModel().setSpeed(wpm); m_cwxSentWpm = wpm; }
        if (changeHz)  { tx.setCwPitch(hz);                   m_cwxSentHz  = hz;  }
        m_cwxTxEndTimer.stop();
        m_cwxPublishedTxTrue = false;
        m_cwxTransmitting = true;
        m_radioModel.cwxModel().send(text);
        disconnect(m_cwxSpeedRestoreConn);
        m_cwxSpeedRestoreConn = connect(
            &m_radioModel.cwxModel(), &CwxModel::queueEmpty,
            this, [this]() {
                // Fast path if queueEmpty fires (sync_cwx=1 or firmware sends queue=0).
                // Identical work to m_cwxTxEndTimer.timeout — whichever fires first wins.
                if (m_cwxSavedWpm > 0 && m_radioModel.cwxModel().speed() == m_cwxSentWpm)
                    m_radioModel.cwxModel().setSpeed(m_cwxSavedWpm);
                if (m_cwxSavedHz  > 0 && m_radioModel.transmitModel().cwPitch() == m_cwxSentHz)
                    m_radioModel.transmitModel().setCwPitch(m_cwxSavedHz);
                m_cwxSavedWpm = 0;
                m_cwxSavedHz  = 0;
                m_cwxSentWpm  = 0;
                m_cwxSentHz   = 0;
                m_cwxTxEndTimer.stop();
                m_cwxTransmitting = false;
                m_cwxPublishedTxTrue = false;
                if (!m_radioModel.isRadioTransmitting())
                    publishRadioStateMqtt();
                disconnect(m_cwxSpeedRestoreConn);
            });
    });

    // MQTT → panadapter overlay display
    connect(m_appletPanel->mqttApplet(), &MqttApplet::displayValueChanged,
            this, [this](const QString& key, const QString& value) {
        if (auto* sw = m_panStack->activeSpectrum()) {
            sw->setMqttDisplayValue(key, value);
        }
    });
    connect(m_appletPanel->mqttApplet(), &MqttApplet::displayCleared,
            this, [this] {
        if (auto* sw = m_panStack->activeSpectrum()) {
            sw->clearMqttDisplay();
        }
    });
    auto mqttAntennaAliasQueue = std::make_shared<MqttAntennaAliasQueue>();
    auto hasStableRadioAliasKey = [this] {
        return m_radioModel.isConnected()
            && (!m_radioModel.chassisSerial().trimmed().isEmpty()
                || !m_radioModel.serial().trimmed().isEmpty());
    };
    auto applyMqttAntennaAlias = [this](const QString& token, const QString& alias) {
        if (alias.trimmed().isEmpty())
            m_radioModel.clearAntennaAlias(token);
        else
            m_radioModel.setAntennaAlias(token, alias);
    };
    auto flushPendingMqttAntennaAliases = [mqttAntennaAliasQueue,
                                           hasStableRadioAliasKey,
                                           applyMqttAntennaAlias] {
        for (const auto& update : mqttAntennaAliasQueue->flush(hasStableRadioAliasKey()))
            applyMqttAntennaAlias(update.token, update.alias);
    };
    connect(m_appletPanel->mqttApplet(), &MqttApplet::antennaAliasRequested,
            this, [mqttAntennaAliasQueue,
                   hasStableRadioAliasKey,
                   applyMqttAntennaAlias,
                   this](const QString& token,
                         const QString& alias) {
        const MqttAntennaAliasUpdate update{token, alias};
        for (const auto& ready : mqttAntennaAliasQueue->receive(
                 update, m_radioModel.isConnected(), hasStableRadioAliasKey()))
            applyMqttAntennaAlias(ready.token, ready.alias);
    });
    connect(&m_radioModel, &RadioModel::connectionStateChanged,
            this, [mqttAntennaAliasQueue,
                   flushPendingMqttAntennaAliases](bool connected) {
        if (connected)
            flushPendingMqttAntennaAliases();
        else
            mqttAntennaAliasQueue->clear();
    });
    connect(&m_radioModel, &RadioModel::infoChanged,
            this, [flushPendingMqttAntennaAliases] { flushPendingMqttAntennaAliases(); });
#endif

    m_spotThread = new QThread(this);
    m_spotThread->setObjectName("SpotClients");
    m_dxCluster->moveToThread(m_spotThread);
    m_rbnClient->moveToThread(m_spotThread);
    m_wsjtxClient->moveToThread(m_spotThread);
    m_spotCollectorClient->moveToThread(m_spotThread);
    m_potaClient->moveToThread(m_spotThread);
#ifdef HAVE_WEBSOCKETS
    m_freedvClient->moveToThread(m_spotThread);
#endif
    m_spotThread->start();

    // Construct each client's sockets/timers on the SpotClients thread (#1929).
    // On Windows, QTcpSocket / QUdpSocket / QWebSocket bind their internal
    // QSocketNotifier to the construction thread's Win32 message loop, so
    // creating them on the main thread before moveToThread() trips a
    // cross-thread sendEvent assert when socket events fire on disconnect.
    QMetaObject::invokeMethod(m_dxCluster, &DxClusterClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_rbnClient, &DxClusterClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_wsjtxClient, &WsjtxClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_spotCollectorClient, &SpotCollectorClient::initialize,
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_potaClient, &PotaClient::initialize,
                              Qt::QueuedConnection);
#ifdef HAVE_WEBSOCKETS
    QMetaObject::invokeMethod(m_freedvClient, &FreeDvClient::initialize,
                              Qt::QueuedConnection);
#endif

    // ── HF Propagation Forecast ────────────────────────────────────────────
    m_propForecast = new PropForecastClient(this);
    connect(m_propForecast, &PropForecastClient::forecastUpdated,
            this, [this](const PropForecast& fc) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            applet->spectrumWidget()->setPropForecast(fc.kIndex, fc.aIndex, fc.sfi);
        }
    });
    // Restore persisted setting — timer only arms if enabled
    if (AppSettings::instance().value("PropForecastEnabled", "False").toString() == "True") {
        m_propForecast->setEnabled(true);
    }

    // ── Spot forwarding: dedup + batch queue + 1/sec flush ────────────────

    // Dedup helper — returns true if spot should be skipped
    auto isDuplicateSpot = [this](const DxSpot& spot) -> bool {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto& as = AppSettings::instance();
        int lifetimeMs;
        if (spot.lifetimeSec > 0)
            lifetimeMs = spot.lifetimeSec * 1000;                      // source-provided
        else if (spot.source == "WSJT-X")
            lifetimeMs = as.value("WsjtxSpotLifetime", 120).toInt() * 1000;
        else if (spot.source == "FreeDV")
            lifetimeMs = as.value("FreeDvSpotLifetime", 120).toInt() * 1000;  // HAVE_WEBSOCKETS
        else
        {
            int sec = as.value("DxClusterSpotLifetimeSec", 0).toInt();
            if (sec <= 0) sec = as.value("DxClusterSpotLifetime", 30).toInt() * 60;
            lifetimeMs = sec * 1000;
        }
        auto it = m_spotDedup.find(spot.dxCall);
        if (it != m_spotDedup.end()) {
            bool sameFreq = std::abs(it->freqMhz - spot.freqMhz) < 0.001;
            bool expired = (now - it->addedMs) > lifetimeMs;
            if (sameFreq && !expired)
                return true;
        }
        m_spotDedup[spot.dxCall] = {spot.freqMhz, now};
        return false;
    };

    auto spotLifetimeSeconds = [](const DxSpot& spot, const QString& source) {
        if (spot.lifetimeSec > 0)
            return spot.lifetimeSec;

        auto& as = AppSettings::instance();
        if (source == "WSJT-X")
            return as.value("WsjtxSpotLifetime", 120).toInt();
        if (source == "FreeDV")
            return as.value("FreeDvSpotLifetime", 120).toInt();

        int sec = as.value("DxClusterSpotLifetimeSec", 0).toInt();
        if (sec <= 0)
            sec = as.value("DxClusterSpotLifetime", 30).toInt() * 60;
        return sec;
    };

    auto spotColorForSource = [](const DxSpot& spot, const QString& source) {
        QString spotColor = spot.color;
        if (spotColor.isEmpty()) {
            auto& as = AppSettings::instance();
            if (source == "DXCluster")
                spotColor = as.value("DxClusterSpotColor", "#D2B48C").toString();
            else if (source == "RBN")
                spotColor = as.value("RbnSpotColor", "#4488FF").toString();
            else if (source == "SpotCollector")
                spotColor = as.value("SpotCollectorSpotColor", "#FFD700").toString();
            else if (source == "FreeDV")
                spotColor = as.value("FreeDvSpotColor", "#FF8C00").toString();  // HAVE_WEBSOCKETS
        }
        if (spotColor.length() == 7)
            spotColor = "#FF" + spotColor.mid(1);
        return spotColor;
    };

    auto addPassiveSpotToModel = [this](const DxSpot& spot, const QString& source,
                                        const QString& color, int lifetimeSec) {
        if (spot.dxCall.trimmed().isEmpty() || spot.freqMhz <= 0.0)
            return;

        QMap<QString, QString> kvs;
        kvs["callsign"] = QString(spot.dxCall).replace(' ', QChar(0x7f));
        kvs["rx_freq"] = QString::number(spot.freqMhz, 'f', 6);
        kvs["tx_freq"] = QString::number(spot.freqMhz, 'f', 6);
        kvs["source"] = source;
        kvs["spotter_callsign"] = spot.spotterCall;
        kvs["lifetime_seconds"] = QString::number(lifetimeSec);
        kvs["timestamp"] = QString::number(QDateTime::currentSecsSinceEpoch());
        if (!spot.comment.isEmpty())
            kvs["comment"] = QString(spot.comment).replace(' ', QChar(0x7f));
        if (!color.isEmpty())
            kvs["color"] = color;

        const int spotId = m_nextPassiveSpotId--;
        m_radioModel.spotModel().applySpotStatus(spotId, kvs);
        if (lifetimeSec > 0) {
            const qint64 expiresAt = QDateTime::currentMSecsSinceEpoch()
                                   + qint64(lifetimeSec) * 1000;
            m_passiveSpotExpiryMs.insert(spotId, expiresAt);
        }
    };

    // Build spot add command and queue for batch send
    auto queueSpotCmd = [this, isDuplicateSpot, spotLifetimeSeconds,
                         spotColorForSource, addPassiveSpotToModel]
                        (const DxSpot& spot, const QString& source) {
        if (!m_radioModel.isConnected()) return;
        if (isDuplicateSpot(spot)) return;
        const int lifetimeSec = spotLifetimeSeconds(spot, source);
        const QString spotColor = spotColorForSource(spot, source);
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            addPassiveSpotToModel(spot, source, spotColor, lifetimeSec);
            return;
        }

        QString call = QString(spot.dxCall).replace(' ', QChar(0x7f));
        QString freq = QString::number(spot.freqMhz, 'f', 6);
        // trigger_action=none disables the radio's internal tune/mode-set on
        // spot click. AetherSDR handles freq via frequencyClicked and mode
        // via SpotAutoSwitchMode client-side, so the radio's stored-mode
        // path (which mishandles non-Flex tokens like "SSB") never fires.
        // Clicks still emit SpotTriggered for external loggers (#341, #1846).
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=" + source
                     + " spotter_callsign=" + spot.spotterCall
                     + " trigger_action=none"
                     + " lifetime_seconds=" + QString::number(lifetimeSec);
        if (!spot.comment.isEmpty())
            cmd += " comment=" + QString(spot.comment).replace(' ', QChar(0x7f));
        if (!spotColor.isEmpty())
            cmd += " color=" + spotColor;
        m_spotCmdBatch.append(cmd);
    };

    // Flush batch: send queued spot commands to radio (1/sec)
    auto* spotCmdTimer = new QTimer(this);
    spotCmdTimer->start(1000);
    connect(spotCmdTimer, &QTimer::timeout, this, [this] {
        if (m_spotCmdBatch.isEmpty() || !m_radioModel.isConnected()) return;
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            m_spotCmdBatch.clear();
            return;
        }
        // Send up to RbnRateLimit commands per tick
        int limit = AppSettings::instance().value("RbnRateLimit", 10).toInt();
        int count = std::min(static_cast<int>(m_spotCmdBatch.size()), limit);
        for (int i = 0; i < count; ++i)
            m_radioModel.sendCommand(m_spotCmdBatch[i]);
        m_spotCmdBatch.remove(0, count);
    });

    auto* passiveSpotExpiryTimer = new QTimer(this);
    passiveSpotExpiryTimer->start(1000);
    connect(passiveSpotExpiryTimer, &QTimer::timeout, this, [this] {
        if (m_passiveSpotExpiryMs.isEmpty())
            return;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QVector<int> expired;
        for (auto it = m_passiveSpotExpiryMs.cbegin(); it != m_passiveSpotExpiryMs.cend(); ++it) {
            if (it.value() <= now)
                expired.append(it.key());
        }

        for (int spotId : expired) {
            m_passiveSpotExpiryMs.remove(spotId);
            m_radioModel.spotModel().removeSpot(spotId);
        }
    });
    connect(&m_radioModel.spotModel(), &SpotModel::spotsCleared,
            this, [this] { m_passiveSpotExpiryMs.clear(); });

    connect(m_dxCluster, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "DXCluster");
    });

    connect(m_rbnClient, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "RBN");
    });

    connect(m_wsjtxClient, &WsjtxClient::spotReceived,
            this, [this, isDuplicateSpot, spotLifetimeSeconds, addPassiveSpotToModel](const DxSpot& spot) {
        if (!m_radioModel.isConnected()) return;
        if (isDuplicateSpot(spot)) return;

        auto& as = AppSettings::instance();
        const QString& msg = spot.comment;
        bool isCQ = msg.startsWith("CQ ");
        bool isPOTA = msg.contains("CQ POTA");
        bool isCallingMe = false;
        {
            QString myCall = as.value("DxClusterCallsign").toString();
            if (!myCall.isEmpty()) {
                QStringList parts = msg.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 2 && parts[0] == myCall)
                    isCallingMe = true;
            }
        }

        // Filter
        bool fCQ   = as.value("WsjtxFilterCQ", "True").toString() == "True";
        bool fPOTA = as.value("WsjtxFilterPOTA", "True").toString() == "True";
        bool fMe   = as.value("WsjtxFilterCallingMe", "True").toString() == "True";
        bool anyFilter = fCQ || fPOTA || fMe;
        if (anyFilter) {
            bool pass = false;
            if (fCQ && isCQ) pass = true;
            if (fPOTA && isPOTA) pass = true;
            if (fMe && isCallingMe) pass = true;
            if (!pass) return;
        }

        // Color
        DxSpot colored = spot;
        if (isCallingMe)
            colored.color = as.value("WsjtxColorCallingMe", "#FF0000").toString();
        else if (isPOTA)
            colored.color = as.value("WsjtxColorPOTA", "#00FFFF").toString();
        else if (isCQ)
            colored.color = as.value("WsjtxColorCQ", "#00FF00").toString();
        else
            colored.color = as.value("WsjtxColorDefault", "#FFFFFF").toString();

        // Compute alpha from SNR: -24→64, 0→192, +10→255 (linear interpolation)
        int alpha;
        if (colored.snr <= -24)
            alpha = 64;
        else if (colored.snr >= 10)
            alpha = 255;
        else if (colored.snr <= 0)
            alpha = 64 + (colored.snr + 24) * (192 - 64) / 24;   // -24..0 → 64..192
        else
            alpha = 192 + colored.snr * (255 - 192) / 10;         // 0..+10 → 192..255

        // Convert to #AARRGGBB format for radio
        if (colored.color.length() == 7)  // #RRGGBB → #AARRGGBB
            colored.color = QString("#%1%2").arg(alpha, 2, 16, QChar('0')).arg(colored.color.mid(1));

        QString call = QString(colored.dxCall).replace(' ', QChar(0x7f));
        QString freq = QString::number(colored.freqMhz, 'f', 6);
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=WSJT-X"
                     + " spotter_callsign=" + colored.spotterCall
                     + " trigger_action=none"  // see comment at queueSpotCmd (#1846)
                     + " lifetime_seconds=" + QString::number(
                           spotLifetimeSeconds(colored, "WSJT-X"));
        if (!colored.comment.isEmpty())
            cmd += " comment=" + QString(colored.comment).replace(' ', QChar(0x7f));
        if (!colored.color.isEmpty())
            cmd += " color=" + colored.color;
        if (!SpotCommandPolicy::shouldSendSpotAddCommands()) {
            addPassiveSpotToModel(colored, "WSJT-X", colored.color,
                                  spotLifetimeSeconds(colored, "WSJT-X"));
            return;
        }
        m_spotCmdBatch.append(cmd);
    });

    connect(m_spotCollectorClient, &SpotCollectorClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "SpotCollector");
    });

    connect(m_potaClient, &PotaClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "POTA");
    });

#ifdef HAVE_WEBSOCKETS
    connect(m_freedvClient, &FreeDvClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "FreeDV");
    });
#endif

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
        m_cwxLocalKeyer = new CwxLocalKeyer(this);
        connect(&m_radioModel.cwxModel(), &CwxModel::transmissionRequested,
                m_cwxLocalKeyer, &CwxLocalKeyer::start);
        connect(&m_radioModel.cwxModel(), &CwxModel::transmissionCancelled,
                m_cwxLocalKeyer, &CwxLocalKeyer::stop);
        connect(m_cwxLocalKeyer, &CwxLocalKeyer::keyStateChanged,
                this, [this](bool down) {
            if (m_audio && m_audio->cwSidetone())
                m_audio->cwSidetone()->setKeyDown(down);
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
            if (m_audio && m_audio->cwSidetone())
                m_audio->cwSidetone()->setKeyDown(down);
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

    // ── Panadapter stream → spectrum widget ───────────────────────────────
    // Route FFT/waterfall data to the correct SpectrumWidget by stream ID
    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, [this](quint32 streamId, const QVector<float>& bins, qint64 emittedNs) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        if (emittedNs > 0) {
            PerfTelemetry::instance().recordFrameAge(
                PerfTelemetry::FrameKind::Panadapter,
                static_cast<double>(PerfTelemetry::nowNs() - emittedNs) / 1000000.0);
        }
        for (auto* pan : m_radioModel.panadapters()) {
            if (pan->panStreamId() == streamId) {
                if (auto* sw = m_panStack->spectrum(pan->panId())) {
                    sw->updateSpectrum(bins);
                    finishPanadapterConnectionAnimation();
                }
                return;
            }
        }
        // Fallback: active spectrum (covers "default" pan before radio connects)
        if (auto* sw = spectrum()) {
            sw->updateSpectrum(bins);
            finishPanadapterConnectionAnimation();
        }
    });
    // ── S History Markers — tap into FFT frames for voice signal detection ──
    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, &MainWindow::onSpectrumReadyForSHistory);

    connect(m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
            this, [this](quint32 streamId, const QVector<float>& bins,
                         double low, double high, quint32 tc, qint64 emittedNs) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        if (emittedNs > 0) {
            PerfTelemetry::instance().recordFrameAge(
                PerfTelemetry::FrameKind::Waterfall,
                static_cast<double>(PerfTelemetry::nowNs() - emittedNs) / 1000000.0);
        }
        for (auto* pan : m_radioModel.panadapters()) {
            if (pan->wfStreamId() == streamId) {
                const double panCenter = pan->centerMhz();
                if (XvtrPolicy::isWaterfallTileOutsidePan(low, high, panCenter)) {
                    // Only reinterpret non-overlapping tile ranges for real XVTR
                    // IF/RF translation. Ordinary HF pans can briefly see stale
                    // tile centers while dragging; shifting those corrupts rows.
                    const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
                    const auto match = XvtrPolicy::matchWaterfallTileTransverterOffset(
                        low, high, panCenter, xvtrs);
                    bool hasXvtrSliceAntenna = false;
                    if (!match.matched) {
                        for (auto* slice : m_radioModel.slices()) {
                            if (!slice || slice->panId() != pan->panId())
                                continue;
                            if (slice->rxAntenna().startsWith(QStringLiteral("XVT"),
                                                               Qt::CaseInsensitive)) {
                                hasXvtrSliceAntenna = true;
                                break;
                            }
                        }
                    }
                    const auto mapped = XvtrPolicy::mapWaterfallTileRange(
                        low, high, panCenter, xvtrs, hasXvtrSliceAntenna);
                    logXvtrWaterfallDecision(streamId, pan->panId(), panCenter,
                                             low, high, mapped, match,
                                             hasXvtrSliceAntenna, xvtrs);
                    low = mapped.lowMhz;
                    high = mapped.highMhz;
                }
                if (auto* sw = m_panStack->spectrum(pan->panId())) {
                    sw->updateWaterfallRow(bins, low, high, tc);
                    finishPanadapterConnectionAnimation();
                }
                return;
            }
        }
        if (auto* sw = spectrum()) {
            sw->updateWaterfallRow(bins, low, high, tc);
            finishPanadapterConnectionAnimation();
        }
    });
    connect(m_radioModel.panStream(), &PanadapterStream::waterfallAutoBlackLevel,
            this, [this](quint32 streamId, quint32 autoBlack) {
        if (m_shuttingDown || !m_panStack) {
            return;
        }
        for (auto* pan : m_radioModel.panadapters()) {
            if (pan->wfStreamId() == streamId) {
                if (auto* sw = m_panStack->spectrum(pan->panId())) {
                    if (sw->wfAutoBlack()) {
                        const int level = std::clamp(static_cast<int>(autoBlack), 0, 125);
                        sw->setWfBlackLevel(level);
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
            if (!bgPath.isEmpty())
                sw->setBackgroundImage(bgPath);
            int bgOpacity = s.value(sw->settingsKey("BackgroundOpacity"), "80").toInt();
            sw->setBackgroundOpacity(bgOpacity);
            QColor bgFill(s.value(sw->settingsKey("BackgroundFillColor"),
                                  "#0a0a14").toString());
            if (bgFill.isValid())
                sw->setBackgroundFillColor(bgFill);
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
        const int xpix = panXpixelsFor(sw);
        const int ypix = panYpixelsFor(sw);
        m_radioModel.sendCommand(
            QString("display pan set %1 xpixels=%2 ypixels=%3")
                .arg(pan->panId()).arg(xpix).arg(ypix));

        // Tell PanadapterStream the ypixels for FFT bin→dBm conversion
        if (pan->panStreamId()) {
            m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
            sw->prepareForFftScaleChange();
        }

        qDebug() << "MainWindow: added panadapter applet for" << pan->panId();

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

                    // Optimistically set local yPixels immediately so FFT frames
                    // arriving before the radio echoes back use correct scaling (#1511).
                    for (auto* a : m_panStack->allApplets()) {
                        auto* s = a->spectrumWidget();
                        auto* p = m_radioModel.panadapter(a->panId());
                        if (!s || !p || !p->panStreamId()) continue;
                        if (panPixelDimensionsReady(s)) {
                            m_radioModel.panStream()->setYPixels(
                                p->panStreamId(), panYpixelsFor(s));
                            s->prepareForFftScaleChange();
                        }
                    }

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
                            const int xpix = panXpixelsFor(sw);
                            const int ypix = panYpixelsFor(sw);
                            m_radioModel.sendCommand(
                                QString("display pan set %1 xpixels=%2 ypixels=%3")
                                    .arg(pan->panId()).arg(xpix).arg(ypix));
                            if (pan->panStreamId()) {
                                m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
                                sw->prepareForFftScaleChange();
                            }
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
        const int xpix = panXpixelsFor(sw);
        const int ypix = panYpixelsFor(sw);
        m_radioModel.sendCommand(
            QString("display pan set %1 xpixels=%2 ypixels=%3")
                .arg(panId).arg(xpix).arg(ypix));
        if (pan->panStreamId()) {
            m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
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
                const int xpix = panXpixelsFor(sw);
                const int ypix = panYpixelsFor(sw);
                m_radioModel.sendCommand(
                    QString("display pan set %1 xpixels=%2 ypixels=%3")
                        .arg(pan->panId()).arg(xpix).arg(ypix));
                if (pan->panStreamId()) {
                    m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
                    sw->prepareForFftScaleChange();
                }
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
        // the user's saved multi-pan layout.
        int remaining = m_panStack->count();
        if (remaining > 1) {
            m_panStack->rearrangeLayout(defaultPanLayoutForCount(remaining));
        }
    });

    // ── Per-panadapter signal wiring (extracted for multi-pan support) ──────
    wirePanadapter(m_panApplet);

    // Display overlay connections are now per-pan in wirePanadapter().

    // ── Panadapter stream → audio engine ──────────────────────────────────
    // All VITA-49 traffic arrives on the single client udpport socket owned
    // by PanadapterStream. It strips the header from IF-Data packets and emits
    // audioDataReady(); we feed that directly to the QAudioSink.
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            m_audio, &AudioEngine::feedAudioData);

    // ── QSO recorder: tap RX audio, trigger on MOX (#1297) ────────────
    // Only RX audio is recorded — TX audio (txRawPcmReady) is int16 and
    // would need separate handling to avoid format mismatch.
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            m_qsoRecorder, &QsoRecorder::feedRxAudio);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_qsoRecorder, &QsoRecorder::onMoxChanged);

    // ── BNR container autostart ─────────────────────────────────────────
#ifdef HAVE_BNR
    if (AppSettings::instance().value("BnrAutostart", "False").toString() == "True") {
        QString container = AppSettings::instance().value("BnrContainerName", "maxine-bnr").toString();
        qDebug() << "BNR: autostarting container" << container;
        QProcess::startDetached("docker", {"start", container});
    }
#endif

    // ── CW decoder: feed audio ──────────────────────────────────────────
    // Audio feed is global (same audio for all pans).
    // Text/stats output is routed to the pan owning the active slice
    // via routeCwDecoderOutput(), which re-wires on active slice change (#864).
    //
    // RX feed is gated on CwDecodeSettings::rxEnabled() (#2417).  Cheap
    // per-packet check so the toggle can flip live from the dialog
    // without disconnecting/reconnecting signal wiring.
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            &m_cwDecoder, [this](const QByteArray& pcm) {
                if (CwDecodeSettings::rxEnabled())
                    m_cwDecoder.feedAudio(pcm);
            });

    // TX-side CW decoder feed (#2417): AudioEngine taps the sidetone
    // generator's mono signal, downsamples 48→24 kHz, and emits the
    // same stereo float32 shape CwDecoder::feedAudio() already accepts.
    // Routed to the dedicated TX decoder so RX and TX decoded text are
    // distinguishable in the panel.  The tap self-gates on
    // AudioEngine::m_cwDecodeTxTapEnabled, which we flip from the
    // moxChanged handler below.
    connect(m_audio, &AudioEngine::txDecodeAudioReady,
            &m_cwDecoderTx, &CwDecoder::feedAudio);

    // Flip the TX-decode tap (and start the decoder if needed) on every
    // MOX edge.  Cheap; refreshCwDecodeState() does all the gating.
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool) { refreshCwDecodeState(); });

    // Push P/CW applet's Pitch + Speed into the TX decoder live (#2417).
    // phoneStateChanged is the bulk-update signal that covers cw_pitch /
    // cw_speed status echoes; refreshCwDecodeState() forwards only when
    // TX-decode is on, so calling it on every phoneStateChanged is cheap.
    connect(&m_radioModel.transmitModel(), &TransmitModel::phoneStateChanged,
            this, [this]() { refreshCwDecodeState(); });

    // ── RTTY decoder: feed audio ────────────────────────────────────────
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            &m_rttyDecoder, [this](const QByteArray& pcm) {
                if (m_rttyDecoder.isRunning())
                    m_rttyDecoder.feedAudio(pcm);
            });

    // ── AF gain from applet panel → radio per-slice audio_level ─────────
    connect(m_appletPanel->rxApplet(), &RxApplet::afGainChanged, this, [this](int v) {
        if (auto* s = activeSlice()) s->setAudioGain(v);
    });
    connect(m_appletPanel->rxApplet(), &RxApplet::directEntryCommitted,
            this, [this](double mhz, const QString& source) {
        if (auto* s = activeSlice()) {
            const QByteArray sourceUtf8 = source.toUtf8();
            applyTuneRequest(s, mhz, TuneIntent::CommandedTargetCenter,
                             sourceUtf8.constData());
        }
    });

    // ── Slice tab toggle: click A/B/C/D → switch active slice (#1278) ──
    connect(m_appletPanel->rxApplet(), &RxApplet::sliceActivationRequested,
            this, &MainWindow::setActiveSlice);
    connect(m_appletPanel->rxApplet(), &RxApplet::calibrateAgcTRequested,
            this, &MainWindow::showAgcCalibrationDialog);
    // Sync slice tab capacity after radio info/status reports actual capacity.
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        if (m_radioModel.model().isEmpty()) {
            return;
        }

        m_appletPanel->setMaxSlices(m_radioModel.maxSlices());
        m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
    });

    // Radio info can arrive after onConnectionStateChanged, so refresh the labels.
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        if (m_radioInfoLabel && !m_radioModel.model().isEmpty())
            m_radioInfoLabel->setText(m_radioModel.model());
        if (m_radioVersionLabel && !m_radioModel.version().isEmpty())
            m_radioVersionLabel->setText(m_radioModel.version());
    });

    // Propagate late-arriving SmartSDR+ subscription + dual-SCU diversity
    // eligibility to all existing VFOs. Both depend on radio info (license
    // subscription / model) that can arrive after a slice is created — in
    // which case onSliceAdded reads empty strings and the VFO gets stuck in
    // the wrong state (#1356 for SmartSDR+, #1503 for DIV).
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        const bool hasPlus = m_radioModel.licenseSubscription().contains("SmartSDR+");
        const QString& model = m_radioModel.model();
        const bool divAllowed = model.contains("6500") || model.contains("6600")
                             || model.contains("6700") || model.contains("8600")
                             || model.contains("AU-520");
        if (m_panStack) {
            for (auto* applet : m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                for (auto* vfo : sw->findChildren<VfoWidget*>()) {
                    vfo->setSmartSdrPlus(hasPlus);
                    vfo->setDiversityAllowed(divAllowed);
                }
            }
        }
    });

    // Client-side DSP buttons (NR2 / NR4 / MNR / BNR / DFNR / RN2) now
    // live only in the AetherDSP applet, which owns its own
    // *EnabledChanged subscriptions.

    connect(m_appletPanel->rxApplet(), &RxApplet::muteAllToggled,
            this, &MainWindow::onMuteAllSlicesToggle);

#ifdef HAVE_RADE
    connect(m_appletPanel->rxApplet(), &RxApplet::radeActivated,
            this, [this](bool on, int sliceId) {
        if (on) activateRADE(sliceId);
        else if (sliceId == m_radeSliceId) deactivateRADE();
    });
#endif

    // ── Tuning step size ───────────────────────────────────────────────────
    // Two connections, split by source.  stepSizeChanged fires for ANY step
    // change, including radio-driven syncs (syncStepFromSlice) after a memory
    // recall or band crossing — so only source-agnostic bookkeeping that must
    // track the radio's current step belongs here.  Per-pan
    // SpectrumWidget::setStepSize connections are made in wirePanadapter() so
    // all pans (including new ones added at runtime) stay in sync.
    connect(m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            this, [this](int step) {
        if (m_flexControlDialog)
            m_flexControlDialog->setStepSize(step);
        // Invalidate persistent encoder accumulators so the next tick rebases
        // and re-snaps to the new step grid. Without this, an in-flight target
        // computed against the previous step size carries an off-grid residual
        // (e.g. step 20 Hz → 500 Hz leaves a 60 Hz tail; #3260).  This must run
        // for radio-driven step changes too, so it stays on stepSizeChanged.
        m_flexTargetMhz = -1.0;
        m_flexCoalesceTimer.stop();
#ifdef HAVE_MIDI
        m_midiTuneTargetMhz = -1.0;
        m_midiTuneIdleTimer.stop();
#endif
    });
    // Deliberate operator step changes only (STEP buttons/scroll, cycle
    // shortcuts, encoder push).  These push to the radio, persist, and show a
    // brief toast.  Routing them through stepSizeChangedByUser keeps them off
    // radio-driven syncs — otherwise every memory-spot recall or band crossing
    // echoes a redundant `slice set step=` and spams a "Step: …" toast
    // (the radio is already authoritative for the slice's step).
    connect(m_appletPanel->rxApplet(), &RxApplet::stepSizeChangedByUser,
            this, [this](int step) {
        // Send step to radio for the active slice
        if (auto* s = m_radioModel.slice(m_activeSliceId))
            m_radioModel.sendCommand(QString("slice set %1 step=%2").arg(s->sliceId()).arg(step));
        // Also save to AppSettings for SpectrumWidget scroll-to-tune
        auto& settings = AppSettings::instance();
        settings.setValue("TuningStepSize", QString::number(step));
        settings.save();
        QString stepStr;
        if (step >= 1000000)
            stepStr = QString("%1 MHz").arg(step / 1000000.0, 0, 'f', step % 1000000 ? 1 : 0);
        else if (step >= 1000)
            stepStr = QString("%1 kHz").arg(step / 1000.0, 0, 'f', step % 1000 ? 1 : 0);
        else
            stepStr = QString("%1 Hz").arg(step);
        statusBar()->showMessage(QString("Step: %1").arg(stepStr), 2000);
    });
    int savedStep = AppSettings::instance().value("TuningStepSize", "100").toInt();
    for (auto* a : m_panStack->allApplets()) a->spectrumWidget()->setStepSize(savedStep);
    m_appletPanel->rxApplet()->setInitialStepSize(savedStep);

    // ── Antenna list from radio → applet panel ─────────────────────────────
    connect(&m_radioModel, &RadioModel::antListChanged,
            m_appletPanel, &AppletPanel::setAntennaList);
    // Overlay-menu antenna wiring is now per-pan in wirePanadapter() (#1260).
    // Antenna list and S-meter are now wired per-widget in onSliceAdded.

    // ── Title bar: Pan Follow ────────────────────────────────────────────────
    connect(m_titleBar, &TitleBar::panFollowToggled,
            this, &MainWindow::setPanFollow);
    if (m_titleBar->isPanFollowChecked()) setPanFollow(true);

    // ── Title bar: PC Audio, master volume, headphone volume ────────────────
    // The remote_audio_rx stream controls the radio's audio routing:
    // stream exists → audio to PC; stream removed → audio to radio speakers.
    // Keep the stream alive when TCI clients need it (#1014).
    connect(m_titleBar, &TitleBar::pcAudioToggled, this, [this](bool on) {
        if (on) {
            // Restart the local audio sink to recover from stale WASAPI sessions
            // (e.g. after Teams/Zoom reconfigures the audio endpoint). (#1569)
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->stopRxStream();
                m_audio->startRxStream();
            });
            m_radioModel.createRxAudioStream();
        } else {
            // Stop the local sink so audio isn't played locally even when
            // the remote_audio_rx stream is still alive for a TCI client
            // (#1571 follow-up).
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->stopRxStream();
            });
            m_radioModel.removeRxAudioStream();
        }
    });
    // Master volume — title bar slider routes through applyMasterVolume()
    // so the TCI `volume:N;` command (#1764) can hit the same code path
    // when m_tciServer is created later in this constructor.
    connect(m_titleBar, &TitleBar::masterVolumeChanged,
            this, &MainWindow::applyMasterVolume);
    connect(m_titleBar, &TitleBar::headphoneVolumeChanged,
            &m_radioModel, &RadioModel::setHeadphoneGain);
    connect(m_titleBar, &TitleBar::lineoutMuteChanged, this, [this](bool muted) {
        m_audio->setMuted(muted);
        m_radioModel.sendCommand(QString("mixer lineout mute %1").arg(muted ? 1 : 0));
        auto& s = AppSettings::instance();
        s.setValue("PcAudioMuted", muted ? "True" : "False");
        s.save();
    });

    // ── PooDoo RX chain status tiles (Phase 0 chassis) ─────────────────────
    // RADIO tile = PC Audio enabled (standard SSB / remote_audio_rx stream).
    // DSP tile   = any client-side NR active (NR4 / DFNR / BNR).
    // SPEAK tile = AudioEngine output unmuted.
    if (auto* chain = m_appletPanel->clientChainApplet()) {
        // RADIO — driven by the existing PC Audio toggle in TitleBar.
        // Also forward to the strip's RX chain so its RADIO tile
        // tracks the same state.
        connect(m_titleBar, &TitleBar::pcAudioToggled, this,
                [this, chain](bool on) {
            chain->setRxPcAudioEnabled(on);
            if (m_aetherialStrip) m_aetherialStrip->setRxPcAudioEnabled(on);
        });

        // DSP — aggregate of every client-side NR module.  These are
        // mutually exclusive in AudioEngine::processRx (chained
        // if/else), so at most one is active at a time.  The tile
        // greens whenever any is on, and its label rotates to the
        // active module's short name (NR2 / RN2 / NR4 / DFNR / MNR /
        // BNR).  Shared state lives in a struct held by shared_ptr
        // captured into each lambda so lifetime ends with the last
        // connected slot.
        struct DspState {
            bool nr2{false}, rn2{false}, nr4{false},
                 dfnr{false}, mnr{false}, bnr{false};
        };
        auto dspState = std::make_shared<DspState>();
        auto pushDsp = [this, chain, dspState]() {
            // Priority order picks the most "specific" / most-recent
            // module if more than one is somehow on at once.  Same
            // order as the audio-thread dispatcher so the displayed
            // label matches what's actually processing.
            QString label;
            if      (dspState->bnr)  label = "BNR";
            else if (dspState->mnr)  label = "MNR";
            else if (dspState->dfnr) label = "DFNR";
            else if (dspState->nr4)  label = "NR4";
            else if (dspState->rn2)  label = "RN2";
            else if (dspState->nr2)  label = "NR2";
            const bool anyOn = !label.isEmpty();
            chain->setRxClientDspActive(anyOn, label);
            if (m_aetherialStrip)
                m_aetherialStrip->setRxClientDspActive(anyOn, label);
        };
        connect(m_audio, &AudioEngine::nr2EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->nr2 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::rn2EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->rn2 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::nr4EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->nr4 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::dfnrEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->dfnr = on; pushDsp(); });
        connect(m_audio, &AudioEngine::mnrEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->mnr = on; pushDsp(); });
        connect(m_audio, &AudioEngine::bnrEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->bnr = on; pushDsp(); });

        // SPEAK — AudioEngine emits mutedChanged on every setMuted() flip.
        connect(m_audio, &AudioEngine::mutedChanged, this,
                [this, chain](bool muted) {
            chain->setRxOutputUnmuted(!muted);
            if (m_aetherialStrip) m_aetherialStrip->setRxOutputUnmuted(!muted);
        });

        // Seed initial state — settings and engine values are already
        // loaded by the time we reach this wiring code.  We pull from
        // the engine (not signals) so an already-on NR module shows up
        // immediately instead of waiting for the next toggle.
        const bool pcOn = AppSettings::instance()
            .value("PcAudioEnabled", "True").toString() == "True";
        chain->setRxPcAudioEnabled(pcOn);
        if (m_aetherialStrip) m_aetherialStrip->setRxPcAudioEnabled(pcOn);
        dspState->nr2  = m_audio->nr2Enabled();
        dspState->rn2  = m_audio->rn2Enabled();
        dspState->nr4  = m_audio->nr4Enabled();
        dspState->dfnr = m_audio->dfnrEnabled();
        dspState->mnr  = m_audio->mnrEnabled();
        dspState->bnr  = m_audio->bnrEnabled();
        pushDsp();
        chain->setRxOutputUnmuted(!m_audio->isMuted());
        if (m_aetherialStrip)
            m_aetherialStrip->setRxOutputUnmuted(!m_audio->isMuted());
    }
    connect(m_titleBar, &TitleBar::headphoneMuteChanged, this, [this](bool muted) {
        m_radioModel.sendCommand(QString("mixer headphone mute %1").arg(muted ? 1 : 0));
    });
    connect(&m_radioModel, &RadioModel::audioOutputChanged, this, [this]() {
        m_titleBar->setHeadphoneVolume(m_radioModel.headphoneGain());
    });

    // Multi-Flex: show when another client is transmitting
    connect(&m_radioModel, &RadioModel::txOwnerChanged,
            m_titleBar, &TitleBar::setOtherClientTx);

    // Multi-Flex: title bar indicator when other clients are connected
    connect(&m_radioModel, &RadioModel::otherClientsChanged,
            m_titleBar, &TitleBar::setMultiFlexStatus);
    connect(&m_radioModel, &RadioModel::clientConnected,
            this, [this](quint32 handle,
                         const QString& source,
                         const QString& station,
                         const QString& program) {
        statusBar()->showMessage(
            clientConnectionStatusMessage(handle, source, station, program),
            3000);
    });

    // Apply saved master volume
    int savedMasterVol = AppSettings::instance().value("MasterVolume", "100").toInt();
    m_audio->setRxVolume(savedMasterVol / 100.0f);

    // Restore saved mute state (#1571)
    bool savedMute = AppSettings::instance().value("PcAudioMuted", "False").toString() == "True";
    if (savedMute) {
        m_audio->setMuted(true);
        m_titleBar->setLineoutMuted(true);
    }

    // ── S-Meter: MeterModel → SMeterWidget (active slice only) ─────────────
    connect(&m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            this, [this](int sliceIndex, float dbm) {
        if (sliceIndex == m_activeSliceId) {
            m_appletPanel->sMeterWidget()->setLevel(dbm);
#ifdef HAVE_HIDAPI
            m_tmate2SmeterDbm = dbm;
            updateTMate2Display();
            updateTMate2Indicators();
#endif
        }
    });
    // Symmetric with the amp-side guard at line ~3654 and the PGXL TCP path
    // at line ~3525: in OPERATE the amp owns the analog S-Meter, so drop the
    // exciter sample here to stop the alternating-writer pulse where exciter
    // (~100 W) and amp (~1500 W) values race into the same widget. (#2927)
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            this, [this](float fwd, float swr) {
        if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate())
            return;
        m_appletPanel->sMeterWidget()->setTxMeters(fwd, swr);
#ifdef HAVE_HIDAPI
        m_tmate2TxWatts = fwd;
        if (m_radioModel.transmitModel().isTransmitting())
            updateTMate2Display();
#endif
    });
    connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setMicMeters);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setTransmitting);

    // ── Tuner: MeterModel TX meters → TunerApplet gauges ────────────────
    // Use TGXL-specific meters when available (disambiguated from PGXL by handle)
    connect(&m_radioModel.meterModel(), &MeterModel::tgxlMetersChanged,
            m_appletPanel->tunerApplet(), &TunerApplet::updateMeters);
    // Note: txMetersChanged NOT connected to TunerApplet — exciter power
    // would overwrite TGXL readings. TGXL meters come from TunerModel
    // via the direct TCP connection (port 9010). (#625)
    m_appletPanel->tunerApplet()->setTunerModel(&m_radioModel.tunerModel());
    m_appletPanel->tunerApplet()->setMeterModel(&m_radioModel.meterModel());

    // Show/hide TUNE button + applet based on TGXL presence
    connect(&m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            m_appletPanel, &AppletPanel::setTunerVisible);
    connect(&m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            this, [this](bool present) {
        m_tgxlContainer->setVisible(present);
        m_tgxlSeparator->setVisible(present);
        // Auto-connect/disconnect direct TGXL connection for manual relay control (#469)
        if (present) {
            QString ip = m_radioModel.tunerModel().tgxlIp();
            if (!ip.isEmpty() && !m_tgxlConn.isConnected()) {
                m_tgxlConn.connectToTgxl(ip);
            }
        } else {
            m_tgxlConn.disconnect();
        }
    });
    // Apply auto-reconnect setting at startup so it's active before any connection is made
    {
        const bool ar = PeripheralSettings::autoReconnect();
        m_tgxlConn.setAutoReconnect(ar);
        m_pgxlConn.setAutoReconnect(ar);
        m_antennaGenius.setAutoReconnect(ar);
    }

    // Wire TgxlConnection to TunerModel
    m_radioModel.tunerModel().setDirectConnection(&m_tgxlConn);
    // Also attempt connection when TGXL IP arrives (may come after presence)
    connect(&m_radioModel.tunerModel(), &TunerModel::stateChanged, this, [this]() {
        auto* tuner = &m_radioModel.tunerModel();
        if (tuner->isPresent() && !tuner->tgxlIp().isEmpty() && !m_tgxlConn.isConnected()) {
            m_tgxlConn.connectToTgxl(tuner->tgxlIp());
        }
    });

    // Auto-connect to PGXL when detected
    connect(&m_radioModel, &RadioModel::amplifierChanged, this, [this](bool present) {
        if (present && !m_radioModel.ampIp().isEmpty() && !m_pgxlConn.isConnected()) {
            m_pgxlConn.connectToPgxl(m_radioModel.ampIp());
        } else if (!present) {
            m_pgxlConn.disconnect();
        }
    });
    // PGXL status → AmpApplet (direct telemetry: vac, vdd, id, temp, tempb, state, etc.)
    connect(&m_pgxlConn, &PgxlConnection::statusUpdated, this, [this](const QMap<QString, QString>& kvs) {
        qCDebug(lcTuner) << "PGXL status:" << kvs;
        auto* amp = m_appletPanel->ampApplet();
        if (kvs.contains("temp")) {
            // Some PGXL firmware encodes both PA module temps as "A/B" in a
            // single field (e.g. "30.5/26.5"); others use separate tempb key.
            const QString tv = kvs["temp"];
            const int slash = tv.indexOf('/');
            if (slash >= 0) {
                amp->setTemp(tv.left(slash).toFloat());
                amp->setTempB(tv.mid(slash + 1).toFloat());
            } else {
                amp->setTemp(tv.toFloat());
            }
        }
        // Separate tempb field (firmware variant)
        if (kvs.contains("tempb"))
            amp->setTempB(kvs["tempb"].toFloat());
        if (kvs.contains("id"))
            amp->setDrainCurrent(kvs["id"].toFloat());
        if (kvs.contains("vdd"))
            amp->setDrainVoltage(kvs["vdd"].toFloat());
        if (kvs.contains("vac"))
            amp->setMainsVoltage(kvs["vac"].toInt());
        if (kvs.contains("state"))
            amp->setState(kvs["state"]);
        if (kvs.contains("fanmode"))
            amp->setFanMode(kvs["fanmode"]);
        if (kvs.contains("meffa"))
            amp->setMeff(kvs["meffa"]);
        // Convert PGXL dBm to watts and feed S-Meter alongside radio meters.
        // Use peakfwd (actual peak power) not fwd (floor/minimum).
        // Skip when amp is STANDBY — peakfwd reads ~0 dBm in standby and would
        // stomp on the exciter feed that should drive the barefoot scale.
        if (kvs.contains("peakfwd") && m_radioModel.hasAmplifier()
                && m_radioModel.ampOperate()) {
            float dbm = kvs["peakfwd"].toFloat();
            float watts = std::pow(10.0f, (dbm - 30.0f) / 10.0f);
            qCDebug(lcTuner) << "PGXL→SMeter: peakfwd=" << dbm << "dBm =" << watts << "W";
            float swr = 1.0f;
            if (kvs.contains("swr")) {
                float rl = std::abs(kvs["swr"].toFloat());
                float rho = std::pow(10.0f, -rl / 20.0f);
                swr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.0f;
            }
            // Ensure S-Meter is in TX mode when PGXL reports transmitting
            if (kvs.value("state").startsWith("TRANSMIT"))
                m_appletPanel->sMeterWidget()->setTransmitting(true);
            else if (kvs.contains("state") && !kvs.value("state").startsWith("TRANSMIT"))
                m_appletPanel->sMeterWidget()->setTransmitting(false);
            m_appletPanel->sMeterWidget()->setTxMeters(watts, swr);
        }
    });
    connect(&m_pgxlConn, &PgxlConnection::connected, this, [this]() {
        qDebug() << "PGXL direct connection established, version:" << m_pgxlConn.version();
        m_appletPanel->ampApplet()->setDirectConnected(true);
    });
    connect(&m_pgxlConn, &PgxlConnection::disconnected, this, [this]() {
        m_appletPanel->ampApplet()->setDirectConnected(false);
    });
    // Radio amplifier status → AmpApplet telemetry (fallback path).
    // The radio proxies PGXL telemetry fields (id, vac, vdd, meffa, temp, tempb, state) in its
    // amplifier status messages, so the applet keeps updating even when the direct
    // PGXL TCP connection isn't established.  When direct TCP IS connected, that
    // path is faster and higher-precision (the radio rebroadcast may round/lag),
    // so we skip the radio fallback to avoid display jitter from two paths
    // alternately writing slightly-different values.
    connect(&m_radioModel, &RadioModel::ampTelemetryUpdated,
            this, [this](const QMap<QString, QString>& kvs) {
        if (m_pgxlConn.isConnected()) return;
        auto* amp = m_appletPanel->ampApplet();
        if (kvs.contains("temp")) {
            const QString tv = kvs["temp"];
            const int slash = tv.indexOf('/');
            if (slash >= 0) {
                amp->setTemp(tv.left(slash).toFloat());
                amp->setTempB(tv.mid(slash + 1).toFloat());
            } else {
                amp->setTemp(tv.toFloat());
            }
        }
        if (kvs.contains("tempb"))
            amp->setTempB(kvs["tempb"].toFloat());
        if (kvs.contains("id"))
            amp->setDrainCurrent(kvs["id"].toFloat());
        if (kvs.contains("vdd"))
            amp->setDrainVoltage(kvs["vdd"].toFloat());
        if (kvs.contains("vac"))
            amp->setMainsVoltage(kvs["vac"].toInt());
        if (kvs.contains("state"))
            amp->setState(kvs["state"]);
        if (kvs.contains("meffa"))
            amp->setMeff(kvs["meffa"]);
    });
    // Fan mode cycle button → direct PGXL command (fan control is not in the radio API)
    connect(m_appletPanel->ampApplet(), &AmpApplet::fanModeChanged, this, [this](const QString& mode) {
        m_pgxlConn.sendCommand(QString("setup fanmode=%1").arg(mode));
    });
    // OPERATE button → PGXL standby/operate command via radio amplifier API
    connect(m_appletPanel->ampApplet(), &AmpApplet::operateToggled, this, [this](bool on) {
        if (!m_radioModel.ampHandle().isEmpty())
            m_radioModel.sendCommand(
                QString("amplifier set %1 operate=%2").arg(m_radioModel.ampHandle()).arg(on ? 1 : 0));
    });

    // Switch Fwd Power gauge scale based on radio max power and amplifier presence.
    // All three power gauges (TxApplet, TunerApplet, SMeterWidget) update together.
    // When the PGXL is in STANDBY we fall back to the barefoot scale — only the
    // radio's forward power is reaching the meter, so the 2kW arc would make
    // every reading look tiny and useless.
    auto updatePowerScale = [this]() {
        int maxW = m_radioModel.transmitModel().maxPowerLevel();
        // Aurora (AU-) radios have an integrated 600W PA (Overlord) but
        // max_power_level only reports the exciter limit (100W). Use model
        // name to detect the true PA capability. (#484)
        const QString& model = m_radioModel.model();
        if (model.startsWith("AU-") && maxW <= 100) {
            maxW = 500;
        }
        const bool ampActive = m_radioModel.hasAmplifier()
                            && m_radioModel.ampOperate();
        m_appletPanel->txApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->tunerApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->sMeterWidget()->setPowerScale(maxW, ampActive);
        m_appletPanel->healthApplet()->setPowerScale(maxW, ampActive);
    };
    connect(&m_radioModel, &RadioModel::amplifierChanged, this, updatePowerScale);
    connect(&m_radioModel, &RadioModel::ampStateChanged, this, updatePowerScale);

    // TGXL indicator: two-line rich text — label on top, state smaller below.
    // Green = OPERATE, amber = BYPASS, grey = STANDBY (matches SmartSDR)
    auto setIndicatorHtml = [](QLabel* nameLbl, QLabel* stateLbl,
                               const QString& state, const QString& color) {
        nameLbl->setStyleSheet(
            QString("QLabel { color: %1; font-size:18px; font-weight:bold; }").arg(color));
        stateLbl->setStyleSheet(
            QString("QLabel { color: %1; font-size:11px; }").arg(color));
        stateLbl->setText(state);
    };

    auto updateTgxlStyle = [this, setIndicatorHtml]() {
        auto& t = m_radioModel.tunerModel();
        if (t.isOperate() && !t.isBypass())
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "OPERATE", "#00e060");
        else if (t.isOperate() && t.isBypass())
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "BYPASS", "#e0a000");
        else
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "STANDBY", "#404858");
    };
    connect(&m_radioModel.tunerModel(), &TunerModel::stateChanged, this, updateTgxlStyle);

    // PGXL indicator: OPERATE (green) or STANDBY (grey) — no bypass for PGXL
    auto updatePgxlStyle = [this, setIndicatorHtml]() {
        if (m_radioModel.ampOperate())
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "OPERATE", "#00e060");
        else
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "STANDBY", "#404858");
    };
    connect(&m_radioModel, &RadioModel::ampStateChanged, this, [this, updatePgxlStyle]() {
        updatePgxlStyle();
        // Sync the AmpApplet button — the direct PGXL TCP path may not deliver
        // a state update fast enough, leaving the button stuck on the old state.
        // RadioModel is authoritative; use it to keep the button consistent.
        m_appletPanel->ampApplet()->setState(
            m_radioModel.ampOperate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    });

    connect(&m_radioModel, &RadioModel::amplifierChanged, this, [this, updatePgxlStyle](bool present) {
        m_pgxlContainer->setVisible(present);
        m_pgxlSeparator->setVisible(present);
        m_appletPanel->setAmpVisible(present);
        if (present) {
            updatePgxlStyle();
            m_appletPanel->ampApplet()->setState(
                m_radioModel.ampOperate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
        }
    });
    connect(&m_radioModel.meterModel(), &MeterModel::ampMetersChanged,
            this, [this](float fwdPwr, float swr, float temp) {
        m_appletPanel->ampApplet()->setFwdPower(fwdPwr);
        m_appletPanel->ampApplet()->setSwr(swr);
        m_appletPanel->ampApplet()->setTemp(temp);
        // S-Meter TX power follows the scale: amp output when PGXL is OPERATE,
        // exciter output when it's STANDBY (txMetersChanged already handles that
        // path, so we just stop overriding it here).
        if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate()) {
            m_appletPanel->sMeterWidget()->setTxMeters(fwdPwr, swr);
            static int ampDbg = 0;
            if (++ampDbg % 50 == 1)
                qCDebug(lcTuner) << "AMP→SMeter: fwd=" << fwdPwr << "W swr=" << swr;
        }
    });
    connect(&m_radioModel.transmitModel(), &TransmitModel::maxPowerLevelChanged,
            this, updatePowerScale);

    // ── Meter applet: all meters consolidated ──────────────────────────────
    m_appletPanel->meterApplet()->setMeterModel(&m_radioModel.meterModel());

    // ── HLTH applet: same meter model — derives antenna-health state from
    //    SWR / power trends across radio / tuner / amp sources.
    m_appletPanel->healthApplet()->setMeterModel(&m_radioModel.meterModel());

    // ── TX applet: meters + model ───────────────────────────────────────────
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_appletPanel->txApplet(), &TxApplet::updateMeters);
    // PEP peak-hold tick on the FWDPWR gauge: feed the raw pre-smoothed
    // sample and reset the hold on un-key. (#2561)
    connect(&m_radioModel.meterModel(), &MeterModel::txPeakChanged,
            m_appletPanel->txApplet(), &TxApplet::updatePeakPower);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel->txApplet(), &TxApplet::setTransmitting);
    m_appletPanel->txApplet()->setTransmitModel(&m_radioModel.transmitModel());
    m_appletPanel->txApplet()->setTunerModel(&m_radioModel.tunerModel());
    // ATU right-click → pre-tune dialog needs RadioModel for slice access
    // and BandPlanManager for region-correct band edges. (#2624)
    m_appletPanel->txApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->txApplet()->setBandPlanManager(m_bandPlanMgr);
    m_appletPanel->rxApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->rxApplet()->setTransmitModel(&m_radioModel.transmitModel());

    // Hide APD row on radios that don't support it
    connect(&m_radioModel.transmitModel(), &TransmitModel::apdStateChanged, this, [this]() {
        m_appletPanel->txApplet()->setApdVisible(
            m_radioModel.transmitModel().apdConfigurable());
    });

    // ── External controllers run on a dedicated worker thread (#502) ────
    // FlexControl, SerialPort, and MIDI controllers are created on the
    // worker thread so their I/O (serial port, RtMidi callbacks, poll timers)
    // never competes with paintEvent. Signals auto-queue to main thread.
    m_extCtrlThread = new QThread(this);
    m_extCtrlThread->setObjectName("ExtControllers");

    // Shared FlexControl coalescing for the USB device and the virtual
    // FlexControl dialog. The timer stays on the main thread because it
    // reads the active slice and updates UI state.
    m_flexCoalesceTimer.setSingleShot(true);
    m_flexCoalesceTimer.setInterval(20);
    connect(&m_flexCoalesceTimer, &QTimer::timeout, this, [this]() {
        if (m_flexTargetMhz < 0.0) return;
        auto* s = activeSlice();
        if (!s) { m_flexTargetMhz = -1.0; return; }
        if (s->isLocked()) {
            s->notifyTuneBlockedByLock();
            // Drop queued tuning so unlock does not replay stale wheel input.
            m_flexTargetMhz = -1.0;
            return;
        }
        const double target = m_flexTargetMhz;
        applyTuneRequest(s, target, TuneIntent::IncrementalTune, "flexcontrol");
    });

#ifdef HAVE_SERIALPORT
    m_serialPort = new SerialPortController;  // no parent — moved to thread
    m_serialPort->moveToThread(m_extCtrlThread);
    m_flexControl = new FlexControlManager;
    m_flexControl->moveToThread(m_extCtrlThread);

    // Serial port signals (auto-queued from worker → main)
    connect(m_serialPort, &SerialPortController::externalPttChanged,
            this, [this](bool active) {
        m_radioModel.setTransmit(active);
    });
    connect(m_serialPort, &SerialPortController::cwKeyChanged,
            this, [this](bool down) {
        m_radioModel.sendCwKey(down);
    });
    connect(m_serialPort, &SerialPortController::cwPaddleChanged,
            this, [this](bool dit, bool dah) {
        m_lastCwPaddleTraceId.store(0, std::memory_order_relaxed);
        m_lastCwPaddleSourceMs.store(0, std::memory_order_relaxed);
        // When the local iambic keyer is running, feed it the raw paddle
        // state — it forwards to the radio AND drives the sidetone gate
        // directly.  Otherwise pass straight through to the radio (radio's
        // RF iambic is still authoritative for the on-air signal).
        if (m_iambicKeyer && m_iambicKeyer->isRunning()) {
            m_iambicKeyer->setPaddleState(dit, dah);
        } else {
            m_radioModel.sendCwPaddle(dit, dah);
        }
    });

    // FlexControl signals (auto-queued from worker → main)
    connect(m_flexControl, &FlexControlManager::tuneSteps,
            this, &MainWindow::handleFlexControlTuneSteps);

    connect(m_flexControl, &FlexControlManager::buttonPressed,
            this, &MainWindow::handleFlexControlButton);
    connect(m_flexControl, &FlexControlManager::buttonPressed,
            this, [this](int button, int action) {
        if (m_hidEncoder->isTMate2())
            noteTMate2Interaction();
        if (m_flexControlDialog)
            m_flexControlDialog->reflectButtonPress(button, action);
    });

    connect(m_flexControl, &FlexControlManager::connectionChanged,
            this, [this](bool connected) {
        m_flexControlConnected = connected;
        const QString port = connected && m_flexControl
            ? m_flexControl->portName()
            : QString();
        if (m_flexControlDialog)
            m_flexControlDialog->setPhysicalReady(connected, port);
        if (m_radioSetupDialog)
            m_radioSetupDialog->setFlexControlConnectionStatus(connected, port);
    });
#endif

#ifdef HAVE_MIDI
    m_midiControl = new MidiControlManager;
    m_midiControl->moveToThread(m_extCtrlThread);
    m_midiTuneIdleTimer.setSingleShot(true);
    m_midiTuneIdleTimer.setInterval(250);
    connect(&m_midiTuneIdleTimer, &QTimer::timeout, this, [this] {
        m_midiTuneTargetMhz = -1.0;
    });

    // Register MIDI params — setters/getters stored on MainWindow for
    // main-thread dispatch. Param metadata still registered on the manager.
    registerMidiParams();

    // MIDI paramActionTrace signal: dispatches setter on main thread (#502)
    // and carries timing metadata for CW/netCW diagnostics.
    connect(m_midiControl, &MidiControlManager::paramActionTrace,
            this, [this](const QString& paramId, float scaledValue,
                         quint64 traceId, quint64 midiCallbackMs,
                         quint64 midiDispatchMs) {
        auto it = m_midiSetters.find(paramId);
        if (it == m_midiSetters.end()) return;
        const quint64 mainMs = cwTraceNowMs();
        if (isCwMomentaryActionId(paramId) && lcCw().isDebugEnabled()) {
            qCDebug(lcCw).noquote().nospace()
                << "CW MIDI main trace=" << traceId
                << " t=" << mainMs << "ms"
                << " param=" << paramId
                << " callbackToMainMs=" << static_cast<qint64>(mainMs - midiCallbackMs)
                << " dispatchToMainMs=" << static_cast<qint64>(mainMs - midiDispatchMs)
                << " scaled=" << QString::number(scaledValue, 'f', 3);
        }
        m_currentMidiTrace = {paramId, traceId, midiCallbackMs, midiDispatchMs};
        if (scaledValue == -1.0f) {
            // Toggle sentinel: read getter, flip, call setter
            auto git = m_midiGetters.find(paramId);
            float cur = (git != m_midiGetters.end() && *git) ? (*git)() : 0.0f;
            it.value()(cur > 0.5f ? 0.0f : 1.0f);
        } else {
            it.value()(scaledValue);
        }
        m_currentMidiTrace = {};
    });

    // MIDI relativeAction signal: coalesced step-based tuning. VFO tune knob
    // steps are exact detents; controller-side jog speed is not multiplied here.
    connect(m_midiControl, &MidiControlManager::relativeAction,
            this, [this](const QString& paramId, int steps) {
        if (paramId == "rx.tuneKnob") {
            auto* s = activeSlice();
            if (!s) {
                m_midiTuneTargetMhz = -1.0;
                m_midiTuneIdleTimer.stop();
                return;
            }
            if (s->isLocked()) {
                s->notifyTuneBlockedByLock();
                m_midiTuneTargetMhz = -1.0;
                m_midiTuneIdleTimer.stop();
                return;
            }
            // Prefer spectrum widget's step (updates immediately on UI change,
            // consistent with keyboard and HID encoder tuning paths).
            int stepHz = (spectrum() && spectrum()->stepSize() > 0)
                         ? spectrum()->stepSize() : s->stepHz();
            if (stepHz <= 0) return;
            // Keep an in-flight target while the wheel is moving.  Radio
            // RF_frequency echoes can lag behind command sends; using the echo
            // as the next base makes rapid MIDI tuning jump backward/forward.
            if (m_midiTuneTargetMhz < 0.0
                || (!m_midiTuneIdleTimer.isActive()
                    && std::abs(m_midiTuneTargetMhz - s->frequency()) > 0.001)) {
                const long long curHz =
                    static_cast<long long>(std::round(s->frequency() * 1e6));
                const long long snapped =
                    ((curHz + stepHz / 2) / stepHz) * stepHz;
                m_midiTuneTargetMhz = snapped / 1e6;
            }
            m_midiTuneTargetMhz += steps * stepHz / 1e6;
            if (spectrum()) spectrum()->setVfoFrequency(m_midiTuneTargetMhz);
            m_midiTuneIdleTimer.start();
            applyTuneRequest(s, m_midiTuneTargetMhz, TuneIntent::IncrementalTune,
                             "midi-relative");
        }
    });

    MidiSettings::instance().load();
    auto savedBindings = MidiSettings::instance().loadBindings();
    for (const auto& b : savedBindings)
        m_midiControl->addBinding(b);
#endif

#ifdef HAVE_HIDAPI
    m_hidEncoder = new HidEncoderManager;
    m_hidEncoder->moveToThread(m_extCtrlThread);
    m_tmate2OverlayTimer.setSingleShot(true);
    connect(&m_tmate2OverlayTimer, &QTimer::timeout, this, [this] {
        m_tmate2Overlay = TMate2Overlay::None;
        m_tmate2OverlayUntilMs = 0;
        updateTMate2Display();
        updateTMate2Indicators();
        restartTMate2IdleTimer();
    });
    m_tmate2IdleTimer.setSingleShot(true);
    connect(&m_tmate2IdleTimer, &QTimer::timeout, this, [this] {
        blankTMate2Display();
    });

    // Hold-detection timers for RC-28 F1/F2 — single-shot 600 ms, one per key so
    // both can be held at once without clobbering each other's state. (#3323)
    for (int i = 0; i < 2; ++i) {
        const int btn = i + 1;
        m_rc28HoldTimer[i] = new QTimer(this);
        m_rc28HoldTimer[i]->setSingleShot(true);
        m_rc28HoldTimer[i]->setInterval(600);
        connect(m_rc28HoldTimer[i], &QTimer::timeout, this, [this, btn] {
            m_rc28HoldConsumed[btn - 1] = true;
            const QString holdField = (btn == 1) ? QStringLiteral("f1Hold")
                                                 : QStringLiteral("f2Hold");
            const QString dflt = (btn == 1) ? QStringLiteral("TuneFast")
                                            : QStringLiteral("ModeCycle");
            const QString actionName = HidEncoderManager::rc28MappingField(holdField, dflt);
            if (actionName.isEmpty() || actionName == "None") return;
            // Hold actions are latched toggles: each hold press flips the state and
            // it stays that way until the next hold press.
            dispatchHidAction(actionName, QString("F%1 hold").arg(btn));
        });
    }

    // Per-encoder action dispatch — routes each dial to its configured action.
    // applyFlexControlWheelAction handles coalescing internally for frequency.
    connect(m_hidEncoder, &HidEncoderManager::tuneSteps,
            this, [this](int encoderIndex, int steps) {
        if (m_hidEncoder->isTMate2())
            noteTMate2Interaction();
        // Fast/Fine direct-tune mode (frequency only) — respect the slice lock
        // here since this path bypasses applyFlexControlWheelAction's own guard.
        // Fast mode: 100 Hz per tick. Fine mode: 1 Hz per tick.
        if (encoderIndex == 0 && (m_hidFastTune || m_hidFineTune)) {
            if (auto* s = activeSlice()) {
                if (s->isLocked()) { s->notifyTuneBlockedByLock(); return; }
                const double stepMhz = m_hidFastTune ? 0.0001 : 0.000001;
                applyTuneRequest(s, s->frequency() + steps * stepMhz,
                                 TuneIntent::IncrementalTune,
                                 m_hidFastTune ? "rc28-fast" : "rc28-fine");
            }
            return;
        }
        const bool isTMate2 = m_hidEncoder->isTMate2();
        const QString actionId = AppSettings::instance()
            .value(QString(isTMate2 ? "TMate2EncoderAction%1" : "HidEncoderAction%1")
                       .arg(encoderIndex),
                   isTMate2 ? tmate2EncoderDefaultAction(encoderIndex)
                             : MainWindow::hidEncoderDefaultAction(encoderIndex))
            .toString();
        applyFlexControlWheelAction(actionId, steps);
    });

    connect(m_hidEncoder, &HidEncoderManager::buttonPressed,
            this, [this](int button, int action) {

        // ── RC-28 F1 / F2: deferred press + hold detection (#3323) ──────────
        // Per-key state (index 0=F1, 1=F2) so the two keys are independent.
        // On press (action==0): start that key's 600 ms hold timer; do NOT fire
        // the short-press action yet.  On the timer firing: run the hold action
        // and set the key's consumed flag.  On release (action==1): if the timer
        // didn't fire, run the short-press action; otherwise clear the flag.
        if (m_hidEncoder->isRC28Compatible() && (button == 1 || button == 2)) {
            const int i = button - 1;
            if (action == 0) {
                m_rc28HoldConsumed[i] = false;
                m_rc28HoldTimer[i]->start();
            } else {  // action == 1 (release)
                m_rc28HoldTimer[i]->stop();
                if (!m_rc28HoldConsumed[i]) {
                    // Short press — fire on release
                    const QString dflt = (button == 1) ? QStringLiteral("StepUp")
                                                       : QStringLiteral("StepDown");
                    const QString pressField = (button == 1) ? QStringLiteral("f1Press")
                                                             : QStringLiteral("f2Press");
                    const QString actionName =
                        HidEncoderManager::rc28MappingField(pressField, dflt);
                    if (!actionName.isEmpty() && actionName != "None")
                        dispatchHidAction(actionName,
                                          QString("F%1 press").arg(button));
                }
                m_rc28HoldConsumed[i] = false;
                // Update the LEDs once the button is up. We never write LEDs
                // while a button is held (that broke the F1 LED); this is the
                // single post-release write, matching FlexRC-28's model. The
                // small delay lets the release report settle first.
                QTimer::singleShot(80, this, [this]{ updateRC28Leds(); });
            }
            return;
        }

        QString actionName;
        if (m_hidEncoder->isRC28Compatible() && button == 3) {
            // RC-28 TX bar is hardwired to PTT (mode configured in the RC-28
            // dialog); it is not remappable via the generic HID-key settings.
            actionName = QStringLiteral("PTT");
        } else if (m_hidEncoder->isTMate2() && button >= 1 && button <= 6) {
            actionName = AppSettings::instance()
                .value(QString("TMate2KeyAction%1").arg(button - 1),
                       tmate2KeyDefaultAction(button - 1))
                .toString();
        } else if (m_hidEncoder->isTMate2() && button >= 9 && button <= 11) {
            const int enc = button - 9;
            actionName = AppSettings::instance()
                .value(QString("TMate2PushAction%1").arg(enc),
                       tmate2PushDefaultAction(enc))
                .toString();
        } else if (button >= 1 && button <= 8) {
            // Generic HID keys (StreamDeck+ LCD buttons 1–8).
            actionName = AppSettings::instance()
                .value(QString("HidKeyAction%1").arg(button - 1), QStringLiteral("None"))
                .toString();
        } else if (button >= 9 && button <= 12) {
            // Encoder press buttons — settings key HidEncoderPushAction{0-3}
            const int enc = button - 9;
            actionName = AppSettings::instance()
                .value(QString("HidEncoderPushAction%1").arg(enc),
                       MainWindow::hidEncoderDefaultPushAction(enc))
                .toString();
        } else {
            return;
        }

        if (actionName == "None" || actionName.isEmpty()) return;

        // PTT: behaviour depends on the RC28Mapping pttMode field, but only
        // when the source is the RC-28 TX bar.  Generic HID keys bound to
        // "PTT" (e.g. StreamDeck+) always use momentary so they are not
        // affected by the RC-28 latched-PTT setting.
        if (actionName == QStringLiteral("PTT")) {
            if (!m_radioModel.isConnected()) return;
            const bool latched = m_hidEncoder->isRC28Compatible()
                && HidEncoderManager::rc28MappingField("pttMode", "Momentary") == "Latched";
            if (latched) {
                // Toggle on press only; ignore release.
                if (action == 0) {
                    m_rc28PttLatched = !m_rc28PttLatched;
                    m_radioModel.setTransmit(m_rc28PttLatched);
                    if (m_rc28MappingDialog && m_hidEncoder->isRC28Compatible())
                        m_rc28MappingDialog->appendButtonEvent(
                            "TX bar press",
                            m_rc28PttLatched ? "TX ON (Latched)" : "TX OFF (Unlatched)");
                }
            } else {
                // Momentary: hold = TX on, release = TX off.
                m_radioModel.setTransmit(action == 0);
                if (m_rc28MappingDialog && m_hidEncoder->isRC28Compatible())
                    m_rc28MappingDialog->appendButtonEvent(
                        "TX bar",
                        action == 0 ? "TX ON (Momentary)" : "TX OFF (Momentary)");
            }
            return;
        }

        // All other actions fire only on press.
        if (action != 0) return;

        // All remaining actions share the same dispatch path used by
        // hold-detection and short-press-on-release.  (#3323)
        dispatchHidAction(actionName, QString("key %1 press").arg(button));
    });

    connect(m_hidEncoder, &HidEncoderManager::connectionChanged,
            this, [this](bool connected, const QString& name) {
        qCDebug(lcDevices) << "HID encoder:" << (connected ? "connected" : "disconnected") << name;
        if (connected) {
            if (m_hidEncoder->isTMate2())
                noteTMate2Interaction();
            refreshStreamDeckLabels();
            updateRC28Leds();
            updateTMate2Display();
            updateTMate2Indicators();
        } else if (m_hidEncoder->isRC28Compatible()) {
            // RC-28 gone: stop any in-flight hold timers so they can't fire an
            // action for an absent encoder, and clear RC-28 stateful flags so a
            // reconnect starts from a clean state. (m_openVid/Pid survive close(),
            // so isRC28Compatible() still identifies the departed device here.)
            for (int i = 0; i < 2; ++i) {
                m_rc28HoldTimer[i]->stop();
                m_rc28HoldConsumed[i] = false;
            }
            m_hidFastTune = false;
            m_hidFineTune = false;
            if (m_rc28PttLatched) {
                // Safety: never leave the radio keyed if a latched-TX RC-28 is
                // unplugged. Drop TX and clear the latch.
                m_rc28PttLatched = false;
                m_radioModel.setTransmit(false);
            }
        } else {
            m_tmate2IdleTimer.stop();
            m_tmate2DisplayBlanked = false;
        }
    });

    // RC-28 TX LED: mirror MOX state to the device's red TRANSMIT LED.
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        updateRC28Leds();
        updateTMate2Display();
        updateTMate2Indicators();
        if (!tx) m_tmate2TxWatts = 0.0f;  // reset cached watts when leaving TX
    });

    // RC-28 F-key LED for a Mute hold action — mute is global (not slice-scoped),
    // so refresh whenever it changes from any source (RC-28, GUI, MIDI…).
    connect(m_audio, &AudioEngine::mutedChanged,
            this, [this](bool) { updateRC28Leds(); });

    // StreamDeck native integration removed — use TCI StreamController plugin instead.
#endif

    // Ulanzi Dial backend.  One concrete implementation per platform —
    // EvdevEncoderManager on Linux (EVIOCGRAB-exclusive),
    // UlanziDialWindowsManager on Windows (hidapi polling),
    // UlanziDialMacOSManager on macOS (IOKit with seize-device).
    // All three expose the same Qt signal contract via the
    // UlanziDialBackend alias.  See #3232 for design notes.
    m_dialBackend = new UlanziDialBackend;
#ifndef Q_OS_MAC
    // macOS keeps the backend on the main thread: IOHIDManager schedules
    // its callbacks on a CFRunLoop, and only the main thread has one
    // integrated with Qt's event loop on macOS.  Linux + Windows
    // backends run cleanly on the dedicated ExtControllers thread.
    m_dialBackend->moveToThread(m_extCtrlThread);
#endif


    m_dialCoalesceTimer.setSingleShot(true);
    m_dialCoalesceTimer.setInterval(20);
    connect(&m_dialCoalesceTimer, &QTimer::timeout, this, [this]() {
        if (m_dialPendingSteps == 0) return;
        auto* s = activeSlice();
        if (!s) { m_dialPendingSteps = 0; return; }
        if (s->isLocked()) {
            s->notifyTuneBlockedByLock();
            m_dialPendingSteps = 0;
            return;
        }
        int stepHz = spectrum() ? spectrum()->stepSize() : 100;
        double newMhz = s->frequency() + m_dialPendingSteps * stepHz / 1e6;
        m_dialPendingSteps = 0;
        applyTuneRequest(s, newMhz, TuneIntent::IncrementalTune, "ulanzi-dial");
    });

    connect(m_dialBackend, &UlanziDialBackend::tuneSteps,
            this, [this](int steps) {
        m_dialPendingSteps += steps;
        if (!m_dialCoalesceTimer.isActive())
            m_dialCoalesceTimer.start();
    });

    connect(m_dialBackend, &UlanziDialBackend::buttonEvent,
            this, [this](const QString& signature, int action) {
        if (action != 1) return;   // only fire on press, not release
        // Look up which pill this hardware signature is bound to (the
        // signature ↔ pill mapping is immutable, in kPillSpecs).  Then
        // look up the user-chosen action for that pill in AppSettings.
        const QString pillId = UlanziDialMapperDialog::pillForSignature(signature);
        if (pillId.isEmpty()) {
            qDebug() << "Ulanzi Dial: unmapped signature" << signature;
            return;
        }
        // Read the user-bound action; fall back to the built-in default
        // for this pill so first-launch bindings work without the user
        // having opened the mapper dialog at all.
        const QString actionId = AppSettings::instance().value(
            UlanziDialMapperDialog::actionSettingsKey(pillId),
            UlanziDialMapperDialog::defaultActionForPill(pillId)).toString();
        // Action IDs are prefix-tagged to disambiguate registries:
        //   "None"         — intentionally unassigned
        //   "shortcut:ID"  — invoke ShortcutManager handler
        //   "midi:ID"      — invoke MidiControlManager setter (Toggle
        //                    flips current value; Trigger sends rangeMax)
        if (actionId == "None") return;
        if (actionId.startsWith("shortcut:")) {
            const QString id = actionId.mid(QStringLiteral("shortcut:").size());
            auto* a = m_shortcutManager.action(id);
            if (a && a->handler) a->handler();
            else qDebug() << "Ulanzi Dial: unknown shortcut" << id;
        }
#ifdef HAVE_MIDI
        else if (actionId.startsWith("midi:")) {
            const QString id = actionId.mid(QStringLiteral("midi:").size());
            const auto* p = m_midiControl ? m_midiControl->findParam(id) : nullptr;
            if (!p || !p->setter) {
                qDebug() << "Ulanzi Dial: unknown MIDI param" << id;
            } else if (p->type == MidiParamType::Toggle) {
                const float mid = (p->rangeMin + p->rangeMax) / 2.0f;
                const float cur = p->getter ? p->getter() : p->rangeMin;
                p->setter(cur > mid ? p->rangeMin : p->rangeMax);
            } else if (p->type == MidiParamType::Trigger) {
                p->setter(p->rangeMax);
            }
        }
#endif
        else {
            qDebug() << "Ulanzi Dial:" << pillId << "→ unknown action" << actionId;
        }
    });

    connect(m_dialBackend, &UlanziDialBackend::connectionChanged,
            this, [](bool connected, const QString& name) {
        qDebug() << "Ulanzi Dial:" << (connected ? "connected" : "disconnected") << name;
    });

    // Kick off scanning on the external-controller thread — only if the
    // user has opted in. On macOS, the backend's IOHIDManagerOpen(...,
    // kIOHIDOptionsTypeSeizeDevice) trips the OS Input Monitoring TCC
    // prompt the moment it is called, regardless of whether a Ulanzi Dial
    // is actually present. Defaulting this off so the vast majority of
    // users — who do not own the peripheral — never see the prompt (#3257).
    if (AppSettings::instance().value("UlanziDialEnabled", "False").toString() == "True") {
        QMetaObject::invokeMethod(m_dialBackend, &UlanziDialBackend::start,
                                  Qt::QueuedConnection);
    }

    // Start the external controller thread — objects are already moved
    m_extCtrlThread->start();

    // Init that must happen on the worker thread (serial port open, etc.)
#ifdef HAVE_SERIALPORT
    QMetaObject::invokeMethod(m_serialPort, [this] {
        m_serialPort->loadSettings();
    });
    m_flexControl->setInvertDirection(
        AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True");
    if (AppSettings::instance().value("FlexControlAutoDetect", "True").toString() == "True") {
        QString fcPort = FlexControlManager::detectPort();
        if (!fcPort.isEmpty()) {
            QMetaObject::invokeMethod(m_flexControl, [this, fcPort] {
                m_flexControl->open(fcPort);
            });
        }
    }
#endif
#ifdef HAVE_MIDI
    if (MidiSettings::instance().autoConnect()) {
        QString dev = MidiSettings::instance().lastDevice();
        if (!dev.isEmpty()) {
            QMetaObject::invokeMethod(m_midiControl, [this, dev] {
                m_midiControl->openPortByName(dev);
            });
        }
    }
#endif

#ifdef HAVE_HIDAPI
    // One-time migration: existing users had HidEncoderAutoDetect=True
    // with the old always-on loadSettings() pattern, and a working
    // RC-28 / PowerMate / Shuttle / StreamDeck+.  Honour that prior
    // intent on first launch after this upgrade so their controller
    // doesn't silently stop working until they discover the new
    // checkbox.  Only flips the new key once; subsequent toggles in
    // Radio Setup → Serial own it from then on.
    {
        auto& s = AppSettings::instance();
        if (!s.contains("HidEncoderEnabled")) {
            // Old code treated absence of HidEncoderAutoDetect as True (implicit default).
            // Match that: if the key was never written, the user had HID on.
            const bool hadAutodetect =
                s.value("HidEncoderAutoDetect", "True").toString() == "True";
            s.setValue("HidEncoderEnabled", hadAutodetect ? "True" : "False");
        } else if (!s.contains("HidEncoderEnabledMigrationV2")) {
            // V2 fix: the first migration wrote "False" for users who never set
            // HidEncoderAutoDetect (old implicit default was True, not False).
            // Correct those installs: if AutoDetect is absent they had it on.
            if (s.value("HidEncoderEnabled") == "False" &&
                    !s.contains("HidEncoderAutoDetect")) {
                s.setValue("HidEncoderEnabled", "True");
            }
            s.setValue("HidEncoderEnabledMigrationV2", "True");
        }
    }
    // Same TCC concern as the Ulanzi gate above (#3257). HidEncoderManager::
    // loadSettings() iterates the supported VID/PID list calling hid_open()
    // for autodetect; HIDAPI's macOS backend opens with
    // kIOHIDOptionsTypeSeizeDevice internally, so on every launch this
    // would prompt for Input Monitoring even on machines without any
    // supported encoder hardware. Default off; user enables in
    // Preferences → Serial when they connect a StreamDeck+ / RC-28 / etc.
    // One-time migration: before RC28Mapping was introduced, F1/F2 actions
    // were stored under the generic HidKeyAction0/1 keys. Run unconditionally
    // so users who had HID disabled at the time of this upgrade still get their
    // old actions migrated when they later enable HID via Preferences. The
    // inner guard (!s.contains("RC28Mapping")) makes it a true one-shot. (#3323)
    {
        auto& s = AppSettings::instance();
        if (!s.contains("RC28Mapping")) {
            const QString k0 = s.value("HidKeyAction0", "StepUp").toString();
            const QString k1 = s.value("HidKeyAction1", "StepDown").toString();
            if (k0 != "StepUp" && !k0.isEmpty() && k0 != "None")
                HidEncoderManager::setRc28MappingField("f1Press", k0);
            if (k1 != "StepDown" && !k1.isEmpty() && k1 != "None")
                HidEncoderManager::setRc28MappingField("f2Press", k1);
        }
    }
    if (AppSettings::instance().value("HidEncoderEnabled", "False").toString() == "True") {
        QMetaObject::invokeMethod(m_hidEncoder, [this] {
            m_hidEncoder->loadSettings();
        });
    }
#endif

    // ── P/CW applet: mic meters + ALC meter + model ────────────────────────
    // Suppress radio CODEC meters when mic_selection=PC (they just show noise).
    // Client-side metering handles PC mic display below.
    // Compression gauge: full 20fps meter rate, gated on actual radio TX + PROC.
    {
        connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
                this, [this](float micLevel, float compLevel, float micPeak, float compPeak) {
            // Mic level: hardware mic uses radio meters, PC uses client-side
            if (m_radioModel.transmitModel().micSelection() != "PC")
                m_appletPanel->phoneCwApplet()->updateMeters(micLevel, compLevel, micPeak, 0.0f);

            // Compression has no useful meaning in RX; FLEX-8000 radios can
            // publish quiescent TX-chain meters there that look fully pegged.
            {
                const auto& tx = m_radioModel.transmitModel();
                const bool showCompression =
                    m_radioModel.isRadioTransmitting() && tx.speechProcessorEnable();
                m_appletPanel->phoneCwApplet()->updateCompression(
                    showCompression ? compPeak : 0.0f);
            }
        });
    }
    connect(&m_radioModel.meterModel(), &MeterModel::swAlcChanged,
            this, [this](float alc) {
        // FLEX-8000 TX-chain meters can publish quiescent RX values near 0 dBFS.
        // Only show SW ALC while the radio interlock says RF is actually keyed.
        m_appletPanel->phoneCwApplet()->updateAlc(
            m_radioModel.isRadioTransmitting() ? alc : -20.0f);
    });
    // Client-side PC mic metering — radio CODEC meters only see hardware mics.
    // Apply VU-style ballistics: fast attack, slow decay (~20 dB/sec).
    {
        auto heldLevel = std::make_shared<float>(-150.0f);
        auto heldPeak  = std::make_shared<float>(-150.0f);
        connect(m_audio, &AudioEngine::pcMicLevelChanged,
                this, [this, heldLevel, heldPeak](float peakDb, float avgDb) {
            if (m_radioModel.transmitModel().micSelection() != "PC" && !m_audio->isRadeMode()) return;
            constexpr float kDecayPerUpdate = 1.0f;  // ~20 dB/sec at 20 updates/sec
            // Level: fast attack, slow decay
            if (avgDb > *heldLevel)
                *heldLevel = avgDb;
            else
                *heldLevel = qMax(avgDb, *heldLevel - kDecayPerUpdate);
            // Peak: fast attack, slower decay
            if (peakDb > *heldPeak)
                *heldPeak = peakDb;
            else
                *heldPeak = qMax(*heldLevel, *heldPeak - kDecayPerUpdate * 0.5f);
            m_appletPanel->phoneCwApplet()->updateMeters(*heldLevel, 0.0f, *heldPeak, 0.0f);
        });
    }
    m_appletPanel->phoneCwApplet()->setTransmitModel(&m_radioModel.transmitModel());



    // ── PHNE applet: VOX + CW controls ──────────────────────────────────────
    m_appletPanel->phoneApplet()->setTransmitModel(&m_radioModel.transmitModel());

    // ── EQ applet: graphic equalizer ─────────────────────────────────────────
    m_appletPanel->eqApplet()->setEqualizerModel(&m_radioModel.equalizerModel());

    // ── Client EQ applets: dedicated TX and RX tiles (Phase 7.1) ───────────
    m_appletPanel->clientEqTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientEqRxApplet()->setAudioEngine(m_audio);

    auto wireEqEditOpen = [this](ClientEqApplet* applet) {
        connect(applet, &ClientEqApplet::editRequested, this,
                [this](ClientEqApplet::Path path) {
            ensureClientEqEditor()->showForPath(path);
        });
    };
    wireEqEditOpen(m_appletPanel->clientEqTxApplet());
    wireEqEditOpen(m_appletPanel->clientEqRxApplet());

    // Push TX low/high filter cutoffs to the EQ canvases as dashed yellow
    // guide lines.  Subscribes to the *dedicated* txFilterCutoffChanged
    // signal — NOT the omnibus phoneStateChanged which fires on every
    // VOX/CW/mic-boost/dexp/etc. transmit-status update and would
    // cascade unnecessary repaints into the audio path during TX.
    auto pushTxFilterCutoffsToEq = [this](int lo, int hi) {
        if (m_appletPanel && m_appletPanel->clientEqTxApplet())
            m_appletPanel->clientEqTxApplet()->setTxFilterCutoffs(lo, hi);
        if (m_clientEqEditor)
            m_clientEqEditor->setTxFilterCutoffs(lo, hi);
        if (m_aetherialStrip)
            m_aetherialStrip->setTxFilterCutoffs(lo, hi);
    };
    {
        const auto& tx = m_radioModel.transmitModel();
        pushTxFilterCutoffsToEq(tx.txFilterLow(), tx.txFilterHigh());
    }
    connect(&m_radioModel.transmitModel(), &TransmitModel::txFilterCutoffChanged,
            this, pushTxFilterCutoffsToEq);

    // RX filter passband guide lines on the RX EQ canvas — fed by the
    // currently-active RX slice.  filterLow / filterHigh on a slice are
    // *offsets* (e.g. -3000..0 for LSB, 0..3000 for USB, -3000..3000 for
    // AM); the EQ canvas plots in absolute audio-frequency.  Convert:
    //   audio_high = max(|lo|, |hi|)
    //   audio_low  = (lo and hi same sign / one zero) ? min(|lo|, |hi|) : 0
    // Then push to the docked RX-bound applet + floating editor (if open).
    // setActiveSlice() and SliceModel::filterChanged both call this lambda
    // so the guides track both slice swaps and live filter drags.
    pushRxFilterCutoffsToEq();
    connect(&m_radioModel, &RadioModel::sliceAdded, this, [this](SliceModel* s) {
        if (!s) return;
        connect(s, &SliceModel::filterChanged, this,
                [this, s](int /*lo*/, int /*hi*/) {
            if (s->sliceId() == m_activeSliceId)
                pushRxFilterCutoffsToEq();
        });
    });

    // ── Client Compressor applets: TX (#1661) + RX (Phase 7.3) ─────────────
    m_appletPanel->clientCompTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientCompRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientCompTxApplet(), &ClientCompApplet::editRequested,
            this, [this]() { ensureClientCompEditor()->showForTx(); });
    connect(m_appletPanel->clientCompRxApplet(), &ClientCompApplet::editRequested,
            this, [this]() { ensureClientCompEditor()->showForRx(); });

    // ── Client Gate applets: TX (#1661 Phase 2) + RX (Phase 7.2) ───────────
    m_appletPanel->clientGateTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientGateRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientGateTxApplet(), &ClientGateApplet::editRequested,
            this, [this]() { ensureClientGateEditor()->showForTx(); });
    connect(m_appletPanel->clientGateRxApplet(), &ClientGateApplet::editRequested,
            this, [this]() { ensureClientGateEditor()->showForRx(); });

    // ── Client De-esser applet: TX sidechain-filtered dynamics (#1661 Phase 3) ─
    m_appletPanel->clientDeEssApplet()->setAudioEngine(m_audio);

    // ── Client Tube applets: TX (#1661) + RX (Phase 7.4) ───────────────────
    m_appletPanel->clientTubeTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientTubeRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientTubeTxApplet(), &ClientTubeApplet::editRequested,
            this, [this]() { ensureClientTubeEditor()->showForTx(); });
    connect(m_appletPanel->clientTubeRxApplet(), &ClientTubeApplet::editRequested,
            this, [this]() { ensureClientTubeEditor()->showForRx(); });

    // ── Client Reverb applet: TX reverb (Freeverb) ─
    m_appletPanel->clientReverbApplet()->setAudioEngine(m_audio);

    // ── RX-side AetherDSP applet — same controls as the Settings menu
    // dialog, embedded as a docked tile in PooDoo Audio (RX).  Parameter
    // changes route through the same per-signal wiring used by the dialog,
    // factored into wireAetherDspWidget() to keep dialog + applet in sync.
    if (auto* a = m_appletPanel->clientRxDspApplet()) {
        a->setAudioEngine(m_audio);
        if (auto* w = a->widget())
            wireAetherDspWidget(w);
    }

    // ── Client PUDU applets: TX (#1661 Phase 5) + RX (Phase 7.5) ───────────
    m_appletPanel->clientPuduTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientPuduRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientPuduTxApplet(), &ClientPuduApplet::editRequested,
            this, [this]() { ensureClientPuduEditor()->showForTx(); });
    connect(m_appletPanel->clientPuduRxApplet(), &ClientPuduApplet::editRequested,
            this, [this]() { ensureClientPuduEditor()->showForRx(); });

    // ── TX signal-chain applet (#1661) ──────────────────────────────────────
    // Visual strip showing MIC → stages → TX with per-stage bypass +
    // drag-drop reorder.  Clicking a stage opens its floating editor.
    //
    // TX-stage applet visibility is independent of bypass state — the
    // chain widget click and the editor Bypass buttons toggle the DSP
    // and refresh the applet's bypass indicator, but do not show or
    // hide the tile.  Users control applet visibility via the applet
    // header ✕ and toolbar toggles (persisted as Applet_<ID>).
    m_appletPanel->clientChainApplet()->setAudioEngine(m_audio);
    // PooDoo TX/RX tab → AppletPanel side filter.  Hides the inactive
    // side's per-stage applet tiles whenever the user flips the chain
    // tab.  Seed the initial side from the saved tab so the first
    // paint is correct.
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::chainModeChanged,
            this, [this](ClientChainApplet::ChainMode mode) {
        if (!m_appletPanel) return;
        m_appletPanel->setPooDooActiveSide(
            mode == ClientChainApplet::ChainMode::Tx
                ? AppletPanel::PooDooSide::Tx
                : AppletPanel::PooDooSide::Rx);
    });
    // Side-filter seed is moved further down — must run AFTER
    // setTxDspChainOrder, otherwise that helper's insertChildWidget
    // calls re-show every TX container we just hid.

    // Seed the PooDoo MIC-ready + TX-pulse indicators from current
    // state — subsequent changes flow through the TransmitModel
    // signal connections above (micStateChanged + moxChanged).
    {
        const auto& tx = m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_appletPanel->clientChainApplet()->setMicInputReady(ready);
        m_appletPanel->clientChainApplet()->setTxActive(
            ready && tx.isTransmitting());
        if (m_aetherialStrip) {
            m_aetherialStrip->setMicInputReady(ready);
            m_aetherialStrip->setTxActive(ready && tx.isTransmitting());
        }
    }
    // Pulse the TX endpoint red when we're transmitting AND PooDoo
    // is actually in the signal path (MIC=PC and DAX off).  Otherwise
    // the pulse would lie about what's being processed.
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool txActive) {
        if (!m_appletPanel || !m_appletPanel->clientChainApplet()) return;
        const auto& tx = m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_appletPanel->clientChainApplet()->setTxActive(ready && txActive);
        if (m_aetherialStrip)
            m_aetherialStrip->setTxActive(ready && txActive);
    });

    // ── PUDU monitor wiring ─────────────────────────────────────
    auto* chainApplet = m_appletPanel->clientChainApplet();
    chainApplet->setMonitorHasRecording(m_finalMonitor->hasRecording());

    // Easter-egg nub on the chain applet → toggle the Aetherial Audio
    // Channel Strip.  Stubbed in step 1 of the strip plan (#2301);
    // step 4 lazy-creates the strip window and toggles visibility.
    connect(chainApplet, &ClientChainApplet::aetherialStripToggleRequested,
            this, &MainWindow::toggleAetherialStrip);

    // User-click → start/stop based on current monitor state.  The
    // monitor's own signals drive the button visuals back.
    connect(chainApplet, &ClientChainApplet::monitorRecordClicked,
            this, [this]() {
        if (m_finalMonitor->isRecording()) {
            m_finalMonitor->stopRecording();
        } else {
            // Don't record while playing — button shouldn't be
            // enabled in that state, but guard anyway.
            if (m_finalMonitor->isPlaying()) m_finalMonitor->stopPlayback();
            m_finalMonitor->startRecording();
        }
    });
    connect(chainApplet, &ClientChainApplet::monitorPlayClicked,
            this, [this]() {
        if (m_finalMonitor->isPlaying()) {
            m_finalMonitor->stopPlayback();
        } else {
            m_finalMonitor->startPlayback();
        }
    });

    // Monitor state → UI updates.  RX audio gating is handled
    // separately via the muteRxRequested wiring above.  State is
    // forwarded both to the docked ClientChainApplet AND to the
    // AetherialAudioStrip's mirrored buttons (when the strip exists).
    connect(m_finalMonitor, &ClientPuduMonitor::recordingStarted,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorRecording(true);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorRecording(true);
    });
    connect(m_finalMonitor, &ClientPuduMonitor::recordingStopped,
            this, [this](int /*durationMs*/) {
        if (m_appletPanel && m_appletPanel->clientChainApplet()) {
            auto* a = m_appletPanel->clientChainApplet();
            a->setMonitorRecording(false);
            a->setMonitorHasRecording(true);
        }
        if (m_aetherialStrip) {
            m_aetherialStrip->setMonitorRecording(false);
            m_aetherialStrip->setMonitorHasRecording(true);
        }
        // Auto-start playback — the mute stays installed across the
        // transition because the monitor only emits muteRxRequested
        // (false) at stopPlayback().
        m_finalMonitor->startPlayback();
    });
    connect(m_finalMonitor, &ClientPuduMonitor::playbackStarted,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorPlaying(true);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorPlaying(true);
    });
    connect(m_finalMonitor, &ClientPuduMonitor::playbackStopped,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorPlaying(false);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorPlaying(false);
    });
    // TX chain applet visibility is independent of bypass state — the
    // user controls show/hide via the applet header ✕ and toolbar
    // toggles, persisted via Applet_<ID>.  Bypassing a stage just
    // shows the applet as bypassed; it doesn't hide the tile.

    // Initial applet-stack order mirrors the persisted chain order
    // for both sides, and stays in sync on every subsequent drag-
    // reorder.
    if (m_audio) {
        m_appletPanel->setTxDspChainOrder(m_audio->txChainStages());
        m_appletPanel->setRxDspChainOrder(m_audio->rxChainStages());
    }
    auto reapplyPooDooSide = [this]() {
        if (!m_appletPanel) return;
        const QString savedTab = AppSettings::instance()
            .value("PooDooAudioActiveTab", "TX").toString();
        m_appletPanel->setPooDooActiveSide(
            savedTab == "RX" ? AppletPanel::PooDooSide::Rx
                             : AppletPanel::PooDooSide::Tx);
    };
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::chainReordered,
            this, [this, reapplyPooDooSide]() {
        if (!m_appletPanel || !m_audio) return;
        m_appletPanel->setTxDspChainOrder(m_audio->txChainStages());
        // setTxDspChainOrder re-shows every reinserted child, so re-
        // apply the side filter to keep the inactive side hidden.
        reapplyPooDooSide();
    });
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxChainReordered,
            this, [this, reapplyPooDooSide]() {
        if (!m_appletPanel || !m_audio) return;
        m_appletPanel->setRxDspChainOrder(m_audio->rxChainStages());
        reapplyPooDooSide();
    });

    // PooDoo TX/RX side-filter seed — runs AFTER setTxDspChainOrder
    // because that helper's insertChildWidget unconditionally shows
    // each child it reinserts, undoing any earlier hide.  Putting the
    // seed here ensures the inactive side stays hidden on first paint.
    {
        const QString savedTab = AppSettings::instance()
            .value("PooDooAudioActiveTab", "TX").toString();
        m_appletPanel->setPooDooActiveSide(
            savedTab == "RX" ? AppletPanel::PooDooSide::Rx
                             : AppletPanel::PooDooSide::Tx);
    }

    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::stageEnabledChanged,
            this, &MainWindow::onTxChainStageEnabledChanged);

    // ── RX chain edit + bypass ──────────────────────────────────────────────
    // Phase 1 routes RX EQ double-clicks to the existing ClientEqEditor in
    // RX path mode.  Click-bypass lands on the engine via the chain widget
    // itself; we just refresh the CEQ applet's Enable toggle here so it
    // stays in sync.
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxEditRequested,
            this, [this](AudioEngine::RxChainStage stage) {
        switch (stage) {
            case AudioEngine::RxChainStage::Eq:
                ensureClientEqEditor()->showForPath(ClientEqApplet::Path::Rx);
                break;
            case AudioEngine::RxChainStage::Gate:
                ensureClientGateEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Comp:
                ensureClientCompEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Tube:
                ensureClientTubeEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Pudu:
                ensureClientPuduEditor()->showForRx();
                break;
            default:
                break;
        }
    });
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxStageEnabledChanged,
            this, [this](AudioEngine::RxChainStage stage, bool /*enabled*/) {
        // Keep the shared per-stage applet's Enable toggle in lock-
        // step with the click-bypass that just fired on the chain
        // widget.  Each stage routes to its own applet refresh.
        if (!m_appletPanel) return;
        switch (stage) {
            case AudioEngine::RxChainStage::Eq:
                if (m_appletPanel->clientEqRxApplet())
                    m_appletPanel->clientEqRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Gate:
                if (m_appletPanel->clientGateRxApplet())
                    m_appletPanel->clientGateRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Comp:
                if (m_appletPanel->clientCompRxApplet())
                    m_appletPanel->clientCompRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Tube:
                if (m_appletPanel->clientTubeRxApplet())
                    m_appletPanel->clientTubeRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Pudu:
                if (m_appletPanel->clientPuduRxApplet())
                    m_appletPanel->clientPuduRxApplet()->refreshEnableFromEngine();
                break;
            default:
                break;
        }
    });

    // ── Antenna Genius / ShackSwitch applets ────────────────────────────────
    // Both share AntennaGeniusModel (ShackSwitch speaks the AG protocol).
    // On device discovery we check the name: "ShackSwitch" → ShackSwitch applet,
    // anything else → Antenna Genius applet. On device lost, hide both.
    m_appletPanel->agApplet()->setModel(&m_antennaGenius);
    m_appletPanel->ssApplet()->setModel(&m_antennaGenius);

    connect(&m_antennaGenius, &AntennaGeniusModel::deviceDiscovered,
            this, [this](const AgDeviceInfo& info) {
        const bool isSS = AntennaGeniusModel::isShackSwitch(info);
        m_appletPanel->setShackSwitchVisible(isSS);
        m_appletPanel->setAgVisible(!isSS);
        if (isSS) {
            // Clear saved AG IP so ShackSwitch never auto-connects via the AG path again
            AppSettings::instance().setValue("AG_ManualIp", QString());
        }
    });

    // On TCP connect: re-check device type (manual-IP path serial is now enriched
    // from discovered list; if still manual, fall back to IP lookup).
    // Also clears AG_ManualIp if we're connected to a ShackSwitch so it won't
    // auto-connect via the AG path on the next startup.
    connect(&m_antennaGenius, &AntennaGeniusModel::connected,
            this, [this]() {
        const AgDeviceInfo& dev = m_antennaGenius.connectedDevice();
        bool isSS = AntennaGeniusModel::isShackSwitch(dev);
        if (!isSS) {
            // UDP may not have arrived yet — look up by IP in discovered list
            for (const auto& d : m_antennaGenius.discoveredDevices()) {
                if (!d.ip.isNull() && d.ip == dev.ip) {
                    isSS = AntennaGeniusModel::isShackSwitch(d);
                    break;
                }
            }
        }
        if (isSS) {
            m_appletPanel->setShackSwitchVisible(true);
            m_appletPanel->setAgVisible(false);
            // Stop the AG row auto-connecting to ShackSwitch on next startup
            AppSettings::instance().setValue("AG_ManualIp", QString());
        }
    });

    connect(&m_antennaGenius, &AntennaGeniusModel::presenceChanged,
            this, [this](bool present) {
        if (!present) {
            m_appletPanel->setAgVisible(false);
            m_appletPanel->setShackSwitchVisible(false);
        }
    });

    // On disconnect, if the device that was connected was a ShackSwitch and
    // there is no longer a saved SS_ManualIp (e.g. the user cleared it via
    // the Peripherals tab), hide the SS button. A UDP-discovered SS will
    // re-show the button via deviceDiscovered if the hardware is still
    // broadcasting. The model never emits presenceChanged(false) on simple
    // disconnect, so this handler is needed to bridge the gap.
    connect(&m_antennaGenius, &AntennaGeniusModel::disconnected,
            this, [this]() {
        const AgDeviceInfo& dev = m_antennaGenius.connectedDevice();
        const bool wasSS = AntennaGeniusModel::isShackSwitch(dev);
        if (!wasSS) return;
        const QString ssIp =
            AppSettings::instance().value("SS_ManualIp", "").toString();
        if (ssIp.isEmpty()) {
            m_appletPanel->setShackSwitchVisible(false);
        }
    });

    // ── Unified CAT ports (kCatPorts slots, configured from settings) ───────────
    // Migrate old dual-server settings to the new per-port schema on first run.
    migrateCatSettings();
    for (int i = 0; i < kCatPorts; ++i) {
        m_catPorts[i] = new CatPort(&m_radioModel, this);
        // Per-user symlink path (GHSA-qxhr-cwrc-pvrm — matches RigctlPty fix).
        m_catPorts[i]->setSymlinkPath(CatPort::defaultSymlinkPath(i));
        // Load persisted dialect and VFO config; port and enabled are read
        // in applyCatPortCount() just before starting.
        const QString prefix = QString("CatPort_%1_").arg(i);
        auto& s = AppSettings::instance();
        QString d = s.value(prefix + "Dialect", "Rigctld").toString();
        CatDialect dial = (d == "FlexCAT") ? CatDialect::FlexCAT
                        : (d == "TS2000")  ? CatDialect::TS2000
                        : CatDialect::Rigctld;
        m_catPorts[i]->setDialect(dial);
        m_catPorts[i]->setVfoA(s.value(prefix + "VfoA", "0").toInt());
        m_catPorts[i]->setVfoB(s.value(prefix + "VfoB", "-1").toInt());
    }

    // Wire the applet to the port objects
    m_appletPanel->catControlApplet()->setPorts(m_catPorts, kCatPorts);
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
    m_tciServer = new TciServer(&m_radioModel, this);
    m_tciServer->setAudioEngine(m_audio);
    m_appletPanel->tciApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->tciApplet()->setTciServer(m_tciServer);

    // TCI applet sliders → TciServer gain setters
    connect(m_appletPanel->tciApplet(), &TciApplet::tciRxGainChanged,
            m_tciServer, &TciServer::setRxChannelGain);
    connect(m_appletPanel->tciApplet(), &TciApplet::tciTxGainChanged,
            m_tciServer, &TciServer::setTxGain);
    connect(m_appletPanel->tciApplet(), &TciApplet::tciTxOverflowModeChanged,
            m_tciServer, &TciServer::setOverflowMode);

    // TciServer level signals → TCI applet meters
    connect(m_tciServer, &TciServer::rxLevel,
            m_appletPanel->tciApplet(), &TciApplet::setTciRxLevel);
    connect(m_tciServer, &TciServer::txLevel,
            m_appletPanel->tciApplet(), &TciApplet::setTciTxLevel);

    // TCI `volume:N;` master-volume SET → mirror on the title bar slider
    // and route through the same applyMasterVolume() slot the slider uses
    // (audio path + persistence + broadcast back to other TCI clients).
    // See issue #1764 — no master-volume TCI hook existed before.
    connect(m_tciServer, &TciServer::masterVolumeRequested,
            this, [this](int pct) {
        if (m_titleBar) m_titleBar->setMasterVolume(pct);
        applyMasterVolume(pct);
    });

    // Wire slice state changes -> TCI broadcasts. TCI receivers are contiguous
    // indexes within our owned slice list; Flex slice ids can be non-zero when
    // another client owns lower-numbered slices.
    auto wireTciSlice = [this](SliceModel* s) {
        if (!m_tciServer || !s)
            return;
        const int trx = m_radioModel.slices().indexOf(s);
        m_tciServer->wireSlice(trx >= 0 ? trx : s->sliceId(), s);
    };
    connect(&m_radioModel, &RadioModel::sliceAdded, this, [wireTciSlice](SliceModel* s) {
        wireTciSlice(s);
    });
    // Wire existing slices (radio may already be connected with slices)
    for (auto* s : m_radioModel.slices())
        wireTciSlice(s);
    m_tciServer->wireSpotModel();

    // Wire RX audio from PanadapterStream → TCI server for audio streaming.
    // TCI audio feeds exclusively from DAX (not audioDataReady) so that
    // audio_mute doesn't kill TCI audio (#1331).
    if (m_radioModel.panStream()) {
        connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                m_tciServer, &TciServer::onDaxAudioReady);
        connect(m_radioModel.panStream(), &PanadapterStream::iqDataReady,
                m_tciServer, &TciServer::onIqDataReady);
        connect(m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
                m_tciServer, &TciServer::onWaterfallRowReady);
    }

    // TCI client count changes no longer auto-create/remove the audio stream.
    // Control-only TCI clients (StreamDeck) don't need audio, and auto-creating
    // the stream overrode the user's explicit PC Audio toggle. Users who need
    // TCI audio (WSJT-X) should enable PC Audio manually. (#1071)
#endif

    // ── DAX IQ wiring on platforms without an audio bridge ──────────────
    //
    // Same class of bug as #1820 (RADE RX on Windows): startDax() is
    // compiled out on platforms without an audio bridge (Windows, Linux
    // without PipeWire), so the DAX IQ stream-status registration handler
    // that lives inside it never runs.  As a result PanadapterStream sees
    // the inbound VITA-49 IQ packets but never knows what channel to
    // route them to — iqDataReady never fires, the GUI applet meter
    // shows nothing, and TCI clients get no IQ frames.
    //
    // Mirror just the IQ-side wiring here (the audio-bridge wiring
    // genuinely needs the bridge so we leave that gated).  On Mac /
    // PipeWire builds startDax() does the same wiring lazily when DAX
    // audio is toggled, so we skip this block to avoid double-connection.
#if !defined(Q_OS_MAC) && !defined(HAVE_PIPEWIRE)
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
#endif

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

    // ── Status bar telemetry ──────────────────────────────────────────────────
    // Single source of truth for quality-level colors used by the footer label
    // and the heartbeat throttle indicator.  Both must stay in sync.
    auto qualityColor = [](const QString& quality) -> QString {
        if (quality == "Fair") return QStringLiteral("#cc9900");
        if (quality == "Poor") return QStringLiteral("#cc3333");
        if (quality == "Good") return QStringLiteral("#00b4d8");
        return QStringLiteral("#00cc66"); // Excellent / Very Good
    };
    // Map an fps cap to the matching quality-level color for the throttle indicator.
    auto fpsCapColor = [](int fpsCap) -> QString {
        if (fpsCap <= 4) return QStringLiteral("#cc3333"); // Poor
        if (fpsCap <= 8) return QStringLiteral("#cc9900"); // Fair
        return QStringLiteral("#00b4d8");                  // Good
    };

    connect(&m_radioModel, &RadioModel::networkQualityChanged,
            this, [this, qualityColor](const QString& quality, int pingMs) {
        const QString color = qualityColor(quality);
        // Append fps cap so users understand why moving the fps slider has no effect.
        // Show "(restoring)" during the min-dwell hold so testers can distinguish
        // stuck throttle from a deliberate stability wait.
        const bool dwellPending = m_adaptiveFpsCap > 0 && m_radioModel.pendingThrottleLift();
        const QString capSuffix = m_adaptiveFpsCap > 0
            ? (dwellPending
               ? QStringLiteral(" · %1 fps cap (restoring)").arg(m_adaptiveFpsCap)
               : QStringLiteral(" · %1 fps cap").arg(m_adaptiveFpsCap))
            : QString();
        m_networkLabel->setText(QString("[<span style='color:%1'>%2</span>]")
            .arg(color, quality + capSuffix));
        Q_UNUSED(pingMs);
        QString tooltip = buildNetworkTooltip(m_radioModel);
        if (m_adaptiveFpsCap > 0) {
            const QString throttleMsg = dwellPending
                ? QStringLiteral("Adaptive throttle holding for link stability — restoring shortly\n\n")
                : QString("Adaptive throttle active: %1 fps cap\n\n").arg(m_adaptiveFpsCap);
            tooltip.prepend(throttleMsg);
        }
        m_networkLabel->setToolTip(tooltip);
    });

    connect(&m_radioModel, &RadioModel::adaptiveThrottleChanged,
            this, [this, fpsCapColor](bool active, int fpsCap) {
        m_adaptiveThrottleActive = active;
        m_adaptiveFpsCap = active ? fpsCap : 0;
        if (m_titleBar)
            m_titleBar->setThrottleFlashColor(active ? fpsCapColor(fpsCap) : QString{});
        if (!active) {
            // Throttle lifted — push each pan's user-configured fps back to the radio.
            // The reconcile timers are suppressed while throttle is active, so they
            // won't have done this automatically.
            if (!m_panStack) return;
            for (auto* applet : m_panStack->allApplets()) {
                if (!applet) continue;
                auto* sw = applet->spectrumWidget();
                if (!sw) continue;
                const QString panId = applet->panId();
                const int userFps = sw->fftFps();
                if (userFps > 0)
                    m_radioModel.sendCommand(
                        QString("display pan set %1 fps=%2").arg(panId).arg(userFps));
                const int userWfMs = sw->wfLineDuration();
                auto* pan = m_radioModel.panadapter(panId);
                if (pan && !pan->waterfallId().isEmpty() && userWfMs > 0)
                    m_radioModel.sendCommand(
                        QString("display panafall set %1 line_duration=%2")
                            .arg(pan->waterfallId()).arg(userWfMs));
            }
        }
    });

    connect(&m_radioModel.meterModel(), &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyVolts) {
        m_lastPaTempC = paTemp;
        m_hasPaTempTelemetry = true;
        updatePaTempLabel();
        m_supplyVoltLabel->setText(QString("%1 V").arg(supplyVolts, 0, 'f', 2));

        // Update station label (nickname arrives via status after connect)
        const QString nick = m_radioModel.nickname();
        if (!nick.isEmpty())
            m_stationLabel->setText(nick);
    });

    auto normalizeOscillatorValue = [](QString value) {
        value = value.trimmed().toLower();
        return value == "ext" ? QStringLiteral("external") : value;
    };
    auto oscillatorName = [normalizeOscillatorValue](const QString& value, bool compact) {
        const QString normalized = normalizeOscillatorValue(value);
        if (normalized == "auto") return QStringLiteral("Auto");
        if (normalized == "external")
            return compact ? QStringLiteral("Ext 10M") : QStringLiteral("External 10 MHz");
        if (normalized == "gpsdo") return QStringLiteral("GPSDO");
        if (normalized == "tcxo") return QStringLiteral("TCXO");
        return value.trimmed().isEmpty() ? QStringLiteral("Unknown") : value.toUpper();
    };
    auto updateFrequencyReferenceLabel = [this, normalizeOscillatorValue, oscillatorName] {
        const QString state = normalizeOscillatorValue(m_radioModel.oscState());
        const QString setting = normalizeOscillatorValue(m_radioModel.oscSetting());
        const bool locked = m_radioModel.oscLocked();

        QString sourceLabel;
        QString statusLabel;
        if (state.isEmpty()) {
            sourceLabel = QStringLiteral("Ref: --");
            statusLabel = QStringLiteral("[Waiting]");
        } else if (state == "gpsdo") {
            if (locked && (m_radioModel.gpsTracked() > 0 || m_radioModel.gpsVisible() > 0)) {
                sourceLabel = QStringLiteral("GPS: %1/%2")
                                  .arg(m_radioModel.gpsTracked())
                                  .arg(m_radioModel.gpsVisible());
            } else {
                sourceLabel = QStringLiteral("Ref: GPSDO");
            }
            statusLabel = locked && !m_radioModel.gpsStatus().isEmpty()
                ? QStringLiteral("[%1]").arg(m_radioModel.gpsStatus())
                : QStringLiteral("[%1]").arg(locked ? "Locked" : "Unlocked");
        } else if (state == "external") {
            sourceLabel = QStringLiteral("Ref: Ext 10M");
            statusLabel = !m_radioModel.extPresent()
                ? QStringLiteral("[No 10M]")
                : QStringLiteral("[%1]").arg(locked ? "Locked" : "Unlocked");
        } else {
            sourceLabel = QStringLiteral("Ref: %1").arg(oscillatorName(state, true));
            statusLabel = QStringLiteral("[%1]").arg(locked ? "Locked" : "Unlocked");
        }

        m_gpsLabel->setText(sourceLabel);
        m_gpsStatusLabel->setText(statusLabel);

        QString tooltip = QStringLiteral("10 MHz reference\nSetting: %1\nActual: %2\nLock: %3")
            .arg(oscillatorName(setting, false),
                 oscillatorName(state, false),
                 locked ? QStringLiteral("Locked") : QStringLiteral("Unlocked"));
        if (state == "external") {
            tooltip += QStringLiteral("\nExternal 10 MHz: %1")
                .arg(m_radioModel.extPresent() ? QStringLiteral("detected")
                                                : QStringLiteral("not detected"));
        }
        if (state == "gpsdo") {
            tooltip += QStringLiteral("\nGPS: %1/%2 satellites")
                .arg(m_radioModel.gpsTracked())
                .arg(m_radioModel.gpsVisible());
            if (!m_radioModel.gpsStatus().isEmpty())
                tooltip += QStringLiteral("\nGPS status: %1").arg(m_radioModel.gpsStatus());
        }
        m_gpsLabel->setToolTip(tooltip);
        m_gpsStatusLabel->setToolTip(tooltip);
    };

    // Frequency reference label from oscillator status (#478)
    // Show radio oscillator state immediately; GPS status only adds details.
    connect(&m_radioModel, &RadioModel::oscillatorChanged, this, updateFrequencyReferenceLabel);
    updateFrequencyReferenceLabel();

    connect(&m_radioModel, &RadioModel::gpsStatusChanged,
            this, [this, normalizeOscillatorValue, updateFrequencyReferenceLabel](
                         const QString& /*status*/, int /*tracked*/, int /*visible*/,
                         const QString& /*grid*/, const QString& /*alt*/,
                         const QString& /*lat*/, const QString& /*lon*/,
                         const QString& utcTime) {
        updateFrequencyReferenceLabel();

        // Use GPS UTC time only when GPSDO is installed and locked.
        // GPS with no antenna/lock sends stale "00:00:00Z" — fall back to system clock.
        if (!utcTime.isEmpty()
            && normalizeOscillatorValue(m_radioModel.oscState()) == "gpsdo"
            && m_radioModel.oscLocked()) {
            m_gpsTimeLabel->setText(utcTime);
            m_useSystemClock = false;
        } else {
            m_useSystemClock = true;
        }
    });

    // System clock fallback when no GPS is installed
    auto* clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, [this] {
        auto utc = QDateTime::currentDateTimeUtc();
        m_gpsDateLabel->setText(utc.toString("yyyy-MM-dd"));
        if (m_useSystemClock)
            m_gpsTimeLabel->setText(utc.toString("HH:mm:ssZ"));
    });
    clockTimer->start(1000);

    // Start discovery — show amber indicator while waiting for connection
    if (m_titleBar) m_titleBar->setDiscovering(true);
    m_discovery.startListening();

    const bool autoConnectToLastRadio =
        AppSettings::instance().value("AutoConnectToLastRadio", "True").toString() == "True";
    const QString startupLastSerial =
        AppSettings::instance().value("LastConnectedRadioSerial").toString();
    if (!startupLastSerial.isEmpty() && autoConnectToLastRadio) {
        m_connPanel->setStatusText("Looking for your radio…");
        setPanadapterConnectionAnimation(true, "Looking for your radio…");
    } else if (!autoConnectToLastRadio) {
        QTimer::singleShot(0, this, &MainWindow::toggleConnectionDialog);
    }

    // Auto-connect to routed radios (probed, not broadcast-discovered)
    connect(m_connPanel, &ConnectionPanel::routedRadioFound,
            this, [this](const RadioInfo& info) {
        if (m_userDisconnected || m_radioModel.isConnected()) return;
        if (AppSettings::instance().value("AutoConnectToLastRadio", "True").toString() != "True")
            return;
        const QString lastSerial = AppSettings::instance()
            .value("LastConnectedRadioSerial").toString();
        if (!lastSerial.isEmpty() && info.serial == lastSerial) {
            QList<quint32> disconnectHandles;
            if (!confirmClientSlotAvailability(info, &disconnectHandles)) {
                m_userDisconnected = true;
                m_connPanel->setStatusText("Connection canceled");
                setPanadapterConnectionAnimation(false);
                return;
            }
            m_radioModel.setPendingClientDisconnects(disconnectHandles);
            qDebug() << "Auto-connecting to routed radio" << info.address.toString();
            m_connPanel->setStatusText("Auto-connecting…");
            setPanadapterConnectionAnimation(true, "Connecting to radio…");
            m_radioModel.connectToRadio(info);
        }
    });

    // Probe saved routed radio on startup
    {
        auto& s = AppSettings::instance();
        const QString routedIp = s.value("LastRoutedRadioIp").toString();
        if (!routedIp.isEmpty() && !m_userDisconnected && autoConnectToLastRadio) {
            m_connPanel->setStatusText("Looking for your radio…");
            setPanadapterConnectionAnimation(true, "Looking for your radio…");
            QTimer::singleShot(500, this, [this, routedIp] {
                m_connPanel->probeRadio(routedIp);
            });
        }
    }

    // Restore saved geometry from XML settings
    auto& s = AppSettings::instance();
    const QString geomB64 = s.value("MainWindowGeometry").toString();
    if (!geomB64.isEmpty()) {
        m_startupGeometryForFirstShow = QByteArray::fromBase64(geomB64.toLatin1());
        if (!m_startupGeometryForFirstShow.isEmpty()) {
            restoreGeometry(m_startupGeometryForFirstShow);
        }
    }
    const QString stateB64 = s.value("MainWindowState").toString();
    if (!stateB64.isEmpty()) {
        restoreState(QByteArray::fromBase64(stateB64.toLatin1()));
    }

    // Restore minimal mode AFTER full-window geometry has been applied.
    // Doing this earlier in the constructor caused toggleMinimalMode(true)
    // to snapshot the default startup rect into FullModeGeometry and to
    // restoreGeometry(MinimalModeGeometry) before the window had been
    // placed on the correct screen — visible on Windows with
    // FramelessWindowHint as a position drift each launch (DWM doesn't
    // cache the position the way it does with native chrome). (#2483)
    if (s.value("MinimalModeEnabled", "False").toString() == "True") {
        toggleMinimalMode(true);
        // Only swap to the minimal-mode blob when it actually exists.
        // MinimalModeGeometry is written by toggleMinimalMode(false) and by
        // closeEvent — never on first enable — so users upgrading from a build
        // that predated the #2483 save path can have MinimalModeEnabled=True
        // with no MinimalModeGeometry.  Overwriting unconditionally would wipe
        // the valid MainWindowGeometry blob with empty bytes and skip the
        // post-show screen restore entirely for exactly the multi-monitor
        // minimal-mode operators this fix targets. (#3319)
        const QByteArray minimalBlob = QByteArray::fromBase64(
            s.value("MinimalModeGeometry", "").toString().toLatin1());
        if (!minimalBlob.isEmpty()) {
            m_startupGeometryForFirstShow = minimalBlob;
        }
    }

    // Restore the Aetherial Audio Channel Strip if it was open on last
    // exit (#2301).  toggleAetherialStrip() lazy-creates and shows.
    if (s.value("AetherialStripVisible", "False").toString() == "True")
        toggleAetherialStrip();
    // Clear stale splitter state — layout has changed across versions.
    s.remove("SplitterState");
    // Force 4-pane sizing: CWX=0, DVK=0 (hidden), applet=260px, center=stretch.
    // Assign by widget identity rather than fixed slot index — buildUI may
    // have called setAppletPanelDockedLeft() which swaps m_panStack and
    // m_appletPanel in the splitter.  Hard-coding "size at index 2 = center,
    // size at index 3 = applet" silently mis-allocates on left-dock startup
    // (panstack squeezed to the right edge with a wide empty slot in the
    // middle).  PR #2733 fixed setAppletPanelDockedLeft itself but this
    // deferred resizer overwrites its work — same fix shape, second site. (#2704)
    QTimer::singleShot(0, this, [this]() {
        if (!m_splitter || !m_appletPanel || !m_panStack) return;
        const int total = m_splitter->width();
        if (total <= 0) return;
        const int appletW = m_appletPanel->maximumWidth();
        const int centerW = qMax(400, total - appletW);
        QList<int> sizes(m_splitter->count(), 0);
        for (int i = 0; i < m_splitter->count(); ++i) {
            QWidget* w = m_splitter->widget(i);
            if (w == m_panStack)         sizes[i] = centerW;
            else if (w == m_appletPanel) sizes[i] = appletW;
        }
        m_splitter->setSizes(sizes);
    });

    // Auto-popup connection dialog if no saved radio
    QString lastSerial = s.value("LastConnectedRadioSerial", "").toString();
    if (lastSerial.isEmpty()) {
        QTimer::singleShot(500, this, [this]() { toggleConnectionDialog(); });
    }

    // Restore the Memory dialog if it was open when the app last exited.
    QTimer::singleShot(0, this, [this]() {
        if (AppSettings::instance().value("MemoryDialogOpen", "False").toString() == "True")
            showMemoryDialog();
    });

    // Track last-seen version (used by Help → What's New)
    {
        auto& settings = AppSettings::instance();
        QString current = QCoreApplication::applicationVersion();
        if (settings.value("LastSeenVersion").toString() != current) {
            settings.setValue("LastSeenVersion", current);
            settings.save();
        }
    }
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    preparePanadapterUiForShutdown();

#ifdef HAVE_RADE
    if (m_radeSliceId >= 0)
        deactivateRADE();
#endif
    // Network diagnostics polls AudioEngine once per second. Destroy the
    // modeless dialog before the audio worker is torn down so no queued refresh
    // can read a dead audio object during application shutdown.
    if (m_networkDiagnosticsDialog) {
        delete m_networkDiagnosticsDialog.data();
        m_networkDiagnosticsDialog = nullptr;
    }
    delete m_networkDiagnosticsHistory;
    m_networkDiagnosticsHistory = nullptr;

    // Ax25HfPacketDecodeDialog::~Ax25HfPacketDecodeDialog calls
    // m_audio->setTncRxTapEnabled(false) via a raw AudioEngine pointer.
    // AudioEngine is freed below before Qt's deleteChildren() runs — same
    // UAF pattern as m_networkDiagnosticsDialog above. Tear it down now
    // while AudioEngine is still alive.
    if (m_ax25HfPacketDecodeDialog) {
        delete m_ax25HfPacketDecodeDialog.data();
        m_ax25HfPacketDecodeDialog = nullptr;
    }

    // Stop audio processing on the worker thread before destruction (#502).
    // Use BlockingQueuedConnection to ensure completion before we proceed.
    if (m_audio && m_audioThread && m_audioThread->isRunning()) {
        AudioEngine* audio = m_audio;
        QMetaObject::invokeMethod(audio, [audio]() {
            audio->setNr2Enabled(false);
            audio->setRn2Enabled(false);
            audio->setBnrEnabled(false);
            audio->stopRxStream();
            audio->stopTxStream();
        }, Qt::BlockingQueuedConnection);
        audio->deleteLater();
        m_audioThread->quit();
        m_audioThread->wait(3000);
    } else {
        delete m_audio;
    }
    if (m_audioThread && m_audioThread->isRunning()) {
        m_audioThread->quit();
        m_audioThread->wait(3000);
    }
    m_audio = nullptr;

#ifdef HAVE_WEBSOCKETS
    // TciServer holds a raw RadioModel* and dereferences it in stop() →
    // releaseDaxForTci(). Qt would delete it as a child of MainWindow during
    // ~QWidget::deleteChildren(), which runs *after* MainWindow's value members
    // (including m_radioModel) have already been destroyed — crash on quit
    // (#2385). Tear it down explicitly here: audio is stopped (no more
    // daxAudioReady cross-thread signals), m_radioModel is still alive (DAX
    // stream-remove commands reach the radio), and we null out TciApplet's raw
    // back-reference first so no dangling pointer remains in the widget tree.
    if (m_appletPanel && m_appletPanel->tciApplet())
        m_appletPanel->tciApplet()->setTciServer(nullptr);
    delete m_tciServer;
    m_tciServer = nullptr;
#endif

    // Stop external controller thread (#502)
    if (m_extCtrlThread && m_extCtrlThread->isRunning()) {
        // Close serial port on its own thread before stopping it to avoid
        // the cross-thread QObject access crash (QSerialPort has thread affinity
        // to m_extCtrlThread; calling close() from main thread hits a fatal assert).
#ifdef HAVE_SERIALPORT
        QMetaObject::invokeMethod(m_serialPort, [this] { m_serialPort->close(); },
                                  Qt::BlockingQueuedConnection);
        // FlexControlManager owns its own QSerialPort.  Close it
        // synchronously on the ExtControllers thread before tearing the
        // thread down — otherwise on Windows the OS handle for the
        // FlexController COM port stays held by the zombie AetherSDR.exe
        // process (deleteLater's DeferredDelete event isn't guaranteed
        // to fire before the worker thread's event loop exits via quit).
        // Other clients of the same port (e.g. SmartSDR's Tuning Knob
        // serial open) then fail until the user kills AetherSDR.exe via
        // TaskManager.  Same pattern as the m_midiControl /
        // m_hidEncoder closes below.
        if (m_flexControl) {
            QMetaObject::invokeMethod(m_flexControl, &FlexControlManager::close,
                                      Qt::BlockingQueuedConnection);
        }
#endif
#ifdef HAVE_MIDI
        if (m_midiControl) {
            QMetaObject::invokeMethod(m_midiControl, &MidiControlManager::closePort,
                                      Qt::BlockingQueuedConnection);
        }
#endif
#ifdef HAVE_HIDAPI
        if (m_hidEncoder) {
            QMetaObject::invokeMethod(m_hidEncoder, &HidEncoderManager::close,
                                      Qt::BlockingQueuedConnection);
        }
#endif
#ifdef HAVE_SERIALPORT
        if (m_serialPort) {
            m_serialPort->deleteLater();
        }
        if (m_flexControl) {
            m_flexControl->deleteLater();
        }
#endif
#ifdef HAVE_MIDI
        if (m_midiControl) {
            m_midiControl->deleteLater();
        }
#endif
        m_extCtrlThread->quit();
        m_extCtrlThread->wait(3000);
        // Delete ExtControllers objects synchronously after the thread stops.
        // deleteLater() races with quit() and can leave destructors unrun.
        // On macOS, UlanziDialMacOSManager::stop() calls
        // IOHIDManagerUnscheduleFromRunLoop(...GetMain()) which must run on the
        // main thread — calling it via BlockingQueuedConnection deadlocks because
        // the main thread is blocked waiting for the cross-thread call to return.
        // Safe to call directly here: the ExtControllers thread has stopped so
        // there is no race on m_dialBackend's state.
        if (m_dialBackend) m_dialBackend->stop();
        delete m_dialBackend;
        m_dialBackend = nullptr;
#ifdef HAVE_HIDAPI
        delete m_hidEncoder;
        m_hidEncoder = nullptr;
#endif
    } else {
#ifdef HAVE_SERIALPORT
        delete m_serialPort;
        delete m_flexControl;
#endif
#ifdef HAVE_MIDI
        delete m_midiControl;
#endif
#ifdef HAVE_HIDAPI
        delete m_hidEncoder;
#endif
    }
#ifdef HAVE_SERIALPORT
    m_serialPort = nullptr;
    m_flexControl = nullptr;
#endif
#ifdef HAVE_MIDI
    m_midiControl = nullptr;
#endif
#ifdef HAVE_HIDAPI
    m_hidEncoder = nullptr;
#endif
}

void MainWindow::preparePanadapterUiForShutdown()
{
    if (m_panadapterUiPreparedForShutdown) {
        return;
    }

    m_panadapterUiPreparedForShutdown = true;
    m_shuttingDown = true;

    if (m_sHistoryExpireTimer) {
        m_sHistoryExpireTimer->stop();
        QObject::disconnect(m_sHistoryExpireTimer, nullptr, this, nullptr);
    }
    m_sHistoryData.clear();
    m_sHistoryPanState.clear();
    m_spectrogramBuffers.clear();

    if (m_layoutRestoreTimer) {
        m_layoutRestoreTimer->stop();
    }
    m_layoutRestoreUntilMs = 0;

    if (m_panStack) {
        m_panStack->setShuttingDown(true);
    }

    if (auto* stream = m_radioModel.panStream()) {
        QObject::disconnect(stream, nullptr, this, nullptr);
    }

    const QList<SpectrumWidget*> spectra = findChildren<SpectrumWidget*>();
    for (SpectrumWidget* spectrum : spectra) {
        if (spectrum) {
            spectrum->prepareForShutdown();
        }
    }

    if (m_panStack) {
        m_panStack->prepareShutdown();
    }
    m_panApplet = nullptr;
    m_cwDecoderApplet = nullptr;

    if (m_appletPanel && m_appletPanel->containerManager()) {
        m_appletPanel->containerManager()->prepareShutdown();
    }
}

namespace {

void setEditorFramelessMode(QWidget* editor, bool on)
{
    if (!editor) {
        return;
    }

    const QRect geom = editor->geometry();
    const bool wasVisible = editor->isVisible();
    Qt::WindowFlags flags = (editor->windowFlags() & ~Qt::WindowType_Mask) | Qt::Window;
    flags.setFlag(Qt::FramelessWindowHint, on);
    editor->setWindowFlags(flags);
    editor->setGeometry(geom);

    if (auto* titleBar = editor->findChild<QWidget*>("editorFramelessTitleBar")) {
        titleBar->setVisible(on);
    }
    if (wasVisible) {
        editor->show();
    }
}

void setDialogFramelessMode(QDialog* dialog, bool on)
{
    if (!dialog) {
        return;
    }

    const QRect geom = dialog->geometry();
    const bool wasVisible = dialog->isVisible();
    Qt::WindowFlags flags = (dialog->windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    dialog->setWindowFlags(flags);
    if (wasVisible) {
        dialog->setGeometry(geom);
    }

    if (auto* titleBar = dialog->findChild<QWidget*>("editorFramelessTitleBar")) {
        titleBar->setVisible(on);
    }
    if (auto* titleBar = dialog->findChild<QWidget*>("framelessWindowTitleBar")) {
        titleBar->setVisible(on);
    }
    if (auto* bodyLayout = dialog->findChild<QVBoxLayout*>("reconnectDialogBodyLayout")) {
        bodyLayout->setContentsMargins(18, on ? 14 : 16, 18, 16);
    }
    if (wasVisible) {
        dialog->show();
    }
}

bool framelessWindowEnabled()
{
    return AppSettings::instance().value("FramelessWindow", "True").toString() == "True";
}

}

ClientEqEditor* MainWindow::ensureClientEqEditor()
{
    if (!m_clientEqEditor) {
        m_clientEqEditor = new ClientEqEditor(m_audio, this);
        setEditorFramelessMode(m_clientEqEditor, framelessWindowEnabled());
        connect(m_clientEqEditor, &ClientEqEditor::bypassToggled,
                this, [this](ClientEqApplet::Path path, bool bypassed) {
            if (!m_appletPanel) return;
            if (path == ClientEqApplet::Path::Tx) {
                // TX applet visibility is independent of bypass state.
                if (m_appletPanel->clientEqTxApplet())
                    m_appletPanel->clientEqTxApplet()->refreshEnableFromEngine();
            } else {
                if (m_appletPanel->clientEqRxApplet())
                    m_appletPanel->clientEqRxApplet()->refreshEnableFromEngine();
                m_appletPanel->setAppletVisible("CEQ-RX", !bypassed);
            }
            if (m_appletPanel->clientChainApplet())
                m_appletPanel->clientChainApplet()->refreshFromEngine();
        });
        // Push current TX + RX filter cutoffs so the dashed guide lines
        // render immediately when the editor opens — the cutoff-change
        // wiring in the MainWindow ctor only fires on subsequent changes.
        const auto& tx = m_radioModel.transmitModel();
        m_clientEqEditor->setTxFilterCutoffs(tx.txFilterLow(), tx.txFilterHigh());
        pushRxFilterCutoffsToEq();

        // Cutoff-line drag → write to the radio.  Shared with the strip's
        // embedded EQ panel via MainWindow::onEqCutoffsDragRequested.
        connect(m_clientEqEditor, &ClientEqEditor::cutoffsDragRequested,
                this, &MainWindow::onEqCutoffsDragRequested);
    }
    return m_clientEqEditor;
}

void MainWindow::onTxChainStageEnabledChanged(
    AudioEngine::TxChainStage stage, bool /*enabled*/)
{
    // Refresh the matching docked applet's enable indicator.  Applet
    // visibility is independent of bypass state for TX chain DSPs.
    if (stage == AudioEngine::TxChainStage::Eq) {
        if (m_appletPanel->clientEqApplet())
            m_appletPanel->clientEqApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::Comp) {
        if (m_appletPanel->clientCompApplet())
            m_appletPanel->clientCompApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::Gate) {
        if (m_appletPanel->clientGateApplet())
            m_appletPanel->clientGateApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::DeEss) {
        if (m_appletPanel->clientDeEssApplet())
            m_appletPanel->clientDeEssApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::Tube) {
        if (m_appletPanel->clientTubeApplet())
            m_appletPanel->clientTubeApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::Enh) {
        if (m_appletPanel->clientPuduApplet())
            m_appletPanel->clientPuduApplet()->refreshEnableFromEngine();
    } else if (stage == AudioEngine::TxChainStage::Reverb) {
        if (m_appletPanel->clientReverbApplet())
            m_appletPanel->clientReverbApplet()->refreshEnableFromEngine();
    }
    // Cross-paint: nudge whichever chain widget didn't initiate the
    // change.  Engine state is the source of truth — both widgets read
    // from it on paint, so a plain update() is enough.
    if (m_appletPanel && m_appletPanel->clientChainApplet())
        m_appletPanel->clientChainApplet()->refreshFromEngine();
    if (m_aetherialStrip)
        m_aetherialStrip->refreshChainPaint();
}

void MainWindow::onEqCutoffsDragRequested(ClientEqApplet::Path path,
                                          int audioLo, int audioHi)
{
    if (path == ClientEqApplet::Path::Tx) {
        auto& txm = m_radioModel.transmitModel();
        if (audioLo != txm.txFilterLow())  txm.setTxFilterLow(audioLo);
        if (audioHi != txm.txFilterHigh()) txm.setTxFilterHigh(audioHi);
        return;
    }
    // RX: convert audio-domain Hz back to slice filter offsets based on
    // the active slice's mode.
    auto* s = activeSlice();
    if (!s) return;
    const QString mode = s->mode();
    int lo = audioLo;
    int hi = audioHi;
    if (mode == "LSB" || mode == "DIGL") {
        // Lower-sideband: filter offsets are negative; the audio low edge
        // maps to the high (closest-to-zero) offset and vice versa.
        lo = -audioHi;
        hi = -audioLo;
    } else if (mode == "AM" || mode == "SAM" || mode == "FM"
            || mode == "NFM" || mode == "DFM" || mode == "DSB") {
        // Symmetric around carrier — only audio_high meaningfully
        // controls bandwidth; audio_low is fixed at 0.
        lo = -audioHi;
        hi =  audioHi;
    }
    // USB / DIGU / FDV / CW / RTTY / others: audio domain matches slice
    // domain directly — pass through.
    s->setFilterWidth(lo, hi);
}

ClientGateEditor* MainWindow::ensureClientGateEditor()
{
    if (!m_clientGateEditor) {
        m_clientGateEditor = new ClientGateEditor(m_audio, this);
        setEditorFramelessMode(m_clientGateEditor, framelessWindowEnabled());
        connect(m_clientGateEditor, &ClientGateEditor::bypassToggled,
                this, [this](ClientGateEditor::Side side, bool bypassed) {
            if (!m_appletPanel) return;
            if (side == ClientGateEditor::Side::Tx) {
                // TX applet visibility is independent of bypass state.
                if (m_appletPanel->clientGateTxApplet())
                    m_appletPanel->clientGateTxApplet()->refreshEnableFromEngine();
            } else {
                if (m_appletPanel->clientGateRxApplet())
                    m_appletPanel->clientGateRxApplet()->refreshEnableFromEngine();
                m_appletPanel->setAppletVisible("GATE-RX", !bypassed);
            }
            if (m_appletPanel->clientChainApplet())
                m_appletPanel->clientChainApplet()->refreshFromEngine();
        });
    }
    return m_clientGateEditor;
}

ClientCompEditor* MainWindow::ensureClientCompEditor()
{
    if (!m_clientCompEditor) {
        m_clientCompEditor = new ClientCompEditor(m_audio, this);
        setEditorFramelessMode(m_clientCompEditor, framelessWindowEnabled());
        connect(m_clientCompEditor, &ClientCompEditor::bypassToggled,
                this, [this](ClientCompEditor::Side side, bool bypassed) {
            if (!m_appletPanel) return;
            if (side == ClientCompEditor::Side::Tx) {
                // TX applet visibility is independent of bypass state.
                if (m_appletPanel->clientCompTxApplet())
                    m_appletPanel->clientCompTxApplet()->refreshEnableFromEngine();
            } else {
                if (m_appletPanel->clientCompRxApplet())
                    m_appletPanel->clientCompRxApplet()->refreshEnableFromEngine();
                m_appletPanel->setAppletVisible("CMP-RX", !bypassed);
            }
            if (m_appletPanel->clientChainApplet())
                m_appletPanel->clientChainApplet()->refreshFromEngine();
        });
    }
    return m_clientCompEditor;
}

ClientTubeEditor* MainWindow::ensureClientTubeEditor()
{
    if (!m_clientTubeEditor) {
        m_clientTubeEditor = new ClientTubeEditor(m_audio, this);
        setEditorFramelessMode(m_clientTubeEditor, framelessWindowEnabled());
        connect(m_clientTubeEditor, &ClientTubeEditor::bypassToggled,
                this, [this](ClientTubeEditor::Side side, bool bypassed) {
            if (!m_appletPanel) return;
            if (side == ClientTubeEditor::Side::Tx) {
                // TX applet visibility is independent of bypass state.
                if (m_appletPanel->clientTubeTxApplet())
                    m_appletPanel->clientTubeTxApplet()->refreshEnableFromEngine();
            } else {
                if (m_appletPanel->clientTubeRxApplet())
                    m_appletPanel->clientTubeRxApplet()->refreshEnableFromEngine();
                m_appletPanel->setAppletVisible("TUBE-RX", !bypassed);
            }
            if (m_appletPanel->clientChainApplet())
                m_appletPanel->clientChainApplet()->refreshFromEngine();
        });
    }
    return m_clientTubeEditor;
}

ClientPuduEditor* MainWindow::ensureClientPuduEditor()
{
    if (!m_clientPuduEditor) {
        m_clientPuduEditor = new ClientPuduEditor(m_audio, this);
        setEditorFramelessMode(m_clientPuduEditor, framelessWindowEnabled());
        connect(m_clientPuduEditor, &ClientPuduEditor::bypassToggled,
                this, [this](ClientPuduEditor::Side side, bool bypassed) {
            if (!m_appletPanel) return;
            if (side == ClientPuduEditor::Side::Tx) {
                // TX applet visibility is independent of bypass state.
                if (m_appletPanel->clientPuduTxApplet())
                    m_appletPanel->clientPuduTxApplet()->refreshEnableFromEngine();
            } else {
                if (m_appletPanel->clientPuduRxApplet())
                    m_appletPanel->clientPuduRxApplet()->refreshEnableFromEngine();
                m_appletPanel->setAppletVisible("PUDU-RX", !bypassed);
            }
            if (m_appletPanel->clientChainApplet())
                m_appletPanel->clientChainApplet()->refreshFromEngine();
        });
    }
    return m_clientPuduEditor;
}

AetherDspDialog* MainWindow::ensureAetherDspDialog()
{
    const bool wasFresh = !m_dspDialog;
    showOrRaisePersistent(m_dspDialog, m_audio);
    if (wasFresh && m_dspDialog) {
        if (auto* w = m_dspDialog->widget()) wireAetherDspWidget(w);
    }
    return m_dspDialog.data();
}

#ifdef HAVE_MQTT
void MainWindow::showMqttSettingsDialog()
{
    const bool wasFresh = !m_mqttSettingsDialog;
    showOrRaisePersistent(m_mqttSettingsDialog);
    if (!wasFresh || !m_mqttSettingsDialog)
        return;

    connect(m_mqttSettingsDialog, &MqttSettingsDialog::settingsSaved,
            this, [this](const QString& password) {
        if (!m_appletPanel || !m_appletPanel->mqttApplet())
            return;
        auto* mqttApplet = m_appletPanel->mqttApplet();
        mqttApplet->setCachedPassword(password);
        mqttApplet->refreshSettings();
        if (m_mqttClient) {
            m_mqttClient->setSubscriptions(mqttSubscriptionTopics(mqttApplet->topicConfig()));
        }
    });
}

void MainWindow::publishCwDecodeMqtt(const QString& text, float cost, bool rx)
{
    if (!m_mqttClient) return;
    if (!isMqttTopicEnabled(QString::fromLatin1(kCwDecodeTopic))) return;
    // No CW panel active → nothing is displayed → don't publish.
    if (!m_cwDecoderApplet || cost >= m_cwDecoderApplet->cwCostThreshold())
        return;
    // Mirror panel normalization: \n → space; drop whitespace-only TX chunks.
    QString clean = text;
    clean.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (!rx && clean.trimmed().isEmpty()) return;
    QJsonObject obj;
    obj[QStringLiteral("text")] = clean;
    obj[QStringLiteral("rx")]   = rx;
    if (auto* s = activeSlice(); s && s->frequency() > 0.0)
        obj[QStringLiteral("freq")] = s->frequency();
    if (rx) {
        if (m_cwLastPitchHz  > 0.0f) obj[QStringLiteral("pitch_hz")]  = m_cwLastPitchHz;
        if (m_cwLastSpeedWpm > 0.0f) obj[QStringLiteral("speed_wpm")] = m_cwLastSpeedWpm;
    } else {
        const auto& tm = m_radioModel.transmitModel();
        if (tm.cwPitch() > 0) obj[QStringLiteral("pitch_hz")]  = tm.cwPitch();
        if (tm.cwSpeed() > 0) obj[QStringLiteral("speed_wpm")] = tm.cwSpeed();
    }
    m_mqttClient->publish(QString::fromLatin1(kCwDecodeTopic),
                          QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::publishRadioStateMqtt()
{
    if (!m_mqttClient) return;
    if (!isMqttTopicEnabled(QString::fromLatin1(kRadioStateTopic))) return;
    if (m_cwxTransmitting) {
        if (!m_radioModel.isRadioTransmitting()) {
            m_cwxTxEndTimer.start(1000);  // might be done; confirm after 1 s silence
            return;
        }
        m_cwxTxEndTimer.stop();           // element started — not done yet
        if (m_cwxPublishedTxTrue) return;
        m_cwxPublishedTxTrue = true;
    }
    auto* s = activeSlice();
    if (!s) return;
    QJsonObject obj;
    obj[QStringLiteral("slice")] = s->letter();
    obj[QStringLiteral("freq")]  = s->frequency();
    obj[QStringLiteral("mode")]  = s->mode();
    obj[QStringLiteral("tx")]    = m_radioModel.isRadioTransmitting();
    m_mqttClient->publish(QString::fromLatin1(kRadioStateTopic),
                          QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
#endif

void MainWindow::wireRadioSetupDialogSignals(RadioSetupDialog* dlg, const QString& prevComp)
{
    if (!dlg) return;
    connect(dlg, &RadioSetupDialog::txBandSettingsRequested,
            m_txBandAction, &QAction::trigger);
    // serialSettingsChanged is the "external-device settings changed" signal in
    // practice — the dialog emits it for serial-port, FlexControl, Ulanzi-dial,
    // and HID-encoder edits. The Ulanzi/HID branches below run regardless of
    // HAVE_SERIALPORT because those backends exist on all platforms (#3257).
    connect(dlg, &RadioSetupDialog::serialSettingsChanged, this, [this]() {
#ifdef HAVE_SERIALPORT
        QMetaObject::invokeMethod(m_serialPort, [this] { m_serialPort->loadSettings(); });
        auto& fcs = AppSettings::instance();
        const bool fcOpen = fcs.value("FlexControlOpen", "False").toString() == "True";
        const QString fcPort = fcs.value("FlexControlPort").toString();
        const bool fcInvert = fcs.value("FlexControlInvertDir", "False").toString() == "True";
        QMetaObject::invokeMethod(m_flexControl, [this, fcOpen, fcPort, fcInvert] {
            if (fcOpen) {
                if (fcPort.isEmpty()) {
                    if (m_flexControl->isOpen())
                        m_flexControl->close();
                } else if (!m_flexControl->isOpen() || m_flexControl->portName() != fcPort) {
                    if (m_flexControl->isOpen())
                        m_flexControl->close();
                    m_flexControl->open(fcPort);
                }
            } else if (m_flexControl->isOpen()) {
                m_flexControl->close();
            }
            m_flexControl->setInvertDirection(fcInvert);
        });
        if (m_flexControlDialog)
            m_flexControlDialog->refreshButtonActions();
        syncFlexControlIndicatorForSettings();
#endif
        // External-device enable evaluation. start()/loadSettings() are
        // idempotent (each guards against re-open), so re-firing them when
        // unrelated settings change is harmless. Toggling the user-facing
        // checkbox from off → on is the moment the OS TCC prompt fires —
        // with user context — instead of every launch (#3257).
        auto& s = AppSettings::instance();
        if (m_dialBackend &&
            s.value("UlanziDialEnabled", "False").toString() == "True") {
            QMetaObject::invokeMethod(m_dialBackend, &UlanziDialBackend::start,
                                      Qt::QueuedConnection);
        }
#ifdef HAVE_HIDAPI
        if (m_hidEncoder &&
            s.value("HidEncoderEnabled", "False").toString() == "True") {
            QMetaObject::invokeMethod(m_hidEncoder, [this] {
                m_hidEncoder->loadSettings();
            });
        }
        refreshStreamDeckLabels();
#endif
    });
#ifdef HAVE_SERIALPORT
    dlg->setFlexControlConnectionStatus(
        m_flexControlConnected,
        m_flexControlConnected && m_flexControl ? m_flexControl->portName() : QString());
#endif
    // Toggle of SliceLetterDisplay → repaint every slice-letter widget
    // by re-emitting letterChanged on each slice (#2606).
    connect(dlg, &RadioSetupDialog::sliceLetterDisplayModeChanged,
            this, [this]() {
        for (auto* s : m_radioModel.slices())
            s->emitLetterRefresh();
    });
    connect(dlg, &QDialog::finished, this, [this, prevComp]() {
#ifdef HAVE_SERIALPORT
        // Re-load serial port settings if changed (on worker thread)
        QMetaObject::invokeMethod(m_serialPort, [this] { m_serialPort->loadSettings(); });
        // Re-check FlexControl open/close state
        auto& fcs = AppSettings::instance();
        bool fcOpen = fcs.value("FlexControlOpen", "False").toString() == "True";
        QString fcPort = fcs.value("FlexControlPort").toString();
        bool fcInvert = fcs.value("FlexControlInvertDir", "False").toString() == "True";
        QMetaObject::invokeMethod(m_flexControl, [this, fcOpen, fcPort, fcInvert] {
            if (fcOpen) {
                if (fcPort.isEmpty()) {
                    if (m_flexControl->isOpen())
                        m_flexControl->close();
                } else if (!m_flexControl->isOpen() || m_flexControl->portName() != fcPort) {
                    if (m_flexControl->isOpen())
                        m_flexControl->close();
                    m_flexControl->open(fcPort);
                }
            } else {
                if (m_flexControl->isOpen()) m_flexControl->close();
            }
            m_flexControl->setInvertDirection(fcInvert);
        });
#endif
        // Re-evaluate CW decode panel and TX tap from the dialog's
        // RX/TX toggles, plus run state vs current slice mode (#2417).
        refreshCwDecodeState();

        // If audio compression changed, recreate the RX audio stream
        QString newComp = m_radioModel.audioCompressionParam();
        if (newComp != prevComp && m_radioModel.isConnected()) {
            qDebug() << "MainWindow: audio compression changed from" << prevComp
                     << "to" << newComp << "— recreating audio stream";
            m_radioModel.removeRxAudioStream();
            QTimer::singleShot(500, this, [this]() {
                m_radioModel.createRxAudioStream();
            });
            updateNr2Availability();  // Disable NR2 if switching to Opus (#1597)
        }
    });
}

// wireAetherDspWidget() lives in MainWindow_Wiring.cpp (#3351 Phase 1d).
void MainWindow::paintEvent(QPaintEvent* event)
{
    // Layer-0 app backdrop.  WA_TranslucentBackground (set in the
    // constructor) disables Qt's default opaque window fill, so this
    // paintEvent is the single source of pixels for any region the rest
    // of the widget tree doesn't paint.  Honours alpha so operators can
    // edit color.background.app down toward translucency and see the
    // desktop bleed through anywhere a child widget doesn't have its own
    // opaque background — useful for A/B testing which applets/docks
    // still need explicit fills before a "glass-mode" theme is viable.
    QPainter p(this);
    const QColor bg = ThemeManager::instance().color("color.background.app");
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), bg.isValid() ? bg : QColor("#0f0f1a"));
    QMainWindow::paintEvent(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);

    if (m_startupGeometryReapplied || m_startupGeometryForFirstShow.isEmpty()) {
        return;
    }

    m_startupGeometryReapplied = true;
    // Defer to a singleShot(0) so the re-apply runs after the constructor has
    // fully returned.  This matters for the minimal-mode path: even though
    // toggleMinimalMode(true) can emit a showEvent mid-construction (via
    // showNormal()), the timer fires only once control unwinds back to the
    // event loop — by which point m_startupGeometryForFirstShow has been
    // finalized (swapped to the MinimalModeGeometry blob when present), so the
    // correct blob is the one that gets re-applied. (#3319)
    QTimer::singleShot(0, this, &MainWindow::reapplyStartupGeometryAfterShow);
}

void MainWindow::reapplyStartupGeometryAfterShow()
{
    if (m_startupGeometryForFirstShow.isEmpty()) {
        return;
    }

    // Pop-out applet containers are restored and shown during construction.
    // Re-apply the main-window geometry after this window is mapped so Qt
    // honors the saved monitor instead of the last pop-out's screen. (#3319)
    restoreGeometry(m_startupGeometryForFirstShow);

    // Test the frame's center against each screen's full geometry rather than
    // the top-left against availableGeometry().  A top-left landing in a
    // taskbar/panel exclusion strip (or in a gap between two displays whose
    // union covers the center) would otherwise be misreported as off-screen;
    // the center on full geometry matches "is this window on a real display".
    bool onScreen = false;
    const QPoint center = frameGeometry().center();
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen && screen->geometry().contains(center)) {
            onScreen = true;
            break;
        }
    }
    if (onScreen) {
        return;
    }

    // Clamp to a connected screen if the saved monitor was removed.
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        move(available.center().x() - width() / 2,
             available.center().y() - height() / 2);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // Anchor the frameless-mode size grip to the bottom-right corner,
    // overlaying the status bar.  Direct child of MainWindow so the
    // grip's native dotted-texture paint isn't suppressed by the
    // status-bar stylesheet.
    if (m_sizeGrip) {
        const int s = m_sizeGrip->width();
        m_sizeGrip->move(width() - s - 1, height() - s - 1);
        m_sizeGrip->raise();
    }
}

#if defined(Q_OS_WIN)
void MainWindow::applyWindowsCustomFrame()
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR desiredStyle = style
        | WS_CAPTION
        | WS_THICKFRAME
        | WS_SYSMENU
        | WS_MINIMIZEBOX
        | WS_MAXIMIZEBOX;
    if (style != desiredStyle) {
        SetWindowLongPtr(hwnd, GWL_STYLE, desiredStyle);
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER
                 | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (!msg || !result || !mainWindowCustomFrameEnabled()) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        if (IsZoomed(msg->hwnd) && windowHandle()
            && windowHandle()->visibility() != QWindow::FullScreen) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            RECT* clientArea = &params->rgrc[0];
            const int border = windowsResizeBorderThickness(msg->hwnd);
            clientArea->top += border;
            clientArea->bottom -= border;
            clientArea->left += border;
            clientArea->right -= border;
        }
        *result = 0;
        return true;
    }

    if (msg->message == WM_NCHITTEST) {
        RECT windowRect;
        if (!GetWindowRect(msg->hwnd, &windowRect)) {
            return QMainWindow::nativeEvent(eventType, message, result);
        }

        const POINT nativePos{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (nativePos.x < windowRect.left || nativePos.x > windowRect.right
            || nativePos.y < windowRect.top || nativePos.y > windowRect.bottom) {
            return QMainWindow::nativeEvent(eventType, message, result);
        }

        const bool canResize = !IsZoomed(msg->hwnd)
            && !(windowState() & Qt::WindowFullScreen);
        if (canResize) {
            const int border = windowsResizeBorderThickness(msg->hwnd);
            const bool onLeft = nativePos.x >= windowRect.left
                && nativePos.x < windowRect.left + border;
            const bool onRight = nativePos.x > windowRect.right - border
                && nativePos.x <= windowRect.right;
            const bool onTop = nativePos.y >= windowRect.top
                && nativePos.y < windowRect.top + border;
            const bool onBottom = nativePos.y > windowRect.bottom - border
                && nativePos.y <= windowRect.bottom;

            if (onTop && onLeft) {
                *result = HTTOPLEFT;
                return true;
            }
            if (onTop && onRight) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (onBottom && onLeft) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (onBottom && onRight) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (onLeft) {
                *result = HTLEFT;
                return true;
            }
            if (onRight) {
                *result = HTRIGHT;
                return true;
            }
            if (onTop) {
                *result = HTTOP;
                return true;
            }
            if (onBottom) {
                *result = HTBOTTOM;
                return true;
            }
        }

        if (m_titleBar && m_titleBar->isVisible()
            && m_titleBar->isSystemMoveAreaAt(QCursor::pos())) {
            *result = HTCAPTION;
            return true;
        }
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    if (event->type() != QEvent::WindowStateChange
        || !m_minimalMode
        || m_exitingMinimalMode
        || m_enteringMinimalMode) {
        return;
    }

    const Qt::WindowStates state = windowState();
    if (!(state & (Qt::WindowMaximized | Qt::WindowFullScreen)))
        return;

    // WM/keyboard/double-click maximized us while in minimal mode.  Defer
    // the exit so we don't tear down geometry mid-event-dispatch; the
    // re-entry guard prevents a second changeEvent (from showNormal inside
    // toggleMinimalMode) from re-scheduling.
    m_exitingMinimalMode = true;
    QTimer::singleShot(0, this, [this]() {
        if (m_minimalMode)
            toggleMinimalMode(false);
        m_exitingMinimalMode = false;
    });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Release the TGXL/PGXL native control sockets explicitly so the radio
    // can resume polling them on behalf of other clients (e.g. Maestro).
    // The radio-disconnect handler does this via a queued connection on
    // RadioModel::connectionStateChanged(false), but closeEvent does not
    // pump the event loop before QMainWindow::closeEvent() returns — so
    // without an explicit call the QTcpSockets are destroyed implicitly
    // during MainWindow tear-down, leaving the TGXL's single control slot
    // in a half-open state and producing the flickering Tun/SWR meters
    // reported on Maestro (#3079).
    m_tgxlConn.disconnect();
    m_pgxlConn.disconnect();

    preparePanadapterUiForShutdown();
    auto& s = AppSettings::instance();
    s.setValue("MainWindowGeometry", saveGeometry().toBase64());
    s.setValue("MainWindowState",   saveState().toBase64());

    // Refresh MinimalModeGeometry on close so a user who launches in
    // Minimal Mode, drags the window, and quits without ever toggling
    // back to full mode still gets their position restored next launch.
    // Without this, MinimalModeGeometry is only written by
    // toggleMinimalMode(false) and stays stale — visible as a position
    // drift each launch, most pronounced with FramelessWindowHint on
    // Windows where the WM no longer caches pos() for us.  Skip when
    // maximized/fullscreen to match the abnormal-state guard in
    // toggleMinimalMode(false). (#2483)
    if (m_minimalMode &&
        !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
        s.setValue("MinimalModeGeometry", saveGeometry().toBase64());
    }

    // Close the applet-panel pop-out window if it's floating.  We
    // must do this explicitly because the window has parent=nullptr
    // (Qt::Window, top-level) so Qt won't auto-close it when the
    // main window exits.  m_shuttingDown gates the eventFilter so
    // AppletPanelFloating stays True for restart persistence.
    if (m_appletPanelFloatWindow) {
        m_appletPanelFloatWindow->close();
    }
    // SplitterState no longer saved (2-pane layout uses stretch factors)
    // ConnPanelCollapsed removed — panel is now a popup dialog

    s.setValue("MemoryDialogOpen",
        (m_memoryDialog && m_memoryDialog->isVisible()) ? "True" : "False");

    // Save active slice frequency/mode so the next empty-radio reconnect can
    // recreate a default slice at a sensible place (see RadioModel slice-list
    // handling). NOT used for panadapter centering — the radio is
    // authoritative for that (#1493).
    auto* sl = activeSlice();
    if (sl) {
        s.setValue("LastFrequency", QString::number(sl->frequency(), 'f', 6));
        s.setValue("LastMode", sl->mode());
    }

    // Save per-slice DAX channel assignments for restore on next launch.
    // Keyed by slice index (A=0, B=1, ...) since radio-assigned IDs change.
    {
        const QList<SliceModel*> slices = m_radioModel.slices();
        for (int i = 0; i < slices.size(); ++i) {
            const QString key = QString("DaxChannel_Slice%1").arg(QChar('A' + i));
            if (slices[i]->daxChannel() > 0) {
                s.setValue(key, QString::number(slices[i]->daxChannel()));
            } else {
                s.remove(key);
            }
        }
    }

    // DAX IQ channel is radio-authoritative — no client-side persistence needed.
    // The radio echoes daxiq_channel in pan status on reconnect.

    // Save client-side DSP state before destructor disables them
    s.setValue("ClientNr2Enabled", m_audio->nr2Enabled() ? "True" : "False");
    s.setValue("ClientRn2Enabled", m_audio->rn2Enabled() ? "True" : "False");
    s.setValue("ClientNr4Enabled", m_audio->nr4Enabled() ? "True" : "False");
    s.setValue("ClientDfnrEnabled", m_audio->dfnrEnabled() ? "True" : "False");
    s.setValue("ClientMnrEnabled", m_audio->mnrEnabled() ? "True" : "False");
    // BNR not persisted — requires manual enable each session
    // DEXP saved on-change in PhoneApplet — do NOT overwrite here, because
    // the radio may have reset DEXP to off (model reflects radio state, not
    // the user's preference).

    s.save();

    // Suppress reconnect dialog during shutdown (#527)
    m_userDisconnected = true;
    m_wanReconnectTimer.stop();
    m_wanReconnectAttemptInProgress = false;
    if (m_reconnectDlg) {
        QDialog* reconnectDialog = m_reconnectDlg;
        m_reconnectDlg = nullptr;
        reconnectDialog->close();
        delete reconnectDialog;
    }

    m_discovery.stopListening();

#ifdef HAVE_RADE
    // Deactivate RADE before disconnecting so the mute-restore command
    // reaches the radio while the connection is still alive. Without this,
    // the destructor's deactivateRADE() runs after disconnectFromRadio()
    // has already closed the socket — audio_mute=1 is left stranded on
    // the radio and the slice appears muted on the next session.
    if (m_radeSliceId >= 0)
        deactivateRADE();
#endif

    m_radioModel.disconnectFromRadio();
    audioStopRx();

    // Stop spot client worker thread
    if (m_spotThread) {
        if (m_spotThread->isRunning()) {
            DxClusterClient* dxCluster = m_dxCluster;
            DxClusterClient* rbnClient = m_rbnClient;
            WsjtxClient* wsjtxClient = m_wsjtxClient;
            SpotCollectorClient* spotCollectorClient = m_spotCollectorClient;
            PotaClient* potaClient = m_potaClient;
#ifdef HAVE_WEBSOCKETS
            FreeDvClient* freedvClient = m_freedvClient;
#endif
            QMetaObject::invokeMethod(dxCluster, [dxCluster] { dxCluster->disconnect(); },
                                      Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(rbnClient, [rbnClient] { rbnClient->disconnect(); },
                                      Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(wsjtxClient, [wsjtxClient] { wsjtxClient->stopListening(); },
                                      Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(spotCollectorClient,
                                      [spotCollectorClient] { spotCollectorClient->stopListening(); },
                                      Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(potaClient, [potaClient] { potaClient->stopPolling(); },
                                      Qt::BlockingQueuedConnection);
#ifdef HAVE_WEBSOCKETS
            QMetaObject::invokeMethod(freedvClient,
                                      [freedvClient] { freedvClient->stopConnection(); },
                                      Qt::BlockingQueuedConnection);
#endif
            dxCluster->deleteLater();
            rbnClient->deleteLater();
            wsjtxClient->deleteLater();
            spotCollectorClient->deleteLater();
            potaClient->deleteLater();
#ifdef HAVE_WEBSOCKETS
            freedvClient->deleteLater();
#endif
            m_spotThread->quit();
            m_spotThread->wait(3000);
        } else {
            delete m_dxCluster;
            delete m_rbnClient;
            delete m_wsjtxClient;
            delete m_spotCollectorClient;
            delete m_potaClient;
#ifdef HAVE_WEBSOCKETS
            delete m_freedvClient;
#endif
        }
        m_dxCluster = nullptr;
        m_rbnClient = nullptr;
        m_wsjtxClient = nullptr;
        m_spotCollectorClient = nullptr;
        m_potaClient = nullptr;
#ifdef HAVE_WEBSOCKETS
        m_freedvClient = nullptr;
#endif
    }

    QMainWindow::closeEvent(event);
}

// keyPressEvent()/keyReleaseEvent() lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
void MainWindow::cancelTransmitFromIndicator()
{
    if (!m_radioModel.isConnected()) {
        statusBar()->showMessage("TX cancel ignored: not connected", 2000);
        return;
    }

    const quint32 owner = m_radioModel.txClientHandle();
    const quint32 ours = m_radioModel.ourClientHandle();
    if (owner != 0 && ours != 0 && owner != ours
        && !m_radioModel.transmitModel().isTransmitting()) {
        statusBar()->showMessage("TX is owned by another station", 2500);
        return;
    }

    m_spacePttActive = false;
    m_cwStraightKeyActive = false;
    m_cwLeftPaddleActive = false;
    m_cwRightPaddleActive = false;
    m_lastCwPaddleTraceId.store(0, std::memory_order_relaxed);
    m_lastCwPaddleSourceMs.store(0, std::memory_order_relaxed);

    if (m_iambicKeyer && m_iambicKeyer->isRunning()) {
        m_iambicKeyer->setPaddleState(false, false);
        m_iambicKeyer->reset();
    }
    if (m_audio && m_audio->cwSidetone())
        m_audio->cwSidetone()->setKeyDown(false);

    const quint64 sourceMs = cwTraceNowMs();
    const quint64 traceId = nextCwTraceId();
    const QString source = QStringLiteral("tx-indicator:cancel");
    m_radioModel.sendCwKey(false, source, traceId, sourceMs);
    m_radioModel.sendCwPtt(false, source, traceId, sourceMs);
    m_radioModel.transmitModel().stopTune();
    m_radioModel.setTransmit(false);

    statusBar()->showMessage("TX cancel requested", 2000);
}

void MainWindow::setCwStraightKeyState(bool down, const QString& source,
                                       quint64 traceId, quint64 sourceMs)
{
    if (m_cwStraightKeyActive == down)
        return;

    m_cwStraightKeyActive = down;
    const QString actionSource = source.isEmpty()
        ? QStringLiteral("cw:straight-key")
        : source;

    if (lcCw().isDebugEnabled()) {
        const quint64 now = cwTraceNowMs();
        qCDebug(lcCw).noquote().nospace()
            << "CW action straight-key trace=" << traceId
            << " t=" << now << "ms"
            << " sinceSourceMs=" << (sourceMs ? static_cast<qint64>(now - sourceMs) : -1)
            << " source=" << actionSource
            << " down=" << down;
    }

    m_radioModel.sendCwKey(down, actionSource, traceId, sourceMs);
}

void MainWindow::setCwLeftPaddleState(bool down, const QString& source,
                                      quint64 traceId, quint64 sourceMs)
{
    if (m_cwLeftPaddleActive == down)
        return;

    m_cwLeftPaddleActive = down;
    pushCwPaddleState(source.isEmpty() ? QStringLiteral("cw:left-paddle") : source,
                      traceId, sourceMs);
}

void MainWindow::setCwRightPaddleState(bool down, const QString& source,
                                       quint64 traceId, quint64 sourceMs)
{
    if (m_cwRightPaddleActive == down)
        return;

    m_cwRightPaddleActive = down;
    pushCwPaddleState(source.isEmpty() ? QStringLiteral("cw:right-paddle") : source,
                      traceId, sourceMs);
}

void MainWindow::pushCwPaddleState(const QString& source,
                                   quint64 traceId, quint64 sourceMs)
{
    const QString actionSource = source.isEmpty()
        ? QStringLiteral("cw:paddle")
        : source;

    m_lastCwPaddleTraceId.store(traceId, std::memory_order_relaxed);
    m_lastCwPaddleSourceMs.store(sourceMs, std::memory_order_relaxed);

    if (lcCw().isDebugEnabled()) {
        const quint64 now = cwTraceNowMs();
        qCDebug(lcCw).noquote().nospace()
            << "CW action paddle trace=" << traceId
            << " t=" << now << "ms"
            << " sinceSourceMs=" << (sourceMs ? static_cast<qint64>(now - sourceMs) : -1)
            << " source=" << actionSource
            << " leftDit=" << m_cwLeftPaddleActive
            << " rightDah=" << m_cwRightPaddleActive
            << " localIambic=" << (m_iambicKeyer && m_iambicKeyer->isRunning());
    }

    if (m_iambicKeyer && m_iambicKeyer->isRunning()) {
        m_iambicKeyer->setPaddleState(m_cwLeftPaddleActive, m_cwRightPaddleActive);
    } else {
        m_radioModel.sendCwPaddle(m_cwLeftPaddleActive, m_cwRightPaddleActive,
                                  actionSource, traceId, sourceMs);
    }
}

// handleCwMomentaryShortcut() lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
void MainWindow::showNetworkDiagnosticsDialog()
{
#ifdef HAVE_WEBSOCKETS
    showOrRaisePersistent(m_networkDiagnosticsDialog,
                          &m_radioModel, m_audio, m_networkDiagnosticsHistory,
                          m_tciServer);
#else
    showOrRaisePersistent(m_networkDiagnosticsDialog,
                          &m_radioModel, m_audio, m_networkDiagnosticsHistory);
#endif
}

void MainWindow::showAgcCalibrationDialog(int sliceId)
{
    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice) {
        return;
    }
    // Provide the measured RF noise floor for the slice's pan (quiet-spot guard).
    AgcCalibrationDialog::NoiseFloorFn floorFn = [this](SliceModel* s) -> float {
        SpectrumWidget* sw = s ? spectrumForSlice(s) : nullptr;
        return sw ? sw->noiseFloorDbm() : std::numeric_limits<float>::quiet_NaN();
    };
    showOrRaisePersistent(m_agcCalibrationDialog, &m_radioModel, m_audio, floorFn);
    if (m_agcCalibrationDialog) {
        m_agcCalibrationDialog->setSlice(slice);
    }
}

void MainWindow::showAx25HfPacketDecodeDialog()
{
    SliceModel* slice = activeSlice();

    // Construct on first open if it didn't already come up via
    // startKissTncOnStartupIfConfigured(). Intentionally NOT using
    // showOrRaisePersistent() here because that template sets
    // WA_DeleteOnClose: closing the window would destroy the dialog and
    // along with it the KISS TCP server, dropping every connected client
    // without warning. The TNC server lifecycle is decoupled from the
    // window's open/close cycle — the dialog stays alive as long as
    // MainWindow does and is just hidden on close.
    if (!m_ax25HfPacketDecodeDialog) {
        auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, slice, this);
        dlg->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        m_ax25HfPacketDecodeDialog = dlg;
        m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
    }
    m_ax25HfPacketDecodeDialog->setAttachedSlice(slice);
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
    m_ax25HfPacketDecodeDialog->show();
    m_ax25HfPacketDecodeDialog->raise();
    m_ax25HfPacketDecodeDialog->activateWindow();
}

void MainWindow::startKissTncOnStartupIfConfigured()
{
    if (m_ax25HfPacketDecodeDialog)
        return; // already constructed (e.g. user opened the window)
    // One-shot migration from legacy flat keys (AetherModemKissTnc*) into
    // the nested-JSON blob (Constitution Principle V). Safe to call on
    // every startup — no-op once the blob exists.
    TncSettings::migrateLegacy();
    if (!TncSettings::startOnStartup())
        return;

    // Construct the AetherModem window hidden and persistent (no WA_DeleteOnClose)
    // so the KISS TCP server runs from launch and survives the window being
    // closed. The dialog's constructor auto-starts the TNC per the same setting.
    auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, activeSlice(), this);
    dlg->setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    m_ax25HfPacketDecodeDialog = dlg;
    m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
}

// External-controller methods (FlexControl, HID encoders / RC-28 / TMate 2 /
// Ulanzi / PowerMate / Shuttle, StreamDeck labels, control-devices snapshot)
// live in MainWindow_Controllers.cpp (#3351 Phase 1a).

void MainWindow::showPropDashboard()
{
    showOrRaisePersistent(m_propDashboardDialog, m_propForecast);
}

// Slider shortcut lease + eventFilter() lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
void MainWindow::toggleConnectionDialog()
{
    if (m_connPanel->isVisible()) {
        m_connPanel->hide();
        return;
    }

    // Position above the status bar, centered on the station label, while staying on-screen.
    QPoint statusBarTop = statusBar()->mapToGlobal(QPoint(0, 0));
    QPoint labelCenter = m_stationNickLabel->mapToGlobal(
        QPoint(m_stationNickLabel->width() / 2, 0));
    QScreen* screen = QApplication::screenAt(labelCenter);
    if (!screen && windowHandle())
        screen = windowHandle()->screen();
    if (!screen)
        screen = QApplication::primaryScreen();

    const QSize dlgSize = m_connPanel->size();
    QPoint pos(labelCenter.x() - dlgSize.width() / 2,
               statusBarTop.y() - dlgSize.height() - 8);

    if (screen) {
        const QRect available = screen->availableGeometry();
        const int maxX = available.left() + available.width() - dlgSize.width();
        const int maxY = available.top() + available.height() - dlgSize.height();
        pos.setX(qMax(available.left(), qMin(pos.x(), maxX)));
        pos.setY(qMax(available.top(), qMin(pos.y(), maxY)));
    }

    m_connPanel->move(pos);
    m_connPanel->show();
    m_connPanel->raise();
    m_connPanel->activateWindow();
}

void MainWindow::showMemoryDialog()
{
    const bool wasFresh = !m_memoryDialog;
    showOrRaisePersistent(m_memoryDialog, &m_radioModel);
    if (wasFresh && m_memoryDialog) {
        connect(m_memoryDialog.data(), &MemoryDialog::memoryActivated,
                this, [this](int memoryIndex) { activateMemorySpot(memoryIndex); });
        connect(m_memoryDialog.data(), &QObject::destroyed, this, [this] {
            if (m_shuttingDown)
                return;
            auto& s = AppSettings::instance();
            s.setValue("MemoryDialogOpen", "False");
            s.save();
        });
    }
}

SliceModel* MainWindow::preferredMemorySlice(const QString& preferredPanId) const
{
    if (preferredPanId.isEmpty())
        return activeSlice();

    if (auto* slice = activeSlice(); slice && slice->panId() == preferredPanId)
        return slice;

    for (auto* slice : m_radioModel.slices()) {
        if (slice && slice->panId() == preferredPanId)
            return slice;
    }

    return nullptr;
}

void MainWindow::showQuickAddMemoryDialog(const QString& preferredPanId)
{
    auto* slice = preferredMemorySlice(preferredPanId);
    if (!slice) {
        statusBar()->showMessage(
            preferredPanId.isEmpty()
                ? "Open a slice before saving a memory."
                : "Open a slice on this pan before saving a memory.",
            3000);
        return;
    }

    const int sliceId = slice->sliceId();
    const QString frequencyText = QString::number(slice->frequency(), 'f', 6);
    const QString summaryText = QString("%1  |  Filter %2 to %3 Hz")
        .arg(slice->mode())
        .arg(slice->filterLow())
        .arg(slice->filterHigh());

    QDialog dialog(this);
    dialog.setWindowTitle("Save Memory");
    dialog.setModal(true);
    dialog.setMinimumWidth(360);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* nameLabel = new QLabel("Name:", &dialog);
    root->addWidget(nameLabel);

    auto* nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText("Enter a memory name");
    root->addWidget(nameEdit);

    auto* freqLabel = new QLabel(QString("Current Frequency: %1 MHz").arg(frequencyText), &dialog);
    AetherSDR::ThemeManager::instance().applyStyleSheet(freqLabel, "QLabel { color: {{color.text.primary}}; font-size: 12px; }");
    root->addWidget(freqLabel);

    auto* summaryLabel = new QLabel(summaryText, &dialog);
    summaryLabel->setStyleSheet("QLabel { color: #70879b; font-size: 11px; }");
    root->addWidget(summaryLabel);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);

    auto* cancelButton = new QPushButton("Cancel", &dialog);
    cancelButton->setAutoDefault(false);
    buttonRow->addWidget(cancelButton);

    auto* saveButton = new QPushButton("Save", &dialog);
    saveButton->setDefault(true);
    saveButton->setEnabled(false);
    buttonRow->addWidget(saveButton);
    root->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(nameEdit, &QLineEdit::textChanged, &dialog, [saveButton](const QString& text) {
        saveButton->setEnabled(!text.trimmed().isEmpty());
    });

    const QPointer<QDialog> dialogGuard(&dialog);
    connect(saveButton, &QPushButton::clicked, &dialog, [this,
                                                         sliceId,
                                                         dialogGuard,
                                                         nameEdit,
                                                         saveButton,
                                                         cancelButton]() {
        auto* currentSlice = m_radioModel.slice(sliceId);
        if (!currentSlice) {
            QMessageBox::warning(this, "Save Memory",
                                 "That slice is no longer available.");
            return;
        }

        saveButton->setEnabled(false);
        cancelButton->setEnabled(false);
        nameEdit->setEnabled(false);

        const QString name = nameEdit->text().trimmed();
        createMemoryFromSlice(&m_radioModel, currentSlice, name, dialogGuard.data(),
            [this, dialogGuard, nameEdit, saveButton, cancelButton, name](int code, const QString& body, int) {
            if (!dialogGuard)
                return;

            if (code == 0) {
                statusBar()->showMessage(
                    QString("Saved \"%1\" to memories.").arg(name),
                    3000);
                dialogGuard->accept();
                return;
            }

            QMessageBox::warning(dialogGuard, "Save Memory",
                                 body.isEmpty()
                                     ? "Couldn't save the current slice to memory."
                                     : body);
            nameEdit->setEnabled(true);
            saveButton->setEnabled(!nameEdit->text().trimmed().isEmpty());
            cancelButton->setEnabled(true);
            nameEdit->setFocus(Qt::OtherFocusReason);
            nameEdit->selectAll();
        });
    });

    nameEdit->setFocus(Qt::OtherFocusReason);
    dialog.exec();
}

void MainWindow::updatePaTempLabel()
{
    const QString unit = m_paTempUseFahrenheit ? "F" : "C";
    if (!m_hasPaTempTelemetry) {
        m_paTempLabel->setText(QString("PA --\u00B0%1").arg(unit));
    } else if (m_paTempUseFahrenheit) {
        const float paTempF = (m_lastPaTempC * 9.0f / 5.0f) + 32.0f;
        m_paTempLabel->setText(QString("PA %1\u00B0F").arg(paTempF, 0, 'f', 1));
    } else {
        m_paTempLabel->setText(QString("PA %1\u00B0C").arg(m_lastPaTempC, 0, 'f', 1));
    }

    m_paTempLabel->setToolTip(
        QString("PA temperature\nClick to switch to %1")
            .arg(m_paTempUseFahrenheit ? "Celsius (\u00B0C)" : "Fahrenheit (\u00B0F)"));
}

void MainWindow::setPaTempDisplayUnit(bool useFahrenheit)
{
    if (m_paTempUseFahrenheit == useFahrenheit)
        return;

    m_paTempUseFahrenheit = useFahrenheit;
    auto& settings = AppSettings::instance();
    settings.setValue(kPaTempUnitSettingKey, useFahrenheit ? "Fahrenheit" : "Celsius");
    settings.save();
    updatePaTempLabel();
}

// ─── Audio thread helpers (#502) ─────────────────────────────────────────────
// These invoke AudioEngine methods on the audio worker thread.

void MainWindow::updatePcAudioTooltip()
{
    if (!m_titleBar || !m_audio)
        return;

    auto describeDevice = [](const QAudioDevice& selected,
                             const QAudioDevice& defaultDevice) {
        const bool usingDefault = selected.isNull();
        const QAudioDevice device = usingDefault ? defaultDevice : selected;
        const QString name = device.description().trimmed();

        if (device.isNull() || name.isEmpty())
            return MainWindow::tr("Unavailable");

        return usingDefault
            ? MainWindow::tr("%1 (system default)").arg(name)
            : name;
    };

    const QAudioDevice inputDevice = m_audio->inputDevice();
    const QAudioDevice outputDevice = m_audio->outputDevice();
    m_titleBar->setPcAudioDevices(
        describeDevice(inputDevice, QMediaDevices::defaultAudioInput()),
        describeDevice(outputDevice, QMediaDevices::defaultAudioOutput()));
}

void MainWindow::audioStartRx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::startRxStream);
}

void MainWindow::audioStopRx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::stopRxStream);
}

void MainWindow::audioStartTx(const QHostAddress& addr, quint16 port)
{
    QMetaObject::invokeMethod(m_audio, [this, addr, port]() {
        m_audio->startTxStream(addr, port);
    });
}

void MainWindow::audioStopTx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::stopTxStream);
}

void MainWindow::setupAudioDeviceChangeMonitor()
{
    m_knownAudioInputIds = audioDeviceIds(QMediaDevices::audioInputs());
    m_knownAudioOutputIds = audioDeviceIds(QMediaDevices::audioOutputs());
    m_knownDefaultAudioInputId = QMediaDevices::defaultAudioInput().id();
    m_knownDefaultAudioOutputId = QMediaDevices::defaultAudioOutput().id();

    m_audioDeviceChangeTimer.setSingleShot(true);
    m_audioDeviceChangeTimer.setInterval(750);
    connect(&m_audioDeviceChangeTimer, &QTimer::timeout,
            this, &MainWindow::handleAudioDeviceListChanged);

    m_audioDeviceMonitor = new QMediaDevices(this);
    connect(m_audioDeviceMonitor, &QMediaDevices::audioInputsChanged,
            this, &MainWindow::scheduleAudioDeviceChangeCheck);
    connect(m_audioDeviceMonitor, &QMediaDevices::audioOutputsChanged,
            this, &MainWindow::scheduleAudioDeviceChangeCheck);
}

void MainWindow::scheduleAudioDeviceChangeCheck()
{
    if (m_shuttingDown)
        return;
    m_audioDeviceChangeTimer.start();
}

void MainWindow::handleAudioDeviceListChanged()
{
    if (!m_audio || m_shuttingDown)
        return;

    if (m_audioDeviceDialogOpen) {
        m_audioDeviceChangeTimer.start();
        return;
    }

    const QList<QAudioDevice> inputDevices = QMediaDevices::audioInputs();
    const QList<QAudioDevice> outputDevices = QMediaDevices::audioOutputs();
    const QAudioDevice defaultInput = QMediaDevices::defaultAudioInput();
    const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();
    const QByteArray currentDefaultInputId = defaultInput.id();
    const QByteArray currentDefaultOutputId = defaultOutput.id();
    const QList<QByteArray> currentInputIds = audioDeviceIds(inputDevices);
    const QList<QByteArray> currentOutputIds = audioDeviceIds(outputDevices);
    const QList<QByteArray> addedInputIds =
        newlyAddedAudioDeviceIds(inputDevices, m_knownAudioInputIds);
    const QList<QByteArray> addedOutputIds =
        newlyAddedAudioDeviceIds(outputDevices, m_knownAudioOutputIds);
    const QList<QByteArray> removedInputIds =
        removedAudioDeviceIds(m_knownAudioInputIds, currentInputIds);
    const QList<QByteArray> removedOutputIds =
        removedAudioDeviceIds(m_knownAudioOutputIds, currentOutputIds);

    m_knownAudioInputIds = currentInputIds;
    m_knownAudioOutputIds = currentOutputIds;
    const bool defaultInputChanged =
        currentDefaultInputId != m_knownDefaultAudioInputId;
    const bool defaultOutputChanged =
        currentDefaultOutputId != m_knownDefaultAudioOutputId;
    m_knownDefaultAudioInputId = currentDefaultInputId;
    m_knownDefaultAudioOutputId = currentDefaultOutputId;

    const QAudioDevice currentInput = m_audio->inputDevice();
    const QAudioDevice currentOutput = m_audio->outputDevice();
    const bool resetInputToDefault =
        !currentInput.isNull() && !audioDevicePresent(inputDevices, currentInput);
    const bool resetOutputToDefault =
        !currentOutput.isNull() && !audioDevicePresent(outputDevices, currentOutput);
    const bool defaultInputNeedsRestart =
        currentInput.isNull() && (!removedInputIds.isEmpty() || defaultInputChanged);
    const bool defaultOutputNeedsRestart =
        currentOutput.isNull() && (!removedOutputIds.isEmpty() || defaultOutputChanged);
    const bool resetInput = resetInputToDefault || defaultInputNeedsRestart;
    const bool resetOutput = resetOutputToDefault || defaultOutputNeedsRestart;
    const bool reinitializePcInput = resetInput
        && m_radioModel.isConnected()
        && m_radioModel.transmitModel().micSelection() == "PC";

    const bool deviceAdded = !addedInputIds.isEmpty() || !addedOutputIds.isEmpty();
    // Only prompt when the user's existing selection is no longer usable;
    // a new arrival while both selections still work is platform-audio
    // churn, not an actionable change (issue #2864).
    const bool currentSelectionStillValid =
        audioDevicePresent(inputDevices, currentInput)
        && audioDevicePresent(outputDevices, currentOutput);
    const bool userChoiceRequired = deviceAdded && !currentSelectionStillValid;
    if (!userChoiceRequired) {
        if (resetInput || resetOutput)
            resetMissingAudioDevicesToDefault(resetInput,
                                              resetOutput,
                                              reinitializePcInput);
        return;
    }

    const bool suppressAudioDeviceNotifications =
        AppSettings::instance()
            .value(kSuppressAudioDeviceNotificationsKey, "False")
            .toString() == "True";
    if (suppressAudioDeviceNotifications) {
        if (resetInput || resetOutput)
            resetMissingAudioDevicesToDefault(resetInput,
                                              resetOutput,
                                              reinitializePcInput);
        return;
    }

    m_audioDeviceDialogOpen = true;
    AudioDeviceChangeDialog dialog(inputDevices,
                                   outputDevices,
                                   currentInput,
                                   currentOutput,
                                   addedInputIds,
                                   addedOutputIds,
                                   this);
    const int result = dialog.exec();
    m_audioDeviceDialogOpen = false;

    if (dialog.dontAskAgainChecked()) {
        auto& settings = AppSettings::instance();
        settings.setValue(kSuppressAudioDeviceNotificationsKey, "True");
        settings.save();
    }

    if (result == QDialog::Accepted) {
        const QAudioDevice selectedInput = dialog.selectedInputDevice();
        const QAudioDevice selectedOutput = dialog.selectedOutputDevice();
        const bool inputChanged =
            !sameAudioDeviceSelection(currentInput, selectedInput);
        const bool reinitializePcInput = inputChanged
            && m_radioModel.isConnected()
            && m_radioModel.transmitModel().micSelection() == "PC";
        applyAudioDeviceSelection(selectedInput,
                                  selectedOutput,
                                  reinitializePcInput);
    } else if (resetInput || resetOutput) {
        resetMissingAudioDevicesToDefault(resetInput,
                                          resetOutput,
                                          reinitializePcInput);
    }
}

void MainWindow::applyAudioDeviceSelection(const QAudioDevice& inputDevice,
                                           const QAudioDevice& outputDevice,
                                           bool reinitializePcInput)
{
    if (!m_audio)
        return;

    QPointer<AudioEngine> audio = m_audio;
    const QHostAddress radioAddress = m_radioModel.radioAddress();
    const bool restartCapture = reinitializePcInput && !radioAddress.isNull();
    QMetaObject::invokeMethod(m_audio, [audio,
                                        inputDevice,
                                        outputDevice,
                                        restartCapture,
                                        radioAddress]() {
        if (!audio)
            return;
        audio->setInputDevice(inputDevice);
        audio->setOutputDevice(outputDevice);
        if (restartCapture && !audio->isTxStreaming())
            audio->startTxStream(radioAddress, 4991);
    }, Qt::QueuedConnection);
}

void MainWindow::resetMissingAudioDevicesToDefault(bool resetInput,
                                                   bool resetOutput,
                                                   bool reinitializePcInput)
{
    if (!m_audio || (!resetInput && !resetOutput))
        return;

    QPointer<AudioEngine> audio = m_audio;
    const QHostAddress radioAddress = m_radioModel.radioAddress();
    const bool restartCapture = reinitializePcInput && !radioAddress.isNull();
    QMetaObject::invokeMethod(m_audio, [audio,
                                        resetInput,
                                        resetOutput,
                                        restartCapture,
                                        radioAddress]() {
        if (!audio)
            return;
        if (resetInput)
            audio->setInputDevice(QAudioDevice{});
        if (resetOutput)
            audio->setOutputDevice(QAudioDevice{});
        if (restartCapture && !audio->isTxStreaming())
            audio->startTxStream(radioAddress, 4991);
    }, Qt::QueuedConnection);
}

// ─── UI Construction ──────────────────────────────────────────────────────────

// buildMenuBar() lives in MainWindow_Menus.cpp (#3351 Phase 1b).

void MainWindow::buildUI()
{
    // ── Title bar + central splitter ─────────────────────────────────────────
    m_titleBar = new TitleBar(this);
    // Embed the menu bar into the title bar (left side)
    m_titleBar->setMenuBar(menuBar());
    updatePcAudioTooltip();
    connect(m_audio, &AudioEngine::inputDeviceChanged,
            this, &MainWindow::updatePcAudioTooltip, Qt::QueuedConnection);
    connect(m_audio, &AudioEngine::outputDeviceChanged,
            this, &MainWindow::updatePcAudioTooltip, Qt::QueuedConnection);
    connect(m_titleBar, &TitleBar::multiFlexClicked,
            this, &MainWindow::showMultiFlexDialog);
    connect(m_titleBar, &TitleBar::minimalModeRequested, this, [this]() {
        toggleMinimalMode(!m_minimalMode);
        if (m_minimalModeAction) {
            QSignalBlocker b(m_minimalModeAction);
            m_minimalModeAction->setChecked(m_minimalMode);
        }
    });
    connect(m_titleBar, &TitleBar::minimalModeWindowedExitRequested, this, [this]() {
        toggleMinimalMode(false);
    });
    // Title-bar dock-side click: clicking the active-side icon hides the
    // applet panel; clicking the inactive-side icon moves it there (and
    // shows it if hidden).
    auto handleDockClick = [this](bool wantLeft) {
        // If currently floating, dock back first so the float window is
        // torn down via the canonical path; otherwise reparenting the
        // contents into the splitter leaves an empty float window alive
        // and visible (the "black box" in #2584).
        if (m_appletPanelFloatWindow) {
            toggleAppletPanelFloating(false);
        }
        const bool dockedLeft = AppSettings::instance()
            .value("AppletPanelDockedLeft", "False").toString() == "True";
        const bool visible = m_appletPanel && m_appletPanel->isVisible();
        if (visible && dockedLeft == wantLeft) {
            setAppletPanelVisible(false);
        } else {
            if (!visible) setAppletPanelVisible(true);
            if (dockedLeft != wantLeft) setAppletPanelDockedLeft(wantLeft);
        }
    };
    connect(m_titleBar, &TitleBar::dockAppletLeftRequested,  this, [handleDockClick]() { handleDockClick(true);  });
    connect(m_titleBar, &TitleBar::dockAppletRightRequested, this, [handleDockClick]() { handleDockClick(false); });
    // Pop-out icon: toggle floating via the shared helper so the icon, the
    // Ctrl+Shift+S shortcut, and the float-window close-X stay in sync.
    connect(m_titleBar, &TitleBar::popOutAppletRequested, this, [this]() {
        toggleAppletPanelFloating(m_appletPanelFloatWindow == nullptr);
    });

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(0);

    auto* central = new QWidget(this);
    auto* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_titleBar);
    vbox->addWidget(m_splitter, 1);
    setCentralWidget(central);

    auto* splitter = m_splitter;

    // Connection panel — modeless dialog; follows View -> Frameless Window.
    m_connPanel = new ConnectionPanel(this);
    m_connPanel->setWindowTitle("Connect to Radio");
    m_connPanel->setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    m_connPanel->setMinimumSize(640, 580);
    m_connPanel->resize(760, 660);
    m_connPanel->hide();

    // CWX panel — left of spectrum, hidden by default
    m_cwxPanel = new CwxPanel(&m_radioModel.cwxModel(), splitter);
    // Provide state probes so CWX can guard its F1-F12 / ESC app-wide
    // shortcuts on mode + transmit state (#1552).
    m_cwxPanel->setActiveModeProvider([this]() {
        auto* s = activeSlice();
        return s ? s->mode() : QString();
    });
    m_cwxPanel->setTransmittingProvider([this]() {
        return m_radioModel.transmitModel().isTransmitting();
    });
    splitter->addWidget(m_cwxPanel);
    m_cwxPanel->hide();

    // DVK panel — left of spectrum, hidden by default (mutually exclusive with CWX)
    m_dvkPanel = new DvkPanel(&m_radioModel.dvkModel(), splitter);
    auto* dvkTransfer = new DvkWavTransfer(&m_radioModel, this);
    m_dvkPanel->setWavTransfer(dvkTransfer);
    splitter->addWidget(m_dvkPanel);
    m_dvkPanel->hide();

    // Centre — panadapter stack (one or more FFT + waterfall panes)
    m_panStack = new PanadapterStack(splitter);
    m_panApplet = nullptr;  // ensure setActivePanApplet sees a change
    setActivePanApplet(m_panStack->addPanadapter("default"));
    splitter->addWidget(m_panStack);

    // Band stack panel signal wiring
    auto* bsPanel = m_panStack->bandStackPanel();

    connect(bsPanel, &BandStackPanel::addRequested, this, [this]() {
        auto* slice = activeSlice();
        if (!slice) return;
        BandStackEntry entry;
        entry.frequencyMhz = slice->frequency();
        entry.mode = slice->mode();
        entry.filterLow = slice->filterLow();
        entry.filterHigh = slice->filterHigh();
        entry.rxAntenna = slice->rxAntenna();
        entry.txAntenna = slice->txAntenna();
        entry.agcMode = slice->agcMode();
        entry.agcThreshold = slice->agcThreshold();
        entry.audioGain = static_cast<int>(slice->audioGain());
        entry.nbOn = slice->nbOn();
        entry.nbLevel = slice->nbLevel();
        entry.nrOn = slice->nrOn();
        entry.nrLevel = slice->nrLevel();
        entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        if (auto* pan = m_radioModel.activePanadapter()) {
            entry.wnbOn = pan->wnbActive();
            entry.wnbLevel = pan->wnbLevel();
        }

        BandStackSettings::instance().addEntry(m_radioModel.serial(), entry);
        BandStackSettings::instance().save();
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.serial(), m_bandPlanMgr);
    });
    connect(bsPanel, &BandStackPanel::recallRequested, this,
            [this](const BandStackEntry& e) {
        auto* slice = activeSlice();
        if (!slice) return;
        int id = slice->sliceId();

        // Mode first (affects filter ranges)
        if (slice->mode() != e.mode) {
            slice->setMode(e.mode);
        }
        applyTuneRequest(slice, e.frequencyMhz,
                         TuneIntent::AbsoluteJump, "bandstack-recall");
        // Filter
        if (e.filterLow != 0 || e.filterHigh != 0) {
            slice->setFilterWidth(e.filterLow, e.filterHigh);
        }
        // Antennas
        if (!e.rxAntenna.isEmpty() && e.rxAntenna != slice->rxAntenna()) {
            m_radioModel.sendCommand(QString("slice set %1 rxant=%2").arg(id).arg(e.rxAntenna));
        }
        if (!e.txAntenna.isEmpty() && e.txAntenna != slice->txAntenna()) {
            m_radioModel.sendCommand(QString("slice set %1 txant=%2").arg(id).arg(e.txAntenna));
        }
        // AGC
        if (!e.agcMode.isEmpty() && e.agcMode != slice->agcMode()) {
            m_radioModel.sendCommand(QString("slice set %1 agc_mode=%2").arg(id).arg(e.agcMode));
        }
        if (e.agcThreshold != slice->agcThreshold()) {
            m_radioModel.sendCommand(QString("slice set %1 agc_threshold=%2").arg(id).arg(e.agcThreshold));
        }
        // Volume
        if (static_cast<int>(slice->audioGain()) != e.audioGain) {
            slice->setAudioGain(static_cast<float>(e.audioGain));
        }
        // NB
        if (e.nbOn != slice->nbOn()) {
            m_radioModel.sendCommand(QString("slice set %1 nb=%2").arg(id).arg(e.nbOn ? 1 : 0));
        }
        if (e.nbLevel != slice->nbLevel()) {
            m_radioModel.sendCommand(QString("slice set %1 nb_level=%2").arg(id).arg(e.nbLevel));
        }
        // NR
        if (e.nrOn != slice->nrOn()) {
            m_radioModel.sendCommand(QString("slice set %1 nr=%2").arg(id).arg(e.nrOn ? 1 : 0));
        }
        if (e.nrLevel != slice->nrLevel()) {
            m_radioModel.sendCommand(QString("slice set %1 nr_level=%2").arg(id).arg(e.nrLevel));
        }
        // WNB (panadapter-level, not slice)
        if (auto* pan = m_radioModel.activePanadapter()) {
            if (e.wnbOn != pan->wnbActive()) {
                m_radioModel.sendCommand(
                    QString("display pan set %1 wnb=%2").arg(pan->panId()).arg(e.wnbOn ? 1 : 0));
            }
            if (e.wnbLevel != pan->wnbLevel()) {
                m_radioModel.sendCommand(
                    QString("display pan set %1 wnb_level=%2").arg(pan->panId()).arg(e.wnbLevel));
            }
        }
    });
    connect(bsPanel, &BandStackPanel::removeRequested, this,
            [this](int index) {
        BandStackSettings::instance().removeEntry(m_radioModel.serial(), index);
        BandStackSettings::instance().save();
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.serial(), m_bandPlanMgr);
    });

    // Clear All — with confirmation to avoid accidental loss during contests
    connect(bsPanel, &BandStackPanel::clearAllRequested, this, [this]() {
        if (BandStackSettings::instance().entries(m_radioModel.serial()).isEmpty())
            return;
        auto answer = QMessageBox::question(
            this, "Clear All Bookmarks",
            "Remove all band stack bookmarks?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        BandStackSettings::instance().clearAllEntries(m_radioModel.serial());
        BandStackSettings::instance().save();
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.serial(), m_bandPlanMgr);
    });

    // Clear band bookmarks (from grouped header right-click)
    connect(bsPanel, &BandStackPanel::clearBandRequested, this,
            [this](double lowMhz, double highMhz) {
        BandStackSettings::instance().clearBandEntries(
            m_radioModel.serial(), lowMhz, highMhz);
        BandStackSettings::instance().save();
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.serial(), m_bandPlanMgr);
    });

    // Group by band toggle
    connect(bsPanel, &BandStackPanel::groupByBandChanged, this, [](bool grouped) {
        BandStackSettings::instance().setGroupByBand(grouped);
        BandStackSettings::instance().save();
    });

    // Auto-expiry setting changed
    connect(bsPanel, &BandStackPanel::autoExpiryChanged, this, [](int minutes) {
        BandStackSettings::instance().setAutoExpiryMinutes(minutes);
        BandStackSettings::instance().save();
    });

    // Auto-save dwell setting changed
    connect(bsPanel, &BandStackPanel::autoSaveDwellChanged, this, [this](int seconds) {
        BandStackSettings::instance().setAutoSaveDwellSeconds(seconds);
        BandStackSettings::instance().save();
        if (!m_bsAutoSaveTimer) return;
        if (seconds <= 0) {
            m_bsAutoSaveTimer->stop();
        } else {
            m_bsAutoSaveTimer->setInterval(seconds * 1000);
            m_bsAutoSaveTimer->start();
        }
    });

    // Auto-expiry timer — runs every 30s, prunes stale bookmarks.
    // Started on radio connect and stopped on disconnect so the tick doesn't
    // fire in an idle app with no radio to prune bookmarks for.
    m_bsExpiryTimer = new QTimer(this);
    m_bsExpiryTimer->setInterval(30000);
    connect(m_bsExpiryTimer, &QTimer::timeout, this, [this]() {
        int minutes = BandStackSettings::instance().autoExpiryMinutes();
        if (minutes <= 0) return;
        if (m_radioModel.serial().isEmpty()) return;
        qint64 maxAge = static_cast<qint64>(minutes) * 60 * 1000;
        int removed = BandStackSettings::instance().removeExpiredEntries(
            m_radioModel.serial(), maxAge);
        if (removed > 0) {
            BandStackSettings::instance().save();
            m_panStack->bandStackPanel()->loadBookmarks(
                m_radioModel.serial(), m_bandPlanMgr);
        }
    });

    // Band-stack auto-save: single-shot per dwell window.  Reset on every
    // active-slice frequency change and on active-slice change; fires once
    // when the slice has been parked on the same freq long enough.
    m_bsAutoSaveTimer = new QTimer(this);
    m_bsAutoSaveTimer->setSingleShot(true);
    connect(m_bsAutoSaveTimer, &QTimer::timeout, this, [this]() {
        const int dwellSec = BandStackSettings::instance().autoSaveDwellSeconds();
        if (dwellSec <= 0) return;
        if (m_radioModel.serial().isEmpty()) return;
        if (m_radioModel.transmitModel().isTransmitting()) return;
        if (QDateTime::currentMSecsSinceEpoch() < m_bsConnectGraceUntilMs) return;

        auto* slice = activeSlice();
        if (!slice) return;
        const double freqMhz = slice->frequency();

        // Skip if any existing entry is within ±100 Hz on this radio
        // (avoids re-stacking the exact same station after a brief retune).
        auto existing = BandStackSettings::instance().entries(m_radioModel.serial());
        for (const auto& e : existing) {
            if (std::abs(e.frequencyMhz - freqMhz) < 0.0001) return;
        }

        // Per-band cap on auto-saved entries: keep at most 5 in the same
        // band before overwriting the oldest auto-saved entry there.  Manual
        // entries are never displaced — they hold their slot indefinitely.
        constexpr int kMaxAutoPerBand = 5;
        int bandIdx = -1;
        for (int b = 0; b < kBandCount; ++b) {
            if (freqMhz >= kBands[b].lowMhz && freqMhz <= kBands[b].highMhz) {
                bandIdx = b;
                break;
            }
        }
        if (bandIdx >= 0) {
            const double bandLow = kBands[bandIdx].lowMhz;
            const double bandHigh = kBands[bandIdx].highMhz;
            int autoCount = 0;
            int oldestAutoIdx = -1;
            qint64 oldestAutoMs = std::numeric_limits<qint64>::max();
            for (int i = 0; i < existing.size(); ++i) {
                const auto& e = existing[i];
                if (!e.autoSaved) continue;
                if (e.frequencyMhz < bandLow || e.frequencyMhz > bandHigh) continue;
                ++autoCount;
                if (e.createdAtMs > 0 && e.createdAtMs < oldestAutoMs) {
                    oldestAutoMs = e.createdAtMs;
                    oldestAutoIdx = i;
                }
            }
            if (autoCount >= kMaxAutoPerBand && oldestAutoIdx >= 0) {
                BandStackSettings::instance().removeEntry(
                    m_radioModel.serial(), oldestAutoIdx);
            }
        }

        BandStackEntry entry;
        entry.autoSaved = true;
        entry.frequencyMhz = freqMhz;
        entry.mode = slice->mode();
        entry.filterLow = slice->filterLow();
        entry.filterHigh = slice->filterHigh();
        entry.rxAntenna = slice->rxAntenna();
        entry.txAntenna = slice->txAntenna();
        entry.agcMode = slice->agcMode();
        entry.agcThreshold = slice->agcThreshold();
        entry.audioGain = static_cast<int>(slice->audioGain());
        entry.nbOn = slice->nbOn();
        entry.nbLevel = slice->nbLevel();
        entry.nrOn = slice->nrOn();
        entry.nrLevel = slice->nrLevel();
        entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        if (auto* pan = m_radioModel.activePanadapter()) {
            entry.wnbOn = pan->wnbActive();
            entry.wnbLevel = pan->wnbLevel();
        }
        BandStackSettings::instance().addEntry(m_radioModel.serial(), entry);
        BandStackSettings::instance().save();
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.serial(), m_bandPlanMgr);
    });
    refreshMemoryBrowsePanel();

    // Sync RadioModel's active pan/wf IDs when PanadapterStack focus changes.
    // This ensures display setting commands (fps, average, black_level, etc.)
    // target the correct pan.
    // Sync RadioModel's active pan ID when PanadapterStack focus changes.
    // This ensures display setting commands (fps, average, black_level, etc.)
    // target the correct pan — activeWfId() derives from activePanadapter()
    // which uses m_activePanId.
    connect(m_panStack, &PanadapterStack::activePanChanged,
            this, [this](const QString& panId) {
        m_radioModel.setActivePanId(panId);

        // Update m_panApplet for the new active pan
        if (auto* applet = m_panStack->panadapter(panId))
            setActivePanApplet(applet);

        // Show/hide CW decode panel based on the new active pan's slice mode
        // — driven through refreshCwDecodeState() so the panel on the
        // landed pan picks up the same RX/TX toggle gating the active
        // slice does (#2417).
        for (auto* sl : m_radioModel.slices()) {
            if (sl->panId() == panId) {
                const bool isCw = (sl->mode() == "CW" || sl->mode() == "CWL");
                const bool anyOn = CwDecodeSettings::anyEnabled();
                if (auto* applet = m_panStack->panadapter(panId))
                    applet->setCwPanelVisible(isCw && anyOn);
                refreshCwDecodeState();
                refreshRttyDecodeState();
                break;
            }
        }
    });
    splitter->setStretchFactor(0, 0);  // CWX panel: fixed width
    splitter->setStretchFactor(1, 0);  // DVK panel: fixed width
    splitter->setStretchFactor(2, 1);  // PanStack: stretch
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);

    // Right — applet panel (includes S-Meter)
    m_appletPanel = new AppletPanel(splitter);
    splitter->addWidget(m_appletPanel);
    splitter->setStretchFactor(3, 0);
    splitter->setCollapsible(3, false);

    // Restore floating-container state from the previous session.
    // Called synchronously here, before the event loop starts, so that legacy
    // float migration singleShot(0) timers posted inside AppletPanel::makeEntry()
    // cannot call saveState() and overwrite ContainerTree before we read it.
    if (m_appletPanel && m_appletPanel->containerManager())
        m_appletPanel->containerManager()->restoreState();

    // Set initial splitter sizes: CWX=0, DVK=0 (both hidden), center=stretch, right=310
    const int centerWidth = qMax(400, width() - 310);
    splitter->setSizes({0, 0, centerWidth, 310});

    // Restore applet panel visibility
    if (AppSettings::instance().value("AppletPanelVisible", "True").toString() != "True")
        m_appletPanel->hide();

    // Restore applet panel dock side ("AppletPanelDockedLeft" — defaults right).
    {
        const bool dockedLeft =
            AppSettings::instance().value("AppletPanelDockedLeft", "False").toString() == "True";
        if (dockedLeft)
            setAppletPanelDockedLeft(true);
        else if (m_titleBar)
            m_titleBar->setAppletDockState(m_appletPanel->isVisible(), false);
    }

    // ── Status bar (SmartSDR-style, double height) ─────────────────────
    statusBar()->setFixedHeight(46);
    statusBar()->setSizeGripEnabled(false);
    // Bottom-right resize grip — direct child of MainWindow (not parented
    // into the status bar, whose stylesheet was suppressing the grip's
    // dotted texture).  Position is maintained in resizeEvent.
    m_sizeGrip = new QSizeGrip(this);
    m_sizeGrip->setFixedSize(16, 16);
    m_sizeGrip->raise();
    m_sizeGrip->setVisible(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    AetherSDR::ThemeManager::instance().applyStyleSheet(statusBar(), "QStatusBar { background: {{color.background.0}}; border-top: 1px solid {{color.background.1}}; }"
        "QStatusBar::item { border: none; }"
        "QLabel { font-size: 21px; background: transparent; }");

    const QString valStyle  = "QLabel { color: #8aa8c0; font-size: 21px; }";
    const QString sepStyle  = "QLabel { color: #304050; font-size: 21px; }";
    const QString greyInd   = "QLabel { color: #404858; font-weight: bold; font-size: 21px; }";
    const QString greenInd  = "QLabel { color: #00e060; font-weight: bold; font-size: 21px; }";
    const QString redInd    = "QLabel { color: #e04040; font-weight: bold; font-size: 21px; }";
    const QString greyIndLg = "QLabel { color: #404858; font-weight: bold; font-size: 24px; }";
    const QString greenIndLg= "QLabel { color: #00e060; font-weight: bold; font-size: 24px; }";

    // Use a container with HBoxLayout for 3-section layout:
    // [left items] → stretch → [STATION centered] → stretch → [right items]
    auto* container = new QWidget(this);
    auto* hbox = new QHBoxLayout(container);
    hbox->setContentsMargins(6, 0, 6, 0);
    hbox->setSpacing(6);

    auto addSep = [&]() -> QLabel* {
        auto* sep = new QLabel(" · ");
        sep->setStyleSheet(sepStyle);
        hbox->addWidget(sep);
        return sep;
    };

    // Hidden connection state label (used by connect/disconnect logic)
    m_connStatusLabel = new QLabel("", this);
    m_connStatusLabel->hide();

    // ── Left section ─────────────────────────────────────────────────────
    // +PAN icon: jagged FFT-spectrum trace with multiple sharp peaks
    // (matching the SSDR visual language) plus a "+" overlay.
    {
        QPixmap pm(36, 28);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing);

        const QColor stroke(255, 255, 255, 210);

        // Polyline: noise floor at y=22, multiple peaks of varying height,
        // with extra detail between peaks for the "real FFT" texture.
        pp.setPen(QPen(stroke, 1.6));
        const QPointF pts[] = {
            { 0, 22}, { 1, 21}, { 2, 22}, { 3, 19}, { 4, 22},   // floor + small peak
            { 5, 21}, { 6, 18}, { 7, 12}, { 8, 17}, { 9, 22},   // tall peak
            {10, 21}, {11, 22}, {12, 16}, {13, 22},             // medium peak
            {14, 21}, {15, 19}, {16, 22},                       // ripple
            {17, 20}, {18, 12}, {19,  4}, {20, 11}, {21, 21},   // tallest peak
            {22, 22}, {23, 21}, {24, 17}, {25, 22},             // medium peak
            {26, 21}, {27, 22}, {28, 18}, {29, 22}, {30, 22}    // small peak + floor
        };
        pp.drawPolyline(pts, sizeof(pts) / sizeof(pts[0]));

        // "+" sign in upper-right.
        pp.setPen(QPen(stroke, 2.2));
        pp.drawLine(30, 4, 30, 14);   // vertical
        pp.drawLine(25, 9, 35, 9);    // horizontal
        pp.end();
        // Band stack toggle: 3 vertically stacked circles
        {
            m_bandStackIndicator = new QLabel;
            m_bandStackIndicator->setPixmap(buildBandStackIndicatorPixmap(false));
            m_bandStackIndicator->setCursor(Qt::PointingHandCursor);
            m_bandStackIndicator->setToolTip("Open band stack panel");
            m_bandStackIndicator->installEventFilter(this);
            hbox->addWidget(m_bandStackIndicator);
        }

        hbox->addSpacing(8);

        auto* addPanBtn = new QLabel;
        addPanBtn->setPixmap(pm);
        addPanBtn->setCursor(Qt::PointingHandCursor);
        addPanBtn->setToolTip("Add Panadapter");
        addPanBtn->installEventFilter(this);
        hbox->addWidget(addPanBtn);
        m_addPanLabel = addPanBtn;
    }

    hbox->addSpacing(8);

    m_tnfIndicator = new QLabel("TNF");
    m_tnfIndicator->setStyleSheet(greyIndLg);
    m_tnfIndicator->setCursor(Qt::PointingHandCursor);
    m_tnfIndicator->setToolTip(buildTnfTooltip(m_radioModel.tnfModel()));
    m_tnfIndicator->installEventFilter(this);
    hbox->addWidget(m_tnfIndicator);
    auto updateTnfTooltip = [this]() {
        if (m_tnfIndicator) {
            m_tnfIndicator->setToolTip(buildTnfTooltip(m_radioModel.tnfModel()));
        }
    };
    connect(&m_radioModel.tnfModel(), &TnfModel::tnfChanged,
            this, [updateTnfTooltip](int) { updateTnfTooltip(); });
    connect(&m_radioModel.tnfModel(), &TnfModel::tnfRemoved,
            this, [updateTnfTooltip](int) { updateTnfTooltip(); });

    m_cwxIndicator = new QLabel("CWX");
    m_cwxIndicator->setStyleSheet(greyIndLg);
    m_cwxIndicator->setCursor(Qt::PointingHandCursor);
    m_cwxIndicator->setToolTip("CW Keyer — click to toggle");
    m_cwxIndicator->installEventFilter(this);
    hbox->addWidget(m_cwxIndicator);

    m_dvkIndicator = new QLabel("DVK");
    m_dvkIndicator->setStyleSheet(greyIndLg);
    m_dvkIndicator->setCursor(Qt::PointingHandCursor);
    m_dvkIndicator->setToolTip("Digital Voice Keyer — click to toggle");
    m_dvkIndicator->installEventFilter(this);
    hbox->addWidget(m_dvkIndicator);

    m_fdxIndicator = new QLabel("FDX");
    m_fdxIndicator->setStyleSheet(greyIndLg);
    m_fdxIndicator->setCursor(Qt::PointingHandCursor);
    m_fdxIndicator->setToolTip("Full Duplex — RX stays active during TX (click to toggle)");
    m_fdxIndicator->installEventFilter(this);
    hbox->addWidget(m_fdxIndicator);

    addSep();

    // Radio model (top) + version (bottom) stacked
    auto* radioStack = new QWidget;
    auto* radioVbox = new QVBoxLayout(radioStack);
    radioVbox->setContentsMargins(0, 0, 0, 0);
    radioVbox->setSpacing(0);
    radioVbox->setAlignment(Qt::AlignVCenter);
    m_radioInfoLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_radioInfoLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_radioInfoLabel->setAlignment(Qt::AlignCenter);
    m_radioVersionLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_radioVersionLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_radioVersionLabel->setAlignment(Qt::AlignCenter);
    radioVbox->addWidget(m_radioInfoLabel);
    radioVbox->addWidget(m_radioVersionLabel);
    hbox->addWidget(radioStack);

    // ── Center stretch → STATION → stretch ───────────────────────────────
    hbox->addStretch(1);

    m_stationNickLabel = new QLabel("N0CALL");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_stationNickLabel, "QLabel { color: {{color.text.primary}}; font-size: 21px; background: {{color.background.0}}; "
        "border: 1px solid rgba(255,255,255,128); padding: 2px 12px; }");
    m_stationNickLabel->setAlignment(Qt::AlignCenter);
    m_stationNickLabel->setCursor(Qt::PointingHandCursor);
    m_stationNickLabel->setToolTip("Double-click to connect/disconnect");
    m_stationNickLabel->installEventFilter(this);
    hbox->addWidget(m_stationNickLabel);
    m_stationLabel = m_stationNickLabel;  // alias for existing references

    hbox->addStretch(1);

    // ── Right section ────────────────────────────────────────────────────
    // Reserve consistent width for the compact telemetry stacks so updates
    // do not cause the status bar to reshuffle as values change.
    constexpr int kTelemetryStackMinWidth = 84;

    // GPS satellites (top) + lock status (bottom) stacked
    auto* gpsStack = new QWidget;
    gpsStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* gpsVbox = new QVBoxLayout(gpsStack);
    gpsVbox->setContentsMargins(0, 0, 0, 0);
    gpsVbox->setSpacing(0);
    gpsVbox->setAlignment(Qt::AlignVCenter);
    m_gpsLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_gpsLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_gpsLabel->setAlignment(Qt::AlignCenter);
    m_gpsStatusLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_gpsStatusLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_gpsStatusLabel->setAlignment(Qt::AlignCenter);
    gpsVbox->addWidget(m_gpsLabel);
    gpsVbox->addWidget(m_gpsStatusLabel);
    hbox->addWidget(gpsStack);

    addSep();

    // CPU (top) + Memory (bottom) stacked
    {
        auto* cpuStack = new QWidget;
        cpuStack->setMinimumWidth(kTelemetryStackMinWidth);
        auto* cpuVbox = new QVBoxLayout(cpuStack);
        cpuVbox->setContentsMargins(0, 0, 0, 0);
        cpuVbox->setSpacing(0);
        cpuVbox->setAlignment(Qt::AlignVCenter);
        m_cpuLabel = new QLabel("CPU: \u2014");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_cpuLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
        m_cpuLabel->setAlignment(Qt::AlignCenter);
        m_cpuLabel->setToolTip("AetherSDR process CPU usage");
        m_memLabel = new QLabel("Mem: \u2014");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_memLabel, "QLabel { color: {{color.text.label}}; font-size: 12px; }");
        m_memLabel->setAlignment(Qt::AlignCenter);
#if defined(Q_OS_WIN)
        m_memLabel->setToolTip("AetherSDR process working set (matches Task Manager)");
#elif defined(Q_OS_MAC)
        m_memLabel->setToolTip("AetherSDR process physical footprint (matches Activity Monitor)");
#else
        m_memLabel->setToolTip("AetherSDR process resident set (VmRSS from /proc/self/status)");
#endif
        cpuVbox->addWidget(m_cpuLabel);
        cpuVbox->addWidget(m_memLabel);
        hbox->addWidget(cpuStack);

        m_cpuTimer = new QTimer(this);
        m_cpuTimer->setInterval(1500);
        connect(m_cpuTimer, &QTimer::timeout, this, [this]() {
            double cpuPct = -1.0;
#ifdef Q_OS_WIN
            static FILETIME prevKernel{}, prevUser{};
            static qint64 prevWall = 0;
            FILETIME creation, exit, kernel, user;
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
                auto toUs = [](const FILETIME& ft) -> qint64 {
                    return (static_cast<qint64>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10;
                };
                qint64 now = QDateTime::currentMSecsSinceEpoch() * 1000;
                qint64 cpuUs = toUs(kernel) + toUs(user);
                qint64 prevCpuUs = toUs(prevKernel) + toUs(prevUser);
                if (prevWall > 0) {
                    qint64 wallDelta = now - prevWall;
                    qint64 cpuDelta = cpuUs - prevCpuUs;
                    if (wallDelta > 0)
                        cpuPct = 100.0 * cpuDelta / wallDelta / QThread::idealThreadCount();
                }
                prevKernel = kernel;
                prevUser = user;
                prevWall = now;
            }
#else
            // POSIX (Linux + macOS): getrusage
            static qint64 prevUserUs = 0, prevSysUs = 0, prevWallMs = 0;
            struct rusage ru;
            if (getrusage(RUSAGE_SELF, &ru) == 0) {
                qint64 userUs = ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
                qint64 sysUs  = ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec;
                qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
                if (prevWallMs > 0) {
                    qint64 wallDelta = (nowMs - prevWallMs) * 1000; // to microseconds
                    qint64 cpuDelta  = (userUs - prevUserUs) + (sysUs - prevSysUs);
                    if (wallDelta > 0)
                        cpuPct = 100.0 * cpuDelta / wallDelta / QThread::idealThreadCount();
                }
                prevUserUs = userUs;
                prevSysUs  = sysUs;
                prevWallMs = nowMs;
            }
#endif
            if (cpuPct >= 0.0) {
                QString color = "#8aa8c0";
                if (cpuPct >= 80.0) color = "#e05050";
                else if (cpuPct >= 50.0) color = "#f0c040";
                m_cpuLabel->setText(QString("CPU: %1%").arg(cpuPct, 0, 'f', 1));
                m_cpuLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 12px; }").arg(color));
            }

            // Memory: report the "what the OS sees right now" footprint, not the
            // per-process high-water mark. ru_maxrss is monotonic and excludes
            // compressed/IOKit/purgeable memory on macOS, so it disagrees badly
            // with Activity Monitor — see issue #3197.
            quint64 memBytes = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                memBytes = pmc.WorkingSetSize;
            }
#elif defined(Q_OS_MAC)
            task_vm_info_data_t info{};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO,
                          reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
                memBytes = info.phys_footprint;
            }
#else
            // /proc files report size 0 via stat(), so QFile::atEnd() returns true
            // immediately (pos >= size → 0 >= 0). Drive the loop off readLine()
            // returning empty instead.
            QFile statusFile("/proc/self/status");
            if (statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QByteArray line;
                while (!(line = statusFile.readLine()).isEmpty()) {
                    if (line.startsWith("VmRSS:")) {
                        memBytes = line.mid(6).trimmed().split(' ').first().toULongLong() * 1024ULL;
                        break;
                    }
                }
            }
#endif
            if (memBytes > 0) {
                double mb = memBytes / (1024.0 * 1024.0);
                m_memLabel->setText(QString("Mem: %1 MB").arg(static_cast<int>(mb)));
            }
        });
        m_cpuTimer->start();
    }

    addSep();

    // PA temp (top) + supply voltage (bottom) stacked
    auto* paStack = new QWidget;
    paStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* paVbox = new QVBoxLayout(paStack);
    paVbox->setContentsMargins(0, 0, 0, 0);
    paVbox->setSpacing(0);
    paVbox->setAlignment(Qt::AlignVCenter);
    m_paTempLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_paTempLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_paTempLabel->setAlignment(Qt::AlignCenter);
    m_paTempLabel->setCursor(Qt::PointingHandCursor);
    m_paTempLabel->installEventFilter(this);
    updatePaTempLabel();
    m_supplyVoltLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_supplyVoltLabel, "QLabel { color: {{color.text.label}}; font-size: 12px; }");
    m_supplyVoltLabel->setAlignment(Qt::AlignCenter);
    paVbox->addWidget(m_paTempLabel);
    paVbox->addWidget(m_supplyVoltLabel);
    hbox->addWidget(paStack);

    addSep();

    // Network label (top) + quality (bottom) stacked
    auto* netStack = new QWidget;
    netStack->setMinimumWidth(kTelemetryStackMinWidth);
    netStack->setCursor(Qt::PointingHandCursor);
    auto* netVbox = new QVBoxLayout(netStack);
    netVbox->setContentsMargins(0, 0, 0, 0);
    netVbox->setSpacing(0);
    netVbox->setAlignment(Qt::AlignVCenter);
    auto* netTitle = new QLabel("Network:");
    AetherSDR::ThemeManager::instance().applyStyleSheet(netTitle, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    netTitle->setAlignment(Qt::AlignCenter);
    netVbox->addWidget(netTitle);
    m_networkLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_networkLabel, "QLabel { color: {{color.text.label}}; font-size: 12px; }");
    m_networkLabel->setTextFormat(Qt::RichText);
    m_networkLabel->setAlignment(Qt::AlignCenter);
    m_networkLabel->setToolTip(buildNetworkTooltip(m_radioModel));
    m_networkLabel->installEventFilter(this);
    m_networkTooltipRefreshTimer.setInterval(1000);
    connect(&m_networkTooltipRefreshTimer, &QTimer::timeout, this, [this] {
        if (!m_networkLabel || !m_networkLabel->underMouse()) {
            m_networkTooltipRefreshTimer.stop();
            return;
        }
        const QString tooltip = buildNetworkTooltip(m_radioModel);
        m_networkLabel->setToolTip(tooltip);
        const QPoint pos = m_networkLabel->mapToGlobal(QPoint(m_networkLabel->width() / 2, 0));
        QToolTip::showText(pos, tooltip, m_networkLabel);
    });
    netVbox->addWidget(m_networkLabel);
    hbox->addWidget(netStack);

    m_tgxlSeparator = addSep();
    m_tgxlSeparator->setVisible(false);

    // TUN container — two-label stack matching the CPU/PA/Network pattern.
    // Minimum width is sized to the longest possible state string ("STANDBY")
    // so the top label never shifts when the bottom label cycles through states.
    m_tgxlContainer = new QWidget;
    m_tgxlContainer->setCursor(Qt::PointingHandCursor);
    m_tgxlContainer->setToolTip("Tuner Genius XL\nClick to cycle OPERATE / BYPASS / STANDBY");
    m_tgxlContainer->setAccessibleName("Tuner Genius XL status");
    m_tgxlContainer->setAccessibleDescription("Click to cycle between OPERATE, BYPASS, and STANDBY");
    m_tgxlContainer->installEventFilter(this);
    m_tgxlContainer->setVisible(false);
    {
        auto* vbox = new QVBoxLayout(m_tgxlContainer);
        vbox->setContentsMargins(0, 0, 0, 4);
        vbox->setSpacing(0);
        vbox->setAlignment(Qt::AlignVCenter);
        m_tgxlIndicator = new QLabel("TUN");
        m_tgxlIndicator->setStyleSheet("QLabel { font-size:18px; font-weight:bold; }");
        m_tgxlIndicator->setAlignment(Qt::AlignCenter);
        m_tgxlStateLabel = new QLabel("STANDBY");
        m_tgxlStateLabel->setStyleSheet("QLabel { font-size:11px; }");
        m_tgxlStateLabel->setAlignment(Qt::AlignCenter);
        // Fix minimum width to the widest state so "TUN" never shifts.
        // Use an explicit QFont at the correct pixel size — the stylesheet hasn't
        // been processed yet so label->font() would return the wrong metrics.
        {
            QFont f = m_tgxlStateLabel->font();
            f.setPixelSize(11);
            const int minW = QFontMetrics(f).horizontalAdvance("STANDBY") + 16;
            m_tgxlStateLabel->setMinimumWidth(minW);
            m_tgxlContainer->setMinimumWidth(minW);
        }
        vbox->addWidget(m_tgxlIndicator);
        vbox->addWidget(m_tgxlStateLabel);
    }
    hbox->addWidget(m_tgxlContainer);

    m_pgxlSeparator = addSep();
    m_pgxlSeparator->setVisible(false);

    // AMP container — same pattern; PGXL has no BYPASS so widest state is "STANDBY".
    m_pgxlContainer = new QWidget;
    m_pgxlContainer->setCursor(Qt::PointingHandCursor);
    m_pgxlContainer->setToolTip("Power Genius XL\nClick to cycle OPERATE / STANDBY");
    m_pgxlContainer->setAccessibleName("Power Genius XL status");
    m_pgxlContainer->setAccessibleDescription("Click to cycle between OPERATE and STANDBY");
    m_pgxlContainer->installEventFilter(this);
    m_pgxlContainer->setVisible(false);
    {
        auto* vbox = new QVBoxLayout(m_pgxlContainer);
        vbox->setContentsMargins(0, 0, 0, 4);
        vbox->setSpacing(0);
        vbox->setAlignment(Qt::AlignVCenter);
        m_pgxlIndicator = new QLabel("AMP");
        m_pgxlIndicator->setStyleSheet("QLabel { font-size:18px; font-weight:bold; }");
        m_pgxlIndicator->setAlignment(Qt::AlignCenter);
        m_pgxlStateLabel = new QLabel("STANDBY");
        m_pgxlStateLabel->setStyleSheet("QLabel { font-size:11px; }");
        m_pgxlStateLabel->setAlignment(Qt::AlignCenter);
        {
            QFont f = m_pgxlStateLabel->font();
            f.setPixelSize(11);
            const int minW = QFontMetrics(f).horizontalAdvance("STANDBY") + 16;
            m_pgxlStateLabel->setMinimumWidth(minW);
            m_pgxlContainer->setMinimumWidth(minW);
        }
        vbox->addWidget(m_pgxlIndicator);
        vbox->addWidget(m_pgxlStateLabel);
    }
    hbox->addWidget(m_pgxlContainer);

    addSep();

    m_txIndicator = new QLabel("TX");
    m_txIndicator->setFixedSize(36, 36);
    m_txIndicator->setAlignment(Qt::AlignCenter);
    m_txIndicator->setCursor(Qt::PointingHandCursor);
    m_txIndicator->setToolTip("Click to cancel TX");
    m_txIndicator->setAccessibleName("Cancel transmit");
    m_txIndicator->setAccessibleDescription("Click to send key up, PTT off, Tune off, and MOX off.");
    m_txIndicator->installEventFilter(this);
    m_txIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 21px; }");
    hbox->addWidget(m_txIndicator);

    addSep();

    // UTC date (top) + UTC time (bottom) stacked, right-aligned. Two-row
    // layout matches all other telemetry stacks in the status bar (#1583).
    auto* timeStack = new QWidget;
    timeStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* timeVbox = new QVBoxLayout(timeStack);
    timeVbox->setContentsMargins(0, 0, 0, 0);
    timeVbox->setSpacing(0);
    timeVbox->setAlignment(Qt::AlignVCenter);
    m_gpsDateLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_gpsDateLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_gpsDateLabel->setAlignment(Qt::AlignCenter);
    m_gpsDateLabel->setMinimumWidth(kTelemetryStackMinWidth);
    m_gpsTimeLabel = new QLabel("");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_gpsTimeLabel, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    m_gpsTimeLabel->setAlignment(Qt::AlignCenter);
    m_gpsTimeLabel->setMinimumWidth(kTelemetryStackMinWidth);
    timeVbox->addWidget(m_gpsDateLabel);
    timeVbox->addWidget(m_gpsTimeLabel);
    hbox->addWidget(timeStack);

    statusBar()->addWidget(container, 1);
    updateBandStackIndicator();

    // S History Markers expiry — sweeps stale detections once per second
    m_sHistoryExpireTimer = new QTimer(this);
    m_sHistoryExpireTimer->setInterval(1000);
    connect(m_sHistoryExpireTimer, &QTimer::timeout,
            this, &MainWindow::expireSHistoryMarkers);
    m_sHistoryExpireTimer->start();

    // CNN signal classifier — load model from next to the executable or ~/.config/AetherSDR/
    {
        const QString exeDir  = QCoreApplication::applicationDirPath();
        // GenericConfigLocation + "/AetherSDR" matches the AppSettings convention
        // and avoids the double-nested ~/.config/AetherSDR/AetherSDR/ path that
        // AppConfigLocation produces when both org and app names are "AetherSDR".
        const QString cfgDir  = QStandardPaths::writableLocation(
                                    QStandardPaths::GenericConfigLocation)
                              + QStringLiteral("/AetherSDR");
        const QString modelFile = QStringLiteral("signal_classifier.onnx");
        QString modelPath;
        if (QFile::exists(exeDir + QLatin1Char('/') + modelFile)) {
            modelPath = exeDir + QLatin1Char('/') + modelFile;
        } else if (QFile::exists(cfgDir + QLatin1Char('/') + modelFile)) {
            modelPath = cfgDir + QLatin1Char('/') + modelFile;
        }
        if (!modelPath.isEmpty()) {
            m_signalClassifier.loadModel(modelPath);
        }
    }
}

// ─── CAT port helpers ─────────────────────────────────────────────────────────

int MainWindow::catPortTargetCount() const
{
    if (!m_radioModel.isConnected()) return 1;
    return RadioModel::maxSlicesForModel(m_radioModel.model());
}

void MainWindow::applyCatPortCount()
{
    auto& s = AppSettings::instance();
    const bool masterOn = s.value("CatEnabled", "False").toString() == "True";
    const int  target   = catPortTargetCount();

    for (int i = 0; i < kCatPorts; ++i) {
        if (!m_catPorts[i]) continue;

        const QString prefix = QString("CatPort_%1_").arg(i);
        const bool portEnabled = s.value(prefix + "Enabled", "False").toString() == "True";
        const int  portNum     = s.value(prefix + "Port", "").toInt();
        const bool shouldRun   = masterOn && portEnabled && (portNum >= 1024) && (i < target);

        if (shouldRun && !m_catPorts[i]->isRunning()) {
            // Re-apply config in case dialect/VFO was changed while stopped
            QString d = s.value(prefix + "Dialect", "Rigctld").toString();
            CatDialect dial = (d == "FlexCAT") ? CatDialect::FlexCAT
                            : (d == "TS2000")  ? CatDialect::TS2000
                            : CatDialect::Rigctld;
            m_catPorts[i]->setDialect(dial);
            m_catPorts[i]->setVfoA(s.value(prefix + "VfoA", "0").toInt());
            m_catPorts[i]->setVfoB(s.value(prefix + "VfoB", "-1").toInt());
            m_catPorts[i]->start(static_cast<quint16>(portNum));
        } else if (!shouldRun && m_catPorts[i]->isRunning()) {
            m_catPorts[i]->stop();
        }
    }

    auto* applet = m_appletPanel ? m_appletPanel->catControlApplet() : nullptr;
    if (applet) {
        applet->setCatEnabled(masterOn);
        // Show hardware max when connected; fall back to kMaxPorts (all letters) when not.
        const int hwSlices = (target > 1) ? target : kCatPorts;
        applet->setMaxSlices(hwSlices);
    }
}

void MainWindow::migrateCatSettings()
{
    auto& s = AppSettings::instance();

    // Only migrate if new schema not yet written
    if (s.contains("CatPort_0_Port")) return;

    // Port 0: old rigctld settings
    QString rigPort = s.value("CatTcpPort", "4532").toString();
    bool rigEnabled = s.value("AutoStartRigctld", "False").toString() == "True";
    s.setValue("CatPort_0_Port",    rigPort);
    s.setValue("CatPort_0_Dialect", "Rigctld");
    s.setValue("CatPort_0_VfoA",    "0");
    s.setValue("CatPort_0_VfoB",    "1");
    s.setValue("CatPort_0_Enabled", rigEnabled ? "True" : "False");

    // Port 1: old SmartCAT settings
    QString catPort = s.value("SmartCatPort", "5001").toString();
    bool catEnabled = s.value("AutoStartCAT", "False").toString() == "True";
    s.setValue("CatPort_1_Port",    catPort);
    s.setValue("CatPort_1_Dialect", "FlexCAT");
    s.setValue("CatPort_1_VfoA",    "0");
    s.setValue("CatPort_1_VfoB",    "1");
    s.setValue("CatPort_1_Enabled", catEnabled ? "True" : "False");

    // Remaining ports: disabled with no port
    for (int i = 2; i < kCatPorts; ++i) {
        const QString pfx = QString("CatPort_%1_").arg(i);
        s.setValue(pfx + "Port",    "");
        s.setValue(pfx + "Dialect", "FlexCAT");
        s.setValue(pfx + "VfoA",    "0");
        s.setValue(pfx + "VfoB",    "-1");
        s.setValue(pfx + "Enabled", "False");
    }

    // Master enable: on if either old server was enabled
    s.setValue("CatEnabled", (rigEnabled || catEnabled) ? "True" : "False");

    s.save();
}

void MainWindow::adjustCatPortCounts(bool connected)
{
    Q_UNUSED(connected)
    applyCatPortCount();
}

// ─── Theme ────────────────────────────────────────────────────────────────────

void MainWindow::applyDarkTheme()
{
    applyAppTheme(this);
}

// ─── Radio/model event handlers ───────────────────────────────────────────────

void MainWindow::applyLeanMode(bool on)
{
    m_leanMode = on;

    // Panadapters: opaque single layer (no wallpaper / fill) + ~30 Hz cap.
    // Also keep every pan's Lean button in sync (the toggle is global).
    for (auto* sw : findChildren<SpectrumWidget*>()) {
        sw->setLeanMode(on);
        if (auto* menu = sw->overlayMenu())
            menu->setLeanChecked(on);
    }

    // VFO panels: opaque, cacheable layer (kills the translucent re-composite).
    for (auto* vfo : findChildren<VfoWidget*>())
        vfo->setOpaqueMode(on);

    // WAVE scope: hidden + feed dropped. Round-trip respects the user's
    // pre-Lean choice (if they had the scope hidden before enabling Lean,
    // disabling Lean must not silently re-show it).
    if (m_appletPanel) {
        if (auto* wave = m_appletPanel->waveApplet()) {
            if (on) {
                m_preLeanWaveActive = wave->isActive();
                wave->setActive(false);
            } else {
                wave->setActive(m_preLeanWaveActive);
            }
        }
    }

    // Meters: throttle their animation repaint so they stop dirtying the shared
    // backing store every frame (which forces a full-window texture re-upload to
    // recomposite with the GPU panadapter — the dominant pooled cost on large/5K
    // windows; see #3283). Native-layering the panel was tried and did not
    // isolate them under Qt 6.11/macOS, so we cap the repaint rate instead.
    MeterSmoother::setLeanThrottle(on);

    DisplaySettings::setLeanMode(on);
}

void MainWindow::onConnectionStateChanged(bool connected)
{
    if (m_shuttingDown) {
        return;
    }

    m_connPanel->setConnected(connected);

    // Pause/resume the discovery re-bind loop in step with the connection
    // lifecycle.  Without this the 5-second close()+bind() churn ran for the
    // whole session on routed/VPN ("Connect by IP") sessions, where UDP
    // broadcasts never reach the client by design (#3420).  Routed and
    // SmartLink/WAN sessions cannot receive local broadcasts at all, so flag
    // them as "remote" to fully quiesce discovery rather than just pausing the
    // re-bind churn.
    const bool remoteConnection =
        m_radioModel.isWan() || m_radioModel.lastRadioInfo().isRouted;
    m_discovery.setConnected(connected, remoteConnection);

    if (connected) {
        m_suppressStartupPanLayoutRearrange = false;
        m_layoutRestoreUntilMs = kPanLayoutRestoreWaitingForFirstPan;
        m_radioInfoLabel->setText(m_radioModel.model());
        m_radioVersionLabel->setText(m_radioModel.version());
        m_stationLabel->setText(m_radioModel.nickname());
        m_connStatusLabel->setText("Connected");
        m_connPanel->setStatusText("Connected");

        // Slice tab toggle is initialized from infoChanged when radio
        // reports its actual slice capacity (#1278).

        // Show DIV button on dual-SCU radios
        {
            const QString& model = m_radioModel.model();
            bool divAllowed = model.contains("6500") || model.contains("6600")
                           || model.contains("6700") || model.contains("8600")
                           || model.contains("AU-520");
            // Set diversity allowed on all existing VFO widgets (including Slice A at startup) (#1503)
            if (m_panStack) {
                for (auto* pan : m_radioModel.panadapters()) {
                    if (auto* sw = m_panStack->spectrum(pan->panId())) {
                        for (auto* slice : m_radioModel.slices()) {
                            if (auto* vfo = sw->vfoWidget(slice->sliceId())) {
                                vfo->setDiversityAllowed(divAllowed);
                            }
                        }
                    }
                }
            }
        }
        // Only start the local RX audio sink if the user wants audio routed
        // to the PC. When PC Audio is off we may still request a
        // remote_audio_rx stream for TCI clients; the sink should stay
        // silent in that case. The PC Audio toggle handler starts/stops
        // the sink when the user flips it.
        // Always sync the button to the setting here so any divergence
        // (e.g. from a profile load before connect) is corrected (#1536).
        {
            const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
            m_titleBar->setPcAudioEnabled(pcAudio);
            if (pcAudio)
                audioStartRx();
        }
        updateNr2Availability();  // Disable NR2 if connected via SmartLink/Opus (#1597)
        // TX audio stream will start when the radio assigns a stream ID
        // Auto-hide the connection dialog on successful connect
        m_connPanel->hide();

        // Close reconnect dialog if it was showing
        if (m_reconnectDlg) {
            QDialog* reconnectDialog = m_reconnectDlg;
            m_reconnectDlg = nullptr;
            reconnectDialog->close();
            reconnectDialog->deleteLater();
        }

        // Load band stack bookmarks for this radio
        BandStackSettings::instance().load();
        // Prune expired entries on startup
        int expiryMin = BandStackSettings::instance().autoExpiryMinutes();
        if (expiryMin > 0) {
            qint64 maxAge = static_cast<qint64>(expiryMin) * 60 * 1000;
            if (BandStackSettings::instance().removeExpiredEntries(
                    m_radioModel.serial(), maxAge) > 0) {
                BandStackSettings::instance().save();
            }
        }
        BandStackPanel* bandStackPanel =
            m_panStack ? m_panStack->bandStackPanel() : nullptr;
        if (bandStackPanel) {
            // Apply saved UI preferences to the panel
            bandStackPanel->setGrouped(BandStackSettings::instance().groupByBand());
            bandStackPanel->setAutoExpiryMinutes(expiryMin);
            bandStackPanel->setAutoSaveDwellSeconds(
                BandStackSettings::instance().autoSaveDwellSeconds());
        }

        // 5-second grace window after connect — suppresses an auto-save fire
        // while the radio finishes pushing initial slice/pan state.
        m_bsConnectGraceUntilMs = QDateTime::currentMSecsSinceEpoch() + 5000;
        if (bandStackPanel) {
            bandStackPanel->loadBookmarks(m_radioModel.serial(), m_bandPlanMgr);
        }
        refreshMemoryBrowsePanel();
        updateBandStackIndicator();

        // Start the auto-expiry timer now that we have a radio to prune for
        if (m_bsExpiryTimer && !m_bsExpiryTimer->isActive())
            m_bsExpiryTimer->start();

        // Apply CAT port counts for the newly connected radio.
        // applyCatPortCount() starts/stops ports up to maxSlicesForModel().
        applyCatPortCount();
#ifdef HAVE_WEBSOCKETS
        // Auto-start TCI WebSocket server if enabled
        if (AppSettings::instance().value("AutoStartTCI", "False").toString() == "True") {
            if (m_tciServer && !m_tciServer->isRunning()) {
                int tciPort = AppSettings::instance().value("TciPort", "50001").toInt();
                m_tciServer->start(static_cast<quint16>(tciPort));
                qDebug() << "AutoStart: TCI on port" << tciPort
                         << " running=" << m_tciServer->isRunning();
            }
            // Only light up the Enable button if the server actually bound —
            // otherwise the UI shows a green "Enable" while the port is in use.
            if (m_appletPanel && m_appletPanel->tciApplet())
                m_appletPanel->tciApplet()->setTciEnabled(
                    m_tciServer && m_tciServer->isRunning());
        }
#endif
        // Populate XVTR bands after radio status settles, and refresh
        // whenever XVTR config changes (add/remove/rename). (#571)  Also
        // pushes the radio's built-in transverter capabilities so the
        // band menu surfaces 4m/2m on FLEX-6500 / FLEX-6700 (#695).
        auto refreshXvtr = [this]() {
            if (m_shuttingDown || !m_panStack || !m_radioModel.isConnected()) {
                return;
            }
            QVector<SpectrumOverlayMenu::XvtrBand> xvtrBands;
            for (const auto& x : m_radioModel.xvtrList()) {
                if (x.isValid)
                    xvtrBands.append({x.name, x.rfFreq, QString("X%1").arg(x.index)});
            }
            const ModelCapabilities caps = m_radioModel.capabilities();
            for (auto* applet : m_panStack->allApplets()) {
                auto* menu = applet->spectrumWidget()->overlayMenu();
                menu->setRadioCapabilities(caps);
                menu->setXvtrBands(xvtrBands);
            }
        };
        QTimer::singleShot(2000, this, refreshXvtr);
        connect(&m_radioModel, &RadioModel::infoChanged, this, refreshXvtr);

        // Apply saved display settings after panadapter is created
        m_displaySettingsPushed = false;

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
        // Delay DAX bridge start until RadioModel's SmartConnect sequence
        // is fully complete (streams created, UDP bound, slices discovered).
        // Auto-start DAX bridge if enabled in settings.
        // Starting too early causes our mic_selection=PC and dax=1 to be
        // overridden by RadioModel's own setup, and DAX stream IDs won't
        // be registered in PanadapterStream yet.
        if (AppSettings::instance().value("AutoStartDAX", "False").toString() == "True") {
            QTimer::singleShot(3000, this, [this]() {
                if (startDax() && m_appletPanel && m_appletPanel->daxApplet())
                    m_appletPanel->daxApplet()->setDaxEnabled(true);
            });
        }
#endif
        // Auto-connect DX cluster if enabled
        {
            auto& cs = AppSettings::instance();
            if (cs.value("DxClusterAutoConnect", "False").toString() == "True") {
                QString host = cs.value("DxClusterHost", "dxc.nc7j.com").toString();
                quint16 cPort = static_cast<quint16>(cs.value("DxClusterPort", 7300).toInt());
                QString call = cs.value("DxClusterCallsign").toString();
                if (!call.isEmpty() && !m_dxCluster->isConnected())
                    QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->connectToCluster(host, cPort, call); });
            }
            // Auto-connect RBN if enabled
            if (cs.value("RbnAutoConnect", "False").toString() == "True") {
                QString host = cs.value("RbnHost", "telnet.reversebeacon.net").toString();
                quint16 rPort = static_cast<quint16>(cs.value("RbnPort", 7000).toInt());
                QString call = cs.value("RbnCallsign").toString();
                if (call.isEmpty())
                    call = cs.value("DxClusterCallsign").toString();
                if (!call.isEmpty() && !m_rbnClient->isConnected())
                    QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->connectToCluster(host, rPort, call); });
            }
            // Auto-start WSJT-X listener if enabled
            if (cs.value("WsjtxAutoStart", "False").toString() == "True") {
                QString wAddr = cs.value("WsjtxAddress", "224.0.0.1").toString();
                quint16 wPort = static_cast<quint16>(cs.value("WsjtxPort", 2237).toInt());
                if (!m_wsjtxClient->isListening())
                    QMetaObject::invokeMethod(m_wsjtxClient, [=] { m_wsjtxClient->startListening(wAddr, wPort); });
            }
            // Auto-start SpotCollector listener if enabled
            if (cs.value("SpotCollectorAutoStart", "False").toString() == "True") {
                quint16 scPort = static_cast<quint16>(cs.value("SpotCollectorPort", 9999).toInt());
                if (!m_spotCollectorClient->isListening())
                    QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->startListening(scPort); });
            }
            // Auto-start POTA polling if enabled
            if (cs.value("PotaAutoStart", "False").toString() == "True") {
                int pInterval = cs.value("PotaPollInterval", 60).toInt();
                if (!m_potaClient->isPolling())
                    QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->startPolling(pInterval); });
            }
#ifdef HAVE_WEBSOCKETS
            // Auto-start FreeDV Reporter if enabled
            if (cs.value("FreeDvAutoStart", "False").toString() == "True") {
                if (!m_freedvClient->isConnected())
                    QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->startConnection(); });
            }
#endif
            // Propagate auto-reconnect setting to all peripheral connections
            const bool autoReconnect = PeripheralSettings::autoReconnect();
            m_tgxlConn.setAutoReconnect(autoReconnect);
            m_pgxlConn.setAutoReconnect(autoReconnect);
            m_antennaGenius.setAutoReconnect(autoReconnect);

            // Auto-connect peripherals with manual IPs (#914)
            QString tgxlIp = cs.value("TGXL_ManualIp", "").toString();
            if (!tgxlIp.isEmpty() && !m_tgxlConn.isConnected()) {
                quint16 tgxlPort = static_cast<quint16>(cs.value("TGXL_ManualPort", "9010").toInt());
                m_tgxlConn.connectToTgxl(tgxlIp, tgxlPort);
            }
            QString pgxlIp = cs.value("PGXL_ManualIp", "").toString();
            if (!pgxlIp.isEmpty() && !m_pgxlConn.isConnected()) {
                quint16 pgxlPort = static_cast<quint16>(cs.value("PGXL_ManualPort", "9008").toInt());
                m_pgxlConn.connectToPgxl(pgxlIp, pgxlPort);
            }
            // If SS_ManualIp is set, connect to ShackSwitch immediately using a
            // synthetic serial so device-type detection works from the start.
            // This bypasses the UDP discovery race condition entirely.
            QString ssIp = cs.value("SS_ManualIp", "").toString();
            if (!ssIp.isEmpty() && !m_antennaGenius.isConnected()) {
                AgDeviceInfo ssInfo;
                ssInfo.ip         = QHostAddress(ssIp);
                ssInfo.port       = 9007;
                ssInfo.serial     = QStringLiteral("ShackSwitch-manual");
                ssInfo.name       = QStringLiteral("ShackSwitch");
                ssInfo.radioPorts = 1;  // ShackSwitch is always single-radio
                m_antennaGenius.connectToDevice(ssInfo);
            }

            // Delay AG manual connect by 7s so UDP discovery can run first.
            // If ShackSwitch already connected above, isConnected() = true → skips.
            // A real AG (no UDP broadcast, no SS_ManualIp) still connects after delay.
            QString agIp = cs.value("AG_ManualIp", "").toString();
            if (!agIp.isEmpty()) {
                quint16 agPort = static_cast<quint16>(cs.value("AG_ManualPort", "9007").toInt());
                if (m_agManualConnectTimer) {
                    m_agManualConnectTimer->stop();
                    m_agManualConnectTimer->deleteLater();
                    m_agManualConnectTimer = nullptr;
                }
                m_agManualConnectTimer = new QTimer(this);
                m_agManualConnectTimer->setSingleShot(true);
                connect(m_agManualConnectTimer, &QTimer::timeout, this, [this, agIp, agPort]() {
                    m_agManualConnectTimer = nullptr;
                    if (!m_antennaGenius.isConnected())
                        m_antennaGenius.connectToAddress(QHostAddress(agIp), agPort);
                });
                m_agManualConnectTimer->start(7000);
            }
        }
#ifdef HAVE_HIDAPI
        updateTMate2Status();
#endif
    } else {
        // Radio disconnected: trim CAT ports back to 1 so apps on channel A
        // stay connected through brief reconnects, higher channels stop cleanly.
        applyCatPortCount();  // catPortTargetCount() returns 1 when !connected

        if (m_layoutRestoreTimer) {
            m_layoutRestoreTimer->stop();
        }
        m_suppressStartupPanLayoutRearrange = false;
        m_layoutRestoreUntilMs = 0;
        if (m_appletPanel) {
            m_appletPanel->clearSliceButtons();
        }

        const bool reconnectWan = !m_userDisconnected && m_radioModel.isWan()
            && !m_pendingWanRadio.serial.isEmpty();
        if (reconnectWan && !m_wanReconnectTimer.isActive()) {
            m_wanReconnectAttemptInProgress = false;
            m_wanReconnectTimer.start();
        }

        if (m_agManualConnectTimer) {
            m_agManualConnectTimer->stop();
            m_agManualConnectTimer->deleteLater();
            m_agManualConnectTimer = nullptr;
        }
        if (m_swrSweep.running)
            finishSwrSweep(true, QStringLiteral("SWR sweep stopped on disconnect"));
        QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->disconnect(); });
        QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->disconnect(); });
        QMetaObject::invokeMethod(m_wsjtxClient, [=] { m_wsjtxClient->stopListening(); });
        QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->stopListening(); });
        QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->stopPolling(); });
#ifdef HAVE_WEBSOCKETS
        QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->stopConnection(); });
#endif
        m_connStatusLabel->setText("Disconnected");
        m_radioInfoLabel->setText("");
        m_radioVersionLabel->setText("");
        m_stationLabel->setText("N0CALL");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_tnfIndicator, "QLabel { color: {{color.background.2}}; font-weight: bold; font-size: 24px; }");
        m_tnfIndicator->setToolTip(buildTnfTooltip(m_radioModel.tnfModel()));
        if (auto* bandStackPanel = m_panStack ? m_panStack->bandStackPanel() : nullptr) {
            bandStackPanel->clear();
        }
        if (m_bsExpiryTimer && m_bsExpiryTimer->isActive())
            m_bsExpiryTimer->stop();
        if (m_bsAutoSaveTimer && m_bsAutoSaveTimer->isActive())
            m_bsAutoSaveTimer->stop();
        if (m_panStack) {
            m_panStack->setBandStackVisible(false);
        }
        refreshMemoryBrowsePanel();
        updateBandStackIndicator();
        m_tgxlContainer->setVisible(false);
        m_tgxlSeparator->setVisible(false);
        m_tgxlConn.disconnect();
        m_pgxlConn.disconnect();
        m_pgxlContainer->setVisible(false);
        m_pgxlSeparator->setVisible(false);
        m_txIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 21px; }");
        m_txIndicator->setText("TX");
        m_connPanel->setStatusText("Not connected");
#ifdef HAVE_HIDAPI
        // Safety: if latched PTT was active when the radio dropped, the radio is
        // now TX-off regardless.  Reset the latch flag so it stays in sync with
        // the radio's actual state on reconnect. (#3323)
        if (m_rc28PttLatched) {
            m_rc28PttLatched = false;
        }
        updateTMate2Status();
#endif
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
        stopDax();
#endif
        audioStopRx();
        audioStopTx();

        for (auto it = m_panFpsReconcileConnections.begin();
             it != m_panFpsReconcileConnections.end(); ++it) {
            QObject::disconnect(it.value());
        }
        m_panFpsReconcileConnections.clear();
        for (auto it = m_wfLineDurationReconcileConnections.begin();
             it != m_wfLineDurationReconcileConnections.end(); ++it) {
            QObject::disconnect(it.value());
        }
        m_wfLineDurationReconcileConnections.clear();
        for (auto it = m_panFpsReconcile.begin();
             it != m_panFpsReconcile.end(); ++it) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
        }
        m_panFpsReconcile.clear();
        for (auto it = m_wfLineDurationReconcile.begin();
             it != m_wfLineDurationReconcile.end(); ++it) {
            if (it->timer) {
                it->timer->stop();
                it->timer->deleteLater();
            }
        }
        m_wfLineDurationReconcile.clear();
        m_adaptiveThrottleActive = false;
        m_adaptiveFpsCap = 0;  // clear cap alongside throttle flag — see #2829 review

        // Clear spectrum/waterfall so the display doesn't look frozen
        if (m_panStack) {
            for (auto* applet : m_panStack->allApplets()) {
                if (applet && applet->spectrumWidget()) {
                    applet->spectrumWidget()->clearDisplay();
                }
            }
        }

        setPanadapterConnectionAnimation(!m_userDisconnected, "Reconnecting to radio…");

        // Show reconnect dialog on unexpected disconnect (only one at a time)
        if (!m_userDisconnected && !m_reconnectDlg) {
            const bool frameless = framelessWindowEnabled();
            m_reconnectDlg = new QDialog(this);
            m_reconnectDlg->setWindowTitle(tr("Radio Disconnected"));
            m_reconnectDlg->setWindowFlag(Qt::FramelessWindowHint, frameless);
            m_reconnectDlg->setModal(false);
            m_reconnectDlg->setFixedSize(400, 150);
            AetherSDR::ThemeManager::instance().applyStyleSheet(m_reconnectDlg, "QDialog { background: {{color.background.0}}; border: 1px solid {{color.background.1}}; }"
                "QLabel { color: {{color.text.primary}}; background: transparent; }"
                "QLabel#reconnectTitle { color: {{color.text.primary}}; font-size: 16px; font-weight: bold; }"
                "QLabel#reconnectBody { color: {{color.text.secondary}}; font-size: 12px; }"
                "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
                "border-radius: 3px; color: {{color.text.primary}}; font-size: 12px; "
                "font-weight: bold; padding: 6px 20px; }"
                "QPushButton:hover { background: {{color.background.1}}; }");

            auto* root = new QVBoxLayout(m_reconnectDlg);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(0);

            auto* titleBar = new FramelessWindowTitleBar(tr("Radio Disconnected"), m_reconnectDlg);
            titleBar->setObjectName(QStringLiteral("framelessWindowTitleBar"));
            titleBar->setVisible(frameless);
            root->addWidget(titleBar);

            auto* content = new QWidget(m_reconnectDlg);
            auto* layout = new QVBoxLayout(content);
            layout->setObjectName(QStringLiteral("reconnectDialogBodyLayout"));
            layout->setContentsMargins(18, frameless ? 14 : 16, 18, 16);
            layout->setSpacing(8);
            layout->setAlignment(Qt::AlignCenter);

            auto* title = new QLabel(tr("Connection to the radio was lost"), content);
            title->setObjectName(QStringLiteral("reconnectTitle"));
            title->setAlignment(Qt::AlignCenter);
            layout->addWidget(title);

            auto* body = new QLabel(tr("AetherSDR is attempting to reconnect automatically."), content);
            body->setObjectName(QStringLiteral("reconnectBody"));
            body->setAlignment(Qt::AlignCenter);
            body->setWordWrap(true);
            layout->addWidget(body);

            auto* dismissBtn = new QPushButton(tr("Disconnect"), content);
            dismissBtn->setFixedWidth(120);
            dismissBtn->setCursor(Qt::PointingHandCursor);
            layout->addWidget(dismissBtn, 0, Qt::AlignCenter);
            root->addWidget(content, 1);
            QDialog* reconnectDialog = m_reconnectDlg;
            connect(reconnectDialog, &QDialog::finished, this, [this, reconnectDialog](int) {
                if (m_reconnectDlg == reconnectDialog) {
                    m_reconnectDlg = nullptr;
                }
                reconnectDialog->deleteLater();
            });
            connect(dismissBtn, &QPushButton::clicked, this, [this]() {
                m_userDisconnected = true;
                m_wanReconnectTimer.stop();
                m_wanReconnectAttemptInProgress = false;
                setPanadapterConnectionAnimation(false);
                if (m_reconnectDlg) {
                    m_reconnectDlg->close();
                }
                auto& s = AppSettings::instance();
                s.remove("LastConnectedRadioSerial");
                s.remove("LastRoutedRadioIp");
                s.save();
                m_connPanel->show();
            });
            m_reconnectDlg->show();
        }
    }
}

void MainWindow::onConnectionError(const QString& msg)
{
    m_connPanel->setStatusText("Error: " + msg);
    m_connStatusLabel->setText("Error");
    statusBar()->showMessage("Connection error: " + msg, 5000);
    if (!m_reconnectDlg)
        setPanadapterConnectionAnimation(false);
}

void MainWindow::onWanCertFingerprintMismatch(const QString& host,
                                              const QString& expectedHex,
                                              const QString& presentedHex)
{
    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951). The WAN handshake is
    // paused — no wan validate has been sent. Block here on a modal
    // for the operator's decision, then route accept/reject back
    // through RadioModel to the underlying WanConnection.
    //
    // Display formatting: insert a colon every two hex chars so the
    // 64-char SHA-256 is scannable. The radio nickname isn't reliably
    // available at this point in the handshake, so we use the host
    // string (IP or hostname) as the user-facing identifier.
    auto pretty = [](QString hex) {
        hex = hex.toLower();
        QString out;
        out.reserve(hex.size() + hex.size() / 2);
        for (int i = 0; i < hex.size(); ++i) {
            if (i > 0 && (i % 2 == 0)) out.append(':');
            out.append(hex.at(i));
        }
        return out;
    };

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("SmartLink certificate changed"));
    box.setText(tr("<b>Certificate changed for %1</b>").arg(host.toHtmlEscaped()));
    box.setInformativeText(tr(
        "<p><b>Expected (pinned):</b><br/><tt>%1</tt></p>"
        "<p><b>Presented:</b><br/><tt>%2</tt></p>"
        "<p>This may be normal (radio firmware update, replaced radio) "
        "or it may indicate a man-in-the-middle attack on the SmartLink "
        "session.</p>"
        "<p>Accept only if you recently updated firmware, replaced the "
        "radio, or otherwise expect the certificate to change.</p>")
        .arg(pretty(expectedHex), pretty(presentedHex)));
    auto* acceptBtn = box.addButton(tr("Accept new certificate"),
                                    QMessageBox::AcceptRole);
    auto* rejectBtn = box.addButton(tr("Reject and disconnect"),
                                    QMessageBox::RejectRole);
    box.setDefaultButton(rejectBtn);
    box.exec();

    if (box.clickedButton() == acceptBtn) {
        statusBar()->showMessage(
            tr("SmartLink certificate updated for %1").arg(host), 4000);
        m_radioModel.acceptPresentedWanCert();
        // If the Radio Setup dialog is open with the SmartLink tab
        // showing, the just-updated pin needs to surface in the table
        // without requiring the operator to close+re-open the tab.
        if (m_radioSetupDialog)
            m_radioSetupDialog->refreshPinnedCertsTable();
    } else {
        statusBar()->showMessage(
            tr("SmartLink certificate rejected for %1").arg(host), 5000);
        m_radioModel.rejectPresentedWanCert();
    }
}

void MainWindow::onRadioMessage(const QString& text, MessageSeverity severity)
{
    // Info messages (e.g. "Client connected from IP …") are routine multi-
    // client notices that the radio sends on every connect / disconnect —
    // they're documented as silent-log in SmartSDR.  Show a status-bar
    // toast instead of a modal popup so the operator notices without an
    // interruptive dialog.  Interlock M-messages are log-only here because
    // the interlock status path already surfaces actionable TX blocks over
    // the panadapter/waterfall.  Other warnings, errors, and fatals are
    // user-actionable and continue to surface as modal QMessageBox to preserve
    // PR #2771's intent for FreeDV/ATU conflicts.  See #2785 for context.
    const bool interlockMessage = text.contains(QStringLiteral("interlock"),
                                                Qt::CaseInsensitive);
    switch (severity) {
    case MessageSeverity::Info:
        qCInfo(lcGui) << "Radio M-message [Info]:" << text;
        if (statusBar())
            statusBar()->showMessage(text, 5000);
        break;
    case MessageSeverity::Warning:
        qCWarning(lcGui) << "Radio M-message [Warning]:" << text;
        if (interlockMessage)
            break;
        QMessageBox::warning(this, tr("Radio"), text);
        break;
    case MessageSeverity::Error:
        qCCritical(lcGui) << "Radio M-message [Error]:" << text;
        if (interlockMessage)
            break;
        QMessageBox::critical(this, tr("Radio — Error"), text);
        break;
    case MessageSeverity::Fatal:
        qCCritical(lcGui) << "Radio M-message [Fatal]:" << text;
        if (interlockMessage)
            break;
        QMessageBox::critical(this, tr("Radio — Fatal"), text);
        break;
    }
}

void MainWindow::setPanadapterConnectionAnimation(bool visible, const QString& label)
{
    const QString nextLabel = label.trimmed().isEmpty()
        ? QStringLiteral("Connecting to radio…")
        : label.trimmed();
    m_panadapterConnectionAnimationVisible = visible;
    m_waitingForFirstPanadapterFrame = visible;
    m_panadapterConnectionAnimationLabel = visible ? nextLabel : QString();

    if (!m_panStack)
        return;

    for (auto* applet : m_panStack->allApplets()) {
        if (!applet)
            continue;
        if (auto* spectrumWidget = applet->spectrumWidget())
            spectrumWidget->setConnectionAnimationVisible(visible, nextLabel);
    }
}

void MainWindow::finishPanadapterConnectionAnimation()
{
    if (!m_waitingForFirstPanadapterFrame || !m_panadapterConnectionAnimationVisible)
        return;

    if (!m_radioModel.isConnected()) {
        return;
    }

    setPanadapterConnectionAnimation(false);
}

void MainWindow::syncMemorySpot(int memoryIndex)
{
    auto it = m_radioModel.memories().constFind(memoryIndex);
    if (it == m_radioModel.memories().constEnd()) {
        removeMemorySpot(memoryIndex);
        return;
    }

    const MemoryEntry& memory = it.value();
    if (memory.freq <= 0.0) {
        removeMemorySpot(memoryIndex);
        return;
    }

    QMap<QString, QString> kvs;
    kvs["callsign"] = memorySpotLabel(memory).replace(' ', QChar(0x7f));
    kvs["rx_freq"] = QString::number(memory.freq, 'f', 6);
    kvs["tx_freq"] = QString::number(memory.freq, 'f', 6);
    kvs["source"] = "Memory";
    kvs["mode"] = memory.mode;
    kvs["color"] = "#FFFFC857";
    const QString comment = memorySpotComment(memory);
    if (!comment.isEmpty())
        kvs["comment"] = QString(comment).replace(' ', QChar(0x7f));

    m_radioModel.spotModel().applySpotStatus(memorySpotId(memoryIndex), kvs);
}

void MainWindow::removeMemorySpot(int memoryIndex)
{
    m_radioModel.spotModel().removeSpot(memorySpotId(memoryIndex));
}

void MainWindow::clearMemorySpotFeed()
{
    QVector<int> ids;
    const auto& spots = m_radioModel.spotModel().spots();
    for (auto it = spots.cbegin(); it != spots.cend(); ++it) {
        if (it.value().source == "Memory")
            ids.append(it.key());
    }
    // Block signals during batch removal to avoid N marker rebuilds (#708)
    m_radioModel.spotModel().blockSignals(true);
    for (int id : ids)
        m_radioModel.spotModel().removeSpot(id);
    m_radioModel.spotModel().blockSignals(false);
    if (!ids.isEmpty())
        emit m_radioModel.spotModel().spotsRefreshed();
}

void MainWindow::rebuildMemorySpotFeed()
{
    for (auto it = m_radioModel.memories().cbegin(); it != m_radioModel.memories().cend(); ++it)
        syncMemorySpot(it.key());
}

void MainWindow::refreshMemoryBrowsePanel()
{
    if (!m_panStack)
        return;
    for (auto* applet : m_panStack->allApplets()) {
        if (!applet)
            continue;
        if (auto* menu = applet->spectrumWidget()->overlayMenu()) {
            menu->setMemories(m_radioModel.memories());
        }
    }
}

void MainWindow::updateBandStackIndicator()
{
    if (!m_bandStackIndicator || !m_panStack)
        return;

    const bool visible = m_panStack->bandStackPanel()->isVisible();
    m_bandStackIndicator->setPixmap(buildBandStackIndicatorPixmap(visible));
    m_bandStackIndicator->setToolTip(visible ? "Close band stack panel"
                                             : "Open band stack panel");
}

bool MainWindow::activateMemorySpot(int memoryIndex, const QString& preferredPanId)
{
    auto* slice = preferredMemorySlice(preferredPanId);
    if (!slice) {
        statusBar()->showMessage(
            preferredPanId.isEmpty()
                ? "Open a slice before recalling a memory."
                : "Open a slice on this pan before recalling a memory.",
            3000);
        return false;
    }
    if (slice->isLocked()) {
        slice->notifyTuneBlockedByLock();
        statusBar()->showMessage("Unlock the target slice before recalling a memory.", 3000);
        return false;
    }

    const auto it = m_radioModel.memories().constFind(memoryIndex);
    if (it == m_radioModel.memories().constEnd())
        return false;

    const int sliceId = slice->sliceId();
    if (!activeSlice() || activeSlice()->sliceId() != sliceId)
        setActiveSlice(sliceId);

    slice = m_radioModel.slice(sliceId);
    if (!slice)
        return false;

    const double memoryFreqMhz = it->freq;
    const bool hasMemoryFrequency = memoryFreqMhz > 0.0;
    const QString slicePanId = slice->panId();

    if (hasMemoryFrequency) {
        const QString memoryBand = BandSettings::bandForFrequency(memoryFreqMhz);
        const QString currentBand = BandSettings::bandForFrequency(slice->frequency());
        if (memoryBand != currentBand) {
            const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
            const auto stackKeyResult =
                XvtrPolicy::resolveBandStackKey(memoryBand, xvtrs, m_radioModel.capabilities());
            if (stackKeyResult.isSupported()) {
                qCDebug(lcProtocol).noquote().nospace()
                    << "MainWindow: memory recall preselecting band stack memory="
                    << memoryIndex
                    << " pan=" << slicePanId
                    << " from_band=" << currentBand
                    << " to_band=" << memoryBand
                    << " key=" << stackKeyResult.key;
                clearSwrSweepForBandChange(-1, slicePanId, memoryBand);
                m_bandSettings.setCurrentBand(memoryBand);
                m_radioModel.sendCommand(
                    QString("display pan set %1 band=%2").arg(slicePanId, stackKeyResult.key));
                QTimer::singleShot(300, this, [this, slicePanId]() {
                    reassertUnmutedSliceAudioForPan(slicePanId);
                });
            } else {
                qCWarning(lcProtocol).noquote().nospace()
                    << "MainWindow: memory recall cannot preselect band stack memory="
                    << memoryIndex
                    << " band=" << memoryBand
                    << " reason=" << stackKeyResult.unsupportedReason
                    << " available_xvtrs=" << xvtrListSummary(xvtrs);
            }
        }
    }

    m_pendingMemoryRevealSliceId = sliceId;
    m_pendingMemoryRevealTargetMhz = hasMemoryFrequency ? memoryFreqMhz : 0.0;
    m_radioModel.sendCommand(QString("memory apply %1").arg(memoryIndex));
    const QString retuneCommand =
        AetherSDR::buildMemoryRecallRetuneCommand(sliceId, it.value());
    if (!retuneCommand.isEmpty())
        m_radioModel.sendCommand(retuneCommand);
    QTimer::singleShot(750, this, [this, sliceId]() {
        if (m_pendingMemoryRevealSliceId != sliceId)
            return;
        const double targetMhz = m_pendingMemoryRevealTargetMhz;
        m_pendingMemoryRevealSliceId = -1;
        m_pendingMemoryRevealTargetMhz = 0.0;
        if (auto* pendingSlice = m_radioModel.slice(sliceId)) {
            const double revealMhz = targetMhz > 0.0 ? targetMhz : pendingSlice->frequency();
            const TuneCenteringResult result =
                revealFrequencyIfNeeded(pendingSlice, revealMhz,
                                        TuneIntent::CommandedTargetCenter,
                                        "memory-apply-timeout");
            logTunePolicyDecision("memory-apply-timeout", TuneIntent::CommandedTargetCenter,
                                  pendingSlice->frequency(), revealMhz,
                                  result);
        }
    });

    // The radio should push the rest of the applied memory state, but keep the
    // local tuning step in sync immediately so wheel/click snap follows along.
    if (it->step > 0)
        m_radioModel.sendCommand(QString("slice set %1 step=%2")
            .arg(sliceId).arg(it->step));
    const QString repeaterFixup =
        AetherSDR::buildMemoryRecallSliceFixupCommand(sliceId, it.value());
    if (!repeaterFixup.isEmpty())
        m_radioModel.sendCommand(repeaterFixup);
    return true;
}

void MainWindow::applyMasterVolume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    if (pcAudio)
        m_audio->setRxVolume(pct / 100.0f);
    else
        m_radioModel.setLineoutGain(pct);
    auto& s = AppSettings::instance();
    s.setValue("MasterVolume", QString::number(pct));
    s.save();
#ifdef HAVE_WEBSOCKETS
    if (m_tciServer) m_tciServer->broadcastMasterVolume(pct);
#endif
}

// onSliceAdded() / onSliceRemoved() lives in MainWindow_Wiring.cpp (#3351 Phase 1d).
SliceModel* MainWindow::activeSlice() const
{
    if (m_activeSliceId < 0) return nullptr;
    return m_radioModel.slice(m_activeSliceId);
}

void MainWindow::pushRxFilterCutoffsToEq()
{
    int audioLow = 0;
    int audioHigh = 0;
    if (auto* s = activeSlice()) {
        const int lo = s->filterLow();
        const int hi = s->filterHigh();
        const int absLo = std::abs(lo);
        const int absHi = std::abs(hi);
        // Same sign (or one zero): one-sided passband — audio range
        // is [min(|lo|, |hi|), max(|lo|, |hi|)].  This covers SSB
        // (one of lo/hi is 0) and CW (lo/hi both same sign around pitch).
        // Opposite signs: symmetric around carrier (AM/FM/SAM) — audio
        // baseband starts at 0 and runs to max(|lo|, |hi|).
        const bool sameSign = (lo >= 0 && hi >= 0) || (lo <= 0 && hi <= 0);
        audioLow  = sameSign ? std::min(absLo, absHi) : 0;
        audioHigh = std::max(absLo, absHi);
    }
    if (m_appletPanel && m_appletPanel->clientEqRxApplet())
        m_appletPanel->clientEqRxApplet()->setRxFilterCutoffs(audioLow, audioHigh);
    if (m_clientEqEditor)
        m_clientEqEditor->setRxFilterCutoffs(audioLow, audioHigh);
}

const char* MainWindow::tuneIntentName(TuneIntent intent)
{
    switch (intent) {
    case TuneIntent::IncrementalTune: return "IncrementalTune";
    case TuneIntent::AbsoluteJump:    return "AbsoluteJump";
    case TuneIntent::CommandedTargetCenter: return "CommandedTargetCenter";
    case TuneIntent::ExplicitPan:     return "ExplicitPan";
    case TuneIntent::RevealOffscreen: return "RevealOffscreen";
    }
    return "Unknown";
}

bool MainWindow::panFollowEnabled() const
{
    return AppSettings::instance().value("PanFollowVfo", "True").toString() == "True";
}

void MainWindow::logTunePolicyDecision(const char* source, TuneIntent intent,
                                       double oldFreqMhz, double newFreqMhz,
                                       const TuneCenteringResult& result) const
{
    qDebug().nospace()
        << "TunePolicy:"
        << " source=" << source
        << " intent=" << tuneIntentName(intent)
        << " oldFreq=" << oldFreqMhz
        << " newFreq=" << newFreqMhz
        << " oldCenter=" << result.oldCenterMhz
        << " newCenter=" << result.newCenterMhz
        << " bandwidth=" << result.bandwidthMhz
        << " followRevealTriggered=" << result.followRevealTriggered
        << " hardCenterUsed=" << result.hardCenterUsed
        << " animationMs=" << result.animationDurationMs;
}

void MainWindow::mirrorDiversityChildFrequency(SliceModel* slice, double mhz)
{
    if (!slice || !slice->isDiversityParent())
        return;

    long long hz = static_cast<long long>(std::round(mhz * 1e6));
    const QString freqStr = QString("%1.%2.%3")
        .arg(static_cast<int>(hz / 1000000))
        .arg(static_cast<int>((hz / 1000) % 1000), 3, 10, QChar('0'))
        .arg(static_cast<int>(hz % 1000), 3, 10, QChar('0'));

    for (auto* other : m_radioModel.slices()) {
        if (!other->isDiversityChild() || other->sliceId() == slice->sliceId())
            continue;
        auto* sw = spectrumForSlice(other);
        if (!sw)
            continue;
        if (auto* vfo = sw->vfoWidget(other->sliceId()))
            vfo->freqLabel()->setText(freqStr);
        sw->setSliceOverlay(other->sliceId(), mhz,
            other->filterLow(), other->filterHigh(),
            other->isTxSlice(), false,
            other->mode(), other->rttyMark(), other->rttyShift(),
            other->ritOn(), other->ritFreq(),
            other->xitOn(), other->xitFreq());
    }
}

MainWindow::BandStackPreselectResult MainWindow::preselectBandStackForTune(
    SliceModel* slice, double mhz, const char* source)
{
    if (!slice || slice->panId().isEmpty())
        return BandStackPreselectResult::NotNeeded;
    if (mhz <= 54.0 && slice->frequency() <= 54.0)
        return BandStackPreselectResult::NotNeeded;

    const QString targetBand = BandSettings::bandForFrequency(mhz);
    const QString currentBand = BandSettings::bandForFrequency(slice->frequency());
    if (targetBand == currentBand)
        return BandStackPreselectResult::NotNeeded;

    const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
    const auto stackKeyResult =
        XvtrPolicy::resolveBandStackKey(targetBand, xvtrs, m_radioModel.capabilities());
    if (!stackKeyResult.isSupported()) {
        QString unsupportedReason = stackKeyResult.unsupportedReason;
        if (mhz > 54.0 && xvtrs.isEmpty()) {
            unsupportedReason =
                QString("Band %1 requires a configured XVTR before Aether can tune it.")
                    .arg(targetBand);
        }
        qCWarning(lcProtocol).noquote().nospace()
            << "MainWindow: direct tune cannot preselect band stack source="
            << (source ? source : "(unknown)")
            << " pan=" << slice->panId()
            << " from_band=" << currentBand
            << " to_band=" << targetBand
            << " freq_mhz=" << QString::number(mhz, 'f', 6)
            << " reason=" << unsupportedReason
            << " available_xvtrs=" << xvtrListSummary(xvtrs);
        statusBar()->showMessage(unsupportedReason, 5000);
        return BandStackPreselectResult::Unsupported;
    }

    qCDebug(lcProtocol).noquote().nospace()
        << "MainWindow: direct tune preselecting band stack source="
        << (source ? source : "(unknown)")
        << " pan=" << slice->panId()
        << " from_band=" << currentBand
        << " to_band=" << targetBand
        << " key=" << stackKeyResult.key;
    clearSwrSweepForBandChange(-1, slice->panId(), targetBand);
    m_bandSettings.setCurrentBand(targetBand);
    m_radioModel.sendCommand(
        QString("display pan set %1 band=%2").arg(slice->panId(), stackKeyResult.key));
    QTimer::singleShot(300, this, [this, panId = slice->panId()]() {
        reassertUnmutedSliceAudioForPan(panId);
    });
    return BandStackPreselectResult::Selected;
}

void MainWindow::applyTuneRequest(SliceModel* slice, double mhz,
                                  TuneIntent intent, const char* source)
{
    if (!slice)
        return;
    if (m_swrSweep.running)
        return;

    const double oldFreqMhz = slice->frequency();
    auto* sw = spectrumForSlice(slice);
    if (slice->isLocked()) {
        slice->notifyTuneBlockedByLock();
        if (slice->sliceId() == m_activeSliceId && sw) {
            m_updatingFromModel = true;
            sw->setVfoFrequency(oldFreqMhz);
            m_updatingFromModel = false;
        }
        return;
    }

    // Absolute-target intents (typed VFO entry, spectrum click, spot recall,
    // bandstack recall) must invalidate any in-flight encoder accumulator.
    // The #1524 1 kHz jitter-suppression tolerance otherwise keeps a stale
    // m_flexTargetMhz when the typed frequency lands inside that window,
    // producing the +60 Hz residual reported in #3260.
    if (intent == TuneIntent::CommandedTargetCenter
        || intent == TuneIntent::AbsoluteJump) {
        m_flexTargetMhz = -1.0;
        m_flexCoalesceTimer.stop();
#ifdef HAVE_MIDI
        m_midiTuneTargetMhz = -1.0;
        m_midiTuneIdleTimer.stop();
#endif
    }

    if (slice->sliceId() == m_activeSliceId && sw)
        sw->setVfoFrequency(mhz);

    const BandStackPreselectResult bandPreselect =
        (intent == TuneIntent::CommandedTargetCenter)
            ? preselectBandStackForTune(slice, mhz, source)
            : BandStackPreselectResult::NotNeeded;
    if (bandPreselect == BandStackPreselectResult::Unsupported) {
        if (slice->sliceId() == m_activeSliceId && sw)
            sw->setVfoFrequency(oldFreqMhz);
        return;
    }

    if (bandPreselect == BandStackPreselectResult::Selected) {
        const int sliceId = slice->sliceId();
        const QString sourceName = QString::fromUtf8(source ? source : "");
        QTimer::singleShot(250, this, [this, sliceId, mhz, sourceName, oldFreqMhz]() {
            auto* pendingSlice = m_radioModel.slice(sliceId);
            if (!pendingSlice || pendingSlice->isLocked() || m_swrSweep.running)
                return;
            if (pendingSlice->sliceId() == m_activeSliceId) {
                if (auto* pendingSw = spectrumForSlice(pendingSlice))
                    pendingSw->setVfoFrequency(mhz);
            }
            pendingSlice->tuneAndRecenter(mhz);
            mirrorDiversityChildFrequency(pendingSlice, mhz);

            const QByteArray sourceUtf8 = sourceName.toUtf8();
            const char* delayedSource = sourceUtf8.constData();
            const TuneCenteringResult result =
                revealFrequencyIfNeeded(pendingSlice, mhz,
                                        TuneIntent::CommandedTargetCenter,
                                        delayedSource);
            logTunePolicyDecision(delayedSource, TuneIntent::CommandedTargetCenter,
                                  oldFreqMhz, mhz, result);
        });
        return;
    }

    slice->setFrequency(mhz);
    mirrorDiversityChildFrequency(slice, mhz);

    const TuneCenteringResult result =
        (intent == TuneIntent::IncrementalTune)
            ? panFollowVfo(slice, mhz, source)
            : revealFrequencyIfNeeded(slice, mhz, intent, source);
    logTunePolicyDecision(source, intent, oldFreqMhz, mhz, result);
}

void MainWindow::applyPanRangeRequest(const QString& panId, double centerMhz,
                                      double bandwidthMhz, const char* source)
{
    if (panId.isEmpty() || bandwidthMhz <= 0.0)
        return;

    centerMhz = std::max(centerMhz, bandwidthMhz / 2.0);

    auto* pan = m_radioModel.panadapter(panId);
    const QString centerStr = QString::number(centerMhz, 'f', 6);
    const QString bandwidthStr = QString::number(bandwidthMhz, 'f', 6);

    if (pan) {
        if (qFuzzyCompare(pan->centerMhz(), centerMhz)
            && qFuzzyCompare(pan->bandwidthMhz(), bandwidthMhz)) {
            return;
        }
        // Update both values together before the radio echo arrives. Explicit
        // zoom workflows are especially sensitive to center/bandwidth skew;
        // splitting them produced the P1/P2 waterfall-loss and zoom-drift bugs.
        pan->applyPanStatus({
            {"center", centerStr},
            {"bandwidth", bandwidthStr},
        });
    }

    m_radioModel.sendCommand(
        QString("display pan set %1 center=%2 bandwidth=%3")
            .arg(panId, centerStr, bandwidthStr));

    qDebug() << "Pan range request:" << source
             << "center" << centerMhz
             << "bandwidth" << bandwidthMhz;
}

void MainWindow::setActiveSlice(int sliceId)
{
    setActiveSliceInternal(sliceId, true);
}

void MainWindow::queueActiveSliceForSpectrumTarget(int sliceId)
{
    if (sliceId < 0 || sliceId == m_activeSliceId) {
        m_pendingSpectrumTargetSliceId = -1;
        return;
    }

    m_pendingSpectrumTargetSliceId = sliceId;
    QTimer::singleShot(0, this, [this, sliceId]() {
        if (m_pendingSpectrumTargetSliceId != sliceId)
            return;
        m_pendingSpectrumTargetSliceId = -1;
        if (auto* s = m_radioModel.slice(sliceId))
            setActiveSliceInternal(s->sliceId(), false);
    });
}

void MainWindow::setActiveSliceInternal(int sliceId, bool revealOffscreen)
{
    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    // Auto-activate the panadapter that owns this slice
    if (m_panStack && !s->panId().isEmpty())
        m_panStack->setActivePan(s->panId());

    const int prevId = m_activeSliceId;
    m_activeSliceId = sliceId;

    // Send "slice set N active=1" only when switching to a different slice
    // (matches SmartSDR pcap — sent on VFO flag click, not on every tune).
    // Guard: don't send if triggered by the radio's own activeChanged echo
    // (m_updatingFromModel is set in the activeChanged handler).
    if (sliceId != prevId && !m_updatingFromModel)
        s->setActive(true);

    // Update RX EQ filter-cutoff guides whenever the active slice swaps —
    // the new slice may have a different mode / filter shape.
    if (sliceId != prevId)
        pushRxFilterCutoffsToEq();

#ifdef HAVE_HIDAPI
    // RC-28 F-key LEDs for slice-scoped hold actions (RIT/XIT/Lock) reflect the
    // active slice's state — refresh them when the active slice changes.
    if (sliceId != prevId)
        updateRC28Leds();
#endif
    if (sliceId != prevId && m_ax25HfPacketDecodeDialog)
        m_ax25HfPacketDecodeDialog->setAttachedSlice(s);
#ifdef HAVE_WEBSOCKETS
    if (sliceId != prevId && m_freedvReporterDialog)
        m_freedvReporterDialog->setActiveSlice(s);
#endif

    // Active slice changed → restart dwell window for the new active slice
    if (sliceId != prevId && m_bsAutoSaveTimer) {
        const int dwellSec = BandStackSettings::instance().autoSaveDwellSeconds();
        if (dwellSec > 0)
            m_bsAutoSaveTimer->start(dwellSec * 1000);
        else
            m_bsAutoSaveTimer->stop();
    }

    // Update all overlay isActive flags on each slice's correct spectrum
    for (auto* sl : m_radioModel.slices()) {
        const bool isActive = (sl->sliceId() == sliceId);
        if (auto* sw = spectrumForSlice(sl))
            sw->setSliceOverlay(sl->sliceId(), sl->frequency(),
                sl->filterLow(), sl->filterHigh(), sl->isTxSlice(), isActive,
                sl->mode(), sl->rttyMark(), sl->rttyShift(),
                sl->ritOn(), sl->ritFreq(), sl->xitOn(), sl->xitFreq());
    }

    // QSO recorder: track active slice for frequency/mode metadata (#1297)
    m_qsoRecorder->setSlice(s);

    // Re-wire applet panel, overlay menu to the new active slice
    if (m_panStack) {
        if (auto* applet = m_panStack->panadapter(s->panId()))
            applet->setSliceId(sliceId, s->letter());
        else if (m_panStack->activeApplet())
            m_panStack->activeApplet()->setSliceId(sliceId, s->letter());
    }
    m_appletPanel->setSlice(s);
    m_appletPanel->updateSliceButtons(m_radioModel.slices(), sliceId);
    // Sync squelch line to newly active slice (handles slice switch without waiting
    // for squelchChanged signal)
    if (auto* sw2 = spectrumForSlice(s))
        sw2->setSquelchLine(s->squelchOn(), s->squelchLevel());
    auto* sw = spectrum();
    if (sw) {
        if (revealOffscreen) {
            const TuneCenteringResult result =
                revealFrequencyIfNeeded(s, s->frequency(),
                                        TuneIntent::RevealOffscreen,
                                        "setActiveSlice");
            logTunePolicyDecision("setActiveSlice", TuneIntent::RevealOffscreen,
                                  s->frequency(), s->frequency(), result);
        }

        sw->overlayMenu()->setSlice(s);

        // Sync step size from the new active slice
        if (s->stepHz() > 0) {
            sw->setStepSize(s->stepHz());
            m_appletPanel->rxApplet()->syncStepFromSlice(s->stepHz(), s->stepList());
        }

        // Switch active VFO widget display (NR2/RN2/RADE are wired permanently
        // in wireVfoWidget, no disconnect/reconnect needed)
        sw->setActiveVfoWidget(sliceId);
    } else if (s->stepHz() > 0) {
        m_appletPanel->rxApplet()->syncStepFromSlice(s->stepHz(), s->stepList());
    }

    if (m_flexControlDialog)
        syncFlexControlDialog();

    // Update filter limits for the active slice's mode
    updateFilterLimitsForMode(s->mode());

    routeCwDecoderOutput();
    refreshCwDecodeState();
    routeRttyDecoderOutput();
    refreshRttyDecodeState();

    // Update CWX/DVK indicator availability for this slice's mode
    updateKeyerAvailability(s->mode());

    // Detect band from frequency
    if (m_bandSettings.currentBand().isEmpty())
        m_bandSettings.setCurrentBand(BandSettings::bandForFrequency(s->frequency()));


    // NOTE: RADE audio mode is now driven by the TX slice (in updateDaxTxMode),
    // not the active/selected slice. Switching which slice the user is looking at
    // should not change the TX audio routing.

    updateSplitState();

    // TX follows active slice (#441) — auto-assign TX when switching slices
    if (!m_splitActive && sliceId != prevId && !s->isTxSlice()
        && AppSettings::instance().value("TxFollowsActiveSlice", "False").toString() == "True") {
        s->setTxSlice(true);
    }

    // Update MEM button target-slice badge on every overlay (#1781)
    refreshMemoryBrowsePanel();

#ifdef HAVE_HIDAPI
    // Rewire StreamDeck+ RIT/XIT state triggers when the active slice changes
    disconnect(m_sdRitConn);
    disconnect(m_sdXitConn);
    m_sdRitConn = connect(s, &SliceModel::ritChanged, this, [this](bool, int){ refreshStreamDeckLabels(); });
    m_sdXitConn = connect(s, &SliceModel::xitChanged, this, [this](bool, int){ refreshStreamDeckLabels(); });
    if (sliceId != prevId) refreshStreamDeckLabels();

    // Rewire RC-28 F-key LED refresh to the active slice's RIT/XIT/Lock state so
    // an F-key whose hold action is one of those tracks changes made by ANY
    // control, not just the RC-28 itself.
    disconnect(m_rc28RitConn);
    disconnect(m_rc28XitConn);
    disconnect(m_rc28LockConn);
    m_rc28RitConn  = connect(s, &SliceModel::ritChanged,    this, [this](bool, int){ updateRC28Leds(); });
    m_rc28XitConn  = connect(s, &SliceModel::xitChanged,    this, [this](bool, int){ updateRC28Leds(); });
    m_rc28LockConn = connect(s, &SliceModel::lockedChanged, this, [this](bool){ updateRC28Leds(); });

    // Rewire TMate 2 status LED to the active slice's Lock state, and push an
    // immediate display update so the LCD shows the new slice's frequency.
    disconnect(m_tmate2LockConn);
    disconnect(m_tmate2ModeConn);
    disconnect(m_tmate2RitConn);
    disconnect(m_tmate2XitConn);
    m_tmate2LockConn = connect(s, &SliceModel::lockedChanged, this,
                                [this](bool){ updateTMate2Status(); });
    m_tmate2ModeConn = connect(s, &SliceModel::modeChanged,   this,
                                [this](const QString&){ updateTMate2Indicators(); });
    m_tmate2RitConn  = connect(s, &SliceModel::ritChanged,    this,
                                [this](bool, int){ updateTMate2Indicators(); });
    m_tmate2XitConn  = connect(s, &SliceModel::xitChanged,    this,
                                [this](bool, int){ updateTMate2Indicators(); });
    if (sliceId != prevId) {
        updateTMate2Display();
        updateTMate2Status();
        updateTMate2Indicators();
    }
#endif

#ifdef HAVE_MQTT
    // Rewire radio state MQTT publish to the new active slice's freq/mode signals.
    disconnect(m_radioStateFreqConn);
    disconnect(m_radioStateModeConn);
    m_radioStateFreqConn = connect(s, &SliceModel::frequencyChanged,
                                   this, [this](double) { m_radioStateCoalesceTimer.start(); });
    m_radioStateModeConn = connect(s, &SliceModel::modeChanged,
                                   this, [this](const QString&) { m_radioStateCoalesceTimer.start(); });
    publishRadioStateMqtt();
#endif

    qDebug() << "MainWindow: active slice set to" << sliceId;
}

void MainWindow::updateFilterLimitsForMode(const QString& mode)
{
    int minHz, maxHz;
    if (mode == "LSB" || mode == "DIGL" || mode == "CWL") {
        minHz = -12000; maxHz = 0;
    } else if (mode == "AM" || mode == "SAM" || mode == "DSB") {
        minHz = -12000; maxHz = 12000;
    } else if (mode == "FM" || mode == "NFM" || mode == "DFM") {
        minHz = -12000; maxHz = 12000;
    } else {
        // USB, DIGU, CW, RTTY, etc.
        minHz = 0; maxHz = 12000;
    }
    if (auto* s = spectrum()) {
        s->setFilterLimits(minHz, maxHz);
        s->setMode(mode);
    }
}

void MainWindow::pushSliceOverlay(SliceModel* s)
{
    if (m_applyingLayout) return;
    auto* sw = spectrumForSlice(s);
    if (!sw) return;
    sw->setSliceOverlay(s->sliceId(), s->frequency(),
        s->filterLow(), s->filterHigh(), s->isTxSlice(),
        s->sliceId() == m_activeSliceId,
        s->mode(), s->rttyMark(), s->rttyShift(),
        s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
}

void MainWindow::syncTxWaterfallSliceToSpectrums()
{
    SliceModel* txSlice = nullptr;
    for (auto* s : m_radioModel.slices()) {
        if (s && s->isTxSlice()) {
            txSlice = s;
            break;
        }
    }

    auto apply = [txSlice](SpectrumWidget* sw) {
        if (!sw) return;
        if (txSlice) {
            sw->setTxWaterfallSlice(txSlice->frequency(),
                                    txSlice->filterLow(),
                                    txSlice->filterHigh(),
                                    txSlice->xitOn(),
                                    txSlice->xitFreq());
        } else {
            sw->clearTxWaterfallSlice();
        }
    };

    if (m_panStack) {
        for (auto* applet : m_panStack->allApplets()) {
            if (applet)
                apply(applet->spectrumWidget());
        }
    } else if (m_panApplet) {
        apply(m_panApplet->spectrumWidget());
    }
}

void MainWindow::disableSplit()
{
    if (!m_splitActive) return;

    m_splitActive = false;

    // Move TX back to the RX slice
    if (auto* rxSlice = m_radioModel.slice(m_splitRxSliceId))
        rxSlice->setTxSlice(true);

    // Destroy the split TX slice
    if (m_splitTxSliceId >= 0)
        m_radioModel.sendCommand(QString("slice remove %1").arg(m_splitTxSliceId));

    m_splitRxSliceId = -1;
    m_splitTxSliceId = -1;
    if (auto* sw = spectrum()) sw->setSplitPair(-1, -1);

    updateSplitState();
}

void MainWindow::updateSplitState()
{
    auto* sw = spectrum();
    if (!sw) return;
    for (auto* s : m_radioModel.slices()) {
        if (auto* w = sw->vfoWidget(s->sliceId())) {
            bool isTxSlice = (m_splitActive && s->sliceId() == m_splitTxSliceId);
            bool isRxSplit = (m_splitActive && s->sliceId() == m_splitRxSliceId);
            w->updateSplitBadge(isTxSlice, isRxSplit);
        }
    }
}

// ── Per-panadapter signal wiring ──────────────────────────────────────────────
// Called once per PanadapterApplet. Connects the SpectrumWidget and its
// OverlayMenu signals to RadioModel, TnfModel, and MainWindow handlers.
// In multi-pan mode (Phase 6+), called for each new panadapter.

void MainWindow::reassertUnmutedSliceAudioForPan(const QString& panId)
{
    const auto slices = m_radioModel.slices();
    if (slices.size() <= 1) return;

    for (auto* slice : slices) {
        if (!slice || slice->panId() != panId || slice->audioMute())
            continue;

        // The model already shows unmuted, so SliceModel::setAudioMute(false)
        // would no-op. Send the command directly to rebuild radio audio routing.
        m_radioModel.sendCommand(
            QString("slice set %1 audio_mute=0").arg(slice->sliceId()));
    }
}

void MainWindow::onMuteAllSlicesToggle()
{
    const auto slices = m_radioModel.slices();

    // Determine intent: mute all if any owned slice is currently unmuted,
    // otherwise unmute all.  RadioModel::slices() returns only owned slices
    // (foreign clients' slices are deleted from m_slices on client_handle).
    bool anyUnmuted = false;
    for (const SliceModel* s : slices) {
#ifdef HAVE_RADE
        if (s && s->sliceId() == m_radeSliceId) continue;  // RADE owns its mute
#endif
        if (s && !s->audioMute()) { anyUnmuted = true; break; }
    }

    for (SliceModel* s : slices) {
#ifdef HAVE_RADE
        // Skip the RADE-managed slice in both directions.
        // Muting: the RADE slice is already forced muted by activateRADE();
        //   setAudioMute(true) would no-op, but skipping is clearer intent.
        // Unmuting: setAudioMute(false) would break RADE's audio gating and
        //   corrupt m_radePrevMute's restore value on deactivateRADE().
        if (s && s->sliceId() == m_radeSliceId) continue;
#endif
        if (s) s->setAudioMute(anyUnmuted);
    }
}

void MainWindow::setActivePanApplet(PanadapterApplet* applet)
{
    if (applet == m_panApplet) return;
    m_panApplet = applet;

    routeCwDecoderOutput();
    routeRttyDecoderOutput();
}

// Route CW decoder text/stats output to the pan that owns the active slice,
// so decoded text appears in the correct pan's CW widget (#864).
void MainWindow::routeCwDecoderOutput()
{
    // Determine which applet should receive CW decoder output:
    // the pan that owns the active audio slice (whose audio feeds the decoder).
    PanadapterApplet* target = nullptr;
    if (auto* s = activeSlice(); s && m_panStack && !s->panId().isEmpty())
        target = m_panStack->panadapter(s->panId());
    if (!target)
        target = m_panApplet;  // fallback to active pan

    if (target == m_cwDecoderApplet) return;

    // Disconnect from old applet
#ifdef HAVE_MQTT
    disconnect(m_cwStatsConn);
#endif
    if (m_cwDecoderApplet) {
        disconnect(&m_cwDecoder, &CwDecoder::textDecoded,
                   m_cwDecoderApplet, &PanadapterApplet::appendCwText);
        disconnect(&m_cwDecoderTx, &CwDecoder::textDecoded,
                   m_cwDecoderApplet, &PanadapterApplet::appendCwTextTx);
        disconnect(&m_cwDecoder, &CwDecoder::statsUpdated,
                   m_cwDecoderApplet, &PanadapterApplet::setCwStats);
        if (auto* pb = m_cwDecoderApplet->lockPitchButton())
            disconnect(pb, &QPushButton::toggled,
                       &m_cwDecoder, &CwDecoder::lockPitch);
        if (auto* sb = m_cwDecoderApplet->lockSpeedButton())
            disconnect(sb, &QPushButton::toggled,
                       &m_cwDecoder, &CwDecoder::lockSpeed);
        disconnect(m_cwDecoderApplet, &PanadapterApplet::pitchRangeChanged,
                   &m_cwDecoder, &CwDecoder::setPitchRange);
        disconnect(m_cwDecoderApplet, &PanadapterApplet::speedRangeChanged,
                   &m_cwDecoder, &CwDecoder::setSpeedRange);
        disconnect(m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                   &m_cwDecoder, &CwDecoder::stop);
        disconnect(m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                   &m_cwDecoderTx, &CwDecoder::stop);
    }

    m_cwDecoderApplet = target;

    // Connect to new applet
    if (m_cwDecoderApplet) {
        connect(&m_cwDecoder, &CwDecoder::textDecoded,
                m_cwDecoderApplet, &PanadapterApplet::appendCwText);
        // TX-side decoded text routes to a separate slot so the panel
        // can render it with a [TX] prefix and distinct color (#2417).
        connect(&m_cwDecoderTx, &CwDecoder::textDecoded,
                m_cwDecoderApplet, &PanadapterApplet::appendCwTextTx);
        connect(&m_cwDecoder, &CwDecoder::statsUpdated,
                m_cwDecoderApplet, &PanadapterApplet::setCwStats);
#ifdef HAVE_MQTT
        m_cwStatsConn = connect(&m_cwDecoder, &CwDecoder::statsUpdated,
                this, [this](float pitchHz, float speedWpm) {
            m_cwLastPitchHz   = pitchHz;
            m_cwLastSpeedWpm  = speedWpm;
        });
#endif
        connect(m_cwDecoderApplet->lockPitchButton(), &QPushButton::toggled,
                &m_cwDecoder, &CwDecoder::lockPitch);
        connect(m_cwDecoderApplet->lockSpeedButton(), &QPushButton::toggled,
                &m_cwDecoder, &CwDecoder::lockSpeed);
        connect(m_cwDecoderApplet, &PanadapterApplet::pitchRangeChanged,
                &m_cwDecoder, &CwDecoder::setPitchRange);
        m_cwDecoder.setPitchRange(m_cwDecoderApplet->pitchRangeLow(),
                                  m_cwDecoderApplet->pitchRangeHigh());
        connect(m_cwDecoderApplet, &PanadapterApplet::speedRangeChanged,
                &m_cwDecoder, &CwDecoder::setSpeedRange);
        m_cwDecoder.setSpeedRange(m_cwDecoderApplet->speedRangeLow(),
                                  m_cwDecoderApplet->speedRangeHigh());
        connect(m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                &m_cwDecoder, &CwDecoder::stop);
        connect(m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                &m_cwDecoderTx, &CwDecoder::stop);
    }
}

// Recompute the CW decoder run state, panel visibility, and the
// AudioEngine TX-side sidetone tap (#2417).  Single chokepoint so the
// independent RX/TX toggles, MOX edges, and slice-mode changes all
// converge on the same decision tree.
void MainWindow::routeRttyDecoderOutput()
{
    PanadapterApplet* target = nullptr;
    if (auto* s = activeSlice(); s && m_panStack && !s->panId().isEmpty())
        target = m_panStack->panadapter(s->panId());
    if (!target) target = m_panApplet;

    if (target == m_rttyDecoderApplet) return;

    if (m_rttyDecoderApplet) {
        disconnect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                   m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        disconnect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                   m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setShiftHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                   &m_rttyDecoder, &RttyDecoder::setBaudRate);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                   &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                   &m_rttyDecoder, &RttyDecoder::stop);
    }

    m_rttyDecoderApplet = target;

    if (m_rttyDecoderApplet) {
        connect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        connect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                &m_rttyDecoder, &RttyDecoder::setShiftHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                &m_rttyDecoder, &RttyDecoder::setBaudRate);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                &m_rttyDecoder, &RttyDecoder::stop);
    }
}

void MainWindow::refreshRttyDecodeState()
{
    auto* s = activeSlice();
    // Only auto-activate for explicit RTTY mode.  DIGL is a general LSB-data
    // mode used for PSK31, FT8, SSTV, etc. — showing a Baudot decoder on
    // those signals would be confusing.  Users who do FSK on DIGL can open
    // the panel manually via the slice context menu (future work).
    const bool isRtty = s && s->mode() == "RTTY";

    if (m_rttyDecoderApplet)
        m_rttyDecoderApplet->setRttyPanelVisible(isRtty);

    if (!isRtty) {
        if (m_rttyDecoder.isRunning()) m_rttyDecoder.stop();
        return;
    }

    // Can't push params or start without the applet — the panel combos
    // hold the user's choices and we have no fallback source for them.
    if (!m_rttyDecoderApplet) return;

    const int markHz = m_rttyDecoderApplet->rttyMarkHz();
    // markHz == 0 means "Auto": follow the radio's rttyMark setting
    const int effectiveMark = (markHz > 0) ? markHz : s->rttyMark();
    m_rttyDecoder.setMarkFreqHz(effectiveMark);
    m_rttyDecoder.setShiftHz(m_rttyDecoderApplet->rttyShiftHz());
    m_rttyDecoder.setBaudRate(m_rttyDecoderApplet->rttyBaud());
    m_rttyDecoder.setReversePolarity(m_rttyDecoderApplet->rttyReverse());

    if (!m_rttyDecoder.isRunning()) m_rttyDecoder.start();
}

void MainWindow::refreshCwDecodeState()
{
    const bool rxOn = CwDecodeSettings::rxEnabled();
    const bool txOn = CwDecodeSettings::txEnabled();
    const bool anyOn = rxOn || txOn;

    auto* s = activeSlice();
    const bool isCw = s && (s->mode() == "CW" || s->mode() == "CWL");

    // Panel is visible only in CW receive mode — the operator's CW
    // text view is anchored to a CW slice's panadapter.  TX-side
    // decode is shown in the same panel, so if there's no CW slice in
    // view, there's no panel either.
    if (m_cwDecoderApplet)
        m_cwDecoderApplet->setCwPanelVisible(isCw && anyOn);

    // RX decoder runs only when RX-decode is on and the operator is
    // listening to a CW slice.  Non-CW slices feed unrelated audio,
    // and the panel is hidden anyway.
    const bool shouldRunRx = isCw && rxOn;
    if (shouldRunRx && !m_cwDecoder.isRunning())
        m_cwDecoder.start();
    else if (!shouldRunRx && m_cwDecoder.isRunning())
        m_cwDecoder.stop();

    // TX decoder runs whenever TX-decode is enabled — the worker
    // sleeps on its ring buffer between transmissions, so leaving it
    // up costs almost nothing and avoids a start/stop hiccup on every
    // MOX edge.
    if (txOn && !m_cwDecoderTx.isRunning())
        m_cwDecoderTx.start();
    else if (!txOn && m_cwDecoderTx.isRunning())
        m_cwDecoderTx.stop();

    // Feed the TX decoder the operator's known pitch + speed from the
    // P/CW applet rather than letting ggmorse auto-detect — for TX-self
    // decode we already know exactly what's being keyed, and detection
    // off the short sidetone bursts is unreliable (gave 55 WPM readouts
    // and fragmented single-letter output on 20 WPM keying). (#2417)
    if (txOn) {
        const auto& tm = m_radioModel.transmitModel();
        if (tm.cwPitch() > 0 && tm.cwSpeed() > 0)
            m_cwDecoderTx.setKnownParameters(
                static_cast<float>(tm.cwPitch()),
                static_cast<float>(tm.cwSpeed()));
    }

    // Gate the sidetone tap solely on txOn.  An earlier version also AND'd
    // with isMox() || isTransmitting() but CLAUDE.md is explicit: the
    // radio never sends mox= in transmit status, and CW keying goes
    // through the netcw UDP stream which doesn't necessarily flip the
    // interlock state machine either — so any MOX-based gate would
    // suppress the tap during CW keying.  The sidetone generator already
    // self-gates: it only produces audio bursts when the operator is
    // keying, and fills silence buffers between.  ggmorse handles the
    // silence fine.
    if (m_audio)
        m_audio->setCwDecodeTxTapEnabled(txOn);
}

void MainWindow::schedulePanFpsReconcile(const QString& panId, int reportedFps)
{
    if (panId.isEmpty() || reportedFps <= 0)
        return;
    // While adaptive throttle is active the radio fps is intentionally below the
    // user's desired value. Don't fight the throttle — MainWindow restores fps
    // when adaptiveThrottleChanged(false) fires.
    if (m_adaptiveThrottleActive) {
        qCDebug(lcProtocol).noquote().nospace()
            << "MainWindow: fps reconcile suppressed for pan=" << panId
            << " reported=" << reportedFps << " (adaptive throttle active)";
        return;
    }

    auto* pan = m_radioModel.panadapter(panId);
    if (!pan)
        return;

    auto& state = m_panFpsReconcile[panId];
    if (!state.spectrum) {
        if (auto* applet = m_panStack->panadapter(panId))
            state.spectrum = applet->spectrumWidget();
    }

    auto* sw = state.spectrum.data();
    if (!sw)
        return;

    const int desiredFps = sw->fftFps();
    if (desiredFps <= 0)
        return;
    if (desiredFps == reportedFps) {
        if (state.timer)
            state.timer->stop();
        state.lastSentMs = 0;
        state.lastSentDesired = -1;
        return;
    }

    if (!state.timer) {
        state.timer = new QTimer(this);
        state.timer->setSingleShot(true);
        state.timer->setInterval(300);
        connect(state.timer, &QTimer::timeout, this, [this, panId]() {
            auto it = m_panFpsReconcile.find(panId);
            if (it == m_panFpsReconcile.end())
                return;

            auto* pan = m_radioModel.panadapter(panId);
            auto* sw = it->spectrum.data();
            if (!sw) {
                if (auto* applet = m_panStack->panadapter(panId)) {
                    sw = applet->spectrumWidget();
                    it->spectrum = sw;
                }
            }
            if (!pan || !sw)
                return;

            const int reported = pan->fps();
            const int desired = sw->fftFps();
            if (reported <= 0 || desired <= 0 || reported == desired)
                return;

            constexpr qint64 kCooldownMs = 5000;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (it->lastSentDesired == desired
                && it->lastSentMs > 0
                && now - it->lastSentMs < kCooldownMs) {
                return;
            }

            qCDebug(lcProtocol).noquote().nospace()
                << "MainWindow: reasserting panadapter FPS pan=" << panId
                << " reported=" << reported
                << " desired=" << desired;
            m_radioModel.sendCommand(
                QString("display pan set %1 fps=%2").arg(panId).arg(desired));
            it->lastSentMs = now;
            it->lastSentDesired = desired;
        });
    }

    state.timer->start();
}

void MainWindow::scheduleWaterfallLineDurationReconcile(const QString& panId, int reportedMs)
{
    if (panId.isEmpty() || reportedMs <= 0)
        return;
    if (m_adaptiveThrottleActive) {
        qCDebug(lcProtocol).noquote().nospace()
            << "MainWindow: wf line_duration reconcile suppressed for pan=" << panId
            << " reported=" << reportedMs << "ms (adaptive throttle active)";
        return;
    }

    auto* pan = m_radioModel.panadapter(panId);
    if (!pan)
        return;

    auto& state = m_wfLineDurationReconcile[panId];
    if (!state.spectrum) {
        if (auto* applet = m_panStack->panadapter(panId))
            state.spectrum = applet->spectrumWidget();
    }

    auto* sw = state.spectrum.data();
    if (!sw)
        return;

    const int desiredMs = sw->wfLineDuration();
    if (desiredMs <= 0)
        return;
    if (desiredMs == reportedMs) {
        if (state.timer)
            state.timer->stop();
        state.lastSentMs = 0;
        state.lastSentDesired = -1;
        return;
    }

    if (!state.timer) {
        state.timer = new QTimer(this);
        state.timer->setSingleShot(true);
        state.timer->setInterval(300);
        connect(state.timer, &QTimer::timeout, this, [this, panId]() {
            auto it = m_wfLineDurationReconcile.find(panId);
            if (it == m_wfLineDurationReconcile.end())
                return;

            auto* pan = m_radioModel.panadapter(panId);
            auto* sw = it->spectrum.data();
            if (!sw) {
                if (auto* applet = m_panStack->panadapter(panId)) {
                    sw = applet->spectrumWidget();
                    it->spectrum = sw;
                }
            }
            if (!pan || !sw)
                return;

            const QString wfId = pan->waterfallId();
            const int reported = pan->waterfallLineDuration();
            const int desired = sw->wfLineDuration();
            if (wfId.isEmpty() || reported <= 0 || desired <= 0 || reported == desired)
                return;

            constexpr qint64 kCooldownMs = 5000;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (it->lastSentDesired == desired
                && it->lastSentMs > 0
                && now - it->lastSentMs < kCooldownMs) {
                return;
            }

            qCDebug(lcProtocol).noquote().nospace()
                << "MainWindow: reasserting waterfall rate pan=" << panId
                << " waterfall=" << wfId
                << " reported_line_duration=" << reported
                << " desired_line_duration=" << desired;
            m_radioModel.sendCommand(
                QString("display panafall set %1 line_duration=%2").arg(wfId).arg(desired));
            it->lastSentMs = now;
            it->lastSentDesired = desired;
        });
    }

    state.timer->start();
}

// Per-pan FPS / waterfall-line-duration reconcilers. Wired from
// wirePanadapter() for fresh pans and from the panadapterReclaimed handler
// for previous-session pans reclaimed on reconnect — the disconnect path
// explicitly tears these connections down, and reclaimed pans never re-emit
// panadapterAdded, so they need this re-wire to keep reconciling.
void MainWindow::wirePanReconcilers(PanadapterApplet* applet, PanadapterModel* pan)
{
    auto* sw = applet->spectrumWidget();
    if (!sw || !pan)
        return;

    auto oldFpsConnection = m_panFpsReconcileConnections.take(applet->panId());
    if (oldFpsConnection)
        QObject::disconnect(oldFpsConnection);

    auto& fpsState = m_panFpsReconcile[applet->panId()];
    fpsState.spectrum = sw;
    m_panFpsReconcileConnections.insert(
        applet->panId(),
        connect(pan, &PanadapterModel::fpsReported,
                this, [this, panId = applet->panId()](int fps) {
            schedulePanFpsReconcile(panId, fps);
        }));
    schedulePanFpsReconcile(applet->panId(), pan->fps());

    auto oldWfLineDurationConnection =
        m_wfLineDurationReconcileConnections.take(applet->panId());
    if (oldWfLineDurationConnection)
        QObject::disconnect(oldWfLineDurationConnection);

    auto& wfLineDurationState = m_wfLineDurationReconcile[applet->panId()];
    wfLineDurationState.spectrum = sw;
    m_wfLineDurationReconcileConnections.insert(
        applet->panId(),
        connect(pan, &PanadapterModel::waterfallLineDurationReported,
                this, [this, panId = applet->panId()](int ms) {
            scheduleWaterfallLineDurationReconcile(panId, ms);
        }));
    scheduleWaterfallLineDurationReconcile(applet->panId(),
                                           pan->waterfallLineDuration());
}

// wirePanadapter() / revealFrequencyIfNeeded() / panFollowVfo() / wireVfoWidget() lives in MainWindow_Wiring.cpp (#3351 Phase 1d).
void MainWindow::updateNr2Availability()
{
    bool opusActive = (m_radioModel.audioCompressionParam() == "opus");
    const QString tooltip = opusActive
        ? "NR2 is not available with compressed (SmartLink) audio"
        : "Client-side spectral noise reduction (Ephraim-Malah MMSE). Right-click for NR2 settings.";

    // If Opus just became active and NR2 is running, disable it
    if (opusActive && m_audio->nr2Enabled()) {
        QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(false); });
        statusBar()->showMessage("NR2 disabled — not available with compressed (SmartLink) audio", 4000);
    }

    // Update the NR2 selector in the AetherDSP applet — the only
    // remaining surface for client-side NR controls.  The modeless
    // AetherDspDialog is created on demand and owns its own enable
    // sync via nr2EnabledChanged + setEnabled-on-show.
    if (auto* a = m_appletPanel ? m_appletPanel->clientRxDspApplet() : nullptr) {
        if (auto* w = a->widget())
            w->setNr2Available(!opusActive, tooltip);
    }
}

void MainWindow::enableNr2WithWisdom()
{
    if (AudioEngine::needsWisdomGeneration()) {
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        const auto result = std::make_shared<std::atomic<int>>(
            static_cast<int>(SpectralNR::WisdomResult::Failed));

        const bool frameless =
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True";

        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("AetherSDR — FFTW Wisdom");
        if (frameless)
            dlg->setWindowFlag(Qt::FramelessWindowHint, true);
        // Modeless — wisdom generation can take minutes; locking the
        // operator out of the radio for that whole window was a worse UX
        // than letting them keep operating while the worker thread runs
        // in the background.  The thread is already off the GUI thread
        // (see QThread::create below); progress callbacks marshal back
        // via QMetaObject::invokeMethod and the Cancel path is wired
        // through QDialog::rejected.
        dlg->setWindowModality(Qt::NonModal);
        // Tool window flag so the dialog floats above the main window
        // without claiming a separate taskbar entry, and stays visible
        // when the operator clicks back to the main UI.
        dlg->setWindowFlag(Qt::Tool, true);
        dlg->setAttribute(Qt::WA_ShowWithoutActivating, true);
        dlg->setMinimumWidth(500);
        AetherSDR::ThemeManager::instance().applyStyleSheet(dlg, "QDialog { background: #050710; }"
            "QLabel { color: {{color.text.secondary}}; background: transparent; }"
            "QProgressBar { text-align: center; font-size: 13px;"
            " font-weight: bold; color: {{color.text.primary}};"
            " background: {{color.background.0}}; border: 1px solid {{color.background.1}}; border-radius: 3px; }"
            "QProgressBar::chunk { background: {{color.accent}}; }");

        auto* root = new QVBoxLayout(dlg);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto* titleBar = new FramelessWindowTitleBar(QStringLiteral("AetherSDR — FFTW Wisdom"), dlg);
        titleBar->setVisible(frameless);
        root->addWidget(titleBar);

        auto* content = new QWidget(dlg);
        auto* body = new QVBoxLayout(content);
        body->setContentsMargins(10, frameless ? 8 : 10, 10, 10);
        body->setSpacing(10);

        auto* label = new QLabel(
            "Optimizing FFT plans for NR2...\n\n"
            "This window will automatically close when wisdom generation is complete.",
            content);
        label->setWordWrap(true);
        body->addWidget(label);

        auto* progress = new QProgressBar(content);
        progress->setRange(0, 100);
        progress->setValue(0);
        body->addWidget(progress);

        auto* buttonRow = new QHBoxLayout();
        buttonRow->addStretch();
        auto* cancelButton = new QPushButton("Cancel", content);
        cancelButton->setAutoDefault(false);
        cancelButton->setDefault(false);
        buttonRow->addWidget(cancelButton);
        body->addLayout(buttonRow);
        root->addWidget(content);

        dlg->show();

        auto* breathe = new QPropertyAnimation(dlg, "windowOpacity", dlg);
        breathe->setDuration(1500);
        breathe->setStartValue(1.0);
        breathe->setKeyValueAt(0.5, 0.55);
        breathe->setEndValue(1.0);
        breathe->setLoopCount(-1);

        const auto requestCancel = [cancelled, label, progress, cancelButton]() {
            if (cancelled->exchange(true))
                return;
            cancelButton->setEnabled(false);
            progress->setRange(0, 0);
            label->setText("Canceling FFTW wisdom generation...\n\n"
                           "Audio will continue unchanged. This may take a moment while the current FFT plan finishes.");
        };
        connect(cancelButton, &QPushButton::clicked, dlg, requestCancel);
        connect(dlg, &QDialog::rejected, dlg, requestCancel);

        const QPointer<QDialog> dlgGuard(dlg);
        auto* thread = QThread::create([cancelled, result, dlgGuard, breathe, label, progress]() {
            const auto wisdomResult = AudioEngine::generateWisdom(
                [cancelled, dlgGuard, breathe, label, progress](int step, int total, const std::string& desc) {
                    if (!dlgGuard)
                        return;
                    if (cancelled->load())
                        return;
                    int pct = total > 0 ? (step * 100 / total) : 0;
                    QString d = QString::fromStdString(desc);
                    QMetaObject::invokeMethod(dlgGuard.data(), [dlgGuard, breathe, label, progress, pct, d]() {
                        if (!dlgGuard)
                            return;
                        if (!d.isEmpty()) {
                            label->setText(d + "\n\n"
                                "This window will automatically close when wisdom generation is complete.");
                            if (progress->value() >= 90 && breathe->state() != QAbstractAnimation::Running)
                                breathe->start();
                        } else {
                            progress->setValue(pct);
                        }
                    });
                },
                [cancelled]() { return cancelled->load(); });
            result->store(static_cast<int>(wisdomResult));
        });
        connect(thread, &QThread::finished, this, [this, dlg, breathe, progress, label, thread, result]() {
            const auto wisdomResult =
                static_cast<SpectralNR::WisdomResult>(result->load());
            const bool ready = wisdomResult == SpectralNR::WisdomResult::Ready
                            || wisdomResult == SpectralNR::WisdomResult::Generated;
            breathe->stop();
            dlg->setWindowOpacity(1.0);
            progress->setRange(0, 100);
            progress->setValue(ready ? 100 : 0);

            if (!ready) {
                label->setText(wisdomResult == SpectralNR::WisdomResult::Cancelled
                    ? "Wisdom generation canceled. Audio was left unchanged."
                    : "Wisdom generation failed. Audio was left unchanged.");
                if (auto* a = m_appletPanel ? m_appletPanel->clientRxDspApplet() : nullptr) {
                    if (auto* w = a->widget())
                        w->syncFromEngine();
                }
                if (m_dspDialog)
                    m_dspDialog->syncFromEngine();
                statusBar()->showMessage("NR2 was not enabled; audio is unchanged", 4000);
                QTimer::singleShot(800, this, [dlg, thread]() {
                    dlg->accept();
                    dlg->deleteLater();
                    thread->deleteLater();
                });
                return;
            }

            label->setText("Wisdom generation complete!");
            QTimer::singleShot(800, this, [this, dlg, thread]() {
                dlg->accept();
                dlg->deleteLater();
                thread->deleteLater();
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(true); });
            });
        });
        thread->start();
    } else {
        QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(true); });
    }
}

SpectrumWidget* MainWindow::spectrum() const
{
    return m_panStack ? m_panStack->activeSpectrum()
                      : (m_panApplet ? m_panApplet->spectrumWidget() : nullptr);
}

// ── UI Scale helpers ────────────────────────────────────────────────────
static constexpr int kScaleSteps[] = {75, 85, 100, 110, 125, 150, 175, 200};
static constexpr int kScaleStepCount = sizeof(kScaleSteps) / sizeof(kScaleSteps[0]);

void MainWindow::applyUiScale(int pct)
{
    int current = AppSettings::instance().value("UiScalePercent", "100").toInt();
    if (pct == current)
        return;

    AppSettings::instance().setValue("UiScalePercent", QString::number(pct));
    AppSettings::instance().save();

    auto answer = QMessageBox::question(this, "UI Scale",
        QString("UI scale changed to %1%. Restart AetherSDR now to apply?").arg(pct),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
#ifdef Q_OS_MAC
        // On macOS, relaunch via 'open -n' so the .app bundle is activated through
        // Launch Services — direct binary exec bypasses the bundle, causing dock
        // duplication and incorrect activation policy in notarized builds.
        // Walk up from the binary (Foo.app/Contents/MacOS/Foo) to the bundle root.
        QDir d = QFileInfo(QCoreApplication::applicationFilePath()).dir(); // .../MacOS
        d.cdUp();  // .../Contents
        d.cdUp();  // .../Foo.app  (or plain build dir in dev)
        if (d.dirName().endsWith(".app")) {
            QStringList openArgs = {"-n", d.absolutePath()};
            const QStringList childArgs = QCoreApplication::arguments().mid(1);
            if (!childArgs.isEmpty())
                openArgs << "--args" << childArgs;
            QProcess::startDetached("open", openArgs);
        } else {
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    QCoreApplication::arguments().mid(1));
        }
#else
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
#endif
        QCoreApplication::quit();
    }
}

void MainWindow::stepUiScale(int direction)
{
    int current = AppSettings::instance().value("UiScalePercent", "100").toInt();
    // Find nearest step in the requested direction
    int best = current;
    if (direction > 0) {
        for (int i = 0; i < kScaleStepCount; ++i) {
            if (kScaleSteps[i] > current) { best = kScaleSteps[i]; break; }
        }
    } else {
        for (int i = kScaleStepCount - 1; i >= 0; --i) {
            if (kScaleSteps[i] < current) { best = kScaleSteps[i]; break; }
        }
    }
    if (best != current)
        applyUiScale(best);
}

void MainWindow::setAppletPanelDockedLeft(bool left)
{
    if (!m_splitter || !m_appletPanel || !m_panStack)
        return;

    // Move m_appletPanel either before m_panStack (left dock) or to the end
    // (right dock).  insertWidget()/addWidget() on an already-attached child
    // reparents it to the new index without destroy/recreate.
    if (left) {
        const int panIdx = m_splitter->indexOf(m_panStack);
        if (panIdx < 0) return;
        m_splitter->insertWidget(panIdx, m_appletPanel);
    } else {
        m_splitter->addWidget(m_appletPanel);
    }

    // Re-apply stretch/collapse rules by widget identity (indices shifted).
    for (int i = 0; i < m_splitter->count(); ++i) {
        QWidget* w = m_splitter->widget(i);
        m_splitter->setStretchFactor(i, w == m_panStack ? 1 : 0);
        m_splitter->setCollapsible(i, false);
    }

    // QSplitter's per-index size array does NOT follow widgets when they
    // move — applet's slot inherits panstack's old (huge) width, which
    // setFixedWidth(260) then visibly caps but leaves the remainder as
    // an unallocated blank strip.  Reassign sizes by widget identity using
    // the panel's actual maximum width (== fixed width).
    //
    // When called during buildUI() (issue #2704: restart with
    // AppletPanelDockedLeft=True), the splitter isn't laid out yet and
    // m_splitter->width() is 0 — falling back to the MainWindow's width()
    // matches the source buildUI() itself uses for its initial centerWidth,
    // so the panstack gets its slot instead of being squeezed to a sliver.
    int total = m_splitter->width();
    if (total <= 0)
        total = width();
    if (total > 0) {
        const int appletW = m_appletPanel->maximumWidth();
        const int centerW = qMax(200, total - appletW);
        QList<int> newSizes(m_splitter->count(), 0);
        for (int i = 0; i < m_splitter->count(); ++i) {
            QWidget* w = m_splitter->widget(i);
            if (w == m_panStack)         newSizes[i] = centerW;
            else if (w == m_appletPanel) newSizes[i] = appletW;
        }
        m_splitter->setSizes(newSizes);
    }

    // Scroll bar to the outside edge: against the window edge when docked
    // left, default position (right of the panel) when docked right.
    m_appletPanel->setScrollBarOnLeft(left);

    AppSettings::instance().setValue("AppletPanelDockedLeft", left ? "True" : "False");
    AppSettings::instance().save();

    if (m_titleBar)
        m_titleBar->setAppletDockState(m_appletPanel->isVisible(), left);
}

void MainWindow::setAppletPanelVisible(bool visible)
{
    if (!m_appletPanel) return;

    // AppletPanel::setFixedWidth(260) means Qt restores the same width on
    // un-hide automatically — the splitter just shrinks PanStack (stretch=1)
    // by 260 and gives the slot back to the applet.
    m_appletPanel->setVisible(visible);

    AppSettings::instance().setValue("AppletPanelVisible", visible ? "True" : "False");
    AppSettings::instance().save();
    if (m_titleBar) {
        const bool dockedLeft = AppSettings::instance()
            .value("AppletPanelDockedLeft", "False").toString() == "True";
        m_titleBar->setAppletDockState(visible, dockedLeft);
    }
}

void MainWindow::toggleAppletPanelFloating(bool floating)
{
    if (floating) floatAppletPanel();
    else          dockAppletPanel();
    AppSettings::instance().setValue(
        "AppletPanelFloating", floating ? "True" : "False");
    AppSettings::instance().save();
    if (m_titleBar) {
        m_titleBar->setAppletFloating(floating);
        // While floating, the panel lives in its own window — neither
        // dock-side icon should remain illuminated.  When re-docking,
        // dockAppletPanel restores the persisted side and re-syncs the
        // dock-side icon via setAppletPanelDockedLeft.
        if (floating) {
            const bool dockedLeft = AppSettings::instance()
                .value("AppletPanelDockedLeft", "False").toString() == "True";
            m_titleBar->setAppletDockState(false, dockedLeft);
        }
    }
}

void MainWindow::setFramelessWindow(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("FramelessWindow", on ? "True" : "False");
    s.save();

    // setWindowFlags() re-creates the native window — save and restore
    // geometry so the window stays where the user put it.
    const QRect geom = geometry();
    const bool wasVisible = isVisible();
    Qt::WindowFlags flags = windowFlags();
#ifdef Q_OS_WIN
    if (flags & Qt::FramelessWindowHint) {
        flags &= ~Qt::FramelessWindowHint;
        setWindowFlags(flags);
        setGeometry(geom);
        if (wasVisible) {
            show();
        }
    }
    applyWindowsCustomFrame();
#else
    if (on)
        flags |= Qt::FramelessWindowHint;
    else
        flags &= ~Qt::FramelessWindowHint;
    setWindowFlags(flags);
    setGeometry(geom);
    if (wasVisible)
        show();
#endif

    // Keep the bottom-right size grip in sync — only useful when frameless.
    if (m_sizeGrip) m_sizeGrip->setVisible(on);

    // Propagate to all currently-floating child windows so they match.
    if (m_panStack) m_panStack->setFramelessMode(on);
    if (m_appletPanel && m_appletPanel->containerManager())
        m_appletPanel->containerManager()->setFramelessMode(on);
    if (m_connPanel)
        m_connPanel->setFramelessMode(on);
    // RadioSetupDialog frameless propagation flows through the
    // m_persistentDialogs loop below (#2781) — all four entry points use
    // showOrRaisePersistent so the dialog is always tracked there.
    if (m_reconnectDlg && m_reconnectDlg->findChild<QWidget*>("framelessWindowTitleBar")) {
        setDialogFramelessMode(m_reconnectDlg, on);
    }
    if (m_aetherialStrip)
        m_aetherialStrip->setFramelessMode(on);
    // Propagate to every PersistentDialog-derived dialog created via
    // showOrRaisePersistent().  QPointer entries auto-null on dialog close
    // (WA_DeleteOnClose); prune those as we go so the list doesn't grow
    // monotonically.
    for (auto it = m_persistentDialogs.begin(); it != m_persistentDialogs.end(); ) {
        if (PersistentDialog* dlg = it->data()) {
            dlg->setFramelessMode(on);
            ++it;
        } else {
            it = m_persistentDialogs.erase(it);
        }
    }
    setEditorFramelessMode(m_clientEqEditor, on);
    setEditorFramelessMode(m_clientCompEditor, on);
    setEditorFramelessMode(m_clientGateEditor, on);
    setEditorFramelessMode(m_clientTubeEditor, on);
    setEditorFramelessMode(m_clientPuduEditor, on);
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget && widget->objectName() == "quindarToneEditor") {
            setDialogFramelessMode(qobject_cast<QDialog*>(widget), on);
        }
    }
}

void MainWindow::toggleAetherialStrip()
{
    if (!m_aetherialStrip) {
        m_aetherialStrip = new AetherialAudioStrip(m_audio, this);
        // Override the parent-window relationship so the strip behaves as
        // an independent window (own taskbar entry, raisable separately).
        m_aetherialStrip->setWindowFlag(Qt::Window, true);
        // Secondary window — must not gate quitOnLastWindowClosed on Windows.
        m_aetherialStrip->setAttribute(Qt::WA_QuitOnClose, false);
        // Seed the embedded EQ with the current TX filter cutoff values
        // so the dashed yellow guide lines render immediately rather than
        // waiting for the next txFilterCutoffChanged signal.
        const auto& tx = m_radioModel.transmitModel();
        m_aetherialStrip->setTxFilterCutoffs(tx.txFilterLow(), tx.txFilterHigh());
        // Drag-to-adjust on the strip's EQ cutoff lines → same handler
        // the floating ClientEqEditor uses, so dragging in the strip
        // writes the same TX filter command to the radio.
        connect(m_aetherialStrip, &AetherialAudioStrip::cutoffsDragRequested,
                this, &MainWindow::onEqCutoffsDragRequested);
        // Wire the strip's RX ADSP widget through the same parameter-
        // change handlers the Settings dialog and docked applet use.
        // Without this, NR2/NR4/DFNR/BNR/MNR controls in the strip
        // emit signals that nothing receives.
        if (auto* adsp = m_aetherialStrip->adspWidget())
            wireAetherDspWidget(adsp);
        // Stage bypass via the strip's chain tiles → same handler the
        // docked Chain applet's signal connects to, so both chain
        // widgets repaint and the matching applet refreshes.
        connect(m_aetherialStrip, &AetherialAudioStrip::stageEnabledChanged,
                this, &MainWindow::onTxChainStageEnabledChanged);

        // RX chain wiring — sibling of the TX hookups above (#2425).
        // Stage bypass on an RX tile fans out to: docked chain applet
        // (so its painted tile repaints), and per-stage RX applets so
        // their Enable toggles stay aligned with the engine state.
        connect(m_aetherialStrip, &AetherialAudioStrip::rxStageEnabledChanged,
                this, [this](AudioEngine::RxChainStage stage, bool /*enabled*/) {
            if (auto* dockedChain = m_appletPanel
                    ? m_appletPanel->clientChainApplet() : nullptr) {
                dockedChain->refreshFromEngine();
            }
            if (!m_appletPanel) return;
            switch (stage) {
                case AudioEngine::RxChainStage::Eq:
                    if (m_appletPanel->clientEqRxApplet())
                        m_appletPanel->clientEqRxApplet()->refreshEnableFromEngine();
                    break;
                case AudioEngine::RxChainStage::Gate:
                    if (m_appletPanel->clientGateRxApplet())
                        m_appletPanel->clientGateRxApplet()->refreshEnableFromEngine();
                    break;
                case AudioEngine::RxChainStage::Comp:
                    if (m_appletPanel->clientCompRxApplet())
                        m_appletPanel->clientCompRxApplet()->refreshEnableFromEngine();
                    break;
                case AudioEngine::RxChainStage::Tube:
                    if (m_appletPanel->clientTubeRxApplet())
                        m_appletPanel->clientTubeRxApplet()->refreshEnableFromEngine();
                    break;
                case AudioEngine::RxChainStage::Pudu:
                    if (m_appletPanel->clientPuduRxApplet())
                        m_appletPanel->clientPuduRxApplet()->refreshEnableFromEngine();
                    break;
                default:
                    break;
            }
        });
        // RX stage double-click → open the RX-side floating editor for
        // that stage.  Mirrors the docked applet's rxEditRequested hook.
        connect(m_aetherialStrip, &AetherialAudioStrip::rxStageEditRequested,
                this, [this](AudioEngine::RxChainStage stage) {
            switch (stage) {
                case AudioEngine::RxChainStage::Eq:
                    ensureClientEqEditor()->showForPath(ClientEqApplet::Path::Rx);
                    break;
                case AudioEngine::RxChainStage::Gate:
                    ensureClientGateEditor()->showForRx();
                    break;
                case AudioEngine::RxChainStage::Comp:
                    ensureClientCompEditor()->showForRx();
                    break;
                case AudioEngine::RxChainStage::Tube:
                    ensureClientTubeEditor()->showForRx();
                    break;
                case AudioEngine::RxChainStage::Pudu:
                    ensureClientPuduEditor()->showForRx();
                    break;
                default:
                    break;
            }
        });
        // ADSP launcher tile → open / focus the AetherDsp Settings
        // dialog, same as the Settings menu action.
        connect(m_aetherialStrip, &AetherialAudioStrip::rxDspEditRequested,
                this, [this]() { ensureAetherDspDialog(); });
        // PUDU monitor record / play — same toggle logic as the docked
        // ClientChainApplet.
        connect(m_aetherialStrip, &AetherialAudioStrip::monitorRecordClicked,
                this, [this]() {
            if (m_finalMonitor->isRecording()) {
                m_finalMonitor->stopRecording();
            } else {
                if (m_finalMonitor->isPlaying()) m_finalMonitor->stopPlayback();
                m_finalMonitor->startRecording();
            }
        });
        connect(m_aetherialStrip, &AetherialAudioStrip::monitorPlayClicked,
                this, [this]() {
            if (m_finalMonitor->isPlaying()) m_finalMonitor->stopPlayback();
            else                            m_finalMonitor->startPlayback();
        });
        // Seed the strip with the monitor's current state.
        m_aetherialStrip->setMonitorRecording(m_finalMonitor->isRecording());
        m_aetherialStrip->setMonitorPlaying(m_finalMonitor->isPlaying());
        m_aetherialStrip->setMonitorHasRecording(m_finalMonitor->hasRecording());
        // Seed the chain's MIC-ready + TX-active indicators (reuse the
        // tx alias declared above for the EQ cutoff seeding).
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_aetherialStrip->setMicInputReady(ready);
        m_aetherialStrip->setTxActive(ready && tx.isTransmitting());
    }
    if (m_aetherialStrip->isVisible()) {
        m_aetherialStrip->hide();
    } else {
        m_aetherialStrip->show();
        m_aetherialStrip->raise();
        m_aetherialStrip->activateWindow();
    }
}

void MainWindow::toggleMinimalMode(bool on)
{
    m_minimalMode = on;
    auto& s = AppSettings::instance();

    if (on) {
        // macOS delivers WindowStateChange asynchronously, so the Maximized /
        // FullScreen bit can survive setFixedWidth(260) below and trigger the
        // changeEvent exit path before we've finished entering.  Guard the
        // enter window against re-entry from a deferred WindowStateChange.
        m_enteringMinimalMode = true;

        // Save full-mode geometry (preserves the Maximized/FullScreen bit so
        // exit can restore the user's pre-minimal window).
        s.setValue("FullModeGeometry", saveGeometry().toBase64());

        // Drop maximized/fullscreen state before forcing the applet width.
        // Without this, macOS keeps the bit set through setFixedWidth(260)
        // and changeEvent schedules a spurious toggleMinimalMode(false).
        if (windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
            showNormal();

        // Save splitter sizes for restore
        s.setValue("MinimalModeSplitterSizes",
            QString::fromLatin1(m_splitter->saveState().toBase64()));

        // Suspend spectrum rendering to save CPU (skip floating pans —
        // they remain visible in their own top-level window)
        if (m_panStack) {
            for (auto* a : m_panStack->allApplets()) {
                if (!m_panStack->isFloating(a->panId()))
                    a->spectrumWidget()->setUpdatesEnabled(false);
            }
        }

        // Hide the splitter (contains spectrum + applet panel) and reparent
        // the applet panel directly into the central layout
        m_splitter->hide();
        m_appletPanel->setParent(centralWidget());
        centralWidget()->layout()->addWidget(m_appletPanel);
        m_appletPanel->show();

        // Strip title bar to heartbeat + logo + restore/feature buttons
        m_titleBar->setMinimalMode(true);
        statusBar()->hide();

        // Force window to applet width
        setMinimumSize(0, 0);
        setFixedWidth(260);

        QByteArray geom = QByteArray::fromBase64(
            s.value("MinimalModeGeometry", "").toByteArray());
        if (!geom.isEmpty())
            restoreGeometry(geom);

        // Defer clearing the guard so any AppKit-deferred WindowStateChange
        // queued by the showNormal() / setFixedWidth() calls above is drained
        // through changeEvent's early-return before the guard drops.
        QTimer::singleShot(0, this, [this]() { m_enteringMinimalMode = false; });

    } else {
        // Sync the View-menu action so non-action entry points (the
        // title-bar maximize button, WM-driven maximize via changeEvent)
        // leave the menu checkbox in the right state.  Blocker prevents
        // toggled→toggleMinimalMode recursion.
        if (m_minimalModeAction) {
            QSignalBlocker b(m_minimalModeAction);
            m_minimalModeAction->setChecked(false);
        }

        // If the WM/double-click maximized or fullscreened us before we
        // got here, the current geometry is the maximized rect — not a
        // useful "minimal mode" geometry to persist.  Un-maximize first
        // and skip the save.  The normal Ctrl+M / maximize-button paths
        // arrive at minimal width with no abnormal state and save as usual.
        const bool abnormalState =
            windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen);
        if (abnormalState)
            showNormal();
        else
            s.setValue("MinimalModeGeometry", saveGeometry().toBase64());

        // Reparent applet panel back into the splitter and restore layout
        m_splitter->addWidget(m_appletPanel);
        m_appletPanel->show();
        QByteArray splitterState = QByteArray::fromBase64(
            s.value("MinimalModeSplitterSizes", "").toByteArray());
        if (!splitterState.isEmpty())
            m_splitter->restoreState(splitterState);
        m_splitter->show();

        // Resume spectrum rendering
        if (m_panStack) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setUpdatesEnabled(true);
        }

        // Release fixed width and restore minimum size
        setFixedWidth(QWIDGETSIZE_MAX);
        setMinimumSize(1024, 400);

        // Restore title bar and status bar
        m_titleBar->setMinimalMode(false);
        statusBar()->show();

        // Restore full geometry
        QByteArray geom = QByteArray::fromBase64(
            s.value("FullModeGeometry", "").toByteArray());
        if (!geom.isEmpty())
            restoreGeometry(geom);

        // Belt-and-suspenders: if FullModeGeometry encoded a state, ensure
        // we land windowed.
        showNormal();
    }

    s.setValue("MinimalModeEnabled", on ? "True" : "False");
    s.save();
}

SpectrumWidget* MainWindow::spectrumForSlice(SliceModel* s) const
{
    if (s && m_panStack) {
        auto* sw = m_panStack->spectrum(s->panId());
        if (sw) return sw;
    }
    return spectrum();  // fallback to active pan
}

void MainWindow::showPanadapterInterlockNotification(const QString& message)
{
    SliceModel* target = nullptr;
    for (auto* s : m_radioModel.slices()) {
        if (s && s->isTxSlice()) {
            target = s;
            break;
        }
    }
    if (!target)
        target = activeSlice();

    if (auto* sw = spectrumForSlice(target))
        sw->showInterlockNotification(message, 5000);
}

// ─── Pan layout application ───────────────────────────────────────────────────

// ─── Keyboard Shortcuts ───────────────────────────────────────────────────────

void MainWindow::updateKeyerAvailability(const QString& mode)
{
    static const QString kActive   = "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }";
    static const QString kAvail    = "QLabel { color: #404858; font-weight: bold; font-size: 24px; }";
    static const QString kDisabled = "QLabel { color: #252530; font-weight: bold; font-size: 24px; }";

    bool isCw  = (mode == "CW" || mode == "CWL");
    bool isSsb = (mode == "USB" || mode == "LSB" || mode == "AM" || mode == "SAM"
                  || mode == "FM" || mode == "NFM" || mode == "DFM");

    // F1-F12 / Esc ApplicationShortcuts: enable the set that matches the
    // active slice's mode, regardless of panel visibility.  The two sets
    // are mutually exclusive so Qt never sees two enabled shortcuts for
    // the same key and won't emit activatedAmbiguously (#2464, #2582).
    if (m_cwxPanel) m_cwxPanel->setShortcutsEnabled(isCw);
    if (m_dvkPanel) m_dvkPanel->setShortcutsEnabled(isSsb);

    // CWX: available in CW modes only
    m_cwxIndicator->setEnabled(isCw);
    if (!isCw && m_cwxPanel->isVisible()) {
        m_cwxPanel->hide();
        m_cwxIndicator->setStyleSheet(kDisabled);
    } else if (m_cwxPanel->isVisible()) {
        m_cwxIndicator->setStyleSheet(kActive);
    } else {
        m_cwxIndicator->setStyleSheet(isCw ? kAvail : kDisabled);
    }
    m_cwxIndicator->setCursor(isCw ? Qt::PointingHandCursor : Qt::ArrowCursor);

    // DVK: available in voice modes (SSB, AM, FM — not DIGU/DIGL)
    m_dvkIndicator->setEnabled(isSsb);
    if (!isSsb && m_dvkPanel->isVisible()) {
        m_dvkPanel->hide();
        m_dvkIndicator->setStyleSheet(kDisabled);
    } else if (m_dvkPanel->isVisible()) {
        m_dvkIndicator->setStyleSheet(kActive);
    } else {
        m_dvkIndicator->setStyleSheet(isSsb ? kAvail : kDisabled);
    }
    m_dvkIndicator->setCursor(isSsb ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void MainWindow::centerActiveSliceInPanadapter(bool forceRadioCenter, double centerMhz)
{
    auto* s = activeSlice();
    if (!s || s->panId().isEmpty()) return;

    auto* sw = spectrumForSlice(s);
    if (!sw) return;

    auto* pan = m_radioModel.panadapter(s->panId());
    const double bandwidthMhz = pan ? pan->bandwidthMhz() : m_radioModel.panBandwidthMhz();
    const double targetMhz = (centerMhz > 0.0) ? centerMhz : s->frequency();

    if (m_panStack) {
        if (auto* applet = m_panStack->panadapter(s->panId()))
            m_panStack->setActivePan(applet->panId());
    }

    // Keep the local spectrum centered immediately so the active slice marker
    // is visible before the radio's status echo arrives.
    sw->setFrequencyRange(targetMhz, bandwidthMhz);
    sw->setVfoFrequency(targetMhz);

    if (forceRadioCenter && m_radioModel.isConnected()) {
        m_radioModel.sendCommand(
            QString("display pan set %1 center=%2")
                .arg(s->panId()).arg(targetMhz, 0, 'f', 6));
    }

    TuneCenteringResult result;
    result.oldCenterMhz = pan ? pan->centerMhz() : targetMhz;
    result.newCenterMhz = targetMhz;
    result.bandwidthMhz = bandwidthMhz;
    result.followRevealTriggered = true;
    result.hardCenterUsed = true;
    logTunePolicyDecision("center-active-slice", TuneIntent::AbsoluteJump,
                          s->frequency(), s->frequency(), result);
}

// registerShortcutActions() lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
void MainWindow::showNr2ParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(this);

    popup->addSlider("Reduction (dB)", 10, 300,
        static_cast<int>(s.value("NR2GainMax", "1.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2GainMax", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2GainMax(val); });
        });

    popup->addSlider("Smoothing",  50, 98,
        static_cast<int>(s.value("NR2GainSmooth", "0.85").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2GainSmooth", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2GainSmooth(val); });
        });

    popup->addSlider("Voice Threshold", 1, 50,
        static_cast<int>(s.value("NR2Qspp", "0.20").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2Qspp", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2Qspp(val); });
        });

    popup->addCheckbox("AE Filter",
        s.value("NR2AeFilter", "True").toString() == "True",
        [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("NR2AeFilter", on ? "True" : "False");
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setNr2AeFilter(on); });
        });

    popup->finalize(
        [this]() { ensureAetherDspDialog(); },
        nullptr  // Reset handled by individual control resetters
    );

    popup->showAt(globalPos);
}

void MainWindow::showNr4ParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(this);

    popup->addSlider("Reduction (dB)", 0, 400,
        static_cast<int>(s.value("NR4ReductionAmount", "10.0").toFloat() * 10),
        [](int v) { return QString::number(v / 10.0f, 'f', 1); },
        [this](int v) {
            float val = v / 10.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4ReductionAmount", QString::number(val, 'f', 1));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr4ReductionAmount(val); });
        });

    popup->addSlider("Smoothing (%)", 0, 100,
        static_cast<int>(s.value("NR4SmoothingFactor", "0.0").toFloat()),
        [](int v) { return QString::number(v); },
        [this](int v) {
            auto& s = AppSettings::instance();
            s.setValue("NR4SmoothingFactor", QString::number(static_cast<float>(v), 'f', 1));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4SmoothingFactor(static_cast<float>(v)); });
        });

    popup->addSlider("Masking Depth", 0, 100,
        static_cast<int>(s.value("NR4MaskingDepth", "0.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4MaskingDepth", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr4MaskingDepth(val); });
        });

    popup->addSlider("Suppression", 0, 100,
        static_cast<int>(s.value("NR4SuppressionStrength", "0.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4SuppressionStrength", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr4SuppressionStrength(val); });
        });

    popup->addCheckbox("Adaptive Noise",
        s.value("NR4AdaptiveNoise", "True").toString() == "True",
        [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("NR4AdaptiveNoise", on ? "True" : "False");
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setNr4AdaptiveNoise(on); });
        });

    popup->finalize(
        [this]() { ensureAetherDspDialog(); },
        nullptr  // Reset handled by individual control resetters
    );

    popup->showAt(globalPos);
}

void MainWindow::showDfnrParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(this);

    popup->addSlider("Attenuation Limit (dB)", 0, 100,
        static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat()),
        [](int v) { return QString::number(v); },
        [this](int v) {
            float db = static_cast<float>(v);
            auto& s = AppSettings::instance();
            s.setValue("DfnrAttenLimit", QString::number(db, 'f', 0));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, db]() { m_audio->setDfnrAttenLimit(db); });
        });

    popup->addSlider("Post-Filter Beta", 0, 30,
        static_cast<int>(s.value("DfnrPostFilterBeta", "0.0").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float beta = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("DfnrPostFilterBeta", QString::number(beta, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_audio, [this, beta]() { m_audio->setDfnrPostFilterBeta(beta); });
        });

    popup->finalize(
        [this]() { ensureAetherDspDialog(); },
        nullptr
    );

    popup->showAt(globalPos);
}

void MainWindow::showMnrSettings()
{
    if (auto* dlg = ensureAetherDspDialog()) {
        dlg->selectTab("MNR");
    }
}

void MainWindow::applyPanLayout(const QString& layoutId)
{
    if (!m_radioModel.isConnected()) return;

    const int needed = panCountForLayoutId(layoutId);
    const int existing = m_panStack->count();

    if (needed < existing) {
        qDebug() << "applyPanLayout: reducing from" << existing << "to" << needed << "pans";

        // Close extra pans from the end (keep the first N)
        auto allApplets = m_panStack->allApplets();
        int toRemove = existing - needed;
        for (int i = allApplets.size() - 1; i >= 0 && toRemove > 0; --i) {
            auto* applet = allApplets[i];
            QString panId = applet->panId();
            if (panId == "default") continue;
            qDebug() << "applyPanLayout: closing pan" << panId;
            m_radioModel.sendCommand(
                QString("display pan remove %1").arg(panId));
            // Radio will send "removed" status → panadapterRemoved signal
            // → PanadapterStack::removePanadapter()
            --toRemove;
        }

        // Rearrange remaining pans after a short delay for radio to process
        QTimer::singleShot(500, this, [this, layoutId]() {
            m_panStack->rearrangeLayout(layoutId);
        });
        return;
    }
    if (needed == existing) {
        // Same count, just rearrange
        m_panStack->rearrangeLayout(layoutId);
        return;
    }

    // Create additional pans to reach the needed count.
    // Keep existing pan(s) alive — no tear-down, no dangling signals.
    const int currentSliceCount = static_cast<int>(m_radioModel.slices().size());
    if (currentSliceCount >= m_radioModel.maxSlices()) {
        showPanadapterSliceCapacityMessage();
        return;
    }

    const int toCreate = needed - existing;
    auto panIds = std::make_shared<QStringList>();

    // Collect existing pan IDs first (they'll be part of the layout)
    for (auto* applet : m_panStack->allApplets())
        panIds->append(applet->panId());

    qDebug() << "applyPanLayout: have" << existing << "pans, creating"
             << toCreate << "more for layout" << layoutId;

    createPansSequentially(layoutId, toCreate, panIds, 0);
}

void MainWindow::createPansSequentially(const QString& layoutId, int total,
                                        std::shared_ptr<QStringList> panIds, int created)
{
    if (created >= total) {
        // All new pans created — wait for radio status to establish PanadapterModels
        qDebug() << "applyPanLayout: all" << total << "new pans created:" << *panIds;
        QTimer::singleShot(800, this, [this, panIds, layoutId]() {
            // The new pans were added to the stack via panadapterAdded handler.
            // Wire any that aren't already wired.
            for (auto* applet : m_panStack->allApplets()) {
                const QString pid = applet->panId();
                auto* pan = m_radioModel.panadapter(pid);
                if (pan) {
                    // Push current state to the spectrum widget
                    applet->spectrumWidget()->setDbmRange(pan->minDbm(), pan->maxDbm());
                    applet->spectrumWidget()->setFrequencyRange(
                        pan->centerMhz(), pan->bandwidthMhz());
                }
            }

            // Rearrange splitter structure for the selected layout
            m_panStack->rearrangeLayout(layoutId);

            m_panApplet = m_panStack->activeApplet();

            qDebug() << "applyPanLayout: layout" << layoutId
                     << "complete, total pans:" << m_panStack->count();
        });
        return;
    }

    m_radioModel.sendCmdPublic(
        "display panafall create x=100 y=100",
        [this, panIds, layoutId, total, created](int code, const QString& body) {
            if (code != 0) {
                qWarning() << "applyPanLayout: panafall create failed, code"
                           << Qt::hex << code << body;
                showPanadapterSliceCapacityMessage();
                return;
            }
            const QString panId = RadioStatusOwnership::parsePanafallCreatePanId(body);

            qDebug() << "applyPanLayout: created pan" << (created + 1) << "of" << total
                     << "id:" << panId;

            if (!panId.isEmpty()) {
                panIds->append(panId);
                // Configure the pan
                m_radioModel.sendCommand(
                    QString("display pan set %1 xpixels=1024 ypixels=700").arg(panId));
            }

            // Create next pan after a brief delay
            QTimer::singleShot(200, this, [this, layoutId, total, panIds, created]() {
                createPansSequentially(layoutId, total, panIds, created + 1);
            });
        });
}

void MainWindow::showPanadapterSliceCapacityMessage()
{
    const int limit = m_radioModel.maxSlices();
    const QString model = m_radioModel.model().isEmpty()
        ? QStringLiteral("This radio")
        : m_radioModel.model();
    statusBar()->showMessage(
        QStringLiteral("Slice capacity is full; cannot add another panadapter (%1 supports %2 slice%3)")
            .arg(model)
            .arg(limit)
            .arg(limit == 1 ? QString() : QStringLiteral("s")),
        kPanadapterSliceCapacityStatusMs);
}

// ─── Band settings capture / restore ──────────────────────────────────────────

BandSnapshot MainWindow::captureCurrentBandState() const
{
    BandSnapshot snap;
    if (auto* s = activeSlice()) {
        snap.frequencyMhz  = s->frequency();
        snap.mode          = s->mode();
        snap.rxAntenna     = s->rxAntenna();
        snap.filterLow     = s->filterLow();
        snap.filterHigh    = s->filterHigh();
        snap.agcMode       = s->agcMode();
        snap.agcThreshold  = s->agcThreshold();
    }
    // Center and bandwidth are radio-authoritative — don't capture.
    if (auto* sw = spectrum()) {
        snap.minDbm          = sw->refLevel() - sw->dynamicRange();
        snap.maxDbm          = sw->refLevel();
        snap.spectrumFrac    = sw->spectrumFrac();
        snap.rfGain          = sw->rfGainValue();
        snap.wnbOn           = sw->wnbActive();
    }
    return snap;
}

void MainWindow::restoreBandState(const BandSnapshot& snap)
{
    m_updatingFromModel = true;
    if (auto* s = activeSlice()) {
        s->setMode(snap.mode);
        TuneCenteringResult result;
        if (auto* pan = m_radioModel.panadapter(s->panId())) {
            result.oldCenterMhz = pan->centerMhz();
            result.bandwidthMhz = pan->bandwidthMhz();
        }
        result.newCenterMhz = snap.frequencyMhz;
        result.followRevealTriggered = true;
        result.hardCenterUsed = true;
        logTunePolicyDecision("restore-band-state", TuneIntent::AbsoluteJump,
                              s->frequency(), snap.frequencyMhz, result);
        s->tuneAndRecenter(snap.frequencyMhz);
        if (!snap.rxAntenna.isEmpty())
            s->setRxAntenna(snap.rxAntenna);
        s->setFilterWidth(snap.filterLow, snap.filterHigh);
        if (!snap.agcMode.isEmpty())
            s->setAgcMode(snap.agcMode);
        s->setAgcThreshold(snap.agcThreshold);
    }
    if (auto* pan = m_radioModel.activePanadapter()) {
        // Don't push center or bandwidth — slice tune recenters the pan and
        // the radio determines bandwidth. Pushing stale saved values causes
        // FFT/waterfall misalignment during the transition (#279, #291).
        // Only restore dBm scale (client-side display preference).
        m_radioModel.sendCommand(
            QString("display pan set %1 min_dbm=%2 max_dbm=%3").arg(pan->panId())
                .arg(static_cast<double>(snap.minDbm), 0, 'f', 2)
                .arg(static_cast<double>(snap.maxDbm), 0, 'f', 2));
    }
    m_radioModel.setPanRfGain(snap.rfGain);
    m_radioModel.setPanWnb(snap.wnbOn);
    if (auto* sw = spectrum()) {
        sw->setSpectrumFrac(snap.spectrumFrac);
        sw->setRfGain(snap.rfGain);
        sw->setWnbActive(snap.wnbOn);
    }
    m_updatingFromModel = false;
}

SliceModel* MainWindow::swrSweepTargetSlice(int requestedSliceId) const
{
    if (requestedSliceId >= 0)
        return m_radioModel.slice(requestedSliceId);

    if (auto* s = activeSlice(); s && s->isTxSlice())
        return s;

    for (auto* s : m_radioModel.slices()) {
        if (s && s->isTxSlice())
            return s;
    }

    return activeSlice();
}

void MainWindow::setSwrSweepInputsLocked(bool locked)
{
    if (locked) {
        m_swrSweep.appletPanelWasEnabled = !m_appletPanel || m_appletPanel->isEnabled();
        m_swrSweep.panStackWasEnabled = !m_panStack || m_panStack->isEnabled();
        if (m_appletPanel)
            m_appletPanel->setEnabled(false);
        if (m_panStack)
            m_panStack->setEnabled(false);
        setFocus(Qt::OtherFocusReason);
        return;
    }

    if (m_appletPanel)
        m_appletPanel->setEnabled(m_swrSweep.appletPanelWasEnabled);
    if (m_panStack)
        m_panStack->setEnabled(m_swrSweep.panStackWasEnabled);
}

void MainWindow::clearSwrSweepPlot()
{
    if (m_swrSweep.running) {
        m_swrSweep.clearPlotOnFinish = true;
        finishSwrSweep(true, QStringLiteral("SWR sweep cleared"));
        return;
    }

    m_swrSweep.samples.clear();
    m_swrSweep.sourceLabel.clear();
    m_swrSweep.originalBandName.clear();
    m_swrSweep.preserveBandSwitchOnFinish = false;
    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }
    statusBar()->showMessage(QStringLiteral("SWR sweep plot cleared"), 2500);
}

void MainWindow::clearSwrSweepForBandChange(int sliceId, const QString& panId,
                                            const QString& newBandName)
{
    if (m_swrSweep.originalBandName.isEmpty()
        || newBandName.isEmpty()
        || newBandName == m_swrSweep.originalBandName) {
        return;
    }

    if (!panId.isEmpty() && !m_swrSweep.panId.isEmpty() && panId != m_swrSweep.panId)
        return;

    if (sliceId >= 0 && m_swrSweep.sliceId >= 0 && sliceId != m_swrSweep.sliceId)
        return;

    if (m_swrSweep.running) {
        m_swrSweep.clearPlotOnFinish = true;
        m_swrSweep.preserveBandSwitchOnFinish = true;
        finishSwrSweep(true, QStringLiteral("SWR sweep disabled on band change"));
        return;
    }

    if (m_swrSweep.samples.isEmpty())
        return;

    m_swrSweep.samples.clear();
    m_swrSweep.sourceLabel.clear();
    m_swrSweep.originalBandName.clear();
    m_swrSweep.preserveBandSwitchOnFinish = false;
    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }
    statusBar()->showMessage(QStringLiteral("SWR sweep cleared on band change"), 2500);
}

void MainWindow::updateSwrSweepOverlay(double currentFreqMhz)
{
    QVector<SpectrumWidget::SwrSweepPoint> points;
    points.reserve(m_swrSweep.samples.size());
    for (const auto& sample : m_swrSweep.samples)
        points.append({sample.freqMhz, sample.swr});

    if (!m_panStack)
        return;

    for (auto* applet : m_panStack->allApplets()) {
        auto* sw = applet ? applet->spectrumWidget() : nullptr;
        if (!sw)
            continue;
        if (applet->panId() == m_swrSweep.panId) {
            sw->setSwrSweepPoints(points, m_swrSweep.running, currentFreqMhz,
                                  m_swrSweep.sourceLabel);
        } else if (m_swrSweep.running) {
            sw->clearSwrSweepPoints();
        }
    }
}

void MainWindow::commandSwrSweepFrequency(double freqMhz, int settleMs)
{
    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    s->setFrequency(freqMhz);
    if (auto* sw = spectrumForSlice(s))
        sw->setSliceOverlayFreq(s->sliceId(), freqMhz);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_swrSweep.commandIssuedAtMs = now;
    m_swrSweep.sampleNotBeforeMs = now + settleMs;
    updateSwrSweepOverlay(freqMhz);
}

void MainWindow::beginSwrSweepRf()
{
    if (!m_swrSweep.running)
        return;

    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    if (m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl) {
        const auto& tuner = m_radioModel.tunerModel();
        if (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL bypass was not confirmed"));
            return;
        }
    }

    const QString bandName = BandSettings::bandForFrequency(m_swrSweep.originalFreqMhz);
    const BandDef& band = BandSettings::bandDef(bandName);
    const double displayCenter = (band.lowMhz + band.highMhz) * 0.5;
    const double displayBw = (band.highMhz - band.lowMhz) + 2.0 * kSwrSweepPanPaddingMhz;

    m_swrSweep.phase = SwrSweepPhase::Sweeping;
    m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    applyPanRangeRequest(m_swrSweep.panId, displayCenter, displayBw, "swr-sweep");

    commandSwrSweepFrequency(m_swrSweep.frequencies.first(), kSwrSweepInitialSettleMs);
    m_radioModel.transmitModel().startTune();
    m_swrSweep.tuneStarted = true;
    m_swrSweepTimer.start(kSwrSweepPollMs);

    statusBar()->showMessage(
        tr("SWR sweep running on %1 with Tune Power %2 W%3. Press Esc to stop.")
            .arg(QString::fromLatin1(band.name))
            .arg(m_swrSweep.sweepTunePower)
            .arg(m_swrSweep.sourceLabel.isEmpty()
                     ? QString()
                     : QStringLiteral(" (%1)").arg(m_swrSweep.sourceLabel)),
        5000);
}

void MainWindow::startSwrSweep(int requestedSliceId, int sweepPowerWatts)
{
    if (m_swrSweep.running)
        return;

    if (!m_radioModel.isConnected()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Connect to a radio before starting an SWR sweep."));
        return;
    }
    if (m_splitActive) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Disable split before running an SWR sweep."));
        return;
    }

    auto* s = swrSweepTargetSlice(requestedSliceId);
    if (!s || !s->isTxSlice()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Select the TX slice before running an SWR sweep."));
        return;
    }
    if (s->isLocked()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Unlock the TX slice before running an SWR sweep."));
        return;
    }
    if (s->panId().isEmpty() || !spectrumForSlice(s)) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("The TX slice needs a visible panadapter before running an SWR sweep."));
        return;
    }

    auto& tx = m_radioModel.transmitModel();
    if (tx.isTuning() || tx.isMox() || tx.isTransmitting()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Stop transmit or tune before starting an SWR sweep."));
        return;
    }
    if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Put the Power Genius XL amplifier in STANDBY before running an SWR sweep."));
        return;
    }

    const QString bandName = BandSettings::bandForFrequency(s->frequency());
    const BandDef& band = BandSettings::bandDef(bandName);
    if (bandName == QLatin1String("GEN") || band.lowMhz <= 0.0 || band.highMhz <= band.lowMhz) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("The current TX frequency is not inside a supported amateur band."));
        return;
    }
    if (bandName == QLatin1String("60m")) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("SWR sweep is disabled on 60 m because the band is channelized."));
        return;
    }

    // Narrow the sweep range to the active regional band plan when one is
    // loaded.  BandDefs.h holds ARRL/US allocations only; without this the
    // sweep transmits outside the user's region (e.g. past 7.200 MHz on
    // 40 m for IARU R1) and trips the radio's interlock.  Mirrors the
    // pattern used by AtuPreTuneDialog::recomputeBands. (#2800)
    //
    // Today the SWR sweep treats the band as a single contiguous range —
    // discrete-channel bands like US 60 m are hard-blocked above. We use
    // contiguousRegionsForBand() (#2822) and union the regions so a
    // future enhancement can walk each region individually without
    // touching the per-region calculation.
    double effectiveLow = band.lowMhz;
    double effectiveHigh = band.highMhz;
    if (m_bandPlanMgr) {
        const auto regions =
            m_bandPlanMgr->contiguousRegionsForBand(band.lowMhz, band.highMhz);
        if (!regions.isEmpty()) {
            effectiveLow = std::max(effectiveLow, regions.first().lowMhz);
            effectiveHigh = std::min(effectiveHigh, regions.last().highMhz);
        }
    }

    const double safeLow = effectiveLow + kSwrSweepEdgeGuardMhz;
    const double safeHigh = effectiveHigh - kSwrSweepEdgeGuardMhz;
    if (safeHigh <= safeLow) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band is too narrow for the configured SWR sweep guard."));
        return;
    }

    const double displayBw = (band.highMhz - band.lowMhz) + 2.0 * kSwrSweepPanPaddingMhz;
    if (displayBw > m_radioModel.maxPanBandwidthMhz()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band is wider than the radio can display in one panadapter."));
        return;
    }

    QVector<double> sweepFreqs;
    for (double f = safeLow; f <= safeHigh + 1.0e-9; f += kSwrSweepStepMhz)
        sweepFreqs.append(std::round(f * 1.0e6) / 1.0e6);
    if (sweepFreqs.isEmpty()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("No in-band sweep points were available."));
        return;
    }
    if (sweepFreqs.size() > kSwrSweepMaxPoints) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band would need too many sweep points for one fast pass."));
        return;
    }

    // License gate.  First-press shows a modal disclaimer; subsequent
    // presses are silent once the user ticks "Remember my answer" and
    // accepts.  Placed after all preconditions clear so the dialog only
    // fires when an actual transmission would follow.
    if (!SwrSweepLicenseDialog::confirm(this)) {
        return;
    }

    m_swrSweep = SwrSweepState{};
    m_swrSweep.running = true;
    m_swrSweep.sliceId = s->sliceId();
    m_swrSweep.panId = s->panId();
    m_swrSweep.originalFreqMhz = s->frequency();
    m_swrSweep.originalBandName = bandName;
    m_swrSweep.frequencies = sweepFreqs;
    m_swrSweep.currentIndex = 0;
    m_swrSweep.originalTunePower = tx.tunePower();
    m_swrSweep.sweepTunePower = sweepPowerWatts;
    m_swrSweep.minimumForwardPowerW = qBound(0.05f,
                                             static_cast<float>(sweepPowerWatts) * 0.05f,
                                             1.0f);
    auto& tuner = m_radioModel.tunerModel();
    m_swrSweep.tgxlOriginalOperate = tuner.isOperate();
    m_swrSweep.tgxlOriginalBypass = tuner.isBypass();
    if (tuner.isPresent() && tuner.isOperate()) {
        m_swrSweep.sourceLabel = QStringLiteral("TGXL BYPASS");
        // Read radio-side SWR even when TGXL is bypassed: in bypass the TGXL
        // is a passive wire-through and stops emitting RL meter packets, so
        // tgxlSwrUpdatedAtMs never advances.  The radio's SWR coupler sees
        // the antenna directly through the bypassed relays — equivalent
        // reading, reliably emitted during the tune carrier.  (#2229)
        m_swrSweep.meterSource = SwrSweepMeterSource::Radio;
        if (!tuner.isBypass()) {
            m_swrSweep.tgxlBypassRequested = true;
            m_swrSweep.tgxlRestoreNeeded = true;
        }
    } else {
        m_swrSweep.sourceLabel = tuner.isPresent()
            ? QStringLiteral("RADIO")
            : QString();
    }

    if (auto* sw = spectrumForSlice(s)) {
        m_swrSweep.originalPanCenterMhz = sw->centerMhz();
        m_swrSweep.originalPanBandwidthMhz = sw->bandwidthMhz();
    }

    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }

    setSwrSweepInputsLocked(true);
    tx.setTunePower(sweepPowerWatts);

    if (m_swrSweep.tgxlBypassRequested) {
        m_swrSweep.phase = SwrSweepPhase::WaitingForTgxlBypass;
        m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        tuner.setBypass(true);
        m_swrSweepTimer.start(kSwrSweepPollMs);
        statusBar()->showMessage(
            tr("Preparing SWR sweep: placing TGXL in BYPASS before applying RF..."),
            5000);
        updateSwrSweepOverlay(-1.0);
        return;
    }

    beginSwrSweepRf();
}

void MainWindow::advanceSwrSweep()
{
    if (!m_swrSweep.running)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto& tuner = m_radioModel.tunerModel();

    if (m_swrSweep.phase == SwrSweepPhase::WaitingForTgxlBypass) {
        if (!tuner.isPresent() || !tuner.isOperate()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL is no longer available"));
            return;
        }
        if (tuner.isBypass()) {
            m_swrSweep.phase = SwrSweepPhase::TgxlBypassSettle;
            m_swrSweep.phaseStartedAtMs = now;
            statusBar()->showMessage(
                tr("TGXL BYPASS confirmed. Waiting for relays to settle..."),
                2500);
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlBypassTimeoutMs) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL did not enter bypass"));
        }
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::TgxlBypassSettle) {
        if (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL left bypass"));
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlRelaySettleMs)
            beginSwrSweepRf();
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::StoppingTune) {
        const bool waitedMinimum = now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTuneStopWaitMs;
        const bool stopped = !m_radioModel.transmitModel().isTuning();
        const bool timedOut = now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTuneStopTimeoutMs;
        if (!waitedMinimum || (!stopped && !timedOut))
            return;

        finishSwrSweepAfterTuneStopped();
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::RestoringTgxl) {
        const bool restored = !tuner.isPresent()
            || tuner.isBypass() == m_swrSweep.tgxlOriginalBypass;
        if (restored) {
            completeSwrSweepFinish();
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlRestoreTimeoutMs) {
            m_swrSweep.tgxlRestoreTimedOut = true;
            completeSwrSweepFinish();
        }
        return;
    }

    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    if (m_swrSweep.phase != SwrSweepPhase::Sweeping)
        return;

    if (m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl
        && (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass())) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: TGXL left bypass"));
        return;
    }

    if (now < m_swrSweep.sampleNotBeforeMs)
        return;

    const auto& meters = m_radioModel.meterModel();
    const bool useTgxlMeters = m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl;
    const bool swrFresh = useTgxlMeters
        ? meters.tgxlSwrUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs
        : meters.swrUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs;
    const bool fwdFresh = useTgxlMeters
        ? meters.tgxlFwdPowerUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs
        : meters.fwdPowerUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs;
    const bool hasForwardPower =
        (useTgxlMeters ? meters.tgxlFwdPower() : meters.fwdPowerInstant())
            >= m_swrSweep.minimumForwardPowerW;
    if (!swrFresh || !fwdFresh || !hasForwardPower) {
        if (now - m_swrSweep.commandIssuedAtMs < kSwrSweepMaxSettleMs)
            return;

        if (!swrFresh) {
            finishSwrSweep(true,
                           useTgxlMeters
                               ? QStringLiteral("SWR sweep stopped: no fresh TGXL SWR meter data")
                               : QStringLiteral("SWR sweep stopped: no fresh SWR meter data"));
        } else {
            finishSwrSweep(true,
                           useTgxlMeters
                               ? QStringLiteral("SWR sweep stopped: no TGXL forward power detected")
                               : QStringLiteral("SWR sweep stopped: no forward power detected"));
        }
        return;
    }

    if (m_swrSweep.currentIndex < 0
        || m_swrSweep.currentIndex >= m_swrSweep.frequencies.size()) {
        finishSwrSweep(false, QStringLiteral("SWR sweep complete"));
        return;
    }

    float swr = useTgxlMeters ? meters.tgxlSwr() : meters.swr();
    if (!std::isfinite(static_cast<double>(swr)) || swr < 1.0f)
        swr = 1.0f;

    const double sampleFreq = m_swrSweep.frequencies[m_swrSweep.currentIndex];
    m_swrSweep.samples.append({sampleFreq, swr});
    updateSwrSweepOverlay(sampleFreq);

    ++m_swrSweep.currentIndex;
    if (m_swrSweep.currentIndex >= m_swrSweep.frequencies.size()) {
        finishSwrSweep(false, QStringLiteral("SWR sweep complete"));
        return;
    }

    commandSwrSweepFrequency(m_swrSweep.frequencies[m_swrSweep.currentIndex],
                             kSwrSweepStepSettleMs);
}

void MainWindow::finishSwrSweep(bool aborted, const QString& reason)
{
    if (!m_swrSweep.running)
        return;

    if (!reason.isEmpty())
        m_swrSweep.finalReason = reason;
    m_swrSweep.finalAborted = m_swrSweep.finalAborted || aborted;

    if (m_swrSweep.phase == SwrSweepPhase::StoppingTune
        || m_swrSweep.phase == SwrSweepPhase::RestoringTgxl) {
        return;
    }

    if (m_radioModel.isConnected() && m_swrSweep.tuneStarted) {
        m_radioModel.transmitModel().stopTune();
        m_swrSweep.phase = SwrSweepPhase::StoppingTune;
        m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_swrSweepTimer.start(kSwrSweepPollMs);
        statusBar()->showMessage(tr("Stopping SWR sweep tune carrier..."), 2500);
        return;
    }

    finishSwrSweepAfterTuneStopped();
}

void MainWindow::finishSwrSweepAfterTuneStopped()
{
    if (!m_swrSweep.running)
        return;

    if (m_radioModel.isConnected()) {
        if (!m_swrSweep.preserveBandSwitchOnFinish) {
            if (auto* s = m_radioModel.slice(m_swrSweep.sliceId);
                s && m_swrSweep.originalFreqMhz > 0.0) {
                s->setFrequency(m_swrSweep.originalFreqMhz);
            }
        }
        if (m_swrSweep.originalTunePower != m_swrSweep.sweepTunePower)
            m_radioModel.transmitModel().setTunePower(m_swrSweep.originalTunePower);

        if (!m_swrSweep.preserveBandSwitchOnFinish
            && m_swrSweep.finalAborted && !m_swrSweep.panId.isEmpty()
            && m_swrSweep.originalPanCenterMhz > 0.0
            && m_swrSweep.originalPanBandwidthMhz > 0.0) {
            applyPanRangeRequest(m_swrSweep.panId,
                                 m_swrSweep.originalPanCenterMhz,
                                 m_swrSweep.originalPanBandwidthMhz,
                                 "swr-sweep-stop");
        }

        auto& tuner = m_radioModel.tunerModel();
        if (m_swrSweep.tgxlRestoreNeeded
            && tuner.isPresent()
            && tuner.isOperate()
            && (tuner.isBypass() != m_swrSweep.tgxlOriginalBypass
                || m_swrSweep.tgxlBypassRequested)) {
            tuner.setBypass(m_swrSweep.tgxlOriginalBypass);
            m_swrSweep.phase = SwrSweepPhase::RestoringTgxl;
            m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_swrSweepTimer.start(kSwrSweepPollMs);
            statusBar()->showMessage(tr("Restoring TGXL tuner state..."), 3000);
            return;
        }
    }

    completeSwrSweepFinish();
}

void MainWindow::completeSwrSweepFinish()
{
    if (!m_swrSweep.running)
        return;

    m_swrSweepTimer.stop();

    const bool clearPlot = m_swrSweep.clearPlotOnFinish;
    const bool aborted = m_swrSweep.finalAborted;
    QString msg = m_swrSweep.finalReason.isEmpty()
        ? (aborted ? QStringLiteral("SWR sweep stopped")
                   : QStringLiteral("SWR sweep complete"))
        : m_swrSweep.finalReason;
    if (m_swrSweep.tgxlRestoreTimedOut)
        msg += QStringLiteral("; TGXL restore was not confirmed");

    m_swrSweep.running = false;
    m_swrSweep.phase = SwrSweepPhase::Idle;

    if (clearPlot) {
        m_swrSweep.samples.clear();
        m_swrSweep.sourceLabel.clear();
        m_swrSweep.originalBandName.clear();
        m_swrSweep.preserveBandSwitchOnFinish = false;
        for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
            if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
                sw->clearSwrSweepPoints();
        }
        msg = QStringLiteral("SWR sweep plot cleared");
    } else {
        updateSwrSweepOverlay(-1.0);
    }

    setSwrSweepInputsLocked(false);

    statusBar()->showMessage(msg, aborted ? 3000 : 5000);
}

// ─── GUI control handlers ─────────────────────────────────────────────────────

#ifdef HAVE_RADE
void MainWindow::activateRADE(int sliceId)
{
    // Guard against duplicate activation (VfoWidget + RxApplet both selecting RADE)
    if (m_radeSliceId == sliceId && m_radeEngine && m_radeEngine->isActive())
        return;

    // If RADE is already active on a different slice, deactivate it first
    if (m_radeSliceId >= 0 && m_radeSliceId != sliceId)
        deactivateRADE();

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    // Capture TX slice owner before potentially moving the badge, so the
    // failure path can restore it.  -1 means no TX slice existed.
    int prevTxSliceId = sliceId;
    if (!s->isTxSlice()) {
        prevTxSliceId = -1;
        for (auto* sl : m_radioModel.slices()) {
            if (sl && sl->isTxSlice()) { prevTxSliceId = sl->sliceId(); break; }
        }
    }

    // RADE needs to be the TX slice so it can transmit modem audio.
    // Move TX badge to the RADE slice automatically.
    if (!s->isTxSlice())
        s->setTxSlice(true);

    // Set radio mode to DIGU/DIGL (passthrough for OFDM modem).
    // Use band convention from BandDefs to pick sideband — 60m is USB
    // despite being below 10 MHz (#875).
    double freqMhz = s->frequency();
    QString mode = "DIGU";
    for (const auto& band : AetherSDR::kBands) {
        if (freqMhz >= band.lowMhz && freqMhz <= band.highMhz) {
            mode = (QString(band.defaultMode) == "LSB") ? "DIGL" : "DIGU";
            break;
        }
    }
    const QString prevMode       = s->mode();
    const int     prevFilterLow  = s->filterLow();
    const int     prevFilterHigh = s->filterHigh();
    s->setMode(mode);
    if (mode == "DIGL")
        s->setFilterWidth(-2250, -750);
    else
        s->setFilterWidth(750, 2250);

    // Remember which slice and its previous mute state
    m_radeSliceId = sliceId;
    m_radePrevMute = s->audioMute();
    s->setAudioMute(true);

    // Auto-deactivate if the slice mode is changed externally (TCI, profile
    // load, remote SmartSDR client) — those paths bypass the VfoWidget and
    // RxApplet combos and would otherwise leave audio_mute=1 stranded.
    connect(s, &SliceModel::modeChanged,
            this, &MainWindow::onRadeSliceModeChanged,
            Qt::UniqueConnection);

    // Create RADE engine on a worker thread for multi-core utilization
    if (!m_radeEngine) {
        m_radeEngine = new RADEEngine;  // no parent — will be moved to worker thread
        m_radeThread = new QThread(this);
        m_radeThread->setObjectName("RADEEngine");
        m_radeEngine->moveToThread(m_radeThread);
        connect(m_radeThread, &QThread::finished, m_radeEngine, &QObject::deleteLater);
        m_radeThread->start();
    }
    // start() must be invoked on the worker thread
    bool ok = false;
    QMetaObject::invokeMethod(m_radeEngine, [this, &ok]() {
        ok = m_radeEngine->start();
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        qCWarning(lcRade) << "MainWindow: RADE engine failed to start — restoring slice state";
        deactivateRADE();
        if (auto* sl = m_radioModel.slice(sliceId)) {
            sl->setMode(prevMode);
            sl->setFilterWidth(prevFilterLow, prevFilterHigh);
            if (prevTxSliceId != sliceId) {
                if (prevTxSliceId >= 0) {
                    if (auto* prevTx = m_radioModel.slice(prevTxSliceId)) {
                        prevTx->setTxSlice(true);
                    }
                } else {
                    sl->setTxSlice(false);
                }
            }
        }
        return;
    }
    m_radioModel.setDigitalVoiceTxSlice(sliceId);

    // Encode the operator callsign into the EOO frame so the far end can decode it.
    // Resolution order mirrors startFreeDvReporting: radio-stored callsign if
    // "Use radio" is set and non-empty, otherwise the user's saved FreeDV callsign.
    {
        auto& cs = AppSettings::instance();
        QString callsign;
        if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
                && !m_radioModel.callsign().isEmpty()) {
            callsign = m_radioModel.callsign();
        } else {
            callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
        }
        if (!callsign.isEmpty()) {
            QMetaObject::invokeMethod(m_radeEngine, [this, callsign]() {
                m_radeEngine->setTxCallsign(callsign);
            }, Qt::QueuedConnection);
        } else {
            qCWarning(lcRade) << "MainWindow: RADE TX EOO callsign not set — configure callsign in SpotHub FreeDV tab";
        }
    }

    // RADE sends VITA-49 modem audio directly (like TCI), so it needs its own
    // dax_tx stream regardless of platform.  On Windows the ExternalDaxRouteOnly
    // path used by updateDaxTxMode() is intentionally blocked by policy (SmartSDR
    // DAX2 owns the audio-device layer), but that policy doesn't apply here —
    // RADE never touches Windows audio devices.  RadeModemTx is always allowed.
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::RadeModemTx);

    // Only route mic→RADE when the RADE slice IS the TX slice.
    // If another slice is TX (e.g. USB voice), leave its audio path alone.
    m_audio->setRadeMode(s->isTxSlice());

    // TX path: mic -> RADEEngine (worker) -> sendModemTxAudio (main)
    connect(m_audio, &AudioEngine::txRawPcmReady,
            m_radeEngine, &RADEEngine::feedTxAudio, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::txModemReady,
            m_audio, &AudioEngine::sendModemTxAudio, Qt::QueuedConnection);

    // Phase 3: PTT Orchestration — three-layer intercept to hold TX open until
    // the EOO frame has fully played out on the radio before set_mox=0 is sent.
    //
    // Layer 1 — PttOffHook: catches MOX button (TxApplet) and TciServer callers
    // that go through TransmitModel::requestPttOff(). Fires BEFORE setMox(false),
    // so the radio stays in TX while EOO is generated and sent.
    m_radioModel.transmitModel().setPttOffHook([this]() {
        if (m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: PttOffHook — EOO already pending, ignoring duplicate";
            return;
        }
        m_radeEooPending = true;
        qCDebug(lcRade) << "MainWindow: PttOffHook — intercepted requestPttOff, deferring for RADE EOO";
        QMetaObject::invokeMethod(m_radeEngine, [this]() {
            m_radeEngine->setEooRequested(true);
        }, Qt::QueuedConnection);
    });

    // Layer 2 — eooFinished: once EOO audio is queued in AudioEngine, close the
    // audio gate (after EOO packets) then wait for the full EOO playout before
    // dropping the carrier. EOO=144ms + silence=60ms + margin=50ms = 254ms.
    connect(m_radeEngine, &RADEEngine::eooFinished, this, [this]() {
        if (!m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: eooFinished — no pending PTT release (EOO triggered without an intercepted unkey; no carrier to release)";
            return;
        }
        m_radeEooPending = false;
        m_radeTxActive = false;

        // Post setTransmitting(false) AFTER the queued txModemReady(eoo/silence)
        // signals so the audio gate closes only after EOO is in the UDP send buffer.
        if (m_audio)
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setTransmitting(false);
            }, Qt::QueuedConnection);

        constexpr int kEooPlaybackMs = RADEEngine::kEooFrameMs
                                     + RADEEngine::kEooSilenceTailMs
                                     + RADEEngine::kEooTransportMarginMs;
        qCDebug(lcRade) << "MainWindow: RADE eooFinished — deferring xmit 0 by"
                        << kEooPlaybackMs << "ms for EOO playback";
        QTimer::singleShot(kEooPlaybackMs, this, [this]() {
            qCDebug(lcRade) << "MainWindow: RADE EOO playback timer expired — releasing radio PTT";
            m_radioModel.setTransmit(false);
        });
    });

    // Layer 3 — moxChanged fallback: catches interlock and hardware PTT paths
    // (radio-initiated unkey) that bypass both layers above.
    // tx=true: new over starting — clear pending flag and reset engine EOO state.
    // tx=false + !pending + isTransmitting: unintercepted unkey — request EOO as
    //   best-effort (radio may already be in RX, but at least the app won't hang).
    m_radeMoxFallbackConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        if (!m_radeEngine || !m_radeEngine->isActive()) return;
        if (tx) {
            m_radeEooPending = false;
            m_radeTxActive = true;
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->resetTx();
            }, Qt::QueuedConnection);
            qCDebug(lcRade) << "MainWindow: MOX asserted — RADE TX state reset for new over";
        } else if (!m_radeEooPending && m_radeTxActive) {
            qCDebug(lcRade) << "MainWindow: moxChanged(false) fallback — unintercepted PTT release, requesting EOO";
            m_radeEooPending = true;
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->setEooRequested(true);
            }, Qt::QueuedConnection);
        }
    });

    // RX path: DAX RX audio -> RADEEngine (worker) -> decoded speech -> speaker (main)
    // Filter by the RADE slice's DAX channel so other slices' DAX audio is ignored.
    // Look up the channel live so it tracks if the user changes DAX assignment.
    int sid = sliceId;
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_radeEngine, [this, sid](int channel, const QByteArray& pcm) {
        auto* s = m_radioModel.slice(sid);
        if (s && channel == s->daxChannel())
            m_radeEngine->feedRxAudio(channel, pcm);
    }, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::rxSpeechReady,
            m_audio, &AudioEngine::feedDecodedSpeech, Qt::QueuedConnection);

    // If TCI or another path already registered a stream — RADE rides it.
    {
        SliceModel* radeSlice = m_radioModel.slice(sliceId);
        int daxCh = radeSlice ? radeSlice->daxChannel() : 0;
        if (daxCh >= 1 && daxCh <= 4) {
            quint32 existing = m_radioModel.panStream()->daxStreamIdForChannel(daxCh);
            if (existing) {
                // TCI or another path already registered a stream — RADE rides it.
                qCDebug(lcRade) << "MainWindow: RADE reusing existing dax_rx ch" << daxCh
                                << "stream" << Qt::hex << existing;
            } else {
                m_radeDaxStreamConn = connect(
                    &m_radioModel, &RadioModel::statusReceived,
                    this, [this, daxCh](const QString& obj, const QMap<QString,QString>& kvs) {
                        if (!obj.startsWith("stream ")) return;
                        if (kvs.value("type") != "dax_rx") return;
                        quint32 streamId = obj.mid(7).toUInt(nullptr, 16);
                        int ch = kvs.value("dax_channel").toInt();
                        if (streamId && ch == daxCh) {
                            m_radioModel.panStream()->registerDaxStream(streamId, ch);
                            m_radeDaxStreamId = streamId;
                            disconnect(m_radeDaxStreamConn);
                            qCDebug(lcRade) << "MainWindow: RADE registered dax_rx ch" << ch
                                            << "stream" << Qt::hex << streamId;
                        }
                    });
                m_radioModel.sendCommand(
                    QString("stream create type=dax_rx dax_channel=%1").arg(daxCh));
            }
        } else {
            qWarning() << "MainWindow: RADE slice" << sliceId
                       << "has no DAX channel assigned — RX audio will not flow";
        }
    }

    // Restore client-side mic gain for RADE. The radio's mic input is unused in
    // RADE mode so PcMicGain applies regardless of the current mic_selection.
    {
        int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
        m_audio->setPcMicGain(gain);
    }
    m_appletPanel->phoneCwApplet()->setRadeActive(true);

    // Start mic capture if not already running
    if (!m_audio->isTxStreaming()) {
        audioStartTx(m_radioModel.radioAddress(), 4991);
    }

    // RADE status indicator in VFO widget.
    // Use spectrumForSlice() to find the correct pan — in multi-pan layouts
    // spectrum() returns the *active* pan, which may not be the RADE slice's pan.
    if (auto* sw = spectrumForSlice(m_radioModel.slice(sliceId))) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true);
            // Show initial unsynchronised state immediately — syncChanged only fires
            // from feedRxAudio() which requires DAX audio to be flowing first.
            vfo->setRadeSynced(false);
            connect(m_radeEngine, &RADEEngine::syncChanged,
                    vfo, &VfoWidget::setRadeSynced);
            connect(m_radeEngine, &RADEEngine::snrChanged,
                    vfo, &VfoWidget::setRadeSnr);
            connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                    vfo, &VfoWidget::setRadeFreqOffset);
            connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                    vfo, &VfoWidget::setRadeCallsign, Qt::QueuedConnection);
        }
    }

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true);
        applet->setRadeSynced(false);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                applet, &RadeApplet::setRadeSynced);
        connect(m_radeEngine, &RADEEngine::snrChanged,
                applet, &RadeApplet::setRadeSnr);
        connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                applet, &RadeApplet::setRadeFreqOffset);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                applet, &RadeApplet::setRadeCallsign, Qt::QueuedConnection);
    }

    // Store far-end callsign received in EOO frame for display / future use.
    connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
            this, [this](const QString& callsign) {
                m_lastRadeRxCallsign = callsign;
                qCDebug(lcRade) << "RADE EOO callsign received:" << callsign;
            });

    // FreeDV Reporter: station reporting when the user has opted in.
    if (AppSettings::instance().value("FreeDvAutoReport", "False").toString() == "True")
        startFreeDvReporting(sliceId);

    qInfo() << "MainWindow: RADE mode activated on slice" << sliceId;
}

void MainWindow::deactivateRADE()
{
    // Capture slice ID before any field mutations below clear it.
    const int radeSliceId = m_radeSliceId;

    // Restore audio mute state on the RADE slice
    if (m_radeSliceId >= 0) {
        if (auto* s = m_radioModel.slice(m_radeSliceId)) {
            disconnect(s, &SliceModel::modeChanged,
                       this, &MainWindow::onRadeSliceModeChanged);
            s->setAudioMute(m_radePrevMute);
        }
        // Clear RADE status label and disconnect VFO signal connections before resetting sliceId.
        // Do this here (with m_radeSliceId still valid) rather than in the m_radeEngine block
        // below where the slice ID has already been cleared.
        if (auto* sw = spectrumForSlice(m_radioModel.slice(m_radeSliceId))) {
            if (auto* vfo = sw->vfoWidget(m_radeSliceId)) {
                vfo->setRadeActive(false);
                if (m_radeEngine) {
                    disconnect(m_radeEngine, &RADEEngine::syncChanged,         vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::snrChanged,           vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,    vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  vfo, nullptr);
                }
            }
        }
        m_radeSliceId = -1;
    }

    m_radioModel.transmitModel().clearPttOffHook();
    disconnect(m_radeMoxFallbackConn);
    m_radeEooPending = false;
    m_radeTxActive = false;

    m_audio->setRadeMode(false);
    m_radioModel.setDigitalVoiceTxSlice(-1);
    m_audio->clearTxAccumulators();  // flush stale RADE modem data
    m_appletPanel->phoneCwApplet()->setRadeActive(false);

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(false);
        if (m_radeEngine) {
            disconnect(m_radeEngine, &RADEEngine::syncChanged,        applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::snrChanged,         applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,  applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived, applet, nullptr);
        }
    }

    // For hardware mics, reset to full gain — the radio controls hardware levels.
    // PC mic keeps its PcMicGain so SSB sessions are unaffected.
    if (m_radioModel.transmitModel().micSelection() != "PC") {
        m_audio->setPcMicGain(100);
    }
    m_lastRadeRxCallsign.clear();

    if (m_radeEngine) {
        disconnect(m_audio, &AudioEngine::txRawPcmReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::txModemReady,
                   m_audio, nullptr);
        disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::rxSpeechReady,
                   m_audio, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooFinished,
                   this, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                   this, nullptr);
        if (m_radeDaxStreamId) {
            // Only send stream remove if TCI has no active clients. If TCI is
            // connected it may have borrowed this stream — removing it would
            // silently kill TCI audio. Leave TCI responsible for cleanup in
            // that case. TODO: replace with proper ref-counting in PanadapterStream
            // so any creator/borrower can safely release independently (#stream-lifecycle).
            bool tciActive = m_tciServer && m_tciServer->clientCount() > 0;
            bool daxBridgeActive = false;
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
            daxBridgeActive = (m_daxBridge != nullptr);
#endif
            if (!tciActive && !daxBridgeActive) {
                m_radioModel.panStream()->unregisterDaxStream(m_radeDaxStreamId);
                if (m_radioModel.isConnected()) {
                    m_radioModel.sendCommand(
                        QString("stream remove 0x%1")
                            .arg(m_radeDaxStreamId, 8, 16, QChar('0')));
                }
            }
            m_radeDaxStreamId = 0;
        }
        disconnect(m_radeDaxStreamConn);
        // Stop on the worker thread, then shut down the thread
        QMetaObject::invokeMethod(m_radeEngine, &RADEEngine::stop,
                                  Qt::BlockingQueuedConnection);
        if (m_radeThread) {
            m_radeThread->quit();
            m_radeThread->wait(2000);
            m_radeThread->deleteLater();
            m_radeThread = nullptr;
        }
        m_radeEngine = nullptr;  // deleteLater handles actual deletion
    }

    stopFreeDvReporting(radeSliceId);

    qInfo() << "MainWindow: RADE mode deactivated";
}

void MainWindow::onRadeSliceModeChanged(const QString& mode)
{
    // Fired when the RADE slice's mode changes via any path — TCI, profile
    // load, remote SmartSDR client. RADE requires DIGU or DIGL; deactivate
    // if the mode leaves that family so audio_mute is always restored.
    if (mode != "DIGU" && mode != "DIGL")
        deactivateRADE();
}

void MainWindow::activateFdvDisplay(int sliceId)
{
    if (m_fdvDisplaySliceId == sliceId)
        return;

    if (m_fdvDisplaySliceId >= 0)
        deactivateFdvDisplay();

    m_fdvDisplaySliceId = sliceId;

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    if (auto* sw = spectrumForSlice(s)) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true, QStringLiteral("FreeDV"));
            vfo->setRadeSynced(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true, QStringLiteral("FreeDV"));
        applet->setRadeSynced(false);
    }
#endif

    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");

    connect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
            this, &MainWindow::onFdvMeterUpdated,
            Qt::UniqueConnection);
    connect(&m_radioModel, &RadioModel::metersChanged,
            this, &MainWindow::onFdvMetersChanged,
            Qt::UniqueConnection);

    m_fdvSynced = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display activated on slice" << sliceId;
}

void MainWindow::deactivateFdvDisplay()
{
    if (m_fdvDisplaySliceId < 0)
        return;

    if (auto* s = m_radioModel.slice(m_fdvDisplaySliceId)) {
        if (auto* sw = spectrumForSlice(s)) {
            if (auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId))
                vfo->setRadeActive(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet())
        applet->setRadeActive(false);
#endif

    disconnect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
               this, &MainWindow::onFdvMeterUpdated);
    disconnect(&m_radioModel, &RadioModel::metersChanged,
               this, &MainWindow::onFdvMetersChanged);

    m_fdvDisplaySliceId = -1;
    m_fdvSnrMeterIndex  = -1;
    m_fdvSynced         = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display deactivated";
}

void MainWindow::onFdvMeterUpdated(int index, float value)
{
    if (index != m_fdvSnrMeterIndex || m_fdvDisplaySliceId < 0)
        return;

    auto* s = m_radioModel.slice(m_fdvDisplaySliceId);
    if (!s) return;
    auto* sw = spectrumForSlice(s);
    if (!sw) return;
    auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId);
    if (!vfo) return;

    const bool synced = (value > -98.9f);
    if (synced != m_fdvSynced) {
        m_fdvSynced = synced;
        vfo->setRadeSynced(synced);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSynced(synced);
#endif
    }
    if (synced) {
        vfo->setRadeSnr(value);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSnr(value);
#endif
    }
}

void MainWindow::onFdvMetersChanged()
{
    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");
}

void MainWindow::startFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    // RADE without WebSockets: reporter client doesn't exist, no-op. (#2204)
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    auto& cs = AppSettings::instance();

    // Callsign: prefer radio-stored value if "Use radio" is set and it's populated.
    QString callsign;
    if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
            && !m_radioModel.callsign().isEmpty()) {
        callsign = m_radioModel.callsign();
    } else {
        callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
    }

    // Grid: GPS (if hardware present, locked, and user prefers it), else user-saved.
    QString grid;
    if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
            && m_radioModel.hasGpsHardware()
            && !m_radioModel.gpsGrid().isEmpty()) {
        grid = m_radioModel.gpsGrid();
    } else {
        grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
    }

    // Refuse to broadcast placeholder data to the public FreeDV Reporter
    // map — the dialog already validates this when the user toggles the
    // checkbox, but RADE auto-activation can hit this path without going
    // through the toggle, so guard here too.
    if (callsign.isEmpty() || grid.isEmpty()) {
        qCWarning(lcDxCluster)
            << "FreeDvReporting: refusing to enable — callsign or grid empty"
            << "(callsign='" << callsign << "', grid='" << grid << "')";
        return;
    }

    const QString message = cs.value("FreeDvMyMessage", "").toString();
    const QString swVer   = QString("AetherSDR %1").arg(QCoreApplication::applicationVersion());
    const double  freqMhz = m_radioModel.slice(sliceId)
                            ? m_radioModel.slice(sliceId)->frequency() : 0.0;

    // Auto-start the WebSocket connection if not already running — reporting is
    // independent of the user's FreeDV spot subscription.
    if (!m_freedvClient->isConnected())
        QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->startConnection(); });

    QMetaObject::invokeMethod(m_freedvClient,
        [this, callsign, grid, message, swVer, freqMhz] {
            m_freedvClient->enableReporting(callsign, grid, message, swVer, freqMhz);
        });

    // TX report: evaluate tune/ATU guard on the main thread so we never report
    // a tune or ATU cycle as a voice transmission.
    m_freedvMoxConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
                const TransmitModel& txm = m_radioModel.transmitModel();
                if (txm.isTuning() || txm.atuStatus() == ATUStatus::InProgress)
                    return;
                QMetaObject::invokeMethod(m_freedvClient, [this, tx] {
                    m_freedvClient->reportTxState(tx);
                });
            });

    if (m_radeEngine) {
        connect(m_radeEngine, &RADEEngine::snrChanged,
                m_freedvClient, &FreeDvClient::updateRxSnr, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                m_freedvClient, &FreeDvClient::updateRxSynced, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                m_freedvClient, &FreeDvClient::updateRxCallsign, Qt::QueuedConnection);
    }
    if (SliceModel* radeSlice = m_radioModel.slice(sliceId)) {
        connect(radeSlice, &SliceModel::frequencyChanged,
                m_freedvClient, &FreeDvClient::reportFreqChange, Qt::QueuedConnection);
    }
#endif  // HAVE_WEBSOCKETS
}

void MainWindow::stopFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    disconnect(m_freedvMoxConn);
    if (m_radeEngine) {
        disconnect(m_radeEngine, &RADEEngine::snrChanged,           m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::syncChanged,          m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  m_freedvClient, nullptr);
    }
    if (auto* radeSlice = m_radioModel.slice(sliceId))
        disconnect(radeSlice, &SliceModel::frequencyChanged, m_freedvClient, nullptr);

    QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->disableReporting(); });
#endif
}

#endif  // HAVE_RADE

#ifdef HAVE_WEBSOCKETS
void MainWindow::showFreeDvReporter()
{
    if (!m_freedvReporterDialog) {
        m_freedvReporterDialog = new FreeDvReporterDialog(this);
        connect(m_freedvClient, &FreeDvClient::stationsCleared,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationsCleared,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationUpdated,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationUpdated,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationRemoved,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationRemoved,
                Qt::QueuedConnection);
        if (auto* s = activeSlice())
            m_freedvReporterDialog->setActiveSlice(s);
        // Seed with current state — bulk_update fires at connect time, before the
        // dialog exists. Without this, the table fills slowly from live events only.
        for (const auto& [sid, info] : m_freedvClient->stations().asKeyValueRange())
            m_freedvReporterDialog->onStationUpdated(sid, info);
    }
    // Resolve GPS-aware grid every open — same logic as startFreeDvReporting()
    // so km/Hdg columns work when GPS grid is active and never written to AppSettings.
    {
        auto& cs = AppSettings::instance();
        QString grid;
        if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
                && m_radioModel.hasGpsHardware()
                && !m_radioModel.gpsGrid().isEmpty()) {
            grid = m_radioModel.gpsGrid();
        } else {
            grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
        }
        if (grid.isEmpty())
            grid = m_freedvClient->myGrid();
        m_freedvReporterDialog->setMyGrid(grid);
    }
    m_freedvReporterDialog->show();
    m_freedvReporterDialog->raise();
    m_freedvReporterDialog->activateWindow();
}
#endif

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
bool MainWindow::startDax()
{
    if (m_daxBridge) return true;

#ifdef Q_OS_MAC
    // Only start if the macOS HAL driver bundle is installed.
    if (!macDaxDriverInstalled()) {
        qWarning() << "MainWindow: DAX HAL plugin not installed";
        QMessageBox::warning(this, "DAX Audio Driver Missing",
            "The AetherSDR DAX audio driver is not installed on this Mac.\n\n"
            "Install the DAX Virtual Audio Driver from the AetherSDR DMG package, "
            "then enable DAX again.");
        return false;
    }
#endif

    m_daxBridge = new DaxBridge(this);
    if (!m_daxBridge->open()) {
        qWarning() << "MainWindow: failed to open DAX audio bridge";
        QMessageBox::warning(this, "DAX Audio Bridge Error",
            "AetherSDR could not open the DAX audio bridge.\n\n"
            "If the DAX driver was just installed, quit and relaunch AetherSDR and try again.");
        delete m_daxBridge;
        m_daxBridge = nullptr;
        return false;
    }

    // Listen for DAX stream status messages to register them in PanadapterStream.
    // The radio sends "stream 0xNNNNNNNN type=dax_rx dax_channel=N" status lines
    // in response to our stream create commands.
    connect(&m_radioModel, &RadioModel::statusReceived,
            m_daxBridge, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        const QStringList parts = obj.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2) return;
        bool ok = false;
        quint32 streamId = parts[1].toUInt(&ok, 0);
        if (!ok) return;
        const bool removed = parts.contains(QStringLiteral("removed")) || kvs.contains(QStringLiteral("removed"));
        if (removed) {
            m_radioModel.panStream()->unregisterDaxStream(streamId);
            qCDebug(lcDax) << "MainWindow: unregistered removed DAX RX stream"
                           << "0x" + QString::number(streamId, 16);
            return;
        }
        QString type = kvs.value("type");
        if (type == "dax_rx") {
            if (!streamStatusBelongsToUs(kvs, m_radioModel.ourClientHandle())) {
                qCDebug(lcDax) << "MainWindow: ignoring DAX RX stream for another client"
                                << "stream=0x" + QString::number(streamId, 16)
                                << "owner=" << kvs.value("client_handle");
                return;
            }
            int ch = kvs.value("dax_channel").toInt();
            if (streamId && ch >= 1 && ch <= 4) {
                m_radioModel.panStream()->registerDaxStream(streamId, ch);
                qCDebug(lcDax) << "MainWindow: registered DAX RX ch" << ch
                                << "stream=0x" + QString::number(streamId, 16);
            }
        }
    });

    // Create DAX RX streams only for channels with slices assigned.
    // FlexLib creates streams on demand, not all 4 unconditionally.
    // Creating unused streams causes the radio to round-robin audio
    // across all of them, starving the active channels.
    m_daxSliceLastCh.clear();
    for (auto* s : m_radioModel.slices()) {
        int ch = s->daxChannel();
        m_daxSliceLastCh[s->sliceId()] = ch;
        if (ch >= 1 && ch <= 4) {
            m_radioModel.sendCommand(
                QString("stream create type=dax_rx dax_channel=%1").arg(ch));
        }
    }

    // #2895: the one-shot loop above only covers slices that ALREADY have a
    // DAX channel at bridge startup (typically just slice 0 / DAX 1). When the
    // user later assigns DAX 2-4 to another slice via the UI, SliceModel only
    // sends `slice set <id> dax=<ch>` — it never sends `stream create
    // type=dax_rx`, so the radio never registers a DAX client (dax_clients
    // stays 0) and sends silence. React to per-slice channel changes here and
    // create/remove the DAX RX stream on demand, mirroring the TCI path
    // (TciServer::ensureDaxForTci, #1331/#1439) and FlexLib
    // RequestDAXRXAudioStream(channel).
    for (auto* s : m_radioModel.slices()) {
        wireDaxSlice(s);
    }
    m_daxSliceConns.append(connect(&m_radioModel, &RadioModel::sliceAdded,
                                   this, [this](SliceModel* s) {
        if (!m_daxBridge || !s) return;
        // Let onDaxChannelChanged() see a 0 -> channel transition for slices
        // restored with DAX already assigned.
        m_daxSliceLastCh[s->sliceId()] = 0;
        wireDaxSlice(s);
        // A slice can arrive already carrying a DAX channel (radio profile
        // restore); make sure its stream exists too.
        if (s->daxChannel() >= 1 && s->daxChannel() <= 4) {
            onDaxChannelChanged(s, s->daxChannel());
        }
    }));

    // Wire DAX RX: PanadapterStream routes registered DAX streams here
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_daxBridge, &DaxBridge::feedDaxAudio);

    // ── DAX IQ stream status + VITA routing ─────────────────────────────
    connect(&m_radioModel, &RadioModel::statusReceived,
            this, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        const QStringList parts = obj.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2) return;
        bool ok = false;
        quint32 streamId = parts[1].toUInt(&ok, 0);
        if (!ok) return;
        const bool removed = parts.contains(QStringLiteral("removed")) || kvs.contains(QStringLiteral("removed"));
        if (removed) {
            m_radioModel.panStream()->unregisterIqStream(streamId);
            m_radioModel.daxIqModel().handleStreamRemoved(streamId);
            qCDebug(lcDax) << "MainWindow: unregistered removed DAX IQ stream"
                           << "0x" + QString::number(streamId, 16);
            return;
        }
        QString type = kvs.value("type");
        if (type == "dax_iq") {
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
        }
    });

    // Route IQ VITA-49 packets to DaxIqModel worker thread
    connect(m_radioModel.panStream(), &PanadapterStream::iqDataReady,
            &m_radioModel.daxIqModel(), &DaxIqModel::feedRawIqPacket);

    // Wire DAX IQ level meters to DAX IQ applet
    connect(&m_radioModel.daxIqModel(), &DaxIqModel::iqLevelReady,
            m_appletPanel->daxIqApplet(), &DaxIqApplet::setDaxIqLevel);

    // Wire DAX IQ enable/disable/rate from DAX IQ applet to DaxIqModel
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqEnableRequested,
            &m_radioModel.daxIqModel(), &DaxIqModel::createStream);
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqDisableRequested,
            &m_radioModel.daxIqModel(), &DaxIqModel::removeStream);
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqRateChanged,
            &m_radioModel.daxIqModel(), &DaxIqModel::setSampleRate);

    // Wire DAX level meters
    connect(m_daxBridge, &DaxBridge::daxRxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxRxLevel);
    connect(m_daxBridge, &DaxBridge::daxTxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxTxLevel);

    // Wire DAX gain sliders
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxRxGainChanged,
            m_daxBridge, &DaxBridge::setChannelGain);
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxTxGainChanged,
            m_daxBridge, &DaxBridge::setTxGain);

    // Apply saved gains to the bridge
    auto& ss = AppSettings::instance();
    for (int i = 1; i <= 4; ++i)
        m_daxBridge->setChannelGain(i, ss.value(QStringLiteral("DaxRxGain%1").arg(i), "0.5").toString().toFloat());
    m_daxBridge->setTxGain(ss.value("DaxTxGain", "0.5").toString().toFloat());

    // Wire DAX TX: apps → bridge → AudioEngine → VITA-49.
    // AudioEngine chooses packet format/routing based on DaxTxLowLatency.
    connect(m_daxBridge, &DaxBridge::txAudioReady,
            this, [this](const QByteArray& pcm) {
        if (m_audio->isRadeMode()) return;
        if (!m_audio->isDaxTxMode()) return;
        QMetaObject::invokeMethod(m_audio, [this, pcm]() { m_audio->feedDaxTxAudio(pcm); });
    });

    // Save current mic selection before forcing PC audio source.
    m_savedMicSelection = m_radioModel.transmitModel().micSelection();

    // Default to the radio-native DAX route (dax=1, int16 mono).  RADE
    // mode overrides this to the low-latency route via setRadeMode()
    // when the user enters RADE — see AudioEngine::setRadeMode().
    m_audio->setDaxTxUseRadioRoute(true);
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
    m_radioModel.sendCommand("transmit set mic_selection=PC");
    // Don't force dax=1 here — radio-side DAX flag follows mode changes
    // via updateDaxTxMode(). Bridge up ≠ DAX TX active. (#534)

    qInfo() << "MainWindow: starting DAX audio bridge";
    return true;
}

void MainWindow::stopDax()
{
    if (!m_daxBridge) return;

    m_audio->setDaxTxMode(false);
    m_audio->clearTxAccumulators();

    // #2895: drop the per-slice daxChannelChanged / sliceAdded reactions wired
    // in startDax() so they don't fire against a torn-down bridge.
    for (const auto& c : m_daxSliceConns) {
        disconnect(c);
    }
    m_daxSliceConns.clear();
    m_daxSliceLastCh.clear();

    disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
               m_daxBridge, nullptr);
    disconnect(m_daxBridge, &DaxBridge::txAudioReady,
               this, nullptr);

    // Remove DAX RX streams from radio and unregister from PanadapterStream
    const auto daxIds = m_radioModel.panStream()->daxStreamIds();
    for (quint32 id : daxIds) {
        m_radioModel.sendCommand(QString("stream remove 0x%1").arg(id, 0, 16));
        m_radioModel.panStream()->unregisterDaxStream(id);
    }

    // Restore original mic selection
    if (!m_savedMicSelection.isEmpty() && m_savedMicSelection != "PC")
        m_radioModel.sendCommand(QString("transmit set mic_selection=%1").arg(m_savedMicSelection));

    m_daxBridge->close();
    delete m_daxBridge;
    m_daxBridge = nullptr;
    qInfo() << "MainWindow: stopping DAX audio bridge";
}

// #2895: connect one slice's daxChannelChanged so a DAX channel (re)assigned
// after the bridge is already up still gets a radio-side DAX RX stream.
void MainWindow::wireDaxSlice(SliceModel* slice)
{
    if (!slice) return;
    m_daxSliceConns.append(connect(slice, &SliceModel::daxChannelChanged,
                                   this, [this, slice](int newCh) {
        if (!m_daxBridge) return;  // bridge torn down — ignore late signals
        onDaxChannelChanged(slice, newCh);
    }));
}

// Handle a slice's DAX channel transitioning. daxChannelChanged only fires on
// an actual value change (SliceModel guards equality), and arrives from both
// the local UI setter and the radio status echo — both are safe here because
// the stream create is made idempotent by the daxStreamIdForChannel() guard.
void MainWindow::onDaxChannelChanged(SliceModel* slice, int newCh)
{
    if (!slice || !m_daxBridge) return;
    auto* ps = m_radioModel.panStream();
    if (!ps) return;

    const int sliceId = slice->sliceId();
    const int oldCh = m_daxSliceLastCh.value(sliceId, 0);
    if (oldCh == newCh) return;
    m_daxSliceLastCh[sliceId] = newCh;

    // 0 -> 1..4 (or 1..4 -> different 1..4): ensure the new channel has a
    // radio-registered DAX RX stream. The stream status echo will register the
    // stream id in PanadapterStream via the dax_rx handler wired in startDax().
    if (newCh >= 1 && newCh <= 4) {
        if (ps->daxStreamIdForChannel(newCh) == 0) {
            m_radioModel.sendCommand(
                QString("stream create type=dax_rx dax_channel=%1").arg(newCh));
            qCInfo(lcDax) << "MainWindow: creating DAX RX stream for channel"
                          << newCh << "(slice" << sliceId << ", #2895)";
        }
        // Re-assert slice -> DAX mapping so the radio registers our stream as a
        // client. Without this dax_clients stays 0 and the radio sends silence
        // (the #1439 workaround, mirrored from TciServer::ensureDaxForTci).
        m_radioModel.sendCommand(
            QString("slice set %1 dax=%2").arg(sliceId).arg(newCh));
    }

    // <old>:1..4 -> now released or moved: remove the old channel's stream if
    // no other slice still references it.
    if (oldCh >= 1 && oldCh <= 4) {
        bool stillUsed = false;
        for (auto* s : m_radioModel.slices()) {
            if (s && s != slice && s->daxChannel() == oldCh) {
                stillUsed = true;
                break;
            }
        }
        if (!stillUsed) {
            const quint32 id = ps->daxStreamIdForChannel(oldCh);
            // Ownership guard: the PanadapterStream DAX map is shared across
            // every DAX consumer — the bridge, TCI (which borrows
            // bridge-created streams, #1331/#1439), and RADE. Removing a stream
            // another consumer is still using would silence WSJT-X or RADE
            // audio — the mirror image of #3270. Only tear down a stream that
            // no other consumer needs. (#2895)
            const bool tciUsing = m_tciServer && m_tciServer->ownsDaxChannel(oldCh);
#ifdef HAVE_RADE
            const bool radeUsing = (id != 0 && id == m_radeDaxStreamId);
#else
            const bool radeUsing = false;
#endif
            if (id != 0 && !tciUsing && !radeUsing) {
                m_radioModel.sendCommand(
                    QString("stream remove 0x%1").arg(id, 0, 16));
                ps->unregisterDaxStream(id);
                qCInfo(lcDax) << "MainWindow: removed DAX RX stream"
                              << "0x" + QString::number(id, 16)
                              << "for channel" << oldCh << "(#2895)";
            } else if (id != 0) {
                qCInfo(lcDax) << "MainWindow: keeping DAX RX stream"
                              << "0x" + QString::number(id, 16)
                              << "for channel" << oldCh
                              << "— still used by" << (tciUsing ? "TCI" : "RADE")
                              << "(#2895)";
            }
        }
    }
}
#endif

// registerMidiParams() lives in MainWindow_Controllers.cpp (#3351 Phase 1a).

// StreamDeck native integration removed — use TCI StreamController plugin instead.

// ─── Applet-panel pop-out (#1713 Phase 6) ───────────────────────────────────
//
// The whole AppletPanel widget (tray buttons + S-Meter + scrollable stack)
// can live either inside m_splitter at the end of the row, or as the sole
// content of its own top-level window.  Reparenting transfers Qt ownership
// cleanly; the splitter pane collapses to zero width when the panel floats,
// and restores to its remembered width when it docks back.

void MainWindow::floatAppletPanel()
{
    if (!m_appletPanel || !m_splitter) return;
    if (m_appletPanelFloatWindow) return;  // already floating

    const bool frameless = framelessWindowEnabled();
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;

    m_appletPanelFloatWindow = new QWidget(nullptr, flags);
    m_appletPanelFloatWindow->setWindowTitle("AetherSDR — Applet Panel");
    m_appletPanelFloatWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    m_appletPanelFloatWindow->setAttribute(Qt::WA_QuitOnClose, false);
    m_appletPanelFloatWindow->setAttribute(Qt::WA_StyledBackground, true);
    applyAppTheme(m_appletPanelFloatWindow);
    auto* layout = new QVBoxLayout(m_appletPanelFloatWindow);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Project frameless title bar (drag-to-move, double-click maximize,
    // min/max/close trio).  Only added when frameless is on; the system
    // frame supplies its own title bar in non-frameless mode.
    if (frameless) {
        auto* titleBar = new FramelessWindowTitleBar(
            QStringLiteral("AetherSDR — Applet Panel"),
            m_appletPanelFloatWindow);
        layout->addWidget(titleBar);
    }

    m_appletPanel->setParent(m_appletPanelFloatWindow);
    layout->addWidget(m_appletPanel);
    m_appletPanel->show();

    // 8-axis edge resize for the frameless variant — same install pattern
    // as the floating dialogs and the main window.  The resizer no-ops
    // when the system frame is on, so installing unconditionally is safe.
    FramelessResizer::install(m_appletPanelFloatWindow);
    // Qt auto-redistributes remaining splitter slots once the panel
    // is reparented out; we don't need to touch the sizes manually.

    // Restore last-known window geometry or default.
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance()
            .value("AppletPanelFloatGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_appletPanelFloatWindow->restoreGeometry(geom);
    } else {
        m_appletPanelFloatWindow->resize(320, 720);
    }

    // Track geometry changes live so the saved position/size stays
    // current without a per-tick timer.
    m_appletPanelFloatWindow->installEventFilter(this);

    // On close → dock back (unchecks the menu action via its own
    // toggled handler, which re-enters dockAppletPanel).  Using
    // QObject::connect with a lambda so we don't need to subclass.
    connect(m_appletPanelFloatWindow, &QObject::destroyed,
            this, [this]() {
        m_appletPanelFloatWindow = nullptr;
    });
    m_appletPanelFloatWindow->show();
}

void MainWindow::dockAppletPanel()
{
    if (!m_appletPanel || !m_splitter) return;
    if (!m_appletPanelFloatWindow) return;  // already docked

    // Save geometry before tearing the window down.
    AppSettings::instance().setValue(
        "AppletPanelFloatGeometry",
        m_appletPanelFloatWindow->saveGeometry().toBase64());

    // Reparent back to the splitter.  addWidget appends, which is
    // the correct position (last slot) matching the pre-float layout.
    m_appletPanel->setParent(m_splitter);
    m_splitter->addWidget(m_appletPanel);
    m_appletPanel->show();

    // Re-apply the canonical 4-slot layout the app uses at startup:
    // CWX=0, DVK=0, center=stretch, applet=260px.  Using fixed sizes
    // instead of saveState()/restoreState() because the saved state
    // is unreliable in the launch-with-float case — saveState() fires
    // at a QTimer::singleShot(0) turn before the splitter has fully
    // laid out, producing captured sizes that don't match the
    // post-show window width.  The splitter isn't user-draggable for
    // applet width anyway (startup always forces 260), so recomputing
    // is both simpler and deterministic.
    const int centerWidth = std::max(400, m_splitter->width() - 260);
    m_splitter->setSizes({0, 0, centerWidth, 260});

    // Restore the user's last-known dock side and re-sync the title-bar
    // icon.  addWidget above places the panel in the right slot; if the
    // user last had it docked left, setAppletPanelDockedLeft moves it
    // back, re-applies sizes by widget identity, and updates the dock-
    // side icon.  Calling it for right-dock is a no-op for layout but
    // still re-syncs the icon, which would otherwise stay stuck on the
    // pre-float side highlight.
    const bool dockedLeft = AppSettings::instance()
        .value("AppletPanelDockedLeft", "False").toString() == "True";
    setAppletPanelDockedLeft(dockedLeft);

    m_appletPanelFloatWindow->removeEventFilter(this);
    m_appletPanelFloatWindow->deleteLater();
    m_appletPanelFloatWindow = nullptr;
}

static double roundToHundredHz(double freqMhz)
{
    return std::round(freqMhz * 10000.0) / 10000.0;
}

void MainWindow::applySHistoryEnabled(bool on)
{
    m_sHistoryEnabled = on;
    AppSettings::instance().setValue("SHistoryMarkersEnabled", on ? "True" : "False");
    AppSettings::instance().save();
    if (m_shuttingDown || !m_panStack) return;
    for (auto* a : m_panStack->allApplets()) {
        a->spectrumWidget()->setShowSHistory(on);
    }
    if (!on && !m_sHistoryQrmEnabled) {
        m_sHistoryData.clear();
        m_sHistoryPanState.clear();
        for (auto* a : m_panStack->allApplets()) {
            a->spectrumWidget()->setSHistoryMarkers({});
        }
    }
}

void MainWindow::applySHistoryQrmEnabled(bool on)
{
    m_sHistoryQrmEnabled = on;
    AppSettings::instance().setValue("SHistoryQrmEnabled", on ? "True" : "False");
    AppSettings::instance().save();
    if (m_shuttingDown || !m_panStack) return;
    for (auto* a : m_panStack->allApplets()) {
        a->spectrumWidget()->setShowSHistoryQrm(on);
    }
    if (!on && !m_sHistoryEnabled) {
        m_sHistoryData.clear();
        m_sHistoryPanState.clear();
        for (auto* a : m_panStack->allApplets()) {
            a->spectrumWidget()->setSHistoryMarkers({});
        }
    }
}

void MainWindow::rebuildSHistoryForPan(const QString& panId)
{
    if (m_shuttingDown || !m_panStack) return;
    auto* sw = m_panStack->spectrum(panId);
    if (!sw) return;

    constexpr qint64 kQrmWindowMs       = 15000; // timestamp retention window
    constexpr qint64 kHitWindowMs       = 1000;
    constexpr int    kMinHits           = 1;
    constexpr qint64 kQualifyMs         = 3000;  // min age before a new signal becomes visible
    constexpr int    kQualifyMinHits    = 3;     // min detections within kQualifyMs to qualify
    // SpotHub Display tab → Signal History → "QRM Gate" slider; default 6 s.
    const qint64 kNarrowQrmGateMs = std::clamp(
        AppSettings::instance().value("SHistoryQrmGateS", 6).toInt(),
        3, 30) * 1000LL;
    // Require 70% frame occupancy over 6 s, derived from observed fps rather than
    // the old hard-coded 105 (which assumed 25 fps and broke at 10 fps or 60 fps).
    const float observedFps = m_sHistoryPanState.value(panId).fpsEwma;
    const int   kNarrowQrmHitsNeeded = static_cast<int>(
        std::clamp(observedFps * (kNarrowQrmGateMs / 1000.0f) * 0.70f, 30.0f, 500.0f));
    constexpr qint64 kVoiceToQrmMs      = 120000;
    constexpr qint64 kHideAfterMs       = 30000; // past-signals history window

    const qint64 now           = QDateTime::currentMSecsSinceEpoch();
    auto&        entries       = m_sHistoryData[panId];
    const qint64 suppressUntil = m_sHistoryPanState.value(panId).suppressUntilMs;

    for (auto& e : entries) {
        // Keep 30 s of timestamps for QRM assessment.
        e.hitTimestamps.erase(
            std::remove_if(e.hitTimestamps.begin(), e.hitTimestamps.end(),
                [now](qint64 t) { return (now - t) > kQrmWindowMs; }),
            e.hitTimestamps.end());

        // How many hits in the last 1 second (display gate).
        const int recentHits1s = static_cast<int>(std::count_if(
            e.hitTimestamps.constBegin(), e.hitTimestamps.constEnd(),
            [now](qint64 t) { return (now - t) <= kHitWindowMs; }));
        const bool currentlyActive = (recentHits1s >= kMinHits);

        // Detect any gap > 1 s in the retained timestamp window.
        bool hasVoiceGap = false;
        constexpr qint64 kMaxQrmGapMs = 1000;
        for (int ti = 1; ti < e.hitTimestamps.size() && !hasVoiceGap; ++ti) {
            if ((e.hitTimestamps[ti] - e.hitTimestamps[ti - 1]) > kMaxQrmGapMs) {
                hasVoiceGap = true;
            }
        }
        if (hasVoiceGap) { e.lastGapMs = now; }

        // QRM classification:
        //   Voice-width (≥1.8 kHz, ≤8 kHz): require 2 unbroken minutes.
        //   True narrow (< 1.8 kHz) / wideband (> 8 kHz): QRM after 6 s of
        //   continuous presence with no gap (checked via lastGapMs so gaps that
        //   age out of the 15 s timestamp window are still honoured).
        // CNN override: if carrierScore > 0.70, treat as narrow carrier even if
        // width falls in the voice range.  Threshold is conservative to avoid
        // mis-classifying real voice.  Has no effect when ONNX is absent (score stays 0.5).
        const bool cnnSaysCarrier = (e.carrierScore > 0.70f);
        const bool isVoiceWidth   = !cnnSaysCarrier
                                    && (e.widthHz >= 1800.0 && e.widthHz <= 8000.0);
        bool qrmQualified;
        if (isVoiceWidth) {
            qrmQualified = (now - e.lastGapMs) >= kVoiceToQrmMs;
        } else {
            const int hitsIn6s = static_cast<int>(std::count_if(
                e.hitTimestamps.constBegin(), e.hitTimestamps.constEnd(),
                [now, kNarrowQrmGateMs](qint64 t) { return (now - t) <= kNarrowQrmGateMs; }));
            // Use lastGapMs so gaps that fell out of the 15 s window still
            // prevent a voice-like signal from being misclassified as QRM.
            const bool noRecentGap = (now - e.lastGapMs) >= kNarrowQrmGateMs;
            qrmQualified = (now - e.firstDetectedMs) >= kNarrowQrmGateMs
                           && noRecentGap
                           && (hitsIn6s >= kNarrowQrmHitsNeeded);
        }
        e.suspectQrm = currentlyActive && qrmQualified && !hasVoiceGap;

        // Hide visible markers absent for 30 seconds (the "past signals" history window).
        if (e.visible && !currentlyActive && (now - e.lastSeenMs) > kHideAfterMs) {
            e.visible = false;
        }

        if (!e.visible) {
            // < 1800 Hz: narrow carrier — QRM-only
            // 1800–8000 Hz: voice path (includes the 1800–2300 Hz borderline zone)
            // > 8000 Hz: wideband interference — QRM-only
            const bool isNarrowCarrier   = (e.widthHz < 1800.0);
            const bool isWidebandCarrier = (e.widthHz > 8000.0);
            if (isNarrowCarrier || isWidebandCarrier) {
                if (e.suspectQrm) { e.visible = true; }
            } else if (currentlyActive) {
                // Qualify by total age since first detection, not streak length.
                // Bursty signals (pileups, intermittent operators) qualify as soon
                // as they have been known for kQualifyMs AND have accumulated at
                // least kQualifyMinHits detections in that window.  This rejects
                // transient noise that appears once and disappears while still
                // allowing intermittent but real signals (pileups, bursty digital).
                const qint64 requiredMs = e.confirmedVoice ? 2000LL : kQualifyMs;
                const int hitsInQualify = static_cast<int>(std::count_if(
                    e.hitTimestamps.constBegin(), e.hitTimestamps.constEnd(),
                    [now, requiredMs](qint64 t) { return (now - t) <= requiredMs; }));
                const bool enoughHits = e.confirmedVoice || (hitsInQualify >= kQualifyMinHits);
                if ((now - e.firstDetectedMs) >= requiredMs && enoughHits && now >= suppressUntil) {
                    e.visible = true;
                    if (!e.suspectQrm) { e.confirmedVoice = true; }
                }
            }
        }
    }

    // SpotHub Display tab → Signal History colour pickers feed these.
    // Defaults preserve historical look (amber for voice, red for QRM).
    const QColor signalsCol(AppSettings::instance()
        .value("SHistoryColorSignals", "#FFC800").toString());
    const QColor qrmCol(AppSettings::instance()
        .value("SHistoryColorQrm",     "#FF0000").toString());
    QVector<SpectrumWidget::SpotMarker> markers;
    for (const auto& e : entries) {
        if (!e.visible) { continue; }
        const bool isQrm = e.suspectQrm;
        const QString label = isQrm
            ? (QStringLiteral("QRM") + AetherSDR::sLabel(e.peakDbm).mid(1))
            : AetherSDR::sLabel(e.peakDbm);
        const QColor col = isQrm ? qrmCol : signalsCol;
        const QString comment = isQrm
            ? QStringLiteral("QRM width=%1 Hz").arg(e.widthHz, 0, 'f', 0)
            : QStringLiteral("Voice width=%1 Hz").arg(e.widthHz, 0, 'f', 0);
        markers.append({
            -1,
            label,
            roundToHundredHz(e.freqMhz),
            col.name(),
            e.mode,
            col,
            isQrm ? QStringLiteral("QRM") : QStringLiteral("SHistory"),
            {},
            comment,
            e.lastSeenMs,
            {}
        });

        // Double mark: voice operator detected on top of a QRM-classified entry.
        // Emit an additional voice marker so the operator can see both the
        // interference AND the person trying to work through it simultaneously.
        // The voice marker ages out independently (30 s after last voice-width hit).
        if (isQrm && (now - e.voiceOverQrmLastMs) < kHideAfterMs) {
            markers.append({
                -1,
                AetherSDR::sLabel(e.peakDbm),
                roundToHundredHz(e.freqMhz),
                signalsCol.name(),
                e.mode,
                signalsCol,
                QStringLiteral("SHistory"),
                {},
                QStringLiteral("Voice on QRM ch, width=%1 Hz").arg(e.widthHz, 0, 'f', 0),
                e.voiceOverQrmLastMs,
                {}
            });
        }
    }
    sw->setSHistoryMarkers(markers);
}

void MainWindow::expireSHistoryMarkers()
{
    if (m_shuttingDown || !m_panStack) return;
    if (!m_sHistoryEnabled && !m_sHistoryQrmEnabled) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Per-tick read keeps the slider live — fires once a second, AppSettings
    // reads are an in-memory hash lookup so the cost is negligible.
    const qint64 kLifetimeMs = std::clamp(
        AppSettings::instance().value("SHistoryLifetimeS", 60).toInt(),
        15, 300) * 1000LL;
    for (auto it = m_sHistoryData.begin(); it != m_sHistoryData.end(); ++it) {
        auto& entries = it.value();
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [now, kLifetimeMs](const SHistoryEntry& e) {
                    return (now - e.lastSeenMs) > kLifetimeMs;
                }),
            entries.end());
        // Rebuild unconditionally: hit-window timestamps age out every second
        // even when no new detections arrive, which hides markers whose peaks
        // have fallen below the 25% threshold.
        rebuildSHistoryForPan(it.key());
    }
}

void MainWindow::onSpectrumReadyForSHistory(quint32 streamId, const QVector<float>& bins, qint64 emittedNs)
{
    Q_UNUSED(emittedNs);
    const bool perfEnabled = PerfTelemetry::instance().enabled();
    const qint64 perfStartNs = perfEnabled ? PerfTelemetry::nowNs() : 0;

    if (m_shuttingDown || !m_panStack || (!m_sHistoryEnabled && !m_sHistoryQrmEnabled)) {
        if (perfEnabled)
            PerfTelemetry::instance().recordSHistorySkipped();
        return;
    }

    // Build voice-only frequency ranges from the active band plan once per frame
    QVector<QPair<double, double>> voiceRanges;
    if (m_bandPlanMgr) {
        for (const auto& seg : m_bandPlanMgr->segments()) {
            if (AetherSDR::isVoiceSegmentLabel(seg.label))
                voiceRanges.append({seg.lowMhz, seg.highMhz});
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool processedFrame = false;
    for (auto* pan : m_radioModel.panadapters()) {
        if (pan->panStreamId() != streamId) continue;
        processedFrame = true;

        const QString panId = pan->panId();
        auto& state = m_sHistoryPanState[panId];
        // Only reset on a genuine band change (centre shifts by >500 kHz).
        // Zoom (bandwidth change) must NOT clear markers — the operator
        // zooms in to inspect signals that are already marked.
        const bool bandChanged =
            std::abs(state.centerMhz - pan->centerMhz()) > 0.5;
        state.centerMhz    = pan->centerMhz();
        state.bandwidthMhz = pan->bandwidthMhz();
        // Track observed frame rate (EWMA) so QRM hit thresholds adapt to
        // actual pan fps rather than assuming 25 fps.
        if (state.lastFrameMs > 0) {
            const float dtSec = static_cast<float>(now - state.lastFrameMs) / 1000.0f;
            if (dtSec > 0.0f && dtSec < 1.0f) {  // ignore stale gaps (pan was paused)
                constexpr float kFpsAlpha = 0.05f;
                state.fpsEwma = state.fpsEwma * (1.0f - kFpsAlpha)
                              + (1.0f / dtSec) * kFpsAlpha;
            }
        }
        state.lastFrameMs = now;
        if (bandChanged) {
            state.suppressUntilMs = now + 10000;
            m_sHistoryData.remove(panId);
            m_spectrogramBuffers.remove(panId);  // old frames are for a different band
        }

        // Read the noise floor that the spectrum widget has already measured
        // from this pan's live FFT stream — no hardcoded dBm, adapts to the
        // current band, antenna, and preamp setup automatically.
        auto* panStack = m_panStack;
        SpectrumWidget* sw = panStack ? panStack->spectrum(pan->panId()) : nullptr;
        const float noiseFloor = sw ? sw->noiseFloorDbm() : -1000.0f;

        // Use the active slice mode so USB pans only show USB markers and
        // LSB pans only show LSB markers — no more double-marking one signal.
        QString sliceMode;
        for (auto* slice : m_radioModel.slices()) {
            if (slice && slice->panId() == pan->panId()) {
                sliceMode = slice->mode();
                break;
            }
        }

        // Push this frame into the spectrogram buffer for CNN classification.
        auto& bufPtr = m_spectrogramBuffers[panId];
        if (!bufPtr) { bufPtr = std::make_shared<AetherSDR::SpectrogramBuffer>(); }
        bufPtr->push(bins, pan->centerMhz(), pan->bandwidthMhz());

        const auto detected =
            AetherSDR::detectVoiceSignals(bins, pan->centerMhz(), pan->bandwidthMhz(),
                                          voiceRanges, noiseFloor, sliceMode);
        auto& entries = m_sHistoryData[panId];
        for (const auto& sig : detected) {
            bool found = false;
            for (auto& e : entries) {
                // Merge window: 2 kHz for voice signals — large enough to absorb
                // frame-to-frame edge jitter (bin quantisation + ±400 Hz gap-fill
                // ≈ up to ~700 Hz shift), small enough to stay below the minimum
                // SSB channel spacing of 2.7 kHz so adjacent stations get their
                // own entries.  Half the signal width for wideband QRM so
                // frame-to-frame centre drift doesn't create duplicates.
                const double mergeHz = (sig.widthHz > 8000.0)
                    ? sig.widthHz / 2.0 : 2000.0;
                if (std::abs(e.freqMhz - sig.freqMhz) < mergeHz / 1e6) {
                    e.lastSeenMs = now;
                    e.hitTimestamps.append(now);
                    // Track widest detection: a signal can look narrow when weak
                    // but wider at peak — the widest seen determines classification.
                    e.widthHz = std::max(e.widthHz, sig.widthHz);
                    if (sig.peakDbm > e.peakDbm) {
                        e.peakDbm = sig.peakDbm;
                        e.freqMhz = sig.freqMhz;
                    }
                    // Voice over QRM: if this entry is already QRM-classified
                    // and the current detection is voice-width, flag for double
                    // marking so both a red QRM and a gold voice marker appear.
                    if (e.suspectQrm
                            && sig.widthHz >= 1800.0 && sig.widthHz <= 8000.0) {
                        e.voiceOverQrmLastMs = now;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                SHistoryEntry newEntry;
                newEntry.freqMhz         = sig.freqMhz;
                newEntry.peakDbm         = sig.peakDbm;
                newEntry.mode            = sig.mode;
                newEntry.firstDetectedMs = now;
                newEntry.lastSeenMs      = now;
                newEntry.widthHz         = sig.widthHz;
                newEntry.hitTimestamps   = {now};
                newEntry.lastGapMs       = now;  // treat appearance as a gap reset
                entries.append(std::move(newEntry));
            }
        }
        // CNN classification for borderline-width entries (1800–2500 Hz).
        // Runs only when the model is loaded; gracefully skipped otherwise.
        if (m_signalClassifier.isLoaded()) {
            const auto cit = m_spectrogramBuffers.constFind(panId);
            AetherSDR::SpectrogramBuffer* buf = (cit != m_spectrogramBuffers.constEnd()) ? cit->get() : nullptr;
            if (buf != nullptr) {
                if (buf->frameCount() >= AetherSDR::SpectrogramBuffer::kMaxFrames) {
                    for (auto& e : entries) {
                        if (e.widthHz >= 1800.0 && e.widthHz <= 2500.0) {
                            const double patchWidthMhz = e.widthHz * 2.0 / 1.0e6;
                            const QVector<float> patch =
                                buf->extractPatch(e.freqMhz, patchWidthMhz);
                            if (!patch.isEmpty()) {
                                const AetherSDR::ClassifierResult res =
                                    m_signalClassifier.classify(
                                        patch,
                                        AetherSDR::SpectrogramBuffer::kMaxFrames,
                                        AetherSDR::SpectrogramBuffer::kPatchFreqBins);
                                if (res.valid) {
                                    // EMA α = 0.15 — gradual update, resilient to
                                    // single-frame misclassification
                                    constexpr float kAlpha = 0.15f;
                                    e.carrierScore = e.carrierScore * (1.0f - kAlpha)
                                                   + res.carrierProb * kAlpha;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Always rebuild so hit-window expiry hides markers promptly
        // even when nothing was detected this frame.
        rebuildSHistoryForPan(panId);
        break;
    }

    if (perfEnabled) {
        if (processedFrame) {
            PerfTelemetry::instance().recordSHistoryProcessed(
                static_cast<double>(PerfTelemetry::nowNs() - perfStartNs) / 1000000.0);
        } else {
            PerfTelemetry::instance().recordSHistorySkipped();
        }
    }
}

// ─── Pan Follow ───────────────────────────────────────────────────────────────

void MainWindow::setPanFollow(bool on)
{
    disconnect(m_panFollowConn);
    disconnect(m_panFollowSliceConn);

    if (!on) return;

    // Re-attach helper: wires frequency tracking to whichever slice 0
    // is currently live. Called on activation and whenever slice 0 is
    // recreated (radio reconnect, slice re-assignment, etc.).
    auto attachToSlice0 = [this]() {
        disconnect(m_panFollowConn);

        auto* s = m_radioModel.slice(0);
        if (!s) {
            // No slice yet — uncheck the button so UI matches reality.
            if (m_titleBar) m_titleBar->setPanFollowChecked(false);
            return;
        }

        auto centerPan = [this, s]() {
            const QString panId = s->panId();
            if (panId.isEmpty()) return;
            const double freq = s->frequency();
            auto* pan = m_radioModel.panadapter(panId);
            if (pan && qFuzzyCompare(pan->centerMhz(), freq)) return;
            const QString freqStr = QString::number(freq, 'f', 6);
            if (pan) pan->applyPanStatus({{"center", freqStr}});
            m_radioModel.sendCommand(
                QString("display pan set %1 center=%2").arg(panId, freqStr));
        };

        centerPan();
        m_panFollowConn = connect(s, &SliceModel::frequencyChanged,
                                  this, [centerPan](double) { centerPan(); });
    };

    attachToSlice0();

    // Re-attach whenever a new slice 0 appears (reconnect / re-assignment).
    m_panFollowSliceConn = connect(&m_radioModel, &RadioModel::sliceAdded,
        this, [this, attachToSlice0](SliceModel* s) {
            if (s && s->sliceId() == 0) attachToSlice0();
        });
}

} // namespace AetherSDR
