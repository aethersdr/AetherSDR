// ─────────────────────────────────────────────────────────────────────────────
// ⚠️  MainWindow is DECOMPOSED (#3351). This file is NOT the default home for new
//     feature code. Feature lifecycle/handlers belong in the matching
//     MainWindow_*.cpp sibling TU; per-object signal wiring belongs in
//     MainWindow_Wiring.cpp. Add here only for genuinely cross-cutting state
//     (central members, the constructor's wireXxx() calls, a guard inside a
//     function that itself can't move). When in doubt, see the map + decision
//     guide: docs/architecture/mainwindow-decomposition.md
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "MainWindowHelpers.h"
#include "WindowGeometryRestore.h"

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
#include "PanRecenterPolicy.h"
#include "PanadapterApplet.h"
#ifdef AETHER_ASR_ENABLED
#include "CopyAssistController.h"
#endif
#include "PanadapterStack.h"
#include "gui/MiniPanApplet.h"
#include "gui/MiniPanScope.h"
#include "gui/MiniPanReslice.h"
#include "PanLayoutDialog.h"
#include "core/RadioMessageTypes.h"   // MessageSeverity for onRadioMessage
#include "core/LogManager.h"
#include "core/ShutdownTrace.h"
#include "core/PerfTelemetry.h"
#include "core/PeripheralSettings.h"
#include "core/VoiceSignalDetector.h"
#include "core/MemoryRecallPolicy.h"
#include "core/StreamStatus.h"
#include "models/PanadapterModel.h"
#include "models/RadioStatusOwnership.h"
#include "models/Nr2SettingsModel.h"
#include "SpectrumWidget.h"
#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#endif
#include "SpectrumOverlayMenu.h"
#include "VfoWidget.h"
#include "AppletPanel.h"
#include "DemoApplet.h"
#include "containers/ContainerManager.h"
#include "RxApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "TxApplet.h"
#include "VkampApplet.h"
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
#include "core/KiwiSdrManager.h"
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
#include "DvkAvailabilityGate.h"
#include "VoiceModeGate.h"
#include "DvkPanel.h"
#include "core/DvkWavTransfer.h"
#include "AmpApplet.h"
#include "MeterApplet.h"
#include "HealthApplet.h"
#include "GpsLocationDialog.h"   // applyCapabilitiesToUi() closes it when hasGpsLocation goes false
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
#include "FramelessMessageBox.h"

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
#include <QAccessible>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
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
#include <QFontMetrics>
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
#include "core/AutomationServer.h"
#include "core/SpotCommandPolicy.h"
#include "core/SpotModeResolver.h"
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#include "RadeApplet.h"
#endif
#include "core/PanadapterStream.h"
#include "core/backends/IRadioBackend.h"   // seam: SimBackend::audioFrameReady wiring
#include "core/backends/hl2/Hl2Backend.h"  // dynamic_cast for WDSP setup progress
#include "core/backends/sim/SimBackend.h"  // dynamic_cast for demo noise controls
#include "workspace/WorkspaceController.h"  // prepareShutdown (phase 7 canvas windows)
#include "workspace/WorkspaceWindow.h"      // shutdown sweep of hidden windows (M1)
#if defined(Q_OS_MAC)
#include "core/VirtualAudioBridge.h"
#include <QFileInfo>
#elif defined(HAVE_PIPEWIRE)
#include "core/PipeWireAudioBridge.h"
#endif
#include <QDebug>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX  // guard against redefinition when an earlier include/toolchain predefines it (#4031)
#endif
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
// kPanLayoutRestore* constants moved to MainWindowHelpers.h (#3351 Phase 2c).
// kSwrSweep* constants moved to MainWindowHelpers.h (#3351 Phase 1e) —
// shared between the constructor timer setup here and MainWindow_SwrSweep.cpp.
constexpr const char* kSuppressAudioDeviceNotificationsKey =
    "SuppressAudioDeviceNotifications";
constexpr const char* kStatusBarCompactLabelObjectName = "statusBarCompactLabel";

QString statusBarCompactLabelStyle(const QString& color)
{
    return QStringLiteral(
        "QLabel#statusBarCompactLabel { color: %1; font-size: 12px; background: transparent; }")
        .arg(color);
}

void applyStatusBarCompactLabelStyle(QLabel* label, const QString& color)
{
    if (!label) {
        return;
    }

    label->setObjectName(kStatusBarCompactLabelObjectName);
    AetherSDR::ThemeManager::instance().applyStyleSheet(label, statusBarCompactLabelStyle(color));
}

int statusBarCompactTextWidth(const QStringList& samples, int horizontalPadding)
{
    QFont font = QApplication::font();
    font.setPixelSize(12);
    const QFontMetrics metrics(font);

    int width = 0;
    for (const QString& sample : samples) {
        width = qMax(width, metrics.horizontalAdvance(sample));
    }
    return width + horizontalPadding;
}

void reserveStatusBarStackWidth(QWidget* stack, const QStringList& samples, int minimumWidth)
{
    if (!stack) {
        return;
    }

    stack->setMinimumWidth(qMax(minimumWidth, statusBarCompactTextWidth(samples, 16)));
}

// The station label's resting type size. Single-token values keep it.
constexpr int kStationFontPx = 21;

// Property carrying the size the label is currently styled at, so the
// stylesheet is only re-applied when it actually changes.
constexpr char kStationFontPxProperty[] = "aetherStationFontPx";

QString statusBarStationLabelStyle(int fontPx)
{
    return QStringLiteral(
        "QLabel { color: {{color.text.primary}}; font-size: %1px; "
        "background: {{color.background.0}}; "
        "border: 1px solid rgba(255,255,255,128); padding: 2px 12px; }")
        .arg(fontPx);
}

// Floor for the shrink below. Smaller than this stops reading as the station
// identity and starts reading as one more compact telemetry label (those are
// 12px), so a name that will not fit even here is elided instead.
constexpr int kStationMinFontPx = 15;

// Text width a multi-word nickname may occupy before it is shrunk. This is a
// layout budget, not a measurement of any particular string: the station box
// is centred between the radio-info stack and the GPS/reference stack, and at
// full size a two-word nickname pushes into the latter. Against our HL2 this
// resolves to kStationMinFontPx for "Hermes-Lite 2".
constexpr int kStationWideTextBudgetPx = 105;

// The largest size in [kStationMinFontPx, kStationFontPx] at which the text
// still fits the budget on ONE line. Returns the floor when nothing fits;
// stationFittedText() then elides at that size.
int stationFittedFontPx(const QString& text)
{
    for (int px = kStationFontPx; px >= kStationMinFontPx; --px) {
        QFont candidate = QApplication::font();
        candidate.setPixelSize(px);
        if (QFontMetrics(candidate).horizontalAdvance(text)
                <= kStationWideTextBudgetPx) {
            return px;
        }
    }
    return kStationMinFontPx;
}

// One line, elided only if it still overruns the budget at the floor size.
QString stationFittedText(const QString& text, int fontPx)
{
    QFont font = QApplication::font();
    font.setPixelSize(fontPx);
    const QFontMetrics metrics(font);
    if (metrics.horizontalAdvance(text) <= kStationWideTextBudgetPx) {
        return text;
    }
    return metrics.elidedText(text, Qt::ElideRight, kStationWideTextBudgetPx);
}

// A callsign or a radio nickname. Single-token values (N0CALL, ANT1-AV640,
// 70CM-RXA-XVTR) render at kStationFontPx, byte-identical to before. A value
// containing whitespace is shrunk to fit on ONE line instead of widening the
// status bar, down to kStationMinFontPx, eliding below that.
//
// WHITESPACE IS THE TRIGGER, NOT WIDTH, and that is not a detail: what
// distinguishes the two is that the HL2 has no operator-set nickname, so
// Hl2Discovery::effectiveNickname() substitutes the model string — two words
// — while every real callsign and Flex nickname is a single token.
//
// A width threshold cannot be used in its place: measured at kStationFontPx on
// real radios, "ANT1-AV640" occupies 157px against "Hermes-Lite 2" at 170px.
// The two are 13px apart, so any budget low enough to shrink the HL2 name
// would shrink the Flex one almost as far.
bool setStatusBarStationText(QLabel* label, const QString& text)
{
    if (!label) {
        return false;
    }

    const QString trimmed = text.trimmed();
    // std::any_of over QChar::isSpace rather than a QRegularExpression: this is
    // reached from the MeterModel::hwTelemetryChanged handler, so a per-call
    // pattern compile would land on every PA-temperature delta.
    const bool multiWord = std::any_of(trimmed.cbegin(), trimmed.cend(),
                                       [](QChar c) { return c.isSpace(); });
    const int fontPx = multiWord ? stationFittedFontPx(trimmed) : kStationFontPx;
    const QString rendered =
        multiWord ? stationFittedText(trimmed, fontPx) : text;

    bool changed = false;
    if (label->property(kStationFontPxProperty).toInt() != fontPx) {
        label->setProperty(kStationFontPxProperty, fontPx);
        AetherSDR::ThemeManager::instance().applyStyleSheet(
            label, statusBarStationLabelStyle(fontPx));
        changed = true;
    }
    if (label->text() != rendered) {
        label->setText(rendered);
        changed = true;
    }
    label->ensurePolished();
    const int minimumWidth = label->sizeHint().width() + 2;
    if (label->minimumWidth() != minimumWidth) {
        label->setMinimumWidth(minimumWidth);
        changed = true;
    }
    return changed;
}

// "Gateware 75" on a radio that supplies a label, "4.2.20.41343" on one that
// does not. The caller never asks which family it is talking to — the word
// arrives from the backend, so a future Icom/Yaesu gets this for free.
QString statusBarVersionText(const QString& label, const QString& version)
{
    if (version.isEmpty() || label.isEmpty()) {
        return version;
    }
    return QStringLiteral("%1 %2").arg(label, version);
}

// Does the model string already say who made the radio?
//
// "FLEX-8400M" does; "IC-705" does not, and neither does "705". The test is
// whether the model STARTS WITH the manufacturer's leading word once both are
// reduced to letters and digits — a containment test anywhere in the string
// would match coincidences, and comparing the whole brand would fail the case
// this exists for ("flexradio" vs "flex8400m").
//
// Returns false for an empty manufacturer, so a backend that reports none gets
// no brand row rather than a blank one.
bool modelStringCarriesManufacturer(const QString& model, const QString& manufacturer)
{
    const auto reduce = [](const QString& s) {
        QString out;
        for (const QChar c : s) {
            if (c.isLetterOrNumber()) {
                out.append(c.toLower());
            }
        }
        return out;
    };

    const QString make = reduce(manufacturer);
    if (make.isEmpty()) {
        return true;   // nothing to show — treat as "already carried"
    }

    // The manufacturer's leading WORD, which is what a model string actually
    // repeats. A word ends at a non-letter or at a lower->upper case boundary,
    // so "FlexRadio" -> "flex" (which "FLEX-8400M" does start with, while
    // "flexradio" would not), "Hermes-Lite" -> "hermes", "Icom" -> "icom".
    QString leadingWord;
    bool previousWasLower = false;
    for (const QChar c : manufacturer) {
        if (!c.isLetter()) {
            if (!leadingWord.isEmpty()) {
                break;
            }
            continue;
        }
        if (previousWasLower && c.isUpper()) {
            break;
        }
        previousWasLower = c.isLower();
        leadingWord.append(c.toLower());
    }
    if (leadingWord.isEmpty()) {
        leadingWord = make;
    }

    return reduce(model).startsWith(leadingWord);
}

QString vfoFrequencyText(double mhz)
{
    const long long hz = static_cast<long long>(std::round(mhz * 1e6));
    return QString("%1.%2.%3")
        .arg(static_cast<int>(hz / 1000000))
        .arg(static_cast<int>((hz / 1000) % 1000), 3, 10, QChar('0'))
        .arg(static_cast<int>(hz % 1000), 3, 10, QChar('0'));
}

#ifdef HAVE_HIDAPI
// tmate2*DefaultAction helpers moved to MainWindow_Controllers.cpp (#3351 Phase 2a).
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

// defaultPanLayoutForCount moved to MainWindow_Session.cpp (#3351 Phase 2c).


// xvtrPolicyBandsFrom / xvtrListSummary / xvtrForBandSummary moved to
// MainWindowHelpers (#3351 Phase 1d) — shared with MainWindow_Wiring.cpp.

// parseStatusHandle / streamStatusBelongsToUs  → core/StreamStatus.h

// logXvtrWaterfallDecision moved to MainWindow_Session.cpp (#3351 Phase 2c).

// quantizeIncrementalFollowDelta moved to MainWindow_Wiring.cpp (#3351 Phase 1d).

}  // namespace

// Pure formatting / parsing helpers formerly defined here as file-scope
// statics now live in MainWindowHelpers.{h,cpp} (#3351 Phase 0). Only
// helpers coupled to the mutable shortcut-lease state below remain.

bool MainWindow::isSameDiversityReceivePair(const SliceModel* slice,
                                            const SliceModel* other)
{
    if (!slice || !other || slice == other
        || !slice->diversity() || !other->diversity()) {
        return false;
    }

    const bool parentChildPair =
        (slice->isDiversityParent() && other->isDiversityChild())
        || (slice->isDiversityChild() && other->isDiversityParent());
    if (parentChildPair) {
        return true;
    }

    return !slice->panId().isEmpty() && slice->panId() == other->panId();
}

// ─── Shortcut guard (file-scope for use as std::function<bool()>) ───────────

static constexpr const char* kPaTempUnitSettingKey = "PaTempDisplayUnit";
// kCw*ActionId/Name constants moved to MainWindowHelpers.h (#3351 Phase 1a)
// — now shared with the MIDI/HID registries in MainWindow_Controllers.cpp.

// s_keyboardShortcutsEnabled / s_sliderShortcutLeaseActive definitions lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).

// isCwMomentaryActionId moved to MainWindow_Controllers.cpp (#3351 Phase 2a).

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
    // SmartLink is a Flex service, so in practice this path only runs with a
    // PanadapterStream present -- but it dereferenced it bare, and "in practice"
    // is what the HL2 bring-up kept disproving. Decline instead of crashing.
    if (!m_radioModel.panStream()) {
        qWarning() << "MainWindow: WAN connect requested with no PanadapterStream"
                   << "— backend does not use VITA-49 transport";
        return;
    }
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
                showConnectionDialog();
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

namespace {
// One session at startup (#3445 Camp B: the multi-radio seam is this vector).
std::vector<std::unique_ptr<RadioSession>> makeInitialSessions()
{
    std::vector<std::unique_ptr<RadioSession>> sessions;
    sessions.push_back(std::make_unique<RadioSession>());
    sessions.front()->setSessionId(0);
    return sessions;
}
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_sessions(makeInitialSessions())
    , m_session(m_sessions.front().get())
    , m_radioModel(m_session->radioModel())
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
        // The filter is application-wide and matches by window, because
        // MainWindow's direct children (QStatusBar, the central widget,
        // QSizeGrip) are all native windows that would otherwise swallow
        // every edge event before the top level saw it — see the
        // FramelessResizer header (#4827).  topMoveReserve = TitleBar::kHeight
        // reserves the whole title bar for its own drag-to-move handler and
        // the menu bar / min-max-close controls it hosts, rather than the
        // resizer's 6 px top-edge margin (previously the default 0, which
        // put that margin *inside* the 32 px title bar and shadowed the
        // first few px of all of them — #4886).  MainWindow therefore has
        // no top-edge resize at all in frameless mode, only left/right/
        // bottom; that trade was already implicit before this PR, since the
        // filter was dead on every edge here until now.  Stays installed
        // across frameless toggles — when the system frame is back on, the
        // platform owns resize and our filter no-ops.
        FramelessResizer::install(this, 6, TitleBar::kHeight);

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

        // One-shot migration: remove the dead LastManualSquelchLevel key.
        // #4592 moved manual squelch memory onto SliceModel (radio-
        // authoritative — AGENTS.md "do NOT persist") and stopped writing
        // this key, but existing installs kept the orphaned row, which is
        // now operator-visible via --config list / the Settings Browser
        // (#4603) — maintainer-flagged on the #4604 review as an optional
        // cleanup.
        if (!s.contains("LastManualSquelchLevelMigratedV4604")) {
            s.remove("LastManualSquelchLevel");
            s.setValue("LastManualSquelchLevelMigratedV4604", "True");
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
    initReceivePresentationSync();

    m_audioThread->setObjectName("AudioEngine");
    m_audio = new AudioEngine;  // no parent — will be moved to thread
    m_audio->setRxBoost(
        AppSettings::instance().value("AudioBoost", "False").toString() == "True");
    m_audio->setRxOutputTrimDb(
        AppSettings::instance().value("RxOutputTrimDb", "0.0").toFloat());
    m_audio->setRxBufferCapMs(
        AppSettings::instance().value("AudioBufferMs", "100").toInt());
    m_audio->moveToThread(m_audioThread);
    m_audioThread->start();
    const auto updateAetherDspPolicy = [this](bool) {
        updateAetherDspModePolicy();
    };
    connect(m_audio, &AudioEngine::nr2EnabledChanged,
            this, updateAetherDspPolicy);
    connect(m_audio, &AudioEngine::nr4EnabledChanged,
            this, updateAetherDspPolicy);
    connect(m_audio, &AudioEngine::mnrEnabledChanged,
            this, updateAetherDspPolicy);
    connect(m_audio, &AudioEngine::dfnrEnabledChanged,
            this, updateAetherDspPolicy);
    connect(m_audio, &AudioEngine::rn2EnabledChanged,
            this, updateAetherDspPolicy);
    connect(m_audio, &AudioEngine::nvAfxEnabledChanged,
            this, updateAetherDspPolicy);
    // Start the CW-sidetone record pump on the audio thread (#2539): queued so
    // its QTimer is created + started on m_audio's thread after the move.
    QMetaObject::invokeMethod(m_audio, [ae = m_audio]() { ae->startCwRecordPump(); },
                              Qt::QueuedConnection);
    syncReceivePresentationDelaysToAudioEngine();
    setupAudioDeviceChangeMonitor();

    // QSO audio recorder (#1297) — lives on main thread, audio feeds are thread-safe
    m_qsoRecorder = new QsoRecorder(this);
    // During playback, block live RX audio from entering the buffer
    connect(m_qsoRecorder, &QsoRecorder::muteRxRequested, this, [this](bool mute) {
        // Covers BOTH producers. The disconnect below is the Flex path and is
        // left exactly as it was; the flag is what mutes a seam backend, whose
        // audio reaches the engine through RadioModel::backendAudioFrameReady
        // and so has no stream connection to drop. (PR #4537 review.)
        m_rxMutedForPlayback = mute;
        if (mute) {
            disconnect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            // Only restore the stream→sink feed for backends that actually use it.
            // A backend that emits its own seam audio (the demo, RFC #4288 Route A)
            // never had this connection — re-adding it here would resurrect the
            // double-feed that wirePanStreamRxAudioSinks() deliberately skips, and
            // Qt permits duplicates, so every unmute would stack another copy.
            if (!backendFeedsEngineDirectly()) {
                connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                        m_audio, &AudioEngine::feedAudioData,
                        Qt::UniqueConnection);
            }
        }
    });

    // A backend that demodulates in-process (HL2, the sim) feeds the recorder
    // over the seam and never uses `remote_audio_rx`, so the PC Audio guard
    // below must not apply to it. Read live rather than cached — the sim in
    // particular does NOT lock PC Audio on (it neither host-modulates nor
    // transmits), so this is load-bearing, not belt-and-braces. See
    // QsoRecordStartPolicy.h.
    m_qsoRecorder->setBackendOwnsRxAudioProvider([this]() {
        auto* backend = m_radioModel.backend();
        return backend && backend->ownsRxAudio();
    });

    // A refused start (#4629). The recorder lives below the UI seam and can only
    // report the REASON — the wording is ours. Informational only, deliberately:
    // an "Enable PC Audio and record" action button would flip a setting the
    // operator turned off on purpose, which is the behaviour #1071 removed.
    connect(m_qsoRecorder, &QsoRecorder::recordingBlocked, this,
            [this](AetherSDR::RecordStartDecision reason) {
        if (reason == AetherSDR::RecordStartDecision::BlockedRecordingModeIsRadio) {
            // Unreachable from the GUI — every operator-facing path tests
            // RecordingMode and sends radio-side to SliceModel without ever
            // touching this recorder. Handled rather than swallowed because a
            // silently discarded refusal is the failure mode this whole change
            // exists to remove; if a future caller forgets to route, this says
            // so instead of leaving a stray header-only WAV.
            showRecorderNotice(QStringLiteral("recording-mode-is-radio"),
                tr("Radio Side Recording Is Selected"),
                tr("The radio is doing the recording, so the client recorder was "
                   "not started and no local file was created.\n\n"
                   "Switch to Client Side in Radio Settings → Recording if you "
                   "want the recording saved on this computer instead."));
            return;
        }
        if (reason != AetherSDR::RecordStartDecision::BlockedPcAudioDisabled)
            return;
        showRecorderNotice(QStringLiteral("pc-audio-disabled"),
            tr("PC Audio Required for Client-Side Recording"),
            tr("Client-Side recording captures the same RX audio stream that PC "
               "Audio uses, so it cannot record while PC Audio is off. No file "
               "was created.\n\n"
               "To record while still listening on the radio itself: enable PC "
               "Audio in the title bar, set master volume to 0, and check that "
               "Radio Settings → Receive → \"Mute local audio when "
               "remote\" is Disabled.\n\n"
               "Or switch to Radio Side recording in Radio Settings → "
               "Recording. That records on the radio and does not use PC Audio."));
    });

    // Recorder failures that were previously silent: the directory could not be
    // created, the file could not be opened, or the recording captured nothing
    // (#4629). These have been emitted since the feature shipped with NOTHING
    // connected to them, so every one of them reached the operator as a missing
    // or empty file and no explanation.
    connect(m_qsoRecorder, &QsoRecorder::recordingError, this,
            [this](const QString& error) {
        // One key for all recorder errors: auto-record's start/idle-stop cycle
        // can raise the zero-capture diagnostic once per over, and stacking a
        // modal per cycle would be worse than the silence it replaced.
        showRecorderNotice(QStringLiteral("recorder-error"),
                           tr("QSO Recording"), error);
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
    // Route external playback sinks (post-DSP monitor, QSO playback) through the
    // user-selected output device rather than the system default — without this
    // they play out of whatever the OS currently considers the default, which is
    // rarely the device the user picked in Radio Settings > Audio (#3361).
    // The AudioOutputRouter is the single registry for output-following sinks:
    // each is seeded immediately and re-seeded on every device change, so a
    // future sink follows correctly just by registering here — no new connect to
    // forget (the "uncoupling" hardening, #3306). The forwarder is a
    // QueuedConnection so followers are touched on the GUI thread, matching the
    // previous hand-wired behaviour (outputDeviceChanged is emitted on the audio
    // worker thread).
    m_outputRouter = new AudioOutputRouter(this);
    connect(m_audio, &AudioEngine::outputDeviceChanged, this, [this]() {
        m_outputRouter->setCurrentDevice(m_audio->outputDevice());
    }, Qt::QueuedConnection);
    m_outputRouter->setCurrentDevice(m_audio->outputDevice());
    m_outputRouter->addFollower(m_finalMonitor);
    m_outputRouter->addFollower(m_qsoRecorder);

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
    // Goes on the panadapter as a transient card, NOT the status bar (#4649).
    // A QStatusBar temporary message hides every permanent widget for its whole
    // duration -- the TX indicator, PA temperature and supply voltage among
    // them -- so putting it there blanks exactly the telemetry an operator
    // wants while transmitting, at the one moment they are transmitting.
    // The status bar stays as a fallback for the case where no panadapter can
    // be resolved, so the warning is never simply dropped.
    connect(&m_radioModel, &RadioModel::txFilterBlockingAudio,
            this, [this](const QString& title, const QString& detail,
                         const QString& panId) {
                if (!panId.trimmed().isEmpty() && m_panStack) {
                    if (SpectrumWidget* sw = m_panStack->spectrum(panId.trimmed())) {
                        sw->showTxFilterNotification(title, detail, 8000);
                        return;
                    }
                }
                statusBar()->showMessage(title + QStringLiteral(" - ") + detail, 8000);
            });

    m_networkDiagnosticsHistory = new NetworkDiagnosticsHistory(&m_radioModel, m_audio, this);
    connect(&m_radioModel, &RadioModel::digitalVoiceWaveformDegradationStarted,
            this, [this](const QString& message) {
        if (!message.isEmpty()) {
            statusBar()->showMessage(message, 10000);
        }
    });

    // Local CW sidetone — every key source (serial, MIDI, TCI, CWX, HID)
    // funnels through RadioModel::sendCwKey/sendCwPaddle, which emits
    // cwKeyDownChanged.  Auto-queued connection so the audio thread sees
    // the state change via atomic without any blocking.
    connect(&m_radioModel, &RadioModel::cwKeyDownChanged,
            this, [this](bool down) {
        if (m_audio)
            m_audio->setCwKeyDown(down);   // keys audible + recorder sidetone
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
        m_rxMutedForPlayback = mute;   // seam backends — see the recorder handler
        if (mute) {
            disconnect(m_radioModel.panStream(),
                       &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            // Same rule as the QSO-recorder unmute above: never resurrect the
            // stream→sink feed for a backend that supplies its own seam audio.
            if (!backendFeedsEngineDirectly()) {
                connect(m_radioModel.panStream(),
                        &PanadapterStream::audioDataReady,
                        m_audio, &AudioEngine::feedAudioData,
                        Qt::UniqueConnection);
            }
        }
    });

    // Band plan manager — must be created before buildMenuBar() which references it
    m_bandPlanMgr = new BandPlanManager(this);
    m_bandPlanMgr->loadPlans();

    m_kiwiSdrManager = new KiwiSdrManager(this);
    m_kiwiSdrManager->setOperatorCallsign(m_radioModel.callsign());

    // UpdateChecker — must be created before buildMenuBar() which references it
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::updateAvailable, this, [this](const QString& ver) {
        const QString current = QCoreApplication::applicationVersion();
        FramelessMessageBox box(this);
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
        FramelessMessageBox::information(this, "Check for Updates",
            QString("AetherSDR is up to date (v%1).").arg(ver));
    });
    connect(m_updateChecker, &UpdateChecker::checkFailed, this, [this]() {
        FramelessMessageBox::warning(this, "Check for Updates",
            "Could not reach GitHub. Check your connection and try again.");
    });

    buildMenuBar();
    buildUI();
    loadCenterLockSettings();
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
    connect(minimalShortcut, &QShortcut::activated,
            this, &MainWindow::toggleMinimalModeFromAction);
    connect(minimalShortcut, &QShortcut::activatedAmbiguously,
            this, &MainWindow::toggleMinimalModeFromAction);

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

    // Discovery + heartbeat + SmartLink wiring → wireDiscovery()
    // (MainWindow_Session.cpp, #3351 Phase 2c).
    wireDiscovery();

    // Spot subsystem wiring (DX cluster / spot clients worker thread /
    // HF propagation / dedup+batch forwarding) → wireSpotSubsystem()
    // (MainWindow_Spots.cpp, #3351 Phase 2b). Hoisted back to the
    // constructor so wiring order stays readable here and the spot
    // subsystem doesn't ride along when RadioSession is extracted.
    wireSpotSubsystem();

    // Net Reminder Scheduler: load the operator's saved nets, start the
    // recompute-and-rearm reminder timer, and create the tray/banner notifiers
    // → initNetScheduler() (MainWindow_Nets.cpp). Operator-scoped, client-side;
    // works without a radio connected.
    initNetScheduler();

    // QRZ callsign lookup: CW spotter → lookup service → contact card →
    // wireCallsignLookup() (MainWindow_Callsign.cpp). Client-side; works
    // without a radio connected.
    wireCallsignLookup();

    // Radio-model + TX-audio-stream wiring → wireRadioModel()
    // (MainWindow_Session.cpp, #3351 Phase 2c).
    wireRadioModel();

    // Pan-stream → spectrum, S-history markers, and multi-pan lifecycle →
    // wirePanLifecycle() (MainWindow_Session.cpp, #3351 Phase 2c).
    wirePanLifecycle();

    // ── Per-panadapter signal wiring (extracted for multi-pan support) ──────
    wirePanadapter(m_panApplet);

    // Display overlay connections are now per-pan in wirePanadapter().

    // ── Panadapter stream → audio engine ──────────────────────────────────
    // All VITA-49 traffic arrives on the single client udpport socket owned by
    // PanadapterStream, which strips IF-Data headers and emits audioDataReady().
    // The QAudioSink feed is wired in one helper so a Flex-backend swap can
    // rebind it (the stream is destroyed/rebuilt on a family change;
    // audioDataReady carries Flex RX audio itself, so a missed rebind is
    // silence rather than a degraded feature).
    wirePanStreamRxAudioSinks();
    // The taps that listen alongside the speaker — QSO recorder RX, CW and RTTY
    // — ride RadioModel's normalized bus instead, so they are wired ONCE here
    // and are never part of the rebind. RadioModel outlives the swap.
    wireRxDemodAudioSinks();
    // Separately, wire the backend-OWNED seam signals (audio + spectrum), which do
    // not flow through PanadapterStream: the demo/sim backend delivers RX audio and
    // its panadapter FFT directly over IRadioBackend (no VITA-49), in the same
    // 24 kHz stereo float32 format, so it feeds the identical AudioEngine path.
    // Harmless for Flex, which never emits those seam signals. Done in a helper so
    // it can be re-run whenever RadioModel rebuilds the backend — the connect-time
    // Flex↔Sim swap (RFC #4288) destroys the old backend and builds a new one, and
    // these connections must follow to the new instance or the demo comes up with
    // no audio and no spectrum ("stuck connecting").
    //
    // Driven by backendRebuilt(), the SAME signal that rebinds the PanadapterStream
    // sinks (wireDiscovery). There used to be a second signal, backendChanged(),
    // for exactly this call — and a backend swap needs BOTH rewirings, so emitting
    // either one alone leaves half the wiring dangling. That is precisely what
    // happened twice: the original revision emitted only backendChanged (pan sinks
    // unbound), and the fix for that emitted only backendRebuilt (seam unbound —
    // no demo audio, no ANF/NB, no legacy-NR2 geometry). One event, one signal.
    wireBackendSeam(m_radioModel.backend());
    connect(&m_radioModel, &RadioModel::backendRebuilt, this,
            [this] { wireBackendSeam(m_radioModel.backend()); });
    connect(m_audio, &AudioEngine::receivePresentationOutputAudioReady,
            this, [this](const QString& source, const QString& sourceId,
                         const QByteArray& pcm, int sampleRate) {
        const ReceivePresentationSource syncSource =
            source == QLatin1String("kiwi") ? ReceivePresentationSource::KiwiSdr
                                            : ReceivePresentationSource::Flex;
        feedReceivePresentationSyncAudio(syncSource, pcm, sourceId, sampleRate);
    });

    // KiwiSDR is a receive-only alternate source for the active slice.
    // Wiring is local-only: no Flex radio commands are sent from this path.
    wireKiwiSdr();

    // ── QSO recorder: tap RX audio + TX monitor, trigger on MOX (#1297) ────
    // RX (float32) comes from RadioModel's normalized RX-audio bus (wired in
    // wireRxDemodAudioSinks() above), so it works on every family rather than
    // only on one with a VITA-49 stream; TX (int16 post-limiter monitor) from
    // AudioEngine::txFinalMonitorPcmReady — the source that carries SSB/phone
    // TX. Without the TX tap, Client-Side recordings were full-length silence
    // during transmit (#3556). The recorder MOX-gates the two so the file is a
    // single time-interleaved RX/TX stream.
    connect(m_audio, &AudioEngine::txFinalMonitorPcmReady,
            m_qsoRecorder, &QsoRecorder::feedTxAudio);
    // Host-modulated backends (HL2) take their transmit audio from the SAME tap
    // the recorder uses: fully processed, after the test tone, compressor and
    // EQ. One path means the TONE button, the microphone and the recording all
    // agree with what actually goes on the air. A Flex radio modulates on the
    // radio side and ignores this.
    connect(m_audio, &AudioEngine::txFinalMonitorPcmReady,
            this, [this](const QByteArray& pcm, bool clientLeveled) {
        m_radioModel.submitTxAudio(pcm, AudioEngine::DEFAULT_SAMPLE_RATE,
                                   clientLeveled);
    });
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_qsoRecorder, &QsoRecorder::onMoxChanged);
    // CW/CWX path (#2539): break-in keys the radio without a local MOX edge and
    // without a mic-driven txFinalMonitorPcmReady, so voice wiring alone records
    // silence during CW. The record pump feeds our local sidetone, and
    // cwRecordingActiveChanged opens the recorder's TX gate for our CW — driven
    // by our own keyer, so another client's TX never gates our recorder.
    connect(m_audio, &AudioEngine::cwSidetoneRecordPcmReady,
            m_qsoRecorder, &QsoRecorder::feedTxAudio);
    connect(m_audio, &AudioEngine::cwRecordingActiveChanged,
            m_qsoRecorder, &QsoRecorder::onMoxChanged);

    // ── CW decoder: feed audio ──────────────────────────────────────────
    // Audio feed is global (same audio for all pans) and lives in
    // wireRxDemodAudioSinks() above (RX feed gated on
    // CwDecodeSettings::rxEnabled(), #2417). Text/stats output is routed to the
    // pan owning the active slice via routeCwDecoderOutput(), which re-wires on
    // active slice change (#864).

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

    // RTTY decoder audio feed is wired in wireRxDemodAudioSinks() above,
    // gated on the decoder being running.

    // ── AF gain from applet panel → radio per-slice audio_level ─────────
    connect(m_appletPanel->rxApplet(), &RxApplet::afGainChanged, this, [this](int v) {
        if (auto* s = activeSlice()) s->setAudioGain(v);
    });

    // Demo Noise applet → PanadapterStream's demo-scene setters. The mixer lives
    // on the network thread, so these are Q_INVOKABLE and invoked QUEUED so the
    // mutation lands there (RFC #4288). No-ops unless the demo radio is connected.
    if (auto* demo = m_appletPanel->demoApplet()) {
        // Route the noise controls to the SimBackend's NoiseMixer — the one whose
        // output is actually audible (SimBackend::onAudioTick → audioFrameReady).
        // PanadapterStream has a second, now-unused NoiseMixer left from the old
        // shim path; wiring the applet to THAT one is why the controls did nothing.
        // simBackend() returns nullptr unless the demo backend is active, so these
        // are safely no-ops on a real radio. Same (main) thread → direct calls.
        auto simBackend = [this]() -> SimBackend* {
            return dynamic_cast<SimBackend*>(m_radioModel.backend());
        };
        connect(demo, &DemoApplet::demoNoiseToggled, this,
                [simBackend](const QString& ch, bool on) {
            if (auto* sim = simBackend()) sim->setDemoNoiseEnabled(ch, on);
        });
        connect(demo, &DemoApplet::demoNoiseLevelChanged, this,
                [simBackend](const QString& ch, double db) {
            if (auto* sim = simBackend()) sim->setDemoNoiseLevel(ch, db);
        });
        connect(demo, &DemoApplet::demoNoiseKnobChanged, this,
                [simBackend](const QString& ch, const QString& knob, double v) {
            if (auto* sim = simBackend()) sim->setDemoNoiseKnob(ch, knob, v);
        });
        connect(demo, &DemoApplet::demoPresetRequested, this,
                [simBackend](const QString& preset) {
            if (auto* sim = simBackend()) sim->loadDemoNoisePreset(preset);
        });
        // Fault-injection buttons (RFC #4288 #4) → backend->invokeExtension("sim",…),
        // the same path the `sim` automation verb uses. The signal carries
        // "<fault> [arg]" (e.g. "swr 3.5"); split on the first space.
        connect(demo, &DemoApplet::demoFaultRequested, this,
                [this](const QString& spec) {
            IRadioBackend* backend = m_radioModel.backend();
            if (!backend)
                return;
            const int sp = spec.indexOf(QLatin1Char(' '));
            const QString fault = (sp < 0) ? spec : spec.left(sp);
            QVariant arg;
            if (sp >= 0) {
                bool okD = false;
                const double d = spec.mid(sp + 1).trimmed().toDouble(&okD);
                arg = okD ? QVariant(d) : QVariant(spec.mid(sp + 1).trimmed());
            }
            backend->invokeExtension(QStringLiteral("sim"), fault.toLower(),
                                     /*requestId=*/0, arg);
        });
    }

    // NOTE: demo VFO/mode/ANF/NB forwarding is NOT wired here.
    //
    // It used to be: four connects made in this constructor against the STARTUP
    // backend's RadioConnection, forwarding to PanadapterStream's NoiseMixer.
    // Both halves were wrong. (a) The sim swap replaces that connection and
    // nothing rebound them, so on the demo they were dead. (b) Even bound, they
    // targeted PanadapterStream's mixer — not SimBackend's m_audio, the one you
    // actually hear — the same wrong-mixer bug already fixed for the applet
    // controls.
    //
    // VFO and mode now need no forwarding at all: they arrive at SimBackend
    // through the IRadioBackend seam (setSliceFrequency / setSliceMode), which is
    // rebuilt per backend swap by construction, and drive the audible mixer from
    // there. ANF/NB are wired per-swap in wireBackendSeam().
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
            m_radioVersionLabel->setText(statusBarVersionText(
                m_radioModel.versionLabel(), m_radioModel.version()));
        refreshRadioIdentityLabels();
        // The station/nickname label is set once in onConnectionStateChanged, but
        // the nickname can land afterwards via a radio-status delta — for a real
        // radio the async "info" reply corrects it, and for the demo SimBackend
        // emits radioChanged ~150ms post-connect, so the box would otherwise stay
        // stale/blank. Refresh it here on the same late-arrival path. (main's
        // stale-nickname fix + RFC #4288's empty-demo-box fix — same handler.)
        if (m_stationLabel && !m_radioModel.nickname().isEmpty()) {
            if (setStatusBarStationText(m_stationLabel, m_radioModel.nickname()))
                updateStatusBarMinimumWidth();
        }
    });

    // Propagate late-arriving SmartSDR+ subscription + dual-SCU diversity
    // eligibility to all existing VFOs. Both depend on radio info (license
    // subscription / model) that can arrive after a slice is created — in
    // which case onSliceAdded reads empty strings and the VFO gets stuck in
    // the wrong state (#1356 for SmartSDR+, #1503 for DIV).
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        const bool hasPlus = m_radioModel.licenseSubscription().contains("SmartSDR+");
        const bool divAllowed = m_radioModel.isDiversityAllowed();
        // Refresh extended-DSP (NRL/NRS/RNN/NRF) visibility here too: onSliceAdded
        // pushes it once at slice creation, but if the `model` status arrives after
        // the slice (e.g. GUIClientID session restore) that one-shot value is stale
        // — the AU-510 symptom where the extended filters never appeared. (#2177)
        const bool hasExtendedDsp = m_radioModel.hasExtendedDspFilters();
        if (m_panStack) {
            for (auto* applet : m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                for (auto* vfo : sw->findChildren<VfoWidget*>()) {
                    vfo->setSmartSdrPlus(hasPlus);
                    vfo->setDiversityAllowed(divAllowed);
                    vfo->setHasExtendedDsp(hasExtendedDsp);
                }
            }
        }
    });

    // "license feature" statuses answer "sub license all" asynchronously, well
    // after the DVK indicator is first painted, and the set is cleared on
    // disconnect — so the DVK entitlement gate has to be re-evaluated on every
    // change rather than sampled once. (The clear-on-disconnect emit is what
    // returns the indicator to its unknown-entitlement, fail-open state before
    // the next radio's statuses arrive.)
    connect(&m_radioModel, &RadioModel::licenseFeaturesChanged, this,
            &MainWindow::updateKeyerAvailability);

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

    connect(m_appletPanel->rxApplet(), &RxApplet::wfmActivated,
            this, [this](bool on, int sliceId) {
        if (on) activateWFM(sliceId);
        // Only the WFM slice may deactivate (a mode change on another slice
        // must not kill a running demod), and not during the activation
        // cooldown, which debounces the slice-sync churn that emits spurious
        // off events right after activation.
        else if (sliceId == m_wfmSliceId && !m_wfmCooldown) deactivateWFM();
    });

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

    // ── Title bar: PC Audio, master volume, headphone volume ────────────────
    // The remote_audio_rx stream controls the radio's audio routing:
    // stream exists → audio to PC; stream removed → audio to radio speakers.
    // TCI clients get RX audio via DAX (#1331), not this stream, so PC Audio
    // is free to toggle the stream purely on its own local-sink needs.
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
            // Stop the local sink so audio isn't played locally, then drop
            // the remote_audio_rx stream. TCI clients get RX audio via DAX
            // (#1331), not this stream, so there's no client to keep it
            // alive for.
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->stopRxStream();
            });
            m_radioModel.removeRxAudioStream();
        }

        // On Icom this CLICK -- and only a click -- asks the radio to switch
        // DATA OFF MOD: the network source while on, and whatever the operator
        // had before we first touched it while off. DATA MOD is deliberately
        // left alone, since digital-mode routing is independent operator state.
        //
        // Guarded on the connection for the same reason the audio calls below
        // are: with no session the write is dropped anyway, and all a
        // disconnected click could still do is raise the unverified-model
        // advisory about a radio that is not there.
        const RadioCapabilities caps = m_radioModel.backendCapabilities();
        if (m_radioModel.isConnected()) {
            m_radioModel.setPcAudioEnabled(on);
        }
        if (m_radioModel.isConnected() && caps.takesTxAudioOverSeam
            && !caps.hostModulates) {
            if (on && !m_audio->isTxStreaming()) {
                audioStartTx(m_radioModel.radioAddress(), 4991);
            } else if (!on && m_audio->isTxStreaming()) {
                audioStopTx();
            }
        }
    });
    // Master volume — title bar slider routes through applyMasterVolume()
    // so the TCI `volume:N;` command (#1764) can hit the same code path
    // when tciServer() is created later in this constructor.
    connect(m_titleBar, &TitleBar::masterVolumeChanged,
            this, &MainWindow::applyMasterVolume);
    connect(m_titleBar, &TitleBar::headphoneVolumeChanged,
            &m_radioModel, &RadioModel::setHeadphoneGain);
    connect(m_titleBar, &TitleBar::lineoutMuteChanged, this, [this](bool muted) {
        m_audio->setMuted(muted);
        m_radioModel.sendCommand(QString("mixer lineout mute %1").arg(muted ? 1 : 0));
    });
    connect(m_audio, &AudioEngine::mutedChanged, this, [this](bool muted) {
        m_titleBar->setLineoutMuted(muted);
        auto& s = AppSettings::instance();
        s.setValue("PcAudioMuted", muted ? "True" : "False");
        s.save();
    });

    // PooDoo RX chain status tiles → wirePooDooTiles()
    // (MainWindow_DspApplets.cpp, #3351 Phase 2d).
    wirePooDooTiles();

    connect(m_titleBar, &TitleBar::headphoneMuteChanged,
            &m_radioModel, &RadioModel::setHeadphoneMute);
    connect(&m_radioModel, &RadioModel::audioOutputChanged, this, [this]() {
        m_titleBar->setHeadphoneVolume(m_radioModel.headphoneGain());
        m_titleBar->setHeadphoneMuted(m_radioModel.headphoneMute());
    });

    // Multi-Flex: show when another client is transmitting
    connect(&m_radioModel, &RadioModel::txOwnerChanged,
            m_titleBar, &TitleBar::setOtherClientTx);

    // Status-bar transmit timer — runs only for operator MOX/PTT/VOX, never
    // TCI/DAX (the model gates the source). Hidden idle; 15s hold + fade on
    // unkey is handled inside TitleBar.
    connect(&m_radioModel, &RadioModel::operatorTransmitChanged,
            m_titleBar, &TitleBar::setOperatorTransmitting);

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
    }


    // Meter wiring (S-Meter / Tuner / MTR / HLTH / TX applets) →
    // wireMeters() (MainWindow_Wiring.cpp, #3351 Phase 2a).
    wireMeters();
    // External-controller wiring → wireExternalControllers()
    // (MainWindow_Controllers.cpp, #3351 Phase 2a).
    wireExternalControllers();

    // Client-DSP applet wiring (P/CW, PHNE, EQ, client DSP family, TX chain,
    // PUDU monitor, RX chain edit) → wireDspApplets()
    // (MainWindow_DspApplets.cpp, #3351 Phase 2d).
    wireDspApplets();
    wireWorkspaceCanvas();   // RFC #4887 phase 3 — MainWindow_Workspace.cpp

    // PROC / NOR / DX / DX+ -> the client compressor, on a radio that modulates
    // on this host. After wireDspApplets(), which is what gives the applet panel
    // the AudioEngine this reaches back through.
    wireHostModulatedVoiceChain();


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

    // Unified CAT ports → wireCatPorts() (MainWindow_Session.cpp, #3351 Phase 2d).
    wireCatPorts();

    // DAX IQ routing wiring — established once at construction for all platforms
    // (independent of the audio bridge) → wireDaxIq() (MainWindow_Session.cpp;
    // extracted #3351 Phase 2d, de-gated #3522).
    wireDaxIq();

    // ── Status bar telemetry ──────────────────────────────────────────────────
    // Quality-level colors come from networkQualityColor() (MainWindowHelpers)
    // so the footer label and the diagnostics tooltip share one mapping and
    // never diverge (e.g. "Off" is neutral grey in both, not green here).
    // Map an fps cap to the matching quality-level color for the throttle indicator.
    auto fpsCapColor = [](int fpsCap) -> QString {
        if (fpsCap <= 4) return QStringLiteral("#cc3333"); // Poor
        if (fpsCap <= 8) return QStringLiteral("#cc9900"); // Fair
        return QStringLiteral("#00b4d8");                  // Good
    };

    connect(&m_radioModel, &RadioModel::networkQualityChanged,
            this, [this](const QString& quality, int pingMs) {
        const QString color = networkQualityColor(quality);
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
        const QString tooltip = buildNetworkTooltip(m_radioModel,
                                                     m_adaptiveFpsCap,
                                                     dwellPending);
        m_networkLabel->setToolTip(tooltip);
    });

    connect(&m_radioModel, &RadioModel::adaptiveThrottleChanged,
            this, [this, fpsCapColor](bool active, int fpsCap) {
        m_adaptiveThrottleActive = active;
        m_adaptiveFpsCap = active ? fpsCap : 0;
        if (m_titleBar)
            m_titleBar->setThrottleFlashColor(active ? fpsCapColor(fpsCap) : QString{});
        if (!active) {
            // Throttle lifted — restore the radio values captured in each
            // SpectrumWidget before the transient cap status arrived. Live
            // fps/line-duration status is deliberately held out of the widgets
            // while throttled, so the cap never becomes profile state.
            if (profileLoadRadioStateWritesHeld()) {
                qCDebug(lcProtocol)
                    << "MainWindow: adaptive throttle restore suppressed during profile load";
                return;
            }
            if (!m_panStack) return;
            for (auto* applet : m_panStack->allApplets()) {
                if (!applet) continue;
                auto* sw = applet->spectrumWidget();
                if (!sw) continue;
                const QString panId = applet->panId();
                // Route through requestPanDisplayRates, not raw Flex wire text: a
                // backend that shapes its own display rate (HL2) has no Flex
                // command sink, so the restore silently never arrived and an FPS
                // change made while throttled was lost for good. The dispatcher
                // picks the seam or the wire per backend. (#4470)
                m_radioModel.requestPanDisplayRates(panId, sw->fftFps(),
                                                    sw->wfLineDuration());
            }
        }
    });

    connect(&m_radioModel.meterModel(), &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyVolts) {
        m_lastPaTempC = paTemp;
        m_hasPaTempTelemetry = m_radioModel.meterModel().hasPaTemp();
        updatePaTempLabel();
        // A bare dash, never a zero, for a rail the radio has not reported —
        // the rule the Radio Health dialog already applies to its registers.
        // No unit either: the unit belongs to the value, and there is no
        // value, so "— V" would still be asserting a reading in volts. This
        // signal fires when EITHER half of the hardware telemetry changes, so
        // on a radio that reports PA temperature and no supply rail (HL2:
        // PATEMP, no "+13.8A") every temperature tick used to repaint the
        // 0.0f initialiser formatted to two decimals — indistinguishable from
        // a measurement.
        //
        // Independent of hasSupplyVoltageTelemetry on purpose: that capability
        // decides whether the OPERATOR IS OFFERED the readout, this decides
        // what the readout may claim. A backend that declares the rail but has
        // not yet received a meter definition is still not entitled to print a
        // number. Same separation as the DAX capability and its crash guard.
        const auto& meters = m_radioModel.meterModel();
        m_supplyVoltLabel->setText(
            meters.hasSupplyVoltage()
                ? QStringLiteral("%1%2 V")
                      .arg(meters.hasPaCurrentMeter()
                               ? QStringLiteral("Vd ") : QString(),
                           QString::number(supplyVolts, 'f', 2))
                : QStringLiteral("—"));

        // Update station label (nickname arrives via status after connect)
        const QString nick = m_radioModel.nickname();
        if (!nick.isEmpty() && setStatusBarStationText(m_stationLabel, nick)) {
            updateStatusBarMinimumWidth();
        }
    });
    connect(&m_radioModel.meterModel(), &MeterModel::paCurrentChanged,
            this, [this](float) { updatePaTempLabel(); });
    connect(&m_radioModel.transmitModel(), &TransmitModel::transmittingChanged,
            this, [this](bool) { updatePaTempLabel(); });
    connect(&m_radioModel.transmitModel(), &TransmitModel::tuneChanged,
            this, [this](bool) { updatePaTempLabel(); });

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
    connect(&m_radioModel, &RadioModel::oscillatorChanged, this,
            [this, updateFrequencyReferenceLabel] {
        updateFrequencyReferenceLabel();
        // Optional GPSDO presence arrives after the connection capability
        // event, so re-evaluate the unit-level GPS gate on live oscillator
        // status rather than leaving the initial hidden state latched.
        applyCapabilitiesToUi(m_radioModel.isConnected(),
                              m_radioModel.backendCapabilities());
    });
    updateFrequencyReferenceLabel();

    connect(&m_radioModel, &RadioModel::gpsStatusChanged,
            this, [this, normalizeOscillatorValue, updateFrequencyReferenceLabel](
                         const QString& /*status*/, int /*tracked*/, int /*visible*/,
                         const QString& /*grid*/, const QString& /*alt*/,
                         const QString& /*lat*/, const QString& /*lon*/,
                         const QString& utcTime) {
        updateFrequencyReferenceLabel();
        // Some firmware establishes presence through the GPS status object
        // rather than an oscillator flag.
        applyCapabilitiesToUi(m_radioModel.isConnected(),
                              m_radioModel.backendCapabilities());

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
        // Show the (UTC) date in the operator's regional field order rather than
        // a fixed ISO yyyy-MM-dd (#3511). Force a 4-digit year: the locale's
        // short format is often 2-digit (e.g. 6/21/26), which loses precision
        // and is easy to misread.
        const QLocale loc = QLocale::system();
        QString dateFmt = loc.dateFormat(QLocale::ShortFormat);
        if (!dateFmt.contains(QLatin1String("yyyy")))
            dateFmt.replace(QLatin1String("yy"), QLatin1String("yyyy"));
        m_gpsDateLabel->setText(loc.toString(utc.date(), dateFmt));
        if (m_useSystemClock)
            m_gpsTimeLabel->setText(utc.toString("HH:mm:ssZ"));
    });
    clockTimer->start(1000);

    // Start discovery — show amber indicator while waiting for connection
    if (m_titleBar) m_titleBar->setDiscovering(true);
    m_discovery.startListening();

    // Demo mode (RFC #4288): surface a synthetic "AetherSDR Demo" entry in the
    // connect list so a user with no radio can connect to it. Gated on the
    // Connect-page "Show the AetherSDR demo simulator" setting (default on); the
    // checkbox there adds/removes it live when toggled.
    if (m_connPanel
        && AppSettings::instance().value("ShowDemoRadio", "True").toString() == "True") {
        m_connPanel->addDemoRadio();
    }

    // Saved-radio autoconnect is controlled solely by AutoConnectToLastRadio for
    // both interactive and automation launches (#4421 restored this after #4401's
    // squash pulled in a later reconciliation; the AETHER_AUTOMATION_NO_AUTOCONNECT
    // override and its helper were removed application-wide).
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
                m_connPanel->probeRadio(routedIp, /*restoreSavedFamily=*/true);
            });
        }
    }

    // Restore saved geometry from XML settings
    auto& s = AppSettings::instance();
    const QString geomB64 = s.value("MainWindowGeometry").toString();
    if (!geomB64.isEmpty()) {
        m_startupGeometryForFirstShow = QByteArray::fromBase64(geomB64.toLatin1());
        if (!m_startupGeometryForFirstShow.isEmpty()) {
            // No #4328 re-anchor here on purpose: the window is not mapped yet,
            // so the custom frame's real margins are not in effect.  showEvent()
            // always schedules reapplyStartupGeometryAfterShow() while this blob
            // is non-empty, and that pass supersedes this placement before the
            // user ever sees it.
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
            if (w == centralPanWidget())  sizes[i] = centerW;
            else if (w == m_appletPanel)  sizes[i] = appletW;
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
    ShutdownTrace destructorTrace("main_window.destructor_body");
    qApp->removeEventFilter(this);

    // The qApp::focusChanged lambda (MainWindow_Shortcuts.cpp) runs
    // releaseSliderShortcutLease(), which touches m_shortcutManager and the
    // m_sliderShortcutLease* members. Those are value members, so they are gone
    // by the time the QWidget base destructor's deleteChildren() clears focus on
    // a focused child and re-emits focusChanged. QObject's context auto-disconnect
    // only fires in ~QObject, which is later still. Sever it here (#4857).
    QObject::disconnect(qApp, &QApplication::focusChanged, this, nullptr);

    preparePanadapterUiForShutdown();

    // KiwiSDR profile clients are QObject children, but their disconnect path emits
    // state changes that are wired back into MainWindow and AudioEngine. Tear
    // them down while MainWindow members are still alive instead of waiting for
    // QWidget child cleanup after member destruction.
    if (m_kiwiSdrManager) {
        ShutdownTrace trace("kiwi.manager.destroy");
        QObject::disconnect(m_kiwiSdrManager, nullptr, this, nullptr);
        if (m_audio) {
            QObject::disconnect(m_kiwiSdrManager, nullptr, m_audio, nullptr);
        }
        m_kiwiSdrManager->disconnectAll();
        delete m_kiwiSdrManager;
        m_kiwiSdrManager = nullptr;
    }

    // Stop the CWX sidetone keyer (its own worker thread) before AudioEngine is
    // torn down below — its onKeyDownChange callback touches m_audio->cwSidetone()
    // (#3623). reset() joins the worker, so no key edge can fire afterward.
    {
        ShutdownTrace trace("cwx.keyer.destroy");
        m_cwxLocalKeyer.reset();
    }

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
        ShutdownTrace trace("ax25.dialog.destroy");
        delete m_ax25HfPacketDecodeDialog.data();
        m_ax25HfPacketDecodeDialog = nullptr;
    }

    // Stop audio processing on the worker thread before destruction (#502).
    // Use BlockingQueuedConnection to ensure completion before we proceed.
    if (m_audio && m_audioThread && m_audioThread->isRunning()) {
        AudioEngine* audio = m_audio;
        {
            ShutdownTrace trace("audio.stop.invoke");
            QMetaObject::invokeMethod(audio, [audio]() {
                ShutdownTrace workerTrace("audio.stop.worker");
                audio->setNr2Enabled(false);
                audio->setRn2Enabled(false);
                audio->setNvAfxEnabled(false);
                audio->stopRxStream();
                audio->stopTxStream();
            }, Qt::BlockingQueuedConnection);
        }
        audio->deleteLater();
        {
            ShutdownTrace trace("audio.thread.join");
            m_audioThread->quit();
            if (!m_audioThread->wait(3000))
                trace.fail("thread_join_timeout");
        }
    } else {
        delete m_audio;
    }
    if (m_audioThread && m_audioThread->isRunning()) {
        ShutdownTrace trace("audio.thread.join_retry");
        m_audioThread->quit();
        if (!m_audioThread->wait(3000))
            trace.fail("thread_join_timeout");
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
    {
        ShutdownTrace trace("tci.server.destroy");
        m_session->shutdownTciServer();
    }
#endif

    // Stop external controller thread (#502)
    if (m_extCtrlThread && m_extCtrlThread->isRunning()) {
        // Close serial port on its own thread before stopping it to avoid
        // the cross-thread QObject access crash (QSerialPort has thread affinity
        // to m_extCtrlThread; calling close() from main thread hits a fatal assert).
#ifdef HAVE_SERIALPORT
        {
            ShutdownTrace trace("controllers.serial.close");
            QMetaObject::invokeMethod(m_serialPort, [this] {
                ShutdownTrace workerTrace("controllers.serial.close.worker");
                m_serialPort->close();
            },
                                      Qt::BlockingQueuedConnection);
        }
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
            ShutdownTrace trace("controllers.flex_control.close");
            QMetaObject::invokeMethod(m_flexControl, [this] {
                ShutdownTrace workerTrace("controllers.flex_control.close.worker");
                m_flexControl->close();
            },
                                      Qt::BlockingQueuedConnection);
        }
#endif
#ifdef HAVE_MIDI
        if (m_midiControl) {
            ShutdownTrace trace("controllers.midi.close");
            QMetaObject::invokeMethod(m_midiControl, [this] {
                ShutdownTrace workerTrace("controllers.midi.close.worker");
                m_midiControl->closePort();
            },
                                      Qt::BlockingQueuedConnection);
        }
#endif
#ifdef HAVE_HIDAPI
        if (m_hidEncoder) {
            ShutdownTrace trace("controllers.hid.close");
            QMetaObject::invokeMethod(m_hidEncoder, [this] {
                ShutdownTrace workerTrace("controllers.hid.close.worker");
                m_hidEncoder->close();
            },
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
        {
            ShutdownTrace trace("controllers.thread.join");
            m_extCtrlThread->quit();
            if (!m_extCtrlThread->wait(3000))
                trace.fail("thread_join_timeout");
        }
        // Delete ExtControllers objects synchronously after the thread stops.
        // deleteLater() races with quit() and can leave destructors unrun.
        // On macOS, UlanziDialMacOSManager::stop() calls
        // IOHIDManagerUnscheduleFromRunLoop(...GetMain()) which must run on the
        // main thread — calling it via BlockingQueuedConnection deadlocks because
        // the main thread is blocked waiting for the cross-thread call to return.
        // Safe to call directly here: the ExtControllers thread has stopped so
        // there is no race on m_dialBackend's state.
        if (m_dialBackend) {
            ShutdownTrace trace("controllers.dial.stop");
            m_dialBackend->stop();
        }
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

    // Canvas windows first (phase 7): persist their geometry hints, flush
    // the workspace document, evict their widgets back to the stack/panel
    // — which still own them — and delete the windows explicitly (the
    // #2495 lesson: a floating top-level left to ~QWidget cleanup crashed
    // at exit).  Must precede the stack/container teardown below, which
    // assumes it can reach every applet.
    if (m_workspaceController) {
        m_workspaceController->prepareShutdown();
    }
    // The controller only knows OPEN windows; hidden ones (hide-and-keep)
    // and windows orphaned by disable() live solely in this map — sweep it
    // whole (red-team #4971 M1: disable-then-quit left every canvas window
    // to ~QWidget, the #2495 crash shape).  Their widgets were evicted
    // when they were hidden, so deletion here orphans nothing.
    for (auto it = m_workspaceWindows.begin(); it != m_workspaceWindows.end();
         ++it) {
        if (it.value()) {
            it.value()->prepareShutdown();
            delete it.value();
        }
    }
    m_workspaceWindows.clear();

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
    if (mode == "LSB" || mode == "DIGL" || mode == "FDVL") {
        // Lower-sideband: filter offsets are negative; the audio low edge
        // maps to the high (closest-to-zero) offset and vice versa. FDVL
        // included since #3434 stores it canonically negative — the old
        // pass-through sent positives that flip-flopped the overlay per
        // drag step against the mirrored radio echo.
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

void MainWindow::toggleAetherDspDialog()
{
    // Sibling of toggleAetherialStrip(): the per-slice DSP-tab ADSP button is a
    // toggle, not a one-way launcher (#3877).  When the dialog is already up,
    // close() deletes it (WA_DeleteOnClose) and clears the QPointer; the next
    // press re-creates and re-wires through ensureAetherDspDialog().
    if (m_dspDialog && m_dspDialog->isVisible())
        m_dspDialog->close();
    else
        ensureAetherDspDialog();
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
    // Agent automation bridge toggle (#3646). The dialog already persisted
    // AutomationBridgeEnabled; here we act on it live. AETHER_AUTOMATION
    // force-enables at launch and the dialog disables the toggle in that
    // case, so a stop request can't arrive for an env-forced bridge.
    connect(dlg, &RadioSetupDialog::automationBridgeToggled, this, [this](bool on) {
        if (on) {
            if (!startAutomationBridge())
                qWarning() << "automation bridge failed to start (socket in use?)";
        } else {
            stopAutomationBridge();
        }
    });
    connect(dlg, &RadioSetupDialog::automationBridgeTokenRotated, this,
            [this](const QString& tok) { setAutomationBridgeToken(tok); });
    connect(dlg, &RadioSetupDialog::automationBridgeTxAllowedChanged, this,
            [this](bool allowed) { setAutomationTxAllowed(allowed); });
    connect(dlg, &RadioSetupDialog::automationBridgeReadOnlyChanged, this,
            [this](bool readOnly) { setAutomationReadOnly(readOnly); });
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
            // close() is called unconditionally, never gated on isOpen(): after
            // a port drop the driver is closed but still *wants* the port and is
            // retrying, and close() is the only thing that clears that. Gating
            // on isOpen() would let the driver reclaim a device the operator
            // had just switched off here. close() is idempotent. (#4574)
            if (fcOpen) {
                if (fcPort.isEmpty()) {
                    m_flexControl->close();
                } else if (!m_flexControl->isOpen() || m_flexControl->portName() != fcPort) {
                    m_flexControl->close();
                    m_flexControl->open(fcPort);
                }
            } else {
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
    // VK3AMP hardware variant changed in Peripherals settings -- rescale
    // the live forward-power gauge to match (see RadioSetupDialog.h's own
    // doc comment on vkampVariantChanged).
    connect(dlg, &RadioSetupDialog::vkampVariantChanged, this, [this]() {
        const int saved = PeripheralSettings::deviceInt(
            "Vkamp", "Variant", static_cast<int>(Vkamp::Variant::W2000));
        m_appletPanel->vkampApplet()->setVariant(static_cast<Vkamp::Variant>(saved));
    });
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
            // Unconditional close() — see the matching comment on the
            // serialSettingsChanged path above. (#4574)
            if (fcOpen) {
                if (fcPort.isEmpty()) {
                    m_flexControl->close();
                } else if (!m_flexControl->isOpen() || m_flexControl->portName() != fcPort) {
                    m_flexControl->close();
                    m_flexControl->open(fcPort);
                }
            } else {
                m_flexControl->close();
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
    //
    // Then undo Qt's phantom-caption clamp now the window is mapped and the custom
    // frame's real (zero) top margin applies.  A false return means Qt itself
    // refused the blob — most often its large-screen-variation bail — and in
    // that case the saved rect is exactly what we must not force. (#4328)
    if (restoreGeometry(m_startupGeometryForFirstShow)) {
        reanchorCustomFrameGeometry(m_startupGeometryForFirstShow);
    }

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

void MainWindow::reanchorCustomFrameGeometry(const QByteArray& geometryBlob)
{
#ifdef Q_OS_WIN
    // Call this after every restoreGeometry() on this window, never instead of
    // one.  Qt's restore runs the saved rect through checkRestoredGeometry(),
    // which reserves PM_TitleBarHeight above the top edge and shaves
    // 2 + PM_TitleBarHeight off a window that would otherwise fill the work
    // area — both correct for a native caption, both pure loss once
    // WM_NCCALCSIZE has taken ours away.  The result is a title-bar-sized gap
    // above the window (#4328), and a matching one below it for anyone who
    // sized the window to their screen.  Re-run the clamp here without the
    // caption term.  See src/gui/WindowGeometryRestore.h for the arithmetic.
    if (!mainWindowCustomFrameEnabled()) {
        return;  // native caption present — Qt's reservation is honest.
    }

    SavedWindowGeometry saved;
    if (!parseSavedWindowGeometry(geometryBlob, &saved)) {
        return;
    }

    // Read the saved state, not the live one.  Qt applies the maximized and
    // fullscreen rects without the clamp, so there is nothing to undo — and
    // reading windowState() instead would make this depend on whether the
    // platform has finished applying it yet.
    //
    // Known limitation: a session that exits maximized still restores DOWN
    // into the clamped normalGeometry Qt stored, so the gap reappears on the
    // first un-maximize and closeEvent() then saves it.  Fixing that means
    // re-applying the saved normal rect on the WindowStateChange out of
    // maximized, which is a bigger change to a state machine that also carries
    // the minimal-mode guards — deliberately out of scope here.
    if (saved.maximized || saved.fullScreen) {
        return;
    }

    // Prefer the screen the user actually left the window on; if that monitor
    // is gone, fall back to wherever Qt just put us.  Either way the rect is
    // clamped into that screen's work area, so the custom title bar — the only
    // mouse drag handle a frameless window has — can never land under a
    // taskbar that moved or appeared between sessions.
    const QScreen* target = QGuiApplication::screenAt(saved.normalRect.center());
    if (!target) {
        target = screen();
    }
    if (!target) {
        return;
    }

    // setGeometry() rather than move(): the custom frame's margins are zero, so
    // frame rect == client rect, and the size has to go back too.
    setGeometry(clampFrameToWorkArea(saved.normalRect, target->availableGeometry()));
#else
    Q_UNUSED(geometryBlob);
#endif
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

void MainWindow::updateStatusBarMinimumWidth()
{
    if (m_minimalMode || !m_statusBarContainer || statusBar()->isHidden()) {
        return;
    }

    if (QLayout* layout = m_statusBarContainer->layout()) {
        layout->activate();
    }
    m_statusBarContainer->updateGeometry();
    statusBar()->updateGeometry();

    const int sizeGripAllowance =
        (m_sizeGrip && m_sizeGrip->isVisible()) ? m_sizeGrip->width() + 4 : 4;
    const int statusMinWidth =
        m_statusBarContainer->minimumSizeHint().width() + sizeGripAllowance;
    const int screenWidthCap =
        screen() ? screen()->availableGeometry().width() : statusMinWidth;
    const int boundedMinWidth =
        qBound(1024, statusMinWidth, qMax(1024, screenWidthCap));
    setMinimumSize(boundedMinWidth, qMax(400, minimumHeight()));
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

    // Principle VI fail-safe (#3888): if a momentary keying key (PTT-hold, CW
    // straight key / paddles) is held when the window is deactivated, its
    // KeyRelease is delivered to whatever now has focus and never reaches our
    // event filter — the flag would stay set and TX would stay keyed. Force the
    // whole family back to RX on deactivation. (Belt-and-suspenders for the
    // app-backgrounded case lives in eventFilter via ApplicationStateChange.)
    if (event->type() == QEvent::ActivationChange && !isActiveWindow())
        failSafeMomentaryKeyingToRx("window-deactivate");

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
    ShutdownTrace closeEventTrace("main_window.close_event");
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
    // Same reasoning applies to the VK3AMP peripheral -- it has no
    // radio-disconnect handler to ride at all (design doc's own "zero radio
    // awareness" -- it never learns the app is closing otherwise), so
    // without this the amp's TCP control socket is torn down implicitly by
    // ~VkampConnection() as MainWindow's members are destructed, well after
    // this function returns, leaving the amp holding a stale half-open
    // connection instead of seeing a clean close.
    m_vkampConn.disconnect();

    // Same event-loop reasoning: the operating-state capture flush normally
    // rides the queued backend disconnected() signal, which never lands
    // during close (two queued hops through the HL2 I/O thread). Flush
    // explicitly while the scope still resolves to the connected radio, so
    // the last tune/drive edit before quit is remembered (RFC #4603 PR 3;
    // PR #4619 review).
    m_radioModel.flushPendingOperatingState();

    // Same reason as the TGXL/PGXL sockets above: the D-STAR helper is stopped
    // by the queued RadioModel::connectionStateChanged(false) handler, which
    // does not run during close (the event loop isn't pumped here). Without an
    // explicit stop, quitting while the radio is connected orphans the helper
    // subprocess — it keeps holding the ThumbDV serial port and can block the
    // next AetherSDR launch from reacquiring it.
    stopDigitalVoiceService(true);

    // Restore the Flex slice mute for any active KiwiSDR audio replacement
    // before we exit. The replacement mutes the radio slice (audio_mute=1) so
    // only the Kiwi stream is heard; with the radio's auto_save enabled that
    // muted state is persisted, so a slice left replaced comes up silent on the
    // next launch — KiwiSDR is client-side only and is not reselected, leaving a
    // plain, muted Flex slice the user must manually unmute (#4158). Restoring
    // the pre-Kiwi mute here clears that. Done before the UI teardown below so
    // the radio connection is still open to receive the command.
    if (m_kiwiSdrManager) {
        for (SliceModel* slice : m_radioModel.slices()) {
            if (!slice || !slice->externalReceiveReplacementActive()) {
                continue;
            }
            const int sliceId = slice->sliceId();
            const bool restoreMute =
                m_kiwiSdrVirtualPreviousMute.contains(sliceId)
                    ? m_kiwiSdrVirtualPreviousMute.take(sliceId)
                    : slice->flexAudioMute();
            slice->setExternalReceiveAudioReplacementMute(false, restoreMute);
        }
    }

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
            const QString key = DaxRestorePolicy::keyForIndex(i);
            if (slices[i]->daxChannel() > 0) {
                s.setValue(key, QString::number(slices[i]->daxChannel()));
            } else {
                s.remove(key);
            }
        }
        // #4558: also drop keys beyond the slice count at quit, so a config
        // poisoned by an earlier multi-slice quit self-heals instead of staying
        // armed forever. Both preconditions — connected AND holding a populated
        // slice list — are load-bearing; DaxRestorePolicy::staleKeysToPrune()
        // carries why, and the unit test pins both.
        for (const QString& key :
             DaxRestorePolicy::staleKeysToPrune(m_radioModel.isConnected(),
                                                slices.size())) {
            s.remove(key);
        }
    }

    // DAX IQ channel is radio-authoritative — no client-side persistence needed.
    // The radio echoes daxiq_channel in pan status on reconnect.

    // Persist an automatically-disabled method as enabled so quitting while an
    // audible CW/digital slice is present does not erase the user's selection.
    // A manual button override clears the remembered method, so actual visible
    // state remains authoritative in that case.
    const QString persistedAetherDspMethod =
        m_aetherDspModePolicy.methodForPersistence(activeAetherDspMethod());
    Nr2SettingsModel::instance().setEnabled(
        persistedAetherDspMethod == QStringLiteral("NR2"));
    s.setValue("ClientRn2Enabled",
               persistedAetherDspMethod == QStringLiteral("RN2") ? "True" : "False");
    s.setValue("ClientNr4Enabled",
               persistedAetherDspMethod == QStringLiteral("NR4") ? "True" : "False");
    s.setValue("ClientDfnrEnabled",
               persistedAetherDspMethod == QStringLiteral("DFNR") ? "True" : "False");
    s.setValue("ClientMnrEnabled",
               persistedAetherDspMethod == QStringLiteral("MNR") ? "True" : "False");
    // BNR not persisted — requires manual enable each session

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

    {
        ShutdownTrace trace("radio.disconnect");
        m_radioModel.disconnectFromRadio();
    }
    audioStopRx();

    // Stop spot client worker thread
    if (m_spotThread) {
        if (m_spotThread->isRunning()) {
            DxClusterClient* dxCluster = m_dxCluster;
            DxClusterClient* rbnClient = m_rbnClient;
            WsjtxClient* wsjtxClient = m_wsjtxClient;
            SpotCollectorClient* spotCollectorClient = m_spotCollectorClient;
            PotaClient* potaClient = m_potaClient;
            EibiClient* eibiClient = m_eibiClient;
            N1MMSpotClient* n1mmSpotClient = m_n1mmSpotClient;
#ifdef HAVE_WEBSOCKETS
            FreeDvClient* freedvClient = m_freedvClient;
#endif
            {
                ShutdownTrace trace("spots.dx_cluster.disconnect");
                QMetaObject::invokeMethod(dxCluster, [dxCluster] {
                    ShutdownTrace workerTrace("spots.dx_cluster.disconnect.worker");
                    dxCluster->disconnect();
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.rbn.disconnect");
                QMetaObject::invokeMethod(rbnClient, [rbnClient] {
                    ShutdownTrace workerTrace("spots.rbn.disconnect.worker");
                    rbnClient->disconnect();
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.wsjtx.stop");
                QMetaObject::invokeMethod(wsjtxClient, [wsjtxClient] {
                    ShutdownTrace workerTrace("spots.wsjtx.stop.worker");
                    wsjtxClient->stopListening();
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.collector.stop");
                QMetaObject::invokeMethod(spotCollectorClient,
                                          [spotCollectorClient] {
                    ShutdownTrace workerTrace("spots.collector.stop.worker");
                    spotCollectorClient->stopListening();
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.pota.stop");
                QMetaObject::invokeMethod(potaClient, [potaClient] {
                    ShutdownTrace workerTrace("spots.pota.stop.worker");
                    potaClient->stopPolling();
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.eibi.stop");
                QMetaObject::invokeMethod(eibiClient, [eibiClient] {
                    ShutdownTrace workerTrace("spots.eibi.stop.worker");
                    eibiClient->setEnabled(false);
                },
                                          Qt::BlockingQueuedConnection);
            }
            {
                ShutdownTrace trace("spots.n1mm.stop");
                QMetaObject::invokeMethod(n1mmSpotClient,
                                          [n1mmSpotClient] {
                    ShutdownTrace workerTrace("spots.n1mm.stop.worker");
                    n1mmSpotClient->stopListening();
                },
                                          Qt::BlockingQueuedConnection);
            }
#ifdef HAVE_WEBSOCKETS
            {
                ShutdownTrace trace("spots.freedv.stop");
                QMetaObject::invokeMethod(freedvClient,
                                          [freedvClient] {
                    ShutdownTrace workerTrace("spots.freedv.stop.worker");
                    freedvClient->stopConnection();
                },
                                          Qt::BlockingQueuedConnection);
            }
#endif
            dxCluster->deleteLater();
            rbnClient->deleteLater();
            wsjtxClient->deleteLater();
            spotCollectorClient->deleteLater();
            potaClient->deleteLater();
            eibiClient->deleteLater();
            n1mmSpotClient->deleteLater();
#ifdef HAVE_WEBSOCKETS
            freedvClient->deleteLater();
#endif
            {
                ShutdownTrace trace("spots.thread.join");
                m_spotThread->quit();
                if (!m_spotThread->wait(3000))
                    trace.fail("thread_join_timeout");
            }
        } else {
            delete m_dxCluster;
            delete m_rbnClient;
            delete m_wsjtxClient;
            delete m_spotCollectorClient;
            delete m_potaClient;
            delete m_eibiClient;
            delete m_n1mmSpotClient;
#ifdef HAVE_WEBSOCKETS
            delete m_freedvClient;
#endif
        }
        m_dxCluster = nullptr;
        m_rbnClient = nullptr;
        m_wsjtxClient = nullptr;
        m_spotCollectorClient = nullptr;
        m_potaClient = nullptr;
        m_eibiClient = nullptr;
        m_n1mmSpotClient = nullptr;
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

    m_pttHoldActive = false;
    m_cwStraightKeyActive = false;
    m_cwLeftPaddleActive = false;
    m_cwRightPaddleActive = false;
    m_lastCwPaddleTraceId.store(0, std::memory_order_relaxed);
    m_lastCwPaddleSourceMs.store(0, std::memory_order_relaxed);

    if (m_iambicKeyer && m_iambicKeyer->isRunning()) {
        m_iambicKeyer->setPaddleState(false, false);
        m_iambicKeyer->reset();
    }
    if (m_audio)
        m_audio->setCwKeyDown(false);   // clear audible + recorder sidetone

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
                          tciServer());
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

// AX.25 dialog + KISS TNC startup live in MainWindow_DigitalModes.cpp (#3351 Phase 1e).
void MainWindow::showPropDashboard()
{
    showOrRaisePersistent(m_propDashboardDialog, m_propForecast);
}

// Slider shortcut lease + eventFilter() lives in MainWindow_Shortcuts.cpp (#3351 Phase 1c).
void MainWindow::toggleConnectionDialog()
{
    if (m_connPanel->isVisible()) {
        hideConnectionDialog();
        return;
    }

    showConnectionDialog();
}

QJsonObject MainWindow::automationTxTimerSnapshot() const
{
    if (!m_titleBar)
        return QJsonObject{{QStringLiteral("visible"), false},
                           {QStringLiteral("running"), false}};
    return QJsonObject::fromVariantMap(m_titleBar->txTimerState());
}

// The agent automation-bridge lifecycle (startAutomationBridge / stop /
// isRunning / endpoint / setAutomationBridgeToken / setAutomationTxAllowed)
// lives in MainWindow_Session.cpp with the rest of the session/subsystem
// wiring — not in this monolith. automationTxTimerSnapshot above is the
// per-frame status accessor and stays here with the other automation*
// snapshot accessors.

void MainWindow::showConnectionDialog()
{
    if (!m_connPanel) {
        return;
    }

    // xcb/XWayland (#4725): once this window has been through a hide() —
    // e.g. the auto-hide-on-successful-connect below — showing it again can
    // leave it wedged in the ICCCM "Withdrawn" WM_STATE indefinitely, even
    // though Qt's own show() succeeds and isVisible() reports true. Verified
    // with `xprop` on the affected system (Ubuntu 26.04, Mutter/XWayland):
    // WM_STATE stays Withdrawn and the window never reappears in
    // _NET_CLIENT_LIST_STACKING — genuinely absent from the screen, not just
    // unfocused or buried. Not reproduced under native Wayland, which has no
    // ICCCM Withdrawn/Normal state machine to get stuck in — consistent with
    // the dialog always working there. Forcing Qt to fully destroy and
    // recreate the native window before every re-show sidesteps whatever
    // stale state Mutter is keying off of. Explicitly scoped to xcb and to a
    // genuine re-show (not already visible): destroying/recreating the native
    // window costs a flicker and drops transient window-manager state, so it
    // should not fire on platforms that never had this bug, or when the
    // dialog is merely raised while already open (windowHandle() is also
    // still null before the very first show, making this a no-op there too).
    // Skipping the already-visible case does mean this cannot rescue a window
    // that is *already* wedged — the exact state in which isVisible() lies —
    // but it never has to: from here on every re-show maps a window the
    // compositor has not seen before, so that state stops being reachable.
    const bool isXcb = QGuiApplication::platformName() == QLatin1String("xcb");
    if (isXcb && !m_connPanel->isVisible()) {
        if (QWindow* win = m_connPanel->windowHandle()) {
            win->destroy();
        }
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

    m_connPanel->fitToScreen(screen);
    // Frame coordinates throughout: QWidget::move() positions a top-level
    // widget by its frame top-left, so anchoring on the client rect would leave
    // the window a title bar lower than intended. screenFitFrameSize() is the
    // same arithmetic constrainedFrameTopLeft() clamps with — one source, so
    // the anchor and the clamp cannot drift apart.
    const QSize frameSize = m_connPanel->screenFitFrameSize();
    QPoint frameTopLeft(labelCenter.x() - frameSize.width() / 2,
                        statusBarTop.y() - frameSize.height() - 8);

    if (screen) {
        frameTopLeft = m_connPanel->constrainedFrameTopLeft(
            frameTopLeft, screen->availableGeometry());
    }

    m_connPanel->move(frameTopLeft);
    m_connPanel->show();

    const auto raiseAndActivate = [this] {
        m_connPanel->raise();
        m_connPanel->activateWindow();
    };
    // Under xcb, deferred to a clean event-loop turn rather than fired
    // synchronously from whatever real input event got us here. Two call
    // sites reach this function from inside a live X11 input/grab context:
    // the station label's double-click (still inside the eventFilter's event
    // dispatch) and the "Connect to Radio..." menu action (still inside the
    // QMenu popup's own grab teardown). raise()/activateWindow() called
    // synchronously there can race that context. Mirrors the identical class
    // of fix already applied to automation-driven button clicks in
    // AutomationServer.cpp (menu/dialog-popup re-entrancy). The grab being
    // raced is X11's, so this is scoped to xcb for the same reason the window
    // recreation above is: every other platform keeps the synchronous
    // activation it has always had. m_connPanel is the timer's context
    // object, so the callback is dropped if the panel is destroyed first.
    if (isXcb) {
        QTimer::singleShot(0, m_connPanel, raiseAndActivate);
    } else {
        raiseAndActivate();
    }
}

void MainWindow::hideConnectionDialog()
{
    if (m_connPanel) {
        m_connPanel->hide();
    }
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
    const auto& meters = m_radioModel.meterModel();
    if (meters.hasPaCurrentMeter()) {
        const bool liveTxCurrent =
            (m_radioModel.transmitModel().isTransmitting()
             || m_radioModel.transmitModel().isTuning())
            && meters.hasPaCurrent();
        m_paTempLabel->setText(liveTxCurrent
            ? QString("Id %1 A").arg(meters.paCurrent(), 0, 'f', 1)
            : QStringLiteral("Id —"));
        m_paTempLabel->setToolTip(QStringLiteral("PA drain current"));
        return;
    }
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
    connect(m_titleBar, &TitleBar::minimalModeRequested,
            this, &MainWindow::toggleMinimalModeFromAction);
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
    // Sizing belongs to the panel now. It sets these same two values in its own
    // constructor and then adjusts them per screen in fitToScreen(), which
    // every show path here runs first — a hardcoded pair at this level went
    // stale the moment the panel gained a row, and that is what #4515 was.
    m_connPanel->hide();

    // CWX panel — left of spectrum, hidden by default
    m_cwxPanel = new CwxPanel(&m_radioModel.cwxModel(), splitter);
    // Provide state probes so CWX can guard its F1-F12 / ESC app-wide
    // shortcuts on the TX slice's mode + transmit state.  CWX keys the TX
    // slice, so the mode guard follows it, not the selected RX slice — matching
    // the indicator's availability (#1552, #4173).
    m_cwxPanel->setTxModeProvider([this]() {
        auto* s = m_radioModel.txSlice();
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

        BandStackSettings::instance().addEntry(m_radioModel.settingsScope(), entry);
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.settingsScope(), m_bandPlanMgr);
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
        BandStackSettings::instance().removeEntry(m_radioModel.settingsScope(), index);
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.settingsScope(), m_bandPlanMgr);
    });

    // Clear All — with confirmation to avoid accidental loss during contests
    connect(bsPanel, &BandStackPanel::clearAllRequested, this, [this]() {
        if (BandStackSettings::instance().entries(m_radioModel.settingsScope()).isEmpty())
            return;
        auto answer = QMessageBox::question(
            this, "Clear All Bookmarks",
            "Remove all band stack bookmarks?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        BandStackSettings::instance().clearAllEntries(m_radioModel.settingsScope());
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.settingsScope(), m_bandPlanMgr);
    });

    // Clear band bookmarks (from grouped header right-click)
    connect(bsPanel, &BandStackPanel::clearBandRequested, this,
            [this](double lowMhz, double highMhz) {
        BandStackSettings::instance().clearBandEntries(
            m_radioModel.settingsScope(), lowMhz, highMhz);
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.settingsScope(), m_bandPlanMgr);
    });

    // Group by band toggle
    connect(bsPanel, &BandStackPanel::groupByBandChanged, this, [](bool grouped) {
        BandStackSettings::instance().setGroupByBand(grouped);
    });

    // Auto-expiry setting changed
    connect(bsPanel, &BandStackPanel::autoExpiryChanged, this, [](int minutes) {
        BandStackSettings::instance().setAutoExpiryMinutes(minutes);
    });

    // Auto-save dwell setting changed
    connect(bsPanel, &BandStackPanel::autoSaveDwellChanged, this, [this](int seconds) {
        BandStackSettings::instance().setAutoSaveDwellSeconds(seconds);
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
                    m_radioModel.settingsScope(), maxAge);
        if (removed > 0) {
            m_panStack->bandStackPanel()->loadBookmarks(
                m_radioModel.settingsScope(), m_bandPlanMgr);
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
        if (profileLoadRadioStateWritesHeld()) return;
        if (m_radioModel.serial().isEmpty()) return;
        if (m_radioModel.transmitModel().isTransmitting()) return;
        if (QDateTime::currentMSecsSinceEpoch() < m_bsConnectGraceUntilMs) return;

        auto* slice = activeSlice();
        if (!slice) return;
        const double freqMhz = slice->frequency();

        // Skip if any existing entry is within ±100 Hz on this radio
        // (avoids re-stacking the exact same station after a brief retune).
        auto existing = BandStackSettings::instance().entries(m_radioModel.settingsScope());
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
                    m_radioModel.settingsScope(), oldestAutoIdx);
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
        BandStackSettings::instance().addEntry(m_radioModel.settingsScope(), entry);
        m_panStack->bandStackPanel()->loadBookmarks(
            m_radioModel.settingsScope(), m_bandPlanMgr);
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

        // Update m_panApplet for the new active pan. setActivePanApplet()
        // re-targets the decoders and refreshes the panels, so visibility
        // follows the active slice (not whichever slice appears first on the
        // newly active pan) and stale docks are cleared on every other pan
        // (#4409).
        if (auto* applet = m_panStack->panadapter(panId))
            setActivePanApplet(applet);
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
        "QLabel { background: transparent; }");

    const QString valStyle  = "QLabel { color: #8aa8c0; font-size: 21px; }";
    const QString sepStyle  = "QLabel { color: #304050; font-size: 21px; }";
    const QString greyInd   = "QLabel { color: #404858; font-weight: bold; font-size: 21px; }";
    const QString greenInd  = "QLabel { color: #00e060; font-weight: bold; font-size: 21px; }";
    const QString redInd    = "QLabel { color: #e04040; font-weight: bold; font-size: 21px; }";
    const QString greyIndLg = "QLabel { color: #404858; font-weight: bold; font-size: 24px; }";
    const QString greenIndLg= "QLabel { color: #00e060; font-weight: bold; font-size: 24px; }";

    // Use a container with HBoxLayout for 3-section layout:
    // [left items] → stretch → [STATION centered] → stretch → [right items]
    m_statusBarContainer = new QWidget(this);
    auto* hbox = new QHBoxLayout(m_statusBarContainer);
    hbox->setContentsMargins(6, 0, 6, 0);
    hbox->setSpacing(6);

    auto addSep = [&]() -> QLabel* {
        auto* sep = new QLabel(" · ");
        sep->setStyleSheet(sepStyle);
        hbox->addWidget(sep);
        return sep;
    };

    // Automation indicator chip — visible ONLY when the agent automation bridge
    // is active (AETHER_AUTOMATION). Uses the canonical AppSettings automation
    // agent name so the chip and tooltip match the identity announced to the
    // radio. Legacy environment names are resolved by AppSettings. (#3646)
    // Kept deliberately separate from the station-nickname label so it never
    // fights radio status updates.
    if (qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        const QString agent = AppSettings::instance().automationAgentName();
        m_automationChip = new QLabel(QStringLiteral("\U0001F916 ") + agent);
        m_automationChip->setObjectName(QStringLiteral("automationChip"));
        m_automationChip->setAccessibleName(
            QStringLiteral("Agent automation bridge active: %1").arg(agent));
        m_automationChip->setStyleSheet(
            "QLabel { color: #0b0e12; background: #f0a000; font-weight: bold;"
            " font-size: 18px; border-radius: 4px; padding: 2px 10px; }");
        m_automationChip->setToolTip(
            QStringLiteral("Agent automation bridge active — other MultiFlex stations see this client as \"%1\"")
                .arg(agent));
        hbox->addWidget(m_automationChip);
        addSep();
    }

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
        addPanBtn->setObjectName("addPanadapterButton");
        addPanBtn->setAccessibleName("Add Panadapter");
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

#ifdef AETHER_ASR_ENABLED
    m_asrIndicator = new QLabel("ASR");
    m_asrIndicator->setStyleSheet(greyIndLg);
    m_asrIndicator->setCursor(Qt::PointingHandCursor);
    m_asrIndicator->setToolTip("Speech-to-text (Copy Assist) — click to toggle");
    m_asrIndicator->installEventFilter(this);
    hbox->addWidget(m_asrIndicator);
#endif

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

    // Manufacturer (top) + radio model (middle) + version (bottom) stacked.
    //
    // THREE SLOTS, NEVER THREE ROWS. The status bar is a fixed 46 px and a 12 px
    // label needs ~16, so a third visible row does not fit — which is fine,
    // because no radio fills all three. A Flex's model string carries its own
    // brand ("FLEX-8400M"), so the make row is hidden and the stack reads
    // model + version exactly as before; an Icom reports "IC-705" and no
    // firmware version at all, so it reads make + model. Each label is hidden
    // when it has nothing to say, so the stack is always two rows tall.
    auto* radioStack = new QWidget;
    auto* radioVbox = new QVBoxLayout(radioStack);
    radioVbox->setContentsMargins(0, 0, 0, 0);
    radioVbox->setSpacing(0);
    radioVbox->setAlignment(Qt::AlignVCenter);
    m_radioMakeLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_radioMakeLabel, QStringLiteral("{{color.text.secondary}}"));
    m_radioMakeLabel->setAlignment(Qt::AlignCenter);
    m_radioMakeLabel->setVisible(false);
    radioVbox->addWidget(m_radioMakeLabel);
    m_radioInfoLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_radioInfoLabel, QStringLiteral("{{color.text.secondary}}"));
    m_radioInfoLabel->setAlignment(Qt::AlignCenter);
    m_radioVersionLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_radioVersionLabel, QStringLiteral("{{color.text.secondary}}"));
    m_radioVersionLabel->setAlignment(Qt::AlignCenter);
    radioVbox->addWidget(m_radioInfoLabel);
    radioVbox->addWidget(m_radioVersionLabel);
    hbox->addWidget(radioStack);

    // ── Center stretch → STATION → stretch ───────────────────────────────
    hbox->addStretch(1);

    m_stationNickLabel = new QLabel("N0CALL");
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        m_stationNickLabel, statusBarStationLabelStyle(kStationFontPx));
    m_stationNickLabel->setProperty(kStationFontPxProperty, kStationFontPx);
    m_stationNickLabel->setAlignment(Qt::AlignCenter);
    m_stationNickLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    setStatusBarStationText(m_stationNickLabel, m_stationNickLabel->text());
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
    auto reserveTelemetryStack = [](QWidget* stack, const QStringList& samples) {
        reserveStatusBarStackWidth(stack, samples, kTelemetryStackMinWidth);
    };

    // GPS satellites (top) + lock status (bottom) stacked
    auto* gpsStack = new QPushButton;
    gpsStack->setObjectName(QStringLiteral("gpsStatusButton"));
    gpsStack->setAutoDefault(false);
    gpsStack->setDefault(false);
    gpsStack->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    gpsStack->setCursor(Qt::PointingHandCursor);
    gpsStack->setFocusPolicy(Qt::TabFocus);
    gpsStack->setToolTip(QStringLiteral("Open GPS & Station Location"));
    gpsStack->setAccessibleName(QStringLiteral("GPS and station location"));
    gpsStack->setAccessibleDescription(
        QStringLiteral("Open the live GPS, map, satellite reception, and time dashboard"));
    // Flat, label-style resting state so the two rows sit on the same baselines
    // as the neighbouring plain-QWidget telemetry stacks, with hover / pressed /
    // focus feedback so the click target stays discoverable.
    //
    // The focus ring uses `outline` rather than `border`: Qt honours QSS
    // `outline` only on `:focus` (it is wired to the focus-rect paint path), so
    // hover must use `border` instead — an `outline` there silently never
    // paints. Neither property perturbs this widget's layout: QStyleSheetStyle
    // does not fold the button's frame into the contents rect its child layout
    // sees, so the two rows keep the sibling stacks' y positions and full label
    // width in every state. Do not add `padding` here — that one does consume
    // layout space and would offset the rows against the borderless siblings.
    ThemeManager::instance().applyStyleSheet(gpsStack, QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 0; }"
        "QPushButton:hover { background: {{color.background.1}}; "
        "border: 1px solid {{color.border.strong}}; }"
        "QPushButton:pressed { background: {{color.background.2}}; }"
        "QPushButton:focus { outline: 1px solid {{color.border.accent}}; }"));
    connect(gpsStack, &QPushButton::clicked,
            this, &MainWindow::showGpsLocationDialog);
    reserveTelemetryStack(gpsStack, {
        QStringLiteral("GPS: 12/12"),
        QStringLiteral("Ref: Ext 10M"),
        QStringLiteral("[Unlocked]"),
        QStringLiteral("[No 10M]")
    });
    auto* gpsVbox = new QVBoxLayout(gpsStack);
    gpsVbox->setContentsMargins(0, 0, 0, 0);
    gpsVbox->setSpacing(0);
    gpsVbox->setAlignment(Qt::AlignVCenter);
    m_gpsLabel = new QLabel("");
    m_gpsLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    applyStatusBarCompactLabelStyle(m_gpsLabel, QStringLiteral("{{color.text.secondary}}"));
    m_gpsLabel->setAlignment(Qt::AlignCenter);
    m_gpsStatusLabel = new QLabel("");
    m_gpsStatusLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    applyStatusBarCompactLabelStyle(m_gpsStatusLabel, QStringLiteral("{{color.text.secondary}}"));
    m_gpsStatusLabel->setAlignment(Qt::AlignCenter);
    gpsVbox->addWidget(m_gpsLabel);
    gpsVbox->addWidget(m_gpsStatusLabel);
    hbox->addWidget(gpsStack);
    m_gpsStatusButton = gpsStack;

    m_gpsSeparator = addSep();

    // CPU (top) + Memory (bottom) stacked
    {
        auto* cpuStack = new QWidget;
        reserveTelemetryStack(cpuStack, {
            QStringLiteral("CPU: 100.0%"),
            QStringLiteral("Mem: 99999 MB")
        });
        auto* cpuVbox = new QVBoxLayout(cpuStack);
        cpuVbox->setContentsMargins(0, 0, 0, 0);
        cpuVbox->setSpacing(0);
        cpuVbox->setAlignment(Qt::AlignVCenter);
        m_cpuLabel = new QLabel("CPU: \u2014");
        applyStatusBarCompactLabelStyle(m_cpuLabel, QStringLiteral("{{color.text.secondary}}"));
        m_cpuLabel->setAlignment(Qt::AlignCenter);
        m_cpuLabel->setToolTip("AetherSDR process CPU usage");
        m_memLabel = new QLabel("Mem: \u2014");
        applyStatusBarCompactLabelStyle(m_memLabel, QStringLiteral("{{color.text.label}}"));
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
                applyStatusBarCompactLabelStyle(m_cpuLabel, color);
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
    reserveTelemetryStack(paStack, {
        QStringLiteral("PA 248.0°F"),
        QStringLiteral("PA 120.0°C"),
        QStringLiteral("99.99 V")
    });
    auto* paVbox = new QVBoxLayout(paStack);
    paVbox->setContentsMargins(0, 0, 0, 0);
    paVbox->setSpacing(0);
    paVbox->setAlignment(Qt::AlignVCenter);
    m_paTempLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_paTempLabel, QStringLiteral("{{color.text.secondary}}"));
    m_paTempLabel->setAlignment(Qt::AlignCenter);
    m_paTempLabel->setCursor(Qt::PointingHandCursor);
    m_paTempLabel->installEventFilter(this);
    updatePaTempLabel();
    m_supplyVoltLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_supplyVoltLabel, QStringLiteral("{{color.text.label}}"));
    m_supplyVoltLabel->setAlignment(Qt::AlignCenter);
    paVbox->addWidget(m_paTempLabel);
    paVbox->addWidget(m_supplyVoltLabel);
    hbox->addWidget(paStack);

    addSep();

    // Network label (top) + quality (bottom) stacked
    auto* netStack = new QWidget;
    reserveTelemetryStack(netStack, {
        QStringLiteral("Network:"),
        QStringLiteral("[Very Good]")
    });
    netStack->setCursor(Qt::PointingHandCursor);
    auto* netVbox = new QVBoxLayout(netStack);
    netVbox->setContentsMargins(0, 0, 0, 0);
    netVbox->setSpacing(0);
    netVbox->setAlignment(Qt::AlignVCenter);
    auto* netTitle = new QLabel("Network:");
    applyStatusBarCompactLabelStyle(netTitle, QStringLiteral("{{color.text.secondary}}"));
    netTitle->setAlignment(Qt::AlignCenter);
    netVbox->addWidget(netTitle);
    m_networkLabel = new QLabel("");
    m_networkLabel->setAccessibleName(tr("Network status"));
    m_networkLabel->setAccessibleDescription(
        tr("Network quality; double-click to open full diagnostics"));
    applyStatusBarCompactLabelStyle(m_networkLabel, QStringLiteral("{{color.text.label}}"));
    m_networkLabel->setTextFormat(Qt::RichText);
    m_networkLabel->setAlignment(Qt::AlignCenter);
    m_networkLabel->setToolTip(buildNetworkTooltip(m_radioModel,
                                                   m_adaptiveFpsCap,
                                                   m_radioModel.pendingThrottleLift()));
    m_networkLabel->installEventFilter(this);
    m_networkTooltipRefreshTimer.setInterval(1000);
    connect(&m_networkTooltipRefreshTimer, &QTimer::timeout, this, [this] {
        if (!m_networkLabel || !m_networkLabel->underMouse()) {
            m_networkTooltipRefreshTimer.stop();
            return;
        }
        const QString tooltip = buildNetworkTooltip(m_radioModel,
                                                     m_adaptiveFpsCap,
                                                     m_radioModel.pendingThrottleLift());
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
    reserveTelemetryStack(timeStack, {
        QStringLiteral("12/31/2026"),
        QStringLiteral("31/12/2026"),
        QStringLiteral("HH:mm:ssZ")
    });
    auto* timeVbox = new QVBoxLayout(timeStack);
    timeVbox->setContentsMargins(0, 0, 0, 0);
    timeVbox->setSpacing(0);
    timeVbox->setAlignment(Qt::AlignVCenter);
    m_gpsDateLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_gpsDateLabel, QStringLiteral("{{color.text.secondary}}"));
    m_gpsDateLabel->setAlignment(Qt::AlignCenter);
    m_gpsDateLabel->setMinimumWidth(kTelemetryStackMinWidth);
    m_gpsTimeLabel = new QLabel("");
    applyStatusBarCompactLabelStyle(m_gpsTimeLabel, QStringLiteral("{{color.text.secondary}}"));
    m_gpsTimeLabel->setAlignment(Qt::AlignCenter);
    m_gpsTimeLabel->setMinimumWidth(kTelemetryStackMinWidth);
    timeVbox->addWidget(m_gpsDateLabel);
    timeVbox->addWidget(m_gpsTimeLabel);
    hbox->addWidget(timeStack);

    statusBar()->addWidget(m_statusBarContainer, 1);
    updateStatusBarMinimumWidth();
    updateBandStackIndicator();

    // S History Markers expiry — sweeps stale detections once per second
    m_sHistoryExpireTimer = new QTimer(this);
    m_sHistoryExpireTimer->setInterval(1000);
    connect(m_sHistoryExpireTimer, &QTimer::timeout,
            this, &MainWindow::expireSHistoryMarkers);
    m_sHistoryExpireTimer->start();

    setupAetherClock();

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
    const int  target   = catPortTargetCount();  // bounds applet VFO letters, not port count

    for (int i = 0; i < kCatPorts; ++i) {
        if (!catPort(i)) continue;

        const QString prefix = QString("CatPort_%1_").arg(i);
        const bool portEnabled = s.value(prefix + "Enabled", "False").toString() == "True";
        const int  portNum     = s.value(prefix + "Port", "").toInt();
        // A CAT port is a control channel, not a 1:1 mapping to a slice — don't
        // cap how many configured ports start by the radio's receiver count
        // (#3693). Receiver capacity bounds the VFO-letter choices per port
        // (catPortTargetCount() feeds the applet), not whether a port runs.
        const bool shouldRun   = masterOn && portEnabled && (portNum >= 1024);

        if (shouldRun && !catPort(i)->isRunning()) {
            // Re-apply config in case dialect/VFO was changed while stopped
            QString d = s.value(prefix + "Dialect", "Rigctld").toString();
            CatDialect dial = (d == "FlexCAT") ? CatDialect::FlexCAT
                            : (d == "TS2000")  ? CatDialect::TS2000
                            : CatDialect::Rigctld;
            catPort(i)->setDialect(dial);
            catPort(i)->setVfoA(s.value(prefix + "VfoA", "0").toInt());
            catPort(i)->setVfoB(s.value(prefix + "VfoB", "-1").toInt());
            catPort(i)->start(static_cast<quint16>(portNum));
        } else if (!shouldRun && catPort(i)->isRunning()) {
            catPort(i)->stop();
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
    s.setValue("CatPort_0_VfoB",    "-1");   // rigctld is single-VFO — no VFO B
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

void MainWindow::onConnectionStateChanged(bool connected)
{
    if (m_shuttingDown) {
        return;
    }

    // A completed connect clears the auto-connect attempt count for that radio;
    // a drop settles any attempt still recorded as in flight. Both go through
    // one place so the count cannot leak across sessions.
    noteAutoConnectFinished(connected);

    m_connPanel->setConnected(connected);
    updateExperimentalRadioSupport(connected);

    // Demo mode: reveal the Demo Noise control tile only while connected to the
    // synthetic demo radio; hide it for real radios. On demo connect, push the
    // applet's control state to the engine so the audio scene == what the sliders
    // show (the applet owns the startup scene — no drift). (RFC #4288)
    if (m_appletPanel) {
        auto* conn = m_radioModel.connection();
        const bool demo = connected && conn && conn->isSyntheticDemo();
        m_appletPanel->setDemoVisible(demo);
        if (demo) {
            if (auto* applet = m_appletPanel->demoApplet())
                applet->pushSceneToEngine();
        }
    }

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
        m_terminalConnectionError.clear();
        m_suppressStartupPanLayoutRearrange = false;
        // #4558: open the last-session DAX restore window for this connect's
        // slice enumeration only (see DaxRestorePolicy.h). The primary close is
        // the first live slice removal; the settle timer is belt-and-braces for
        // a session that never removes one, and 10 s clears a slow WAN/SmartLink
        // status trickle with a wide margin. The generation it carries keeps a
        // previous connect's pending timeout from closing this window.
        m_daxRestore.onConnected();
        const int daxGen = m_daxRestore.generation();
        QTimer::singleShot(10000, this, [this, daxGen]() {
            m_daxRestore.onSettleTimeout(daxGen);
        });
        m_layoutRestoreUntilMs = kPanLayoutRestoreWaitingForFirstPan;
        m_radioInfoLabel->setText(m_radioModel.model());
        m_radioVersionLabel->setText(statusBarVersionText(
            m_radioModel.versionLabel(), m_radioModel.version()));
        refreshRadioIdentityLabels();
        setStatusBarStationText(m_stationLabel, m_radioModel.nickname());
        updateStatusBarMinimumWidth();
        m_connStatusLabel->setText("Connected");
        m_connPanel->setStatusText("Connected");

        // Slice tab toggle is initialized from infoChanged when radio
        // reports its actual slice capacity (#1278).

        // Show DIV button on dual-SCU radios (ModelCapabilities table, Principle I)
        {
            const bool divAllowed = m_radioModel.isDiversityAllowed();
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
        // to the PC. TCI clients get RX audio via DAX (#1331), not
        // remote_audio_rx, so PC Audio off means no stream and no sink.
        // The PC Audio toggle handler starts/stops the sink when the user
        // flips it.
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
            BandStackSettings::instance().removeExpiredEntries(
                m_radioModel.settingsScope(), maxAge);
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
            bandStackPanel->loadBookmarks(
                m_radioModel.settingsScope(), m_bandPlanMgr);
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
            if (tciServer() && !tciServer()->isRunning()) {
                int tciPort = AppSettings::instance().value("TciPort", "50001").toInt();
                tciServer()->start(static_cast<quint16>(tciPort));
                qDebug() << "AutoStart: TCI on port" << tciPort
                         << " running=" << tciServer()->isRunning();
            }
            // Only light up the Enable button if the server actually bound —
            // otherwise the UI shows a green "Enable" while the port is in use.
            if (m_appletPanel && m_appletPanel->tciApplet())
                m_appletPanel->tciApplet()->setTciEnabled(
                    tciServer() && tciServer()->isRunning());
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
            const QStringList declaredBands = m_radioModel.declaredBands();
            for (auto* applet : m_panStack->allApplets()) {
                auto* menu = applet->spectrumWidget()->overlayMenu();
                menu->setRadioCapabilities(caps);
                menu->setDeclaredBands(declaredBands);
                menu->setXvtrBands(xvtrBands);
                applyTuningRangeToOverlayMenu(menu);
                applyNotchCapabilities(applet->spectrumWidget());
                applyRadioSideDspToPanDisplay(applet->spectrumWidget());
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
        scheduleDigitalVoiceAutoStart();
        // Auto-connect DX cluster if enabled
        {
            auto& cs = AppSettings::instance();
            if (cs.value("DxClusterAutoConnect", "False").toString() == "True") {
                QString host = cs.value("DxClusterHost", "dxc.nc7j.com").toString();
                quint16 cPort = static_cast<quint16>(cs.value("DxClusterPort", 7300).toInt());
                QString call = cs.value("DxClusterCallsign").toString();
                if (!call.isEmpty() && !m_dxCluster->isConnected())
                    QMetaObject::invokeMethod(m_dxCluster, [=, this] { m_dxCluster->connectToCluster(host, cPort, call); });
            }
            // Auto-connect RBN if enabled
            if (cs.value("RbnAutoConnect", "False").toString() == "True") {
                QString host = cs.value("RbnHost", "telnet.reversebeacon.net").toString();
                quint16 rPort = static_cast<quint16>(cs.value("RbnPort", 7000).toInt());
                QString call = cs.value("RbnCallsign").toString();
                if (call.isEmpty())
                    call = cs.value("DxClusterCallsign").toString();
                if (!call.isEmpty() && !m_rbnClient->isConnected())
                    QMetaObject::invokeMethod(m_rbnClient, [=, this] { m_rbnClient->connectToCluster(host, rPort, call); });
            }
            // Auto-start WSJT-X listener if enabled
            if (cs.value("WsjtxAutoStart", "False").toString() == "True") {
                QString wAddr = cs.value("WsjtxAddress", "224.0.0.1").toString();
                quint16 wPort = static_cast<quint16>(cs.value("WsjtxPort", 2237).toInt());
                if (!m_wsjtxClient->isListening())
                    QMetaObject::invokeMethod(m_wsjtxClient, [=, this] { m_wsjtxClient->startListening(wAddr, wPort); });
            }
            // Auto-start SpotCollector listener if enabled
            if (cs.value("SpotCollectorAutoStart", "False").toString() == "True") {
                quint16 scPort = static_cast<quint16>(cs.value("SpotCollectorPort", 9999).toInt());
                if (!m_spotCollectorClient->isListening())
                    QMetaObject::invokeMethod(m_spotCollectorClient, [=, this] { m_spotCollectorClient->startListening(scPort); });
            }
            // Auto-start POTA polling if enabled
            if (cs.value("PotaAutoStart", "False").toString() == "True") {
                int pInterval = cs.value("PotaPollInterval", 60).toInt();
                if (!m_potaClient->isPolling())
                    QMetaObject::invokeMethod(m_potaClient, [=, this] { m_potaClient->startPolling(pInterval); });
            }
            // Auto-start N1MM/DXLog spot listener if enabled (#2906)
            if (cs.value("N1MMSpotAutoStart", "False").toString() == "True") {
                quint16 nPort = static_cast<quint16>(cs.value("N1MMSpotPort", 12060).toInt());
                if (!m_n1mmSpotClient->isListening())
                    QMetaObject::invokeMethod(m_n1mmSpotClient, [=, this] { m_n1mmSpotClient->startListening(nPort); });
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
        stopDigitalVoiceService(false);

        // #4558: the restore window cannot span a disconnect — the next connect
        // reopens it for its own enumeration. Disconnect teardown does not emit
        // sliceRemoved, so without this the window would stay armed until the
        // settle timer happened to fire.
        m_daxRestore.onDisconnected();

        // Radio disconnected: trim CAT ports back to 1 so apps on channel A
        // stay connected through brief reconnects, higher channels stop cleanly.
        applyCatPortCount();  // catPortTargetCount() returns 1 when !connected

        if (m_layoutRestoreTimer) {
            m_layoutRestoreTimer->stop();
        }
        m_suppressStartupPanLayoutRearrange = false;
        m_layoutRestoreUntilMs = 0;
        clearKiwiSdrPanDisplaySourceOverrides();
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
        QMetaObject::invokeMethod(m_dxCluster, [=, this] { m_dxCluster->disconnect(); });
        QMetaObject::invokeMethod(m_rbnClient, [=, this] { m_rbnClient->disconnect(); });
        QMetaObject::invokeMethod(m_wsjtxClient, [=, this] { m_wsjtxClient->stopListening(); });
        QMetaObject::invokeMethod(m_spotCollectorClient, [=, this] { m_spotCollectorClient->stopListening(); });
        QMetaObject::invokeMethod(m_potaClient, [=, this] { m_potaClient->stopPolling(); });
#ifdef HAVE_WEBSOCKETS
        QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->stopConnection(); });
#endif
        const bool terminalConnectionFailure = !m_terminalConnectionError.isEmpty();
        m_connStatusLabel->setText(terminalConnectionFailure ? "Error" : "Disconnected");
        m_radioInfoLabel->setText("");
        m_radioVersionLabel->setText("");
        m_radioManufacturer.clear();
        refreshRadioIdentityLabels();
        setStatusBarStationText(m_stationLabel, QStringLiteral("N0CALL"));
        updateStatusBarMinimumWidth();
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
            setBandStackPanelVisible(false);
        }
        refreshMemoryBrowsePanel();
        updateBandStackIndicator();
        m_tgxlContainer->setVisible(false);
        m_tgxlSeparator->setVisible(false);
        m_tgxlConn.disconnect();
        m_pgxlConn.disconnect();
        m_pgxlContainer->setVisible(false);
        m_pgxlSeparator->setVisible(false);
        updateStatusBarMinimumWidth();
        m_txIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 21px; }");
        m_txIndicator->setText("TX");
        m_connPanel->setStatusText(
            terminalConnectionFailure
                ? tr("Error: %1").arg(m_terminalConnectionError)
                : tr("Not connected"));
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
        // Clear the cached DAX-TX stream id on connection loss. RadioModel already
        // resets its own dax_tx state on disconnect, but AudioEngine::m_txStreamId
        // was left holding the OLD id — so after a reconnect the AX.25 modem's
        // "create the stream only if txStreamId()==0" guard saw a stale non-zero
        // id, skipped ensureDaxTxStream(), and pumped modem audio to a dead stream
        // (bare carrier, no modulation). Resetting it here makes the next transmit
        // re-create the DAX-TX stream on the new connection.
        if (m_audio)
            m_audio->setTxStreamId(0);

        for (auto it = m_panDisplayStatusConnections.cbegin();
             it != m_panDisplayStatusConnections.cend(); ++it) {
            for (const QMetaObject::Connection& connection : it.value()) {
                QObject::disconnect(connection);
            }
        }
        m_panDisplayStatusConnections.clear();
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

        setPanadapterConnectionAnimation(
            !m_userDisconnected && !terminalConnectionFailure,
            "Reconnecting to radio…");

        if (terminalConnectionFailure) {
            showConnectionDialog();
        }

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
                showConnectionDialog();
            });
            m_reconnectDlg->show();
        }
    }
}

void MainWindow::onConnectionError(const QString& msg)
{
    // A failed auto-connect must count, or the retry is unbounded — see
    // maybeAutoConnectToDiscoveredRadio(). Done here rather than in that slot
    // because this is the only place a connect is known to have ended badly.
    noteAutoConnectFinished(false);
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
        FramelessMessageBox::warning(this, tr("Radio"), text);
        break;
    case MessageSeverity::Error:
        qCCritical(lcGui) << "Radio M-message [Error]:" << text;
        if (interlockMessage)
            break;
        FramelessMessageBox::critical(this, tr("Radio — Error"), text);
        break;
    case MessageSeverity::Fatal:
        qCCritical(lcGui) << "Radio M-message [Fatal]:" << text;
        if (interlockMessage)
            break;
        FramelessMessageBox::critical(this, tr("Radio — Fatal"), text);
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

void MainWindow::wireBackendSeam(IRadioBackend* backend)
{
    if (!backend)
        return;
    // Connections to a PRIOR backend are already gone — Qt drops a connection when
    // either endpoint is destroyed, and RadioModel destroys the old backend before
    // building the new one (teardownBackend/setupBackend on a family switch).
    //
    // We still disconnect explicitly before each connect below, so this helper is
    // idempotent even if called twice for the SAME live backend. That matters
    // because doubling audioFrameReady → feedAudioData makes the engine consume at
    // double rate — the audible scratchy buzz this branch already had to fix once —
    // and Qt::UniqueConnection cannot protect the lambda connects at all.
    disconnect(backend, &IRadioBackend::audioFrameReady, m_audio, nullptr);

    // Demo/sim backend delivers RX audio directly over the seam (no VITA-49, no
    // PanadapterStream) — same 24 kHz stereo float32 format, so it feeds the
    // identical AudioEngine path.
    //
    // SIM ONLY, and the cast is load-bearing. This was originally unconditional,
    // on the reasoning that FlexBackend never emits audioFrameReady so the
    // connection stays idle for "real backends". That holds for Flex and NOT for
    // HL2: HL2 demodulates in-process and audioFrameReady is its ONLY audio
    // route (Hl2Backend.cpp, emit audioFrameReady). It therefore arrived here
    // AND via the RadioModel::backendAudioFrameReady relay in
    // MainWindow_Session.cpp, whose gate — backendFeedsEngineDirectly() — excludes only
    // the sim. Every HL2 frame was delivered twice and the engine consumed at
    // double rate: measured 48043 Hz at the raw tap against a nominal 24000
    // (ratio 2.002), audible as popping and crackling on every mode.
    //
    // Any future in-process backend needs the same treatment. The gate belongs
    // on "does this backend own its RX audio", not on a list of families that
    // happen not to emit the signal today.
    if (dynamic_cast<SimBackend*>(backend) != nullptr) {
        connect(backend, &IRadioBackend::audioFrameReady,
                m_audio, &AudioEngine::feedAudioData);
    }

    // HL2 only, and deliberately: the client-side WDSP chains are this family's
    // alone. Opening them measures FFTW plans, which on a machine with no cached
    // wisdom takes tens of seconds — long enough that a silent connect animation
    // reads as a hung application. Say what is happening in the label that is
    // already on screen rather than adding a dialog for it.
    if (auto* hl2Backend = dynamic_cast<hl2::Hl2Backend*>(backend)) {
        // Disconnected first, like every other lambda connect in this function:
        // the helper promises to be idempotent for the same live backend, and
        // Qt::UniqueConnection cannot cover a lambda.
        disconnect(hl2Backend, &hl2::Hl2Backend::dspSetupProgress, this, nullptr);
        disconnect(hl2Backend, &hl2::Hl2Backend::dspSetupFinished, this, nullptr);
        connect(hl2Backend, &hl2::Hl2Backend::dspSetupProgress, this,
                [this](const QString& stage, int done, int total) {
            // Only while the connect animation is up. A DSP rebuild can happen
            // mid-session (a span change), and hijacking the panadapter with a
            // connect label for it would be a worse lie than saying nothing.
            if (!m_panadapterConnectionAnimationVisible)
                return;
            const QString label = total > 1
                ? tr("%1 (%2 of %3)").arg(stage).arg(done + 1).arg(total)
                : stage;
            setPanadapterConnectionAnimation(true, label);
        });
        // Put the label back to the generic one, so what remains of the connect
        // — the wire coming up, the first EP6 packet — is not still described as
        // DSP setup.
        connect(hl2Backend, &hl2::Hl2Backend::dspSetupFinished, this, [this] {
            if (m_panadapterConnectionAnimationVisible)
                setPanadapterConnectionAnimation(true, tr("Connecting to radio…"));
        });
    }

    // The demo delivers native 128-sample frames, which the improved 1024/4 NR2
    // geometry (#4400) mangles (wobble + dead DSP/RADE); tell the engine to run
    // the MAIN NR2 filter on the original 256/2 geometry while the demo is the
    // source. Real backends clear it, so they keep the improved geometry.
    if (m_audio) {
        const bool isDemo = dynamic_cast<SimBackend*>(backend) != nullptr;
        QMetaObject::invokeMethod(m_audio, [audio = m_audio, isDemo]() {
            audio->setMainSourceLegacyNr2(isDemo);
        }, Qt::QueuedConnection);
    }

    // Demo ANF / NB → the AUDIBLE mixer (SimBackend::m_audio).
    //
    // Wired HERE, per backend swap, rather than once in the constructor: the
    // signals come from the synthetic RadioConnection that SimBackend owns, so a
    // ctor-time binding against the startup backend's connection is dead the
    // moment the sim swaps in. Qt drops these automatically when the backend is
    // destroyed, so re-running per swap cannot duplicate them.
    if (auto* sim = dynamic_cast<SimBackend*>(backend)) {
        if (auto* conn = sim->connection()) {
            disconnect(conn, &RadioConnection::demoAnfChanged, sim, nullptr);
            disconnect(conn, &RadioConnection::demoNbChanged, sim, nullptr);
            connect(conn, &RadioConnection::demoAnfChanged, sim,
                    [sim](bool on) { sim->setDemoAnf(on); }, Qt::QueuedConnection);
            connect(conn, &RadioConnection::demoNbChanged, sim,
                    [sim](bool on) { sim->setDemoNb(on); }, Qt::QueuedConnection);

            // Demo VFO / mode → the AUDIBLE mixer, via the same seam intents a
            // real backend receives.
            //
            // These have to be forwarded from the synthetic wire rather than left
            // to RadioModel's seam calls: the demo's SliceModel is materialised by
            // the synthetic `slice 0 …` status, and that creation path never wires
            // the frequency/mode intents to the backend (it wires them to
            // m_flexBackend, which is null in demo mode). So operator tuning
            // reached the wire as "slice tune 0 …", RadioConnection re-emitted it
            // as demoVfoChanged — and nothing consumed it. Result: the birdie
            // never moved and sideband never changed, in demo mode only.
            disconnect(conn, &RadioConnection::demoVfoChanged, sim, nullptr);
            disconnect(conn, &RadioConnection::demoModeChanged, sim, nullptr);
            connect(conn, &RadioConnection::demoVfoChanged, sim,
                    [sim](double mhz) { sim->setSliceFrequency(0, mhz * 1.0e6); },
                    Qt::QueuedConnection);
            connect(conn, &RadioConnection::demoModeChanged, sim,
                    [sim](const QString& mode) { sim->setSliceMode(0, mode); },
                    Qt::QueuedConnection);
        }
    }

    // Spectrum seam (RFC #4288 Route A): NOT rendered here.
    //
    // IRadioBackend::spectrumFrameReady is consumed by
    // RadioModel::onBackendSpectrumFrame, which re-emits it on the NEUTRAL
    // panFeed path (panFeedSpectrumReady / panFeedWaterfallRowReady) that
    // wireDiscovery() already renders for every backend. An earlier revision
    // also drew it directly here with sw->updateSpectrum(); that bypassed
    // panFeed's other consumers — the adaptive RX filter and the S-history
    // markers received nothing — and drew each frame twice once the relay's
    // stream id was fixed to match the demo's real pan. One producer, one path.
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
                emit bandStackRestoreStarting(slicePanId);
                clearSwrSweepForBandChange(-1, slicePanId, memoryBand);
                m_bandSettings.setCurrentBand(memoryBand);
                // #4142: during the profile-load hold a bare sendCommand()
                // band= write is silently destroyed and the recall lands on
                // the wrong band stack. requestPanBand defers it instead; the
                // dispatch signal starts the reconstruction guard at replay.
                m_radioModel.requestPanBand(slicePanId, stackKeyResult.key);
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

// QSO-recorder notices (#4629 review). Two properties the blocking
// QMessageBox::warning() convenience does not have, both of which turned out to
// matter:
//
//   NON-BLOCKING. warning() spins a nested event loop until the operator
//   clicks. The producers here are the automation bridge's reply path — where
//   it stalled `record start` until a human dismissed the box, observed
//   directly while testing this branch — and QsoRecorder::onMoxChanged, where
//   it would block the GUI thread mid-transmission.
//
//   DEDUPED. Auto-record retries on every MOX rising edge and the zero-capture
//   diagnostic can fire once per over, so a repeating condition would stack a
//   box per transmission, each one re-entering the event loop of the last.
//   QsoRecorder suppresses repeats at the source for auto-record; this is the
//   backstop that holds regardless of which producer fires.
void MainWindow::showRecorderNotice(const QString& key,
                                    const QString& title,
                                    const QString& text)
{
    if (m_recorderNotice) {
        if (m_recorderNoticeKey == key) {
            // Same cause still unacknowledged — surface the existing box rather
            // than adding another saying the same thing.
            m_recorderNotice->raise();
            m_recorderNotice->activateWindow();
            return;
        }
        // A different problem supersedes the old message.
        m_recorderNotice->close();
    }

    auto* box = new QMessageBox(QMessageBox::Warning, title, text,
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    m_recorderNotice = box;
    m_recorderNoticeKey = key;
    box->open();   // NOT exec(): returns immediately, no nested event loop
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
    if (tciServer()) tciServer()->broadcastMasterVolume(pct);
#endif
}

// onSliceAdded() / onSliceRemoved() lives in MainWindow_Wiring.cpp (#3351 Phase 1d).
QString MainWindow::rfGainSettingsKey(SpectrumWidget* sw) const
{
    if (!sw)
        return QStringLiteral("DisplayRfGain");
    const QString base = sw->settingsKey(QStringLiteral("DisplayRfGain"));
    const QString family = m_radioModel.backendCapabilities().family;
    if (family.isEmpty() || family == QLatin1String("flex"))
        return base;                       // unchanged for Flex and when unknown
    return base + QLatin1Char('_') + family;
}

void MainWindow::applyTuningRangeToOverlayMenu(SpectrumOverlayMenu* menu) const
{
    if (!menu)
        return;
    const RadioCapabilities caps = m_radioModel.backendCapabilities();
    // Zero/zero when the backend reports no range, which the menu reads as
    // "unconstrained" — so a Flex, and a disconnected session, both keep every
    // band button live exactly as before.
    menu->setTuningRangeMhz(caps.tuningMinHz / 1.0e6, caps.tuningMaxHz / 1.0e6);
}

void MainWindow::applyNotchCapabilities(SpectrumWidget* sw) const
{
    if (!sw)
        return;
    const RadioCapabilities caps = m_radioModel.backendCapabilities();
    // A DISCONNECTED session keeps the controls rather than having them appear
    // on connect — same reasoning as applyTuningRangeToOverlayMenu, which reads
    // an unreported range as unconstrained. maxNotchFilters defaults to 0, and
    // 0 means "cannot notch", so without this a disconnected app would hide the
    // +TNF button it has always shown.
    const bool connected = m_radioModel.isConnected();
    const int maxNotches = connected ? caps.maxNotchFilters : 1000;
    const bool hasDepth = connected ? caps.notchHasDepth : true;
    // Guard against a backend that declares a ceiling but no width bounds.
    const int minWidth = (connected && caps.notchMinWidthHz > 0.0)
                             ? static_cast<int>(caps.notchMinWidthHz + 0.5) : 10;
    const int maxWidth = (connected && caps.notchMaxWidthHz > 0.0)
                             ? static_cast<int>(caps.notchMaxWidthHz + 0.5) : 12000;

    sw->setNotchCapabilities(maxNotches, hasDepth, minWidth, maxWidth);
    if (auto* menu = sw->overlayMenu())
        menu->setNotchesSupported(maxNotches > 0);
}

// Repaint the make / model / version stack from whatever is currently known.
//
// One owner for all three labels, because their inputs arrive on different
// signals: the model and version come with infoChanged, the manufacturer with
// capabilitiesChanged, and either can land first. Each label hides when it has
// nothing to say — see the stack's construction for why the third row must
// never be visible at the same time as the other two.
void MainWindow::refreshRadioIdentityLabels()
{
    if (!m_radioInfoLabel || !m_radioVersionLabel || !m_radioMakeLabel) {
        return;
    }

    const QString model = m_radioInfoLabel->text();
    const bool showMake = !m_radioManufacturer.isEmpty()
                          && !model.isEmpty()
                          && !modelStringCarriesManufacturer(model, m_radioManufacturer);
    m_radioMakeLabel->setText(showMake ? m_radioManufacturer : QString());
    m_radioMakeLabel->setVisible(showMake);
    m_radioInfoLabel->setVisible(!model.isEmpty());
    m_radioVersionLabel->setVisible(!m_radioVersionLabel->text().isEmpty());
    updateStatusBarMinimumWidth();
}

void MainWindow::applyCapabilitiesToUi(bool connected, const RadioCapabilities& caps)
{
    // See the header for why every flag is `!connected || caps.x` and why each
    // surface gets exactly one owning call.

    // ── Status-bar identity: who made the radio ───────────────────────────
    // NOT `!connected || caps.manufacturer` — the permissive-on-disconnect rule
    // is for controls that would look broken when greyed out with no radio
    // attached. A brand name is a fact about a connected radio, so it clears
    // with the rest of the identity block.
    m_radioManufacturer = connected ? caps.manufacturer : QString();
    refreshRadioIdentityLabels();

    // ── Mic sources: MIC / BAL / LINE / ACC are Flex connectors ────────────
    // A radio that cannot have its input chosen by a client collapses to PC.
    if (m_appletPanel) {
        m_appletPanel->setSelectableMicInputs(!connected || caps.hasSelectableMicInputs);
        // The mic-level gauge follows the METER, not the capability: a Flex
        // does not let a client pick its input either and still publishes
        // MICPEAK. Absence of the meter is the only thing that means the face
        // can never move.
        // Empty on disconnect, which RESTORES the operator's own list rather
        // than stranding them on the last radio's three filters.
        m_appletPanel->setRadioFilterWidths(connected ? caps.rxFilterWidthsHz
                                                      : QList<int>{});
        m_appletPanel->setMicLevelMeterAvailable(
            !connected || m_radioModel.meterModel().hasMicPeakMeter());
    }

    // ── Display dBm scale: who owns it ─────────────────────────────────────
    // A backend that decodes its scope at a fixed calibration (Icom CI-V) has
    // no range command and never echoes one back, so the noise-floor auto-
    // adjust must not try to move a reference level the radio will not confirm.
    // `!connected ||` restores the permissive default on disconnect so the
    // setting cannot leak from an Icom into the next radio connected.
    {
        const bool radioOwnsScale = !connected || caps.radioOwnsDbmScale;
        const QList<SpectrumWidget*> spectra = findChildren<SpectrumWidget*>();
        for (SpectrumWidget* spectrum : spectra) {
            spectrum->setRadioOwnsDbmScale(radioOwnsScale);
        }
    }

    // ── Profiles: the PROF applet, the Profiles menu, and both dialogs ──────
    const bool profiles = !connected || caps.hasProfiles;
    if (m_appletPanel) {
        m_appletPanel->setProfilesVisible(profiles);
    }
    if (m_profilesMenu) {
        // Hide the whole menu, not just its two dialog entries: the rest of it
        // is the radio's global-profile list, which on a radio without profiles
        // is permanently empty. A "Profiles" menu containing nothing but two
        // dialogs that can only report emptiness is worse than no menu.
        m_profilesMenu->menuAction()->setVisible(profiles);
    }
    // Close a dialog already open when the capability goes away — leaving the
    // Profile Manager on screen listing a store the connected radio does not
    // have is the same lie the menu entry would be.
    if (!profiles) {
        if (m_profileManagerDialog) {
            m_profileManagerDialog->close();
        }
        if (m_profileImportExportDialog) {
            m_profileImportExportDialog->close();
        }
    }

    // ── DAX: the DAX and DAX-IQ applets, and the autostart toggle ──────────
    //
    // VISIBILITY ONLY. startDax()'s null-check on panStream() is a separate
    // crash guard and stays exactly where it is — the two are not merged. This
    // stops the operator being offered the controls; that stops a session that
    // somehow reaches the bridge anyway from segfaulting on a null stream.
    const bool dax = !connected || caps.hasDaxStreams;
    if (m_appletPanel) {
        m_appletPanel->setDaxStreamsVisible(dax);
    }
    for (VfoWidget* vfo : findChildren<VfoWidget*>())
        vfo->setDaxVisible(dax);
    if (m_autoDaxAction) {
        m_autoDaxAction->setVisible(dax);
    }

    // ── Extended DSP: the NRS / RNN / NRF buttons in every slice VFO ────────
    //
    // Read through hasExtendedDspFilters() rather than off caps directly. That
    // accessor already applies the permissive rule for this flag in the form it
    // needs: disconnected, it answers from the model-name table, so unplugging
    // restores the filters a saved session's radio model implies instead of
    // blanking them. Taking caps.hasExtendedDsp here would force false on the
    // disconnect edge, because the struct is default-constructed with no
    // backend.
    //
    // The existing pushes at slice creation and on infoChanged stay — they
    // cover a VFO built after this ran. This one covers the reverse: a backend
    // revising the capability while the VFOs already exist.
    const bool extendedDsp = m_radioModel.hasExtendedDspFilters();

    // ── Radio-side DSP: NR / NB / ANF / NRL / ANFL / ANFT in every slice VFO ──
    //
    // Both accessors apply their own permissive rule for the disconnected case,
    // which is why neither reads off `caps` here.
    const bool radioSideDsp = m_radioModel.hasRadioSideDsp();
    // The two NARROWER claims under it, read straight off `caps`.
    //
    // No permissive-on-disconnect rule for either, and they differ on which way
    // that falls. hasLmsNoiseFilters follows radioSideDsp's own accessor
    // upstream — with no radio attached radioSideDsp is already permissive, so
    // taking caps here would hide NRL/ANFL/ANFT on the disconnect edge of a
    // Flex session; it therefore only narrows while CONNECTED. hasManualNotch
    // is the reverse: MN is a new button that must not appear on a radio that
    // has not claimed it, disconnected included.
    const bool lmsNoiseFilters = !connected || caps.hasLmsNoiseFilters;
    const bool manualNotch = connected && caps.hasManualNotch;
    // Same non-permissive rule as manualNotch, and for the mirror of its
    // reason: this one can only ADD the NB button, so a permissive read would
    // put NB on screen for radios that claim neither capability.
    const bool hostNoiseBlanker = connected && caps.hasHostNoiseBlanker;

    if (m_panStack) {
        for (auto* applet : m_panStack->allApplets()) {
            auto* sw = applet->spectrumWidget();
            if (!sw) {
                continue;
            }
            for (auto* vfo : sw->findChildren<VfoWidget*>()) {
                vfo->setHasExtendedDsp(extendedDsp);
                vfo->setHasRadioSideDsp(radioSideDsp);
                vfo->setHasLmsNoiseFilters(lmsNoiseFilters);
                vfo->setHasManualNotch(manualNotch);
                vfo->setHasHostNoiseBlanker(hostNoiseBlanker);
                // The VFO's filter grid and the RX applet's are two views of one
                // radio; only the applet was being told what the hardware has.
                vfo->setRadioFilterWidths(connected ? caps.rxFilterWidthsHz
                                                    : QList<int>{});
            }
            // WNB lives in the pan's overlay menu, not the VFO.
            applyRadioSideDspToPanDisplay(sw);
            // Notch controls follow the same rule: they belong to the pan, and
            // whether the radio can back them is a capability. This is the
            // connect-time push — the per-pan sites cover panadapters created
            // afterwards.
            applyNotchCapabilities(sw);
        }
    }

    // ── APD: one owning method in TxApplet ANDs this with apdConfigurable ────
    //
    // NOT a replacement for apdConfigurable, which stays the authority on
    // whether a Flex reports the predistorter as configurable. This is the
    // second input: apdConfigurable only ever arrives from Flex status, so on a
    // backend that never sends it the row's state would otherwise depend on
    // session history rather than on the connected radio.
    if (m_appletPanel && m_appletPanel->txApplet()) {
        m_appletPanel->txApplet()->setRadioSideDspAvailable(radioSideDsp);
    }

    // ── The 8-band graphic EQ ───────────────────────────────────────────────
    //
    // NO LONGER GATED on hasRadioSideDsp. It used to be, on the reasoning that
    // EqualizerModel emits `eq RXsc`/`eq TXsc` and those reach nothing without a
    // Flex command plane — which was true of the COMMANDS but is the wrong
    // conclusion about the CONTROL. The equalizer the sliders are asking for
    // exists on every family: ClientEq is already in both audio paths, and
    // wireHostModulatedVoiceChain() maps the eight octave bands onto it for any
    // backend without a Flex command plane. Hiding the applet removed a working
    // control rather than an empty one.
    //
    // Still visible-only. The Flex-verb emission in EqualizerModel is unchanged
    // and still goes nowhere on those backends; what makes the sliders act is
    // the ClientEq mapping, and applyGraphicEqToClientEq() excludes Flex so the
    // two never both apply.
    if (m_appletPanel) {
        m_appletPanel->setHardwareEqVisible(true);
    }

    // ── PA supply voltage: the lower row of the status bar's PA stack ───────
    //
    // m_supplyVoltLabel ONLY. m_paTempLabel and the paVbox around them are left
    // alone deliberately: an HL2 reports PA temperature genuinely, so hiding the
    // stack would delete a working readout in order to suppress a broken
    // sibling. The one being suppressed is fed from the Flex-named "+13.8A"
    // meter, and MeterModel emits hwTelemetryChanged whenever EITHER half
    // changes — so on a radio that reports only PA temperature the volts half
    // arrives as its 0.0f initialiser on every tick.
    if (m_supplyVoltLabel) {
        m_supplyVoltLabel->setVisible(!connected || caps.hasSupplyVoltageTelemetry);
        if (!connected) {
            // Drop the previous session's readings instead of leaving them on
            // screen with no radio attached — the same "do not assert what the
            // radio did not report" rule this gate exists for. MeterModel::clear()
            // resets the underlying sentinels, but no further hwTelemetryChanged
            // arrives after a disconnect to repaint either label, so the text has
            // to be dropped here or a Flex's last rail voltage survives its own
            // disconnect. Each row reuses its own established no-value rendering:
            // a bare dash for the rail, "PA --" for the temperature.
            m_supplyVoltLabel->setText(QStringLiteral("—"));
            m_hasPaTempTelemetry = false;
            updatePaTempLabel();
        }
        // Republishing the minimum width cannot currently matter here, and the
        // call is kept only so the gate stays correct if that stops being true:
        // paStack's minimum is PINNED by reserveTelemetryStack() with a
        // "99.99 V" sample, so hiding one of its children leaves
        // m_statusBarContainer->minimumSizeHint() unchanged, and
        // updateStatusBarMinimumWidth() only ever READS that hint. So no stale
        // minimum, gap or clipped size grip is reachable today — and the HL2
        // reclaims no width from the hidden row either, nor would it:
        // "PA 248.0°F" is the wider sample.
        updateStatusBarMinimumWidth();
    }

    // ── The status-bar CWX / DVK / FDX toggles ──────────────────────────────
    //
    // HIDDEN, not disabled. Each of these three is a verb the radio's firmware
    // executes — `cwx …`, `dvk …`, `radio set full_duplex_enabled=` — and on a
    // backend with no command plane to carry it the control has nothing behind
    // it at all. A greyed-out button says "not right now"; these are "not on
    // this radio, ever", and permanently dim labels in the status bar read as a
    // fault the operator can go looking for.
    //
    // TWO neighbours in this row are deliberately NOT gated:
    //
    //   ASR — Copy Assist is host-side. AsrAudioTap subscribes to the engine's
    //   post-DSP RX audio and whisper runs here, so it works on every family.
    //   Hiding it would remove a working control, the mistake the hardware EQ
    //   gate above documents.
    //
    //   TNF — `tnf` is a Flex command-plane verb and by the test above it looks
    //   like it belongs here, but a host-side notch is landing and these are
    //   the surfaces it will drive. Gating it now would mean deleting the
    //   control and putting it straight back. See RadioCapabilities.h.
    //
    // The panels are hidden with their buttons. CWX and DVK are dockable
    // splitter children that survive a reconnect, so a panel left open from a
    // Flex session would otherwise stay on screen next to a hidden button, its
    // F-key rows still drawn against a radio that refuses every send.
    const bool cwx = !connected || caps.hasRadioSideCwKeyer;
    const bool dvk = !connected || caps.hasVoiceKeyer;
    const bool fdx = !connected || caps.hasFullDuplex;

    if (m_cwxIndicator) {
        m_cwxIndicator->setVisible(cwx);
    }
    if (!cwx && m_cwxPanel) {
        m_cwxPanel->hide();
    }
    if (m_dvkIndicator) {
        m_dvkIndicator->setVisible(dvk);
    }
    if (!dvk && m_dvkPanel) {
        m_dvkPanel->hide();
    }
    if (m_fdxIndicator) {
        m_fdxIndicator->setVisible(fdx);
    }
    // updateKeyerAvailability() owns the enabled/dim state and the F1-F12 arming
    // for the two keyers, and applies the same capabilities. Run it here so a
    // mid-session revision disarms the shortcuts at the moment the buttons go,
    // rather than waiting for the next TX-slice mode change.
    updateKeyerAvailability();

    // ── Flex platform features that are not DSP ─────────────────────────────
    if (m_waveformsAction) {
        m_waveformsAction->setVisible(!connected || caps.hasWaveforms);
    }
    if (m_multiFlexAction) {
        m_multiFlexAction->setVisible(!connected || caps.hasMultiClientSessions);
    }

    // ── GPS: the status-bar position readout and the dialog it opens ────────
    //
    // Family capability and unit presence are separate facts. Flex radios can
    // support GPS, but optional-GPSDO 6000-series units must not expose the
    // dashboard until oscillator/GPS status confirms that this unit has one.
    // A control that can only ever wait is worse than an absent one — it reads
    // as a fix that has not arrived yet rather than a receiver that does not
    // exist.
    //
    // NB this stack carries the 10 MHz reference readout as well as the
    // satellite count, so hiding it removes both. That is right for a radio
    // declaring no GPS position source. The reference readout remains part of
    // this GPS-specific stack, so it follows the same unit-presence gate.
    //
    // Hidden WITH its trailing separator, or the divider is left stranded
    // between the neighbouring telemetry stacks.
    //
    // The dialog is closed rather than merely unreachable, for the same reason
    // the Profile Manager is above: a live GPS dashboard left on screen against
    // a radio that has no receiver reports "Waiting for a valid GPS fix"
    // forever, which reads as a broken fix rather than an absent one.
    const bool gps = !connected
        || (caps.hasGpsLocation && m_radioModel.hasGpsHardware());
    if (m_gpsStatusButton) {
        m_gpsStatusButton->setVisible(gps);
    }
    if (m_gpsSeparator) {
        m_gpsSeparator->setVisible(gps);
    }
    if (!gps && m_gpsLocationDialog) {
        m_gpsLocationDialog->close();  // QPointer — guarded above
    }
    // Recompute the status bar's floor, because these two widgets are ~90 px of
    // it. Every other status-bar visibility site in this file pairs the two, and
    // omitting it here happens to work only by connection order: on the connect
    // and disconnect edges onConnectionStateChanged() recomputes AFTER this runs,
    // because wireRadioModel() binds it before setupBackend() binds
    // publishCapabilities. A mid-session capability revision — which
    // capabilitiesChanged now delivers, and which is the whole point of routing
    // through publishCapabilities() — has no such recompute behind it, and leaves
    // the container's minimumSizeHint above the window minimum (a clipped status
    // bar at narrow widths) or below it (a window that cannot be narrowed).
    updateStatusBarMinimumWidth();

}

// Takes the WIDGET, not just its menu, because the waterfall auto-black gate has
// to reach both and they must never disagree about whether HW exists — the menu
// decides what the button shows, the widget decides what actually renders.
void MainWindow::applyRadioSideDspToPanDisplay(SpectrumWidget* sw) const
{
    if (!sw) {
        return;
    }
    auto* menu = sw->overlayMenu();
    if (menu) {
        menu->setRadioSideDspAvailable(m_radioModel.hasRadioSideDsp());
        // The Black Level button's HW position, which is a radio-side display
        // computation rather than radio-side audio DSP — so it needs its own
        // capability, not a ride on hasRadioSideDsp. (#4606)
        menu->setRadioSideAutoBlackAvailable(
            m_radioModel.hasRadioSideWaterfallAutoBlack());
        // The per-pan DAX button and panel, which the capability gate previously
        // missed — so an HL2 kept IQ Ch / DAX Ch selectors that reach nothing.
        menu->setDaxStreamsAvailable(m_radioModel.hasDaxStreams());
    }
    // A MASK, not a rewrite: the operator's stored HW preference survives a
    // session on a radio that has no hardware black level, and comes back by
    // itself on the next Flex. Does not write AppSettings.
    sw->setRadioSideAutoBlackAvailable(
        m_radioModel.hasRadioSideWaterfallAutoBlack());
    // NO RadioModel push from here, deliberately. This runs once PER PAN, from
    // loops in applyCapabilitiesToUi and the infoChanged-bound XVTR refresh —
    // but setWaterfallAutoBlackSource() is global state applied to activeWfId(),
    // so pushing a per-pan value would let the last pan in the loop overwrite
    // the ACTIVE pan's radio setting, and would emit one `display panafall set
    // … auto_black=` per pan on every infoChanged. This function is UI
    // visibility only.
    //
    // The model learns the effective source where it always did, and where the
    // scope is right: the once-per-connect push in wirePanLifecycle (reset via
    // m_displaySettingsPushed on each connection edge) and the operator's own
    // click. Both already use effectiveWfAutoBlackRadioSide(). Nothing is lost
    // by dropping it here — on a backend the mask applies to there is no Flex
    // command plane for auto_black to reach, and the renderer is gated on the
    // effective value in intensityToWaterfallLevel(). (#4606)
}

SliceModel* MainWindow::activeSlice() const
{
    SliceModel* cached = nullptr;
    if (m_activeSliceId >= 0) {
        cached = m_radioModel.slice(m_activeSliceId);
        if (cached && cached->isActive()) {
            return cached;
        }
    }
    for (SliceModel* slice : m_radioModel.slices()) {
        if (slice && slice->isActive()) {
            return slice;
        }
    }
    if (cached) {
        return cached;
    }
    return nullptr;
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

void MainWindow::pushSliceFrequencyToOverlays(SliceModel* slice, double mhz)
{
    if (!slice) {
        return;
    }

    mhz = centerLockDisplayFrequency(slice, mhz);
    snapCenterLocksForTuningSlice(slice, mhz, true);

    const QString freqStr = vfoFrequencyText(mhz);
    auto pushOne = [this, mhz, &freqStr](SliceModel* s) {
        if (!s) {
            return;
        }
        SpectrumWidget* sw = spectrumForSlice(s);
        if (!sw) {
            return;
        }
        if (VfoWidget* vfo = sw->vfoWidget(s->sliceId())) {
            vfo->freqLabel()->setText(freqStr);
        }
        sw->setSliceOverlay(s->sliceId(), mhz,
            s->filterLow(), s->filterHigh(),
            s->isTxSlice(), s->sliceId() == m_activeSliceId,
            s->mode(), s->rttyMark(), s->rttyShift(),
            s->ritOn(), s->ritFreq(),
            s->xitOn(), s->xitFreq(),
            s->diversity(), s->isDiversityParent(),
            s->isDiversityChild(), s->diversityIndex());
    };

    pushOne(slice);
    if (!slice->diversity()) {
        return;
    }

    for (SliceModel* other : m_radioModel.slices()) {
        if (!isSameDiversityReceivePair(slice, other)) {
            continue;
        }
        pushOne(other);
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
    emit bandStackRestoreStarting(slice->panId());
    clearSwrSweepForBandChange(-1, slice->panId(), targetBand);
    m_bandSettings.setCurrentBand(targetBand);
    // #4142: the cross-band typed tune is the reported bug's worst variant —
    // the `slice tune` half survives the hold while a bare sendCommand()
    // band= write is silently destroyed, so the slice lands outside the pan.
    // requestPanBand defers the band-stack swap and replays it band-first; the
    // dispatch signal starts the reconstruction guard at replay.
    m_radioModel.requestPanBand(slice->panId(), stackKeyResult.key);
    QTimer::singleShot(300, this, [this, panId = slice->panId()]() {
        reassertUnmutedSliceAudioForPan(panId);
    });
    return BandStackPreselectResult::Selected;
}

bool MainWindow::tuneBlockedByGuards(SliceModel* slice)
{
    if (!slice)
        return true;
    if (m_swrSweep.running)
        return true;
    if (slice->isLocked()) {
        slice->notifyTuneBlockedByLock();
        auto* sw = spectrumForSlice(slice);
        if (slice->sliceId() == m_activeSliceId && sw) {
            m_updatingFromModel = true;
            pushSliceFrequencyToOverlays(slice, slice->frequency());
            m_updatingFromModel = false;
        }
        return true;
    }
    return false;
}

void MainWindow::applyTuneRequest(SliceModel* slice, double mhz,
                                  TuneIntent intent, const char* source)
{
    if (!slice)
        return;
    if (tuneBlockedByGuards(slice))
        return;

    const double oldFreqMhz = slice->frequency();

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

    holdCenterLockTuneTarget(slice, mhz);
    pushSliceFrequencyToOverlays(slice, mhz);

    const BandStackPreselectResult bandPreselect =
        (intent == TuneIntent::CommandedTargetCenter)
            ? preselectBandStackForTune(slice, mhz, source)
            : BandStackPreselectResult::NotNeeded;
    if (bandPreselect == BandStackPreselectResult::Unsupported) {
        m_centerLockTuneHoldBySlice.remove(slice->sliceId());
        pushSliceFrequencyToOverlays(slice, oldFreqMhz);
        return;
    }

    if (bandPreselect == BandStackPreselectResult::Selected) {
        const int sliceId = slice->sliceId();
        const QString sourceName = QString::fromUtf8(source ? source : "");
        QTimer::singleShot(250, this, [this, sliceId, mhz, sourceName, oldFreqMhz]() {
            auto* pendingSlice = m_radioModel.slice(sliceId);
            if (!pendingSlice || pendingSlice->isLocked() || m_swrSweep.running)
                return;
            holdCenterLockTuneTarget(pendingSlice, mhz);
            pushSliceFrequencyToOverlays(pendingSlice, mhz);
            pendingSlice->tuneAndRecenter(mhz);

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

    const TuneCenteringResult result =
        (intent == TuneIntent::IncrementalTune)
            ? panFollowVfo(slice, mhz, source)
            : revealFrequencyIfNeeded(slice, mhz, intent, source);
    logTunePolicyDecision(source, intent, oldFreqMhz, mhz, result);
}

QJsonObject MainWindow::automationTune(double mhz, int sliceId)
{
    // sliceId -1 = active slice (the original verb shape). A named slice is
    // resolved directly so bridge scripts don't need the racy select → tune →
    // restore flap to drive a non-active slice.
    SliceModel* slice = (sliceId >= 0) ? m_radioModel.slice(sliceId)
                                       : activeSlice();
    if (!slice) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            sliceId >= 0
                                ? QStringLiteral("no slice with id %1").arg(sliceId)
                                : QStringLiteral("no slice to tune")}};
    }
    if (sliceId >= 0 && !m_radioModel.sliceMayBelongToUs(sliceId)) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("refused: slice %1 belongs to another client").arg(sliceId)}};
    }
    if (slice->isLocked()) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("refused: slice %1 is VFO-locked").arg(slice->letter())}};
    }
    if (m_swrSweep.running) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: SWR sweep is running")}};
    }

    applyTuneRequest(slice, mhz, TuneIntent::IncrementalTune, "automation-tune");
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("tune"), mhz},
                       {QStringLiteral("sliceId"), slice->sliceId()},
                       {QStringLiteral("letter"), slice->letter()}};
}

QJsonObject MainWindow::automationSetCenterLock(int sliceId, bool enabled)
{
    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("no slice with id %1").arg(sliceId)}};
    }

    setCenterLockForSlice(slice, enabled);
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("slice"), QStringLiteral("centerlock")},
                       {QStringLiteral("id"), sliceId},
                       {QStringLiteral("enabled"), enabled}};
}

void MainWindow::applyPanRangeRequest(const QString& panId, double centerMhz,
                                      double bandwidthMhz, const char* source)
{
    if (panId.isEmpty() || bandwidthMhz <= 0.0)
        return;

    centerMhz = std::max(centerMhz, bandwidthMhz / 2.0);

    if (kiwiSdrPanDisplaysKiwi(panId)) {
        return;
    }

    auto* pan = m_radioModel.panadapter(panId);

    if (pan) {
        // Effective (pending-else-model) geometry: during the profile-load
        // hold the model deliberately lags a deferred request; comparing
        // against it would treat the user's pending zoom as "already there"
        // — or re-issue it forever (#4142).
        if (qFuzzyCompare(m_radioModel.effectivePanCenterMhz(panId), centerMhz)
            && qFuzzyCompare(m_radioModel.effectivePanBandwidthMhz(panId),
                             bandwidthMhz)) {
            return;
        }
    }

    // Center and bandwidth travel together in one command. Explicit zoom
    // workflows are especially sensitive to center/bandwidth skew; splitting
    // them produced the P1/P2 waterfall-loss and zoom-drift bugs.
    //
    // #4142: this pair is classified profile-owned, so it was silently DROPPED
    // during a profile load — zoom and drag, not just typed frequency entry.
    // requestPanCenter() defers and replays it, and only advances the local
    // model when the command actually reaches the wire.
    const bool sent = m_radioModel.requestPanCenter(panId, centerMhz, bandwidthMhz);

    qDebug() << "Pan range request:" << source
             << "center" << centerMhz
             << "bandwidth" << bandwidthMhz
             << (sent ? "" : "(deferred: profile load)");
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

    // Keep the mini-pan centred on the active VFO (rebind to the new slice).
    refreshMiniPanFollow();

    // Send "slice set N active=1" only when switching to a different slice
    // (matches SmartSDR pcap — sent on VFO flag click, not on every tune).
    // Guard: don't send if triggered by the radio's own activeChanged echo
    // (m_updatingFromModel is set in the activeChanged handler). During profile
    // recall, RadioModel's profile-load hold suppresses this radio write so we
    // do not dirty the radio's restoring GUIClient session.
    if (sliceId != prevId && !m_updatingFromModel) {
        // setActive() emits activeChanged(true) synchronously before the wire
        // write (#3854 optimistic edge), re-entering the activeChanged handler
        // for a selection we are already performing. Mark the slice so that
        // handler can drop exactly this echo and nothing else.
        const int previousEdgeSliceId = m_optimisticActiveEdgeSliceId;
        m_optimisticActiveEdgeSliceId = sliceId;
        s->setActive(true);
        m_optimisticActiveEdgeSliceId = previousEdgeSliceId;
    }

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
        if (dwellSec > 0 && !profileLoadRadioStateWritesHeld())
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
                sl->ritOn(), sl->ritFreq(), sl->xitOn(), sl->xitFreq(),
                sl->diversity(), sl->isDiversityParent(),
                sl->isDiversityChild(), sl->diversityIndex());
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
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();
    syncFlexRxPanToAudioEngine();
    // Sync squelch line to newly active slice (handles slice switch without
    // waiting for squelchChanged signal).
    syncActiveSliceSquelchLineToSpectrums();
    syncActiveSliceAutoSquelchToSpectrums();
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

    // Update CWX/DVK indicator availability (follows the TX slice, #4173) and
    // the ASR indicator's (follows the ACTIVE slice, #4825) — this call is what
    // covers an active-slice SWITCH for ASR; the mode-change edge on a non-TX
    // active slice is handled in the modeChanged wiring.
    updateKeyerAvailability();

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

#ifdef AETHER_ASR_ENABLED
    // A retune (or a switch to another slice) is a new listening context — clear
    // the Copy Assist decode window so text from the old frequency doesn't linger,
    // and mark the new frequency in the transcript log.
    disconnect(m_copyAssistFreqConn);
    m_copyAssistFreqConn = connect(s, &SliceModel::frequencyChanged, this, [this](double mhz) {
        if (m_copyAssistController) {
            m_copyAssistController->onRetune(mhz);
        }
    });
    if (sliceId != prevId && m_copyAssistController) {
        m_copyAssistController->onRetune(s->frequency());
    }
#endif

    qDebug() << "MainWindow: active slice set to" << sliceId;
}

// ── Mini-pan glue ─────────────────────────────────────────────────────────────
// The mini-pan applet is pure presentation, and it is a VIEW — it creates no
// radio objects at all. It re-slices the FFT bins of the pan the active slice
// already lives on down to a +/-5 or +/-10 kHz window centred on that slice's
// PASSBAND (MiniPan::passbandCenterOffsetHz — on SSB the carrier sits at the
// edge of the filter, so centring on it wasted half the view).
//
// That is the whole architecture: no dedicated pan (no slot consumed, nothing
// to leak on quit, no reconnect zombie, no active-pan hijack) and no slice
// (the FLEX auto-creates one on every pan create, which is where the phantom
// slice came from). Resolution is the main pan's bin width, so it tracks
// whatever the operator has the main pan zoomed to. (#4562)

MiniPanApplet* MainWindow::miniPanApplet() const
{
    return m_appletPanel ? m_appletPanel->miniPanApplet() : nullptr;
}

void MainWindow::refreshMiniPanFollow()
{
    disconnect(m_miniPanFreqConn);
    disconnect(m_miniPanFiltConn);
    auto* applet = miniPanApplet();
    if (!applet || !m_miniPanFeedWanted) return;

    auto* s = activeSlice();
    if (!s) {
        applet->setVfoMhz(0.0);
        applet->setPassbandHz(0, 0);
        applet->scope()->updateSpectrum(QVector<float>{});
        return;
    }
    // Everything here is local: the readout and the passband shading are drawn
    // from model state, and the trace is re-sliced from bins the main pan is
    // already streaming. Nothing is sent to the radio, so tuning needs no
    // debounce -- the old "display pan set ... center=" push is gone with the
    // dedicated pan.
    //
    // A filter change moves the VIEW as well as the shading, because the window
    // is centred on the passband -- feedMiniPanFromPanFrame re-derives that from
    // the same slice on the next frame, so the two cannot disagree.
    m_miniPanFreqConn = connect(s, &SliceModel::frequencyChanged, this,
                                [this](double mhz) {
        if (auto* a = miniPanApplet()) a->setVfoMhz(mhz);
    });
    m_miniPanFiltConn = connect(s, &SliceModel::filterChanged, this,
                                [this]() {
        if (auto* cur = activeSlice(); cur)
            if (auto* a = miniPanApplet())
                a->setPassbandHz(cur->filterLow(), cur->filterHigh());
    });
    applet->setVfoMhz(s->frequency());
    applet->setPassbandHz(s->filterLow(), s->filterHigh());
}

void MainWindow::teardownMiniPanFeed()
{
    disconnect(m_miniPanFreqConn);
    disconnect(m_miniPanFiltConn);
    if (auto* a = miniPanApplet())
        a->scope()->updateSpectrum(QVector<float>{});
}

// Re-slice one main-pan FFT frame down to the mini-pan's window. The mapping
// itself lives in MiniPanReslice.h so it can be unit-tested without a radio.
void MainWindow::feedMiniPanFromPanFrame(const PanadapterModel* pan,
                                         const QVector<float>& bins)
{
    auto* applet = miniPanApplet();
    if (!applet || !pan || bins.size() < 2) return;
    auto* s = activeSlice();
    if (!s) return;

    const double panBw = pan->bandwidthMhz();
    if (panBw <= 0.0) return;

    const double span = applet->spanMhz();
    // Floor for samples outside the pan: the frame's own minimum, so the gap
    // sits at the bottom of the view instead of inventing a level.
    const float floorDbm = *std::min_element(bins.cbegin(), bins.cend());

    // Mirror the source pan's display settings so the mini-pan reads as a
    // magnifier on the main trace rather than a differently-styled second
    // opinion. Pulled per frame rather than wired signal-by-signal: the values
    // are plain member reads, the setters are change-gated, and pulling cannot
    // drift out of sync or miss a control we forgot to connect.
    //
    // FFT AVG and FFT FPS need nothing here — both are radio-side pan
    // properties ("display pan set … average=", requestPanDisplayRates), so
    // re-slicing this pan's frames already carries them.
    auto* scope = applet->scope();
    if (auto* sw = m_panStack ? m_panStack->spectrum(pan->panId()) : nullptr) {
        // The vertical window is the WIDGET's refLevel/dynamicRange, not the
        // pan's min_dbm/max_dbm. FFT Floor slides refLevel client-side
        // (applyNoiseFloorAutoAdjust) and only pushes a damped, thresholded
        // min_dbm/max_dbm to the radio afterwards — so mirroring the model
        // tracked the radio's lagging echo instead of the scale the main pan is
        // actually drawing with, and the floor slider appeared to do nothing
        // here. refLevel is the top of the display; the range hangs below it.
        scope->setDbmRange(sw->refLevel() - sw->dynamicRange(), sw->refLevel());
        scope->setTraceAppearance(sw->fftLineColor(), sw->fftFillColor(),
                                  sw->fftFillAlpha(), sw->fftLineWidth());
        scope->setHeatMap(sw->fftHeatMap());
        scope->setShowGrid(sw->showGrid());
    } else {
        // No applet for this pan (it is not in the main stack): fall back to the
        // radio-reported range, which is the best available answer.
        scope->setDbmRange(pan->minDbm(), pan->maxDbm());
    }

    // Centre the window on the PASSBAND centre, not the carrier: on SSB the
    // carrier sits at the edge of the filter, so a carrier-centred cut spent
    // half of an already narrow view on the rejected sideband. Re-derived from
    // the slice here rather than read back off the applet, so the trace is cut
    // from this frame's actual filter state; MiniPanScope places the hairline
    // and the passband wash with the same helper.
    const double centerMhz =
        s->frequency()
        + AetherSDR::MiniPan::passbandCenterOffsetHz(s->filterLow(),
                                                     s->filterHigh()) / 1.0e6;

    scope->updateSpectrum(
        AetherSDR::MiniPan::resliceWindow(
            bins,
            pan->centerMhz() - panBw / 2.0, panBw,
            centerMhz - span / 2.0, span,
            AetherSDR::MiniPan::kResliceOutputBins, floorDbm));
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
        s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq(),
        s->diversity(), s->isDiversityParent(),
        s->isDiversityChild(), s->diversityIndex());
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
    // Derive the split-pair visualization from model truth so the panadapter
    // reflects split regardless of who initiated it — GUI button, rigctld, CAT,
    // TCI, or front panel. A slice is "TX-in-split" when it is the TX slice and a
    // distinct RX slice shares its panadapter; that RX slice is "RX-in-split".
    // (#3726) This drops the rendering dependence on the GUI-only m_splitActive/
    // m_splitTxSliceId/m_splitRxSliceId flags — those still drive the SWAP/teardown
    // *actions*, which need the GUI-created TX slice id. Consistent with RFC #3715:
    // consumers derive from the model, not per-consumer state.

    // Resolve, per pan, the TX slice and its RX partner.
    QHash<QString, SliceModel*> txByPan;     // panId -> TX slice
    QHash<QString, SliceModel*> rxByPan;     // panId -> chosen RX partner
    for (auto* s : m_radioModel.slices())
        if (s && s->isTxSlice())
            txByPan.insert(s->panId(), s);

    for (auto* s : m_radioModel.slices()) {
        if (!s || s->isTxSlice()) continue;       // RX candidates only
        // A diversity CHILD is not a split partner (#3980). The child is an
        // RX-only beamforming slave that deliberately SHARES its parent's
        // panadapter, so it matches the "distinct RX slice on the TX slice's
        // pan" shape below and would otherwise be badged SPLIT while the real
        // TX slice is badged SWAP — and acting on that badge transmits on the
        // wrong slice.
        // Skip ONLY the child, not the parent: the child is strictly RX-only so
        // it can never be the TX slice, and a diversity PARENT can be a genuine
        // RX split partner to a *separate* TX slice on the same pan (split-TX +
        // diversity-RX on one pan, reachable on 2-SCU radios) — suppressing the
        // parent's badge there would be a regression.
        if (s->diversity() && s->isDiversityChild()) continue;
        if (!txByPan.contains(s->panId())) continue;  // need a distinct TX slice here
        auto*& chosen = rxByPan[s->panId()];      // default-inserts nullptr
        // Prefer the GUI-tracked RX (exact pairing for the SPLIT button), then the
        // active slice, otherwise the first distinct RX slice found on the pan.
        if (!chosen) chosen = s;
        else if (chosen->sliceId() == m_splitRxSliceId) continue;  // already best
        else if (s->sliceId() == m_splitRxSliceId) chosen = s;
        else if (s->sliceId() == m_activeSliceId)  chosen = s;
    }

    auto applyToSpectrum = [&](SpectrumWidget* sw) {
        if (!sw) return;
        int pairRxId = -1, pairTxId = -1;
        for (auto* s : m_radioModel.slices()) {
            auto* w = sw->vfoWidget(s->sliceId());
            if (!w) continue;
            auto* tx = txByPan.value(s->panId(), nullptr);
            auto* rx = rxByPan.value(s->panId(), nullptr);
            const bool paired   = (tx && rx);
            const bool isTxSlice = paired && (s == tx);
            const bool isRxSplit = paired && (s == rx);
            w->updateSplitBadge(isTxSlice, isRxSplit);
            if (paired) { pairTxId = tx->sliceId(); pairRxId = rx->sliceId(); }
        }
        sw->setSplitPair(pairRxId, pairTxId);
    };

    if (m_panStack) {
        for (auto* applet : m_panStack->allApplets())
            if (applet) applyToSpectrum(applet->spectrumWidget());
    } else if (auto* sw = spectrum()) {
        applyToSpectrum(sw);
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

        // A KiwiSDR-replaced slice shows unmuted (the Kiwi stream is the audio)
        // but the Flex slice must stay muted on the radio. Its visible
        // audioMute() is false, so without this guard a band-change reassert
        // would blast audio_mute=0 and fight the replacement (the status parser
        // re-mutes it, but that is a needless round-trip and a brief unmute
        // glitch). Retaining the Kiwi across a band change (#4158) requires
        // leaving these muted. flexAudioMute() carries the true radio mute.
        if (slice->externalReceiveReplacementActive()) {
            continue;
        }

        // The model already shows unmuted, so SliceModel::setAudioMute(false)
        // would no-op. Send the command directly to rebuild radio audio routing.
        m_radioModel.sendCommand(
            QString("slice set %1 audio_mute=0").arg(slice->sliceId()));
    }
}

void MainWindow::onMuteAllSlicesToggle()
{
    const auto slices = m_radioModel.slices();
    const auto kiwiOwnsSliceMute = [this](const SliceModel* s) {
        return s && s->sliceId() == m_kiwiSdrAudioSliceId;
    };

    // Determine intent: mute all if any owned slice is currently unmuted,
    // otherwise unmute all.  RadioModel::slices() returns only owned slices
    // (foreign clients' slices are deleted from m_slices on client_handle).
    bool anyUnmuted = false;
    for (const SliceModel* s : slices) {
#ifdef HAVE_RADE
        if (s && s->sliceId() == m_radeSliceId) continue;  // RADE owns its mute
#endif
        if (kiwiOwnsSliceMute(s)) continue;  // Kiwi Audio owns its replaced slice
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
        if (kiwiOwnsSliceMute(s)) continue;
        if (s) s->setAudioMute(anyUnmuted);
    }
}

void MainWindow::setActivePanApplet(PanadapterApplet* applet)
{
    if (applet == m_panApplet) return;
    m_panApplet = applet;

    // Re-target the decoders to the new active pan, then refresh so the moved
    // panel becomes visible on the new target and is cleared on every other pan.
    // route*() only hides the old target; refresh*() decides visibility from the
    // active slice — pairing them here means every caller stays consistent
    // without having to remember the follow-up refresh (#4409).
    routeCwDecoderOutput();
    routeRttyDecoderOutput();
    refreshCwDecodeState();
    refreshRttyDecodeState();
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
        // A panel can still be visible when startup status ordering moves the
        // decoder target. Hide it before dropping ownership so a later refresh
        // cannot leave an orphaned CW dock on the old pan (#4409).
        m_cwDecoderApplet->setCwPanelVisible(false);
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
        disconnect(m_cwDecoderApplet, &PanadapterApplet::cwRxTextDisplayed,
                   &m_cwCallsignSpotter, &CwCallsignSpotter::feedText);
    }

    // Text from the old applet's stream must never concatenate with the
    // new one's — a station boundary, as far as the spotter is concerned.
    m_cwCallsignSpotter.clear();

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
        connect(m_cwDecoderApplet, &PanadapterApplet::cwRxTextDisplayed,
                &m_cwCallsignSpotter, &CwCallsignSpotter::feedText);
    }
}

// Recompute the CW decoder run state, panel visibility, and the
// AudioEngine TX-side sidetone tap (#2417).  Single chokepoint so the
// independent RX/TX toggles, MOX edges, and slice-mode changes all
// converge on the same decision tree.
// RTTY decoder routing lives in MainWindow_DigitalModes.cpp (#3351 Phase 1e).
void MainWindow::setDecoderPanelVisibleOnly(
    PanadapterApplet* target, bool shouldShow,
    void (PanadapterApplet::*setter)(bool))
{
    // Exactly one panel — the current decoder target — may be visible; every
    // other applet is hidden so a target moved during startup status ordering
    // can't leave an orphaned decoder dock behind (#4409).
    if (m_panStack) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (applet)
                (applet->*setter)(applet == target && shouldShow);
        }
    } else if (target) {
        (target->*setter)(shouldShow);
    }
}

void MainWindow::refreshCwDecodeState()
{
    const bool rxOn = CwDecodeSettings::rxEnabled();
    const bool txOn = CwDecodeSettings::txEnabled();
    const bool anyOn = rxOn || txOn;

    auto* s = activeSlice();
    const bool isCw = s && isCwMode(s->mode());

    // Panel is visible only in CW receive mode — the operator's CW
    // text view is anchored to a CW slice's panadapter.  TX-side
    // decode is shown in the same panel, so if there's no CW slice in
    // view, there's no panel either.
    setDecoderPanelVisibleOnly(m_cwDecoderApplet, isCw && anyOn,
                               &PanadapterApplet::setCwPanelVisible);

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
    // Single-session guard.  All six call sites (the DSP applet, two
    // shortcut paths, two wiring paths and the menu) funnel through here,
    // and the wisdom cache does not exist until generation *completes* —
    // so without this, a second NR2 request mid-generation stacks a second
    // dialog and a second generateWisdom() worker, and the two race on the
    // wisdom temp/final files in SpectralNR.cpp.
    //
    // Deliberately NOT cleared when the operator cancels: cancellation is
    // cooperative and only takes effect when the in-flight FFTW_PATIENT
    // plan finishes, which on the size-262144 tail is minutes away.  The
    // guard has to outlive the cancel request and stay up until the worker
    // actually exits, or a restart during that window recreates exactly
    // the file race above.  The teardown paths below clear it via the
    // QPointer when the dialog is deleted.
    if (m_nr2WisdomDialog) {
        m_nr2WisdomDialog->show();
        m_nr2WisdomDialog->raise();
        return;
    }

    if (AudioEngine::needsWisdomGeneration()) {
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        const auto result = std::make_shared<std::atomic<int>>(
            static_cast<int>(SpectralNR::WisdomResult::Failed));

        const bool frameless =
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True";

        auto* dlg = new QDialog(this);
        dlg->setObjectName("nr2WisdomDialog");
        dlg->setWindowTitle("AetherSDR — FFTW Wisdom");
        if (frameless) {
            dlg->setWindowFlag(Qt::FramelessWindowHint, true);
        }
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
#ifdef Q_OS_MAC
        // Qt maps Qt::Tool to an NSPanel, which AppKit hides whenever the
        // application deactivates — fatal for a dialog that is modeless by
        // design and lives for minutes.  This attribute is what turns that
        // off: QWidgetPrivate::create() forwards it to the QWindow property
        // _q_macAlwaysShowToolWindow, which QCocoaWindow reads to decide
        // NSWindow.hidesOnDeactivate.  It is read during window creation, so
        // it MUST stay ahead of the first show()/winId() — moving it below
        // would silently no-op on macOS with nothing failing elsewhere.
        dlg->setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
#endif
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
        label->setObjectName("nr2WisdomStatusLabel");
        label->setWordWrap(true);
        body->addWidget(label);

        auto* progress = new QProgressBar(content);
        progress->setObjectName("nr2WisdomProgressBar");
        progress->setRange(0, 100);
        progress->setValue(0);
        body->addWidget(progress);

        auto* activityRow = new QHBoxLayout();
        activityRow->setContentsMargins(0, 0, 0, 0);
        auto* activityLabel = new QLabel("Working", content);
        activityLabel->setObjectName("nr2WisdomActivityLabel");
        activityLabel->setAccessibleName("FFTW wisdom generation activity");
        activityRow->addWidget(activityLabel);
        activityRow->addStretch();
        auto* elapsedLabel = new QLabel("Elapsed: 0:00", content);
        elapsedLabel->setObjectName("nr2WisdomElapsedTimeLabel");
        elapsedLabel->setAccessibleName("Elapsed time");
        activityRow->addWidget(elapsedLabel);
        body->addLayout(activityRow);

        auto* reassuranceLabel = new QLabel(
            "Still working — FFTW planning can take several minutes on some systems.", content);
        reassuranceLabel->setObjectName("nr2WisdomReassuranceLabel");
        reassuranceLabel->setAccessibleName("FFTW wisdom generation reassurance");
        reassuranceLabel->setWordWrap(true);
        // Reserve the row while hidden.  This line toggles on every quiet
        // interval, and without the reservation each toggle relayouts the
        // dialog — the tool window grows when it appears and is left with
        // dead space when it goes.  #4728 is a report about this window
        // being visually unstable at exactly this point in the run, so the
        // replacement liveness cue must not reintroduce geometry churn.
        QSizePolicy reassurancePolicy = reassuranceLabel->sizePolicy();
        reassurancePolicy.setRetainSizeWhenHidden(true);
        reassuranceLabel->setSizePolicy(reassurancePolicy);
        reassuranceLabel->hide();
        body->addWidget(reassuranceLabel);

        const auto announceLabel = [](QLabel* target) {
            target->setAccessibleName(target->text());
            QAccessibleEvent event(target, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        };

        auto* buttonRow = new QHBoxLayout();
        buttonRow->addStretch();
        auto* cancelButton = new QPushButton("Cancel", content);
        cancelButton->setObjectName("nr2WisdomCancelButton");
        cancelButton->setAutoDefault(false);
        cancelButton->setDefault(false);
        buttonRow->addWidget(cancelButton);
        body->addLayout(buttonRow);
        root->addWidget(content);

        m_nr2WisdomDialog = dlg;
        dlg->show();

        auto* activityTimer = new QTimer(dlg);
        activityTimer->setInterval(500);
        const auto elapsedTimer = std::make_shared<QElapsedTimer>();
        const auto lastCallbackTimer = std::make_shared<QElapsedTimer>();
        elapsedTimer->start();
        lastCallbackTimer->start();
        connect(activityTimer, &QTimer::timeout, dlg,
            [cancelled, activityLabel, elapsedLabel, reassuranceLabel,
                elapsedTimer, lastCallbackTimer, announceLabel]() {
                const qint64 elapsedSeconds = elapsedTimer->elapsed() / 1000;
                elapsedLabel->setText(QString("Elapsed: %1:%2")
                    .arg(elapsedSeconds / 60)
                    .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0')));
                const int dotCount = static_cast<int>((elapsedTimer->elapsed() / 500) % 4);
                activityLabel->setText(QString("%1%2")
                    .arg(cancelled->load() ? "Canceling" : "Working")
                    .arg(QString(dotCount, QLatin1Char('.'))));
                if (cancelled->load()) {
                    reassuranceLabel->setText(
                        "Cancel requested — waiting for the current FFT plan to finish.");
                    reassuranceLabel->show();
                } else {
                    const bool quiet = lastCallbackTimer->elapsed() >= 10000;
                    if (quiet && !reassuranceLabel->isVisible()) {
                        reassuranceLabel->show();
                        announceLabel(reassuranceLabel);
                    } else if (!quiet) {
                        reassuranceLabel->hide();
                    }
                }
            });
        activityTimer->start();

        const auto requestCancel = [cancelled, label, progress, cancelButton,
                                       activityLabel, reassuranceLabel, announceLabel]() {
            if (cancelled->exchange(true)) {
                return;
            }
            // activityLabel is left to the activity timer: it reads
            // `cancelled` and switches to the animated "Canceling..." on its
            // next tick.  Setting a static string here would be overwritten
            // within 500 ms, and the animated form is the better affordance
            // anyway — it keeps proving the app is alive while the in-flight
            // FFT plan finishes.
            reassuranceLabel->setText(
                "Cancel requested — waiting for the current FFT plan to finish.");
            reassuranceLabel->show();
            announceLabel(reassuranceLabel);
            cancelButton->setEnabled(false);
            progress->setRange(0, 0);
            label->setText("Canceling FFTW wisdom generation...\n\n"
                           "Audio will continue unchanged. This may take a moment while the current FFT plan finishes.");
        };
        connect(cancelButton, &QPushButton::clicked, dlg, requestCancel);
        connect(dlg, &QDialog::rejected, dlg, requestCancel);

        const QPointer<QDialog> dlgGuard(dlg);
        auto* thread = QThread::create([cancelled, result, dlgGuard, label, progress,
                                           reassuranceLabel, lastCallbackTimer]() {
            const auto wisdomResult = AudioEngine::generateWisdom(
                [cancelled, dlgGuard, label, progress, reassuranceLabel,
                    lastCallbackTimer](int step, int total, const std::string& desc) {
                    if (!dlgGuard) {
                        return;
                    }
                    if (cancelled->load()) {
                        return;
                    }
                    int pct = total > 0 ? (step * 100 / total) : 0;
                    QString d = QString::fromStdString(desc);
                    QMetaObject::invokeMethod(dlgGuard.data(), [dlgGuard, label, progress, reassuranceLabel,
                                                                  lastCallbackTimer, pct, d]() {
                        if (!dlgGuard) {
                            return;
                        }
                        lastCallbackTimer->restart();
                        reassuranceLabel->hide();
                        if (!d.isEmpty()) {
                            label->setText(d + "\n\n"
                                "This window will automatically close when wisdom generation is complete.");
                        } else {
                            progress->setValue(pct);
                        }
                    });
                },
                [cancelled]() { return cancelled->load(); });
            result->store(static_cast<int>(wisdomResult));
        });
        connect(thread, &QThread::finished, this, [this, dlg, progress, label, thread, result,
                                                     activityTimer, activityLabel, reassuranceLabel,
                                                     announceLabel]() {
            const auto wisdomResult =
                static_cast<SpectralNR::WisdomResult>(result->load());
            const bool ready = wisdomResult == SpectralNR::WisdomResult::Ready
                            || wisdomResult == SpectralNR::WisdomResult::Generated;
            activityTimer->stop();
            reassuranceLabel->hide();
            progress->setRange(0, 100);
            progress->setValue(ready ? 100 : 0);

            if (!ready) {
                activityLabel->setText("Stopped");
                label->setText(wisdomResult == SpectralNR::WisdomResult::Cancelled
                    ? "Wisdom generation canceled. Audio was left unchanged."
                    : "Wisdom generation failed. Audio was left unchanged.");
                announceLabel(label);
                if (auto* a = m_appletPanel ? m_appletPanel->clientRxDspApplet() : nullptr) {
                    if (auto* w = a->widget()) {
                        w->syncFromEngine();
                    }
                }
                if (m_dspDialog) {
                    m_dspDialog->syncFromEngine();
                }
                statusBar()->showMessage("NR2 was not enabled; audio is unchanged", 4000);
                QTimer::singleShot(800, this, [dlg, thread]() {
                    dlg->accept();
                    dlg->deleteLater();
                    thread->deleteLater();
                });
                return;
            }

            activityLabel->setText("Complete");
            label->setText("Wisdom generation complete!");
            announceLabel(label);
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
// Duplicated in AutomationServer::doScale (bridge can't include GUI
// headers) — if a step is added here, add it there too.
static constexpr int kScaleSteps[] = {75, 85, 100, 110, 125, 150, 175, 200};
static constexpr int kScaleStepCount = sizeof(kScaleSteps) / sizeof(kScaleSteps[0]);

void MainWindow::applyUiScale(int pct)
{
    int current = AppSettings::instance().value("UiScalePercent", "100").toInt();
    if (pct == current)
        return;

    AppSettings::instance().setValue("UiScalePercent", QString::number(pct));
    AppSettings::instance().save();

    auto answer = FramelessMessageBox::question(this, "UI Scale",
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
        const int panIdx = m_splitter->indexOf(centralPanWidget());
        if (panIdx < 0) return;
        m_splitter->insertWidget(panIdx, m_appletPanel);
    } else {
        m_splitter->addWidget(m_appletPanel);
    }

    // Re-apply stretch/collapse rules by widget identity (indices shifted).
    for (int i = 0; i < m_splitter->count(); ++i) {
        QWidget* w = m_splitter->widget(i);
        m_splitter->setStretchFactor(i, w == centralPanWidget() ? 1 : 0);
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
            if (w == centralPanWidget())  newSizes[i] = centerW;
            else if (w == m_appletPanel)  newSizes[i] = appletW;
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

void MainWindow::trackPersistentDialog(PersistentDialog* dialog)
{
    if (!dialog) {
        return;
    }
    m_persistentDialogs.removeIf([dialog](const QPointer<PersistentDialog>& tracked) {
        return tracked.isNull() || tracked.data() == dialog;
    });
    m_persistentDialogs.append(QPointer<PersistentDialog>(dialog));
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
    if (m_titleBar)
        m_titleBar->setChildDialogsFramelessMode(on);
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

void MainWindow::toggleMinimalModeFromAction()
{
    toggleMinimalMode(!m_minimalMode);
    if (m_minimalModeAction) {
        QSignalBlocker blocker(m_minimalModeAction);
        m_minimalModeAction->setChecked(m_minimalMode);
    }
}

void MainWindow::toggleMinimalMode(bool on)
{
    // Canvas mode owns the shell; minimal mode owns the window — they
    // cannot overlap.  Entering minimal from canvas used to reparent an
    // EMPTY applet panel (every applet lent to the canvas, every pan on
    // it, inside the splitter minimal is about to hide): a 260 px window
    // of nothing (8600 field report).  Exit canvas first — synchronously,
    // inside the same user action, so the operator sees one motion, not
    // two — and remember to bring it back on the way out.
    if (on && m_workspaceController && m_workspaceController->isEnabled()) {
        m_canvasWasOnBeforeMinimal = true;
        toggleWorkspaceCanvas(false, /*preserveEnabledPreference=*/true);
    }

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
        // Re-anchor as well as restore: this window is already mapped, so Qt
        // reapplies its caption-reserving clamp on every entry and the #4328
        // gap would come straight back on one Ctrl+M round trip — then stick,
        // because closeEvent() saves whatever origin is current.
        if (!geom.isEmpty() && restoreGeometry(geom))
            reanchorCustomFrameGeometry(geom);

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
        updateStatusBarMinimumWidth();

        // Restore full geometry
        QByteArray geom = QByteArray::fromBase64(
            s.value("FullModeGeometry", "").toByteArray());
        const bool restored = !geom.isEmpty() && restoreGeometry(geom);

        // Belt-and-suspenders: if FullModeGeometry encoded a state, ensure
        // we land windowed.
        showNormal();

        // After showNormal(), not before: leaving maximized drops us onto Qt's
        // clamped normal geometry, which is the rect that carries the #4328
        // phantom-caption offset.  Re-anchoring first would just be undone.
        if (restored)
            reanchorCustomFrameGeometry(geom);

        // The round trip ends where it started: minimal entered from
        // canvas mode returns to canvas mode.  Deferred one event-loop
        // turn — the splitter was just re-shown and geometry restored,
        // and mounting the canvas inside the same turn is the shell-swap
        // hazard the boot and disable paths already dodge (wl_subsurface).
        if (m_canvasWasOnBeforeMinimal) {
            QTimer::singleShot(0, this, [this] {
                // Keep the intent until the deferred activation actually
                // succeeds.  A scripted re-entry during this event-loop turn
                // must not consume it and strand the operator in Classic.
                if (m_minimalMode || !m_workspaceController)
                    return;
                if (m_workspaceController->isEnabled()) {
                    m_canvasWasOnBeforeMinimal = false;
                    return;
                }
                toggleWorkspaceCanvas(true);
            });
        }
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

void MainWindow::showPanadapterInterlockNotification(const QString& message,
                                                     const QString& key,
                                                     const QString& panId)
{
    if (!panId.trimmed().isEmpty() && m_panStack) {
        if (SpectrumWidget* sw = m_panStack->spectrum(panId.trimmed())) {
            sw->showInterlockNotification(message, key, 5000);
            return;
        }
    }

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
        sw->showInterlockNotification(message, key, 5000);
}

// ─── Pan layout application ───────────────────────────────────────────────────

// ─── Keyboard Shortcuts ───────────────────────────────────────────────────────

void MainWindow::updateKeyerAvailability()
{
    static const QString kActive   = "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }";
    static const QString kAvail    = "QLabel { color: #404858; font-weight: bold; font-size: 24px; }";
    static const QString kDisabled = "QLabel { color: #252530; font-weight: bold; font-size: 24px; }";

    // QWidget::setStyleSheet() does not compare before it acts — it unpolishes
    // and repolishes the widget every call. This function is no longer reached
    // only on mode and connection edges: applyCapabilitiesToUi() calls it, and
    // that slot is itself re-invoked from the gpsStatusChanged lambda, which a
    // GPSDO Flex drives periodically because the status carries UTC time. Three
    // labels restyled per second for no change is cheap but pointless, so the
    // rewrite is skipped when the sheet already matches.
    const auto setIndicatorStyle = [](QLabel* label, const QString& sheet) {
        if (label && label->styleSheet() != sheet) {
            label->setStyleSheet(sheet);
        }
    };

    // CWX and DVK both key the radio's TX slice, so their availability and
    // F1-F12 shortcuts follow that slice — not the selected RX slice.  FlexLib
    // scopes CWX to the TX slice (reference/FlexLib_API_v4.1.5.39794/FlexLib/
    // CWX.cs:186-199 getTXFrequency() returns the IsTransmitSlice's Freq).
    // Driving both keyers from the *same* slice's mode keeps the two F1-F12
    // sets mutually exclusive (CW vs voice can't both be true), so Qt never
    // sees two enabled ApplicationShortcuts per key and won't emit
    // activatedAmbiguously (#2464, #2582, #4173).
    SliceModel* txSlice = m_radioModel.txSlice();
    const QString txMode = txSlice ? txSlice->mode() : QString();
    // Both keyers carry a family gate ahead of the mode gate: a radio with no
    // text buffer and no voice recorder never gains one by switching mode, so
    // the capability is ANDed into the availability that drives the enabled
    // state, the panel auto-hide AND the F1-F12 arming below.
    //
    // The BUTTONS are hidden entirely by applyCapabilitiesToUi(); this exists
    // because the shortcuts are ApplicationShortcuts that stay armed whether or
    // not their button is on screen. Without it an HL2 in CW would keep F1-F12
    // firing `cwx send` into a backend that has no such verb — a keypress that
    // does nothing, which is exactly the report the DVK entitlement gate below
    // was added for.
    //
    // Through RadioModel's accessors, not a local backendCapabilities() read:
    // the same two questions are asked by the FlexControl macro action, the MQTT
    // CW-transmit topic, TCI's cw_msg / cw_macros and the bridge's `cwx` verb,
    // and they carry the permissive disconnected rule with them so no caller can
    // forget it. A default-constructed RadioCapabilities says false, so a raw
    // read here would hide the keyers with nothing attached.
    const bool hasCwKeyer = m_radioModel.hasRadioSideCwKeyer();
    const bool hasVoiceKeyer = m_radioModel.hasVoiceKeyer();

    const bool txIsCw  = hasCwKeyer
                         && isCwMode(txMode);
    // Voice-mode test through the shared predicate: the ASR block below asks
    // the same question of a DIFFERENT slice, and one list keeps the two from
    // drifting on which modes count (VoiceModeGate.h).
    const bool txIsSsb = isVoiceMode(txMode);

    // DVK carries a second, mode-independent gate: the radio's own DVK
    // entitlement. Unlike the mode gate it survives every mode change, so it is
    // evaluated once here and applied to the indicator, the panel and the
    // F1-F12 shortcuts alike — otherwise the keys stay armed and each keypress
    // is refused by the radio (the "silently does nothing" report).
    const DvkIndicatorBlocker dvkBlocker = dvkIndicatorBlocker(
        txIsSsb,
        m_radioModel.licenseFeatureSeen(kDvkLicenseFeature),
        m_radioModel.licenseFeatureEnabled(kDvkLicenseFeature));
    // hasVoiceKeyer is ANDed in HERE rather than into the mode test, because
    // isVoiceMode() is shared with the ASR indicator below and Copy Assist is
    // host-side — folding a radio-side voice-keyer capability into the shared
    // predicate would take a working transcription feature down with the keyer.
    const bool dvkAvailable = hasVoiceKeyer
                              && (dvkBlocker == DvkIndicatorBlocker::None);
    const bool dvkUnlicensed = (dvkBlocker == DvkIndicatorBlocker::NotLicensed);

    if (m_cwxPanel) m_cwxPanel->setShortcutsEnabled(txIsCw);
    if (m_dvkPanel) m_dvkPanel->setShortcutsEnabled(dvkAvailable);

    // Only auto-hide an open panel when a TX slice *exists* and is in the wrong
    // mode (a deliberate mode change).  A momentary "no TX slice" — during a TX
    // handoff between slices, or a band-recall that drops+recreates the TX slice
    // — must not yank an open panel the user can't get back (updateKeyer only
    // hides; showing is user-driven) (#4173).
    m_cwxIndicator->setEnabled(txIsCw);
    if (txSlice && !txIsCw && m_cwxPanel->isVisible()) {
        m_cwxPanel->hide();
        setIndicatorStyle(m_cwxIndicator, kDisabled);
    } else if (m_cwxPanel->isVisible()) {
        setIndicatorStyle(m_cwxIndicator, kActive);
    } else {
        setIndicatorStyle(m_cwxIndicator, txIsCw ? kAvail : kDisabled);
    }
    m_cwxIndicator->setCursor(txIsCw ? Qt::PointingHandCursor : Qt::ArrowCursor);

    // DVK: available in voice modes (SSB, AM, FM — not DIGU/DIGL), and only on
    // a radio that reports the DVK entitlement (see dvkIndicatorBlocker).
    m_dvkIndicator->setEnabled(dvkAvailable);
    // An unlicensed radio said so itself, so hide an open panel whether or not
    // a TX slice currently exists — the mode gate's "keep it open across a
    // transient no-TX-slice" caveat (#4173) applies only to the mode gate.
    if ((dvkUnlicensed || (txSlice && !txIsSsb)) && m_dvkPanel->isVisible()) {
        m_dvkPanel->hide();
        setIndicatorStyle(m_dvkIndicator, kDisabled);
    } else if (m_dvkPanel->isVisible()) {
        setIndicatorStyle(m_dvkIndicator, kActive);
    } else {
        setIndicatorStyle(m_dvkIndicator, dvkAvailable ? kAvail : kDisabled);
    }
    m_dvkIndicator->setCursor(dvkAvailable ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);
    // An unlicensed radio gets a tooltip naming the missing subscription; the
    // mode gate keeps the normal one, since the panel's own title and the F-key
    // rows already make "wrong mode" obvious once it opens.
    m_dvkIndicator->setToolTip(dvkIndicatorTooltip(dvkBlocker));

#ifdef AETHER_ASR_ENABLED
    // ASR (Copy Assist): the inverse of CWX on mode — available in voice modes
    // only, dimmed in CW and DIGx/RTTY — but NOT on slice. It follows the slice
    // the operator has SELECTED, which is also the slice the rest of Copy Assist
    // tracks: setActiveSliceInternal() rebinds m_copyAssistFreqConn to that
    // slice's frequencyChanged and calls onRetune() on a switch. The CW decoder,
    // this feature's CW-mode counterpart, gates on the same slice
    // (refreshCwDecodeState()).
    //
    // The selected slice is a PROXY, not the audio source, and the difference
    // matters to anyone changing this: the tap subscribes to
    // AudioEngine::receivePresentationPostDspAudioReady and AsrTapPolicy locks
    // onto a RECEIVER (the Flex, the applet Kiwi, an external Kiwi) on a
    // first-block-wins rule with a 2 s release window — never onto a slice. On a
    // Flex that stream is every audible slice already mixed together. So this
    // gate answers "is the operator listening to something transcribable",
    // which is a heuristic; it does not and cannot name the audio being decoded.
    //
    // It was gated on the TX slice for a visually consistent indicator row, and
    // that cost the feature entirely with TX off: no slice carries isTxSlice(),
    // so txMode is empty and a receive-only or antenna-disconnected operator
    // could not open Copy Assist at all (#4825). Row consistency is the weaker
    // constraint — CWX and DVK key the TX slice and genuinely belong to it, so
    // the three indicators may now disagree, which is correct.
    //
    // NOTE the auto-hide below now has teeth it did not have on the TX gate:
    // hiding the panel calls setAsrEnabled(false) (PanadapterApplet), which
    // disables the tap and drops the receiver lock. Selecting a CW slice
    // therefore STOPS a running transcription, where before only a TX-slice mode
    // change could. Deliberate — the indicator must track the selected slice to
    // be worth anything — but it is the sharp edge of this change.
    SliceModel* asrSlice = activeSlice();
    const bool asrIsVoice = asrSlice && isVoiceMode(asrSlice->mode());
    if (m_asrIndicator) {
        // The keyers' shape, and for the keyers' reason: only a slice that
        // EXISTS and is in the wrong mode closes an open panel. A slice that is
        // momentarily ABSENT is not a mode change, and on this radio it is
        // routinely not even a removal — with band_persistence a FLEX band
        // recall DROPS the slice and RE-CREATES it under the same id a moment
        // later (KiwiRebindTracker.h, #4158). On the ordinary single-slice
        // setup that empties slices(), so a null-slice auto-hide would stop
        // transcription on every band change and never restore it: this
        // function only ever hides, showing is user-driven.
        //
        // Disconnect also lands here with a null slice (RadioModel clears
        // m_slices and capabilitiesChanged drives applyCapabilitiesToUi ->
        // this function). Leaving the panel up there is the right outcome too —
        // the last transcript stays readable, and the enabled-while-visible
        // rule below means it can be closed by hand.
        //
        // A non-null slice is NOT enough on its own, because a band recall does
        // not have to empty slices() to reach here. With a second slice on the
        // pan, onSliceRemoved() re-selects it (slices.first(), TopologyFallback)
        // instead of taking the empty branch — so recalling a band on the
        // transcribed slice hands the gate a surviving CW/DIGx slice, and a
        // slice-exists guard alone sees a live slice in the wrong mode and
        // tears the panel down. Same #4158 rebuild, one slice further along.
        // m_bandRecallSelection is the window that already answers "this pan is
        // mid-rebuild, treat radio-driven selection as synchronization-only"
        // (BandRecallSelectionGuard.h, armed on the band write and refreshed by
        // onSliceRemoved()), which is exactly the question being asked here.
        //
        // The decision itself lives in VoiceModeGate.h so a test can pin it —
        // this function is not reachable from the gating-test target — and the
        // reason each clause is there is stated with it (#4932 review).
        const bool bandRecallInFlight =
            asrSlice
            && m_bandRecallSelection.isActive(asrSlice->panId(),
                                              QDateTime::currentMSecsSinceEpoch());
        if (shouldAutoHideCopyAssist(
                m_copyAssistApplet && m_copyAssistApplet->isCopyAssistVisible(),
                asrSlice != nullptr,
                asrSlice ? asrSlice->mode() : QString(),
                bandRecallInFlight)) {
            m_copyAssistApplet->setCopyAssistVisible(false);
        }

        // Read visibility AFTER the auto-hide, not before. The enabled state
        // and the cursor both key off it, and computing them from the pre-hide
        // value left the indicator enabled with a pointing hand over a panel
        // that had just been closed: the click passed the isEnabled() guard in
        // MainWindow_Shortcuts.cpp, showCopyAssist() re-opened the panel, and
        // this function closed it again in the same handler — an affordance
        // that promised something and did nothing, until some unrelated
        // refresh happened along (PR #4932 review).
        const bool asrVisible =
            m_copyAssistApplet && m_copyAssistApplet->isCopyAssistVisible();

        // Enabled when the mode allows OPENING it, or whenever the panel is
        // already open so it can always be CLOSED. The second half exists
        // because a disabled QLabel swallows its own clicks
        // (MainWindow_Shortcuts.cpp) and CopyAssistPanel has no close button of
        // its own — without it, any state that leaves the panel open while the
        // gate says no strands a panel the operator cannot dismiss. Fixing that
        // here, in the panel's own affordance, is what lets the auto-hide above
        // keep the conservative shape (PR #4932 review).
        m_asrIndicator->setEnabled(asrIsVoice || asrVisible);
        // An open panel reads kActive whatever the mode says — including the
        // band-recall and disconnect cases the auto-hide deliberately skips,
        // where ASR is genuinely still running.
        setIndicatorStyle(m_asrIndicator,
                          asrVisible ? kActive
                                     : (asrIsVoice ? kAvail : kDisabled));
        // Cursor tracks the ENABLED state, not the mode, so the hand appears on
        // exactly the clicks that do something — including the close-an-open-
        // panel case where the mode gate says no.
        m_asrIndicator->setCursor((asrIsVoice || asrVisible)
                                      ? Qt::PointingHandCursor
                                      : Qt::ArrowCursor);
    }
#endif
}

void MainWindow::centerActiveSliceInPanadapter(bool forceRadioCenter, double centerMhz)
{
    auto* s = activeSlice();
    if (!s || s->panId().isEmpty()) return;

    auto* sw = spectrumForSlice(s);
    if (!sw) return;

    auto* pan = m_radioModel.panadapter(s->panId());
    // A kiwi-display pan centers with the WIDGET's live span and no radio
    // write: its radio-side geometry froze at kiwi assignment (#3825/#4081),
    // so the model bandwidth is stale the moment the operator zooms — pairing
    // the new center with it would snap the zoom back — see PanRecenterPolicy.
    const bool kiwiDisplayActive = kiwiSdrPanDisplaysKiwi(s->panId());
    const double bandwidthMhz = PanRecenterPolicy::recenterBandwidthMhz(
        kiwiDisplayActive, sw->bandwidthMhz(),
        pan ? pan->bandwidthMhz() : m_radioModel.panBandwidthMhz());
    const double targetMhz = (centerMhz > 0.0) ? centerMhz : s->frequency();
    // Keep the view's low edge >= 0 Hz. The non-kiwi path clamps via
    // requestPanCenter -> applyTuneCenteringWrite, but a kiwi-display pan renders
    // widget-local with no radio echo to correct a negative edge, so a wide span
    // on an LF/VLF/MW slice (0.137 - 0.25 = -0.113 MHz) would draw off-screen.
    // Matches std::max(center, bw/2) in the sibling recenter paths.
    const double viewCenterMhz = std::max(targetMhz, bandwidthMhz / 2.0);

    if (m_panStack) {
        if (auto* applet = m_panStack->panadapter(s->panId()))
            m_panStack->setActivePan(applet->panId());
    }

    // #4142 — ask the radio FIRST, so the local view can only advance if the
    // command actually reached the wire. During a profile load this center write
    // is suppressed; centering the spectrum on a center the radio never took is
    // exactly what projects honest tiles into a lying frame and bakes black rows
    // into waterfall history. requestPanCenter() defers and replays it instead.
    bool centerDeferred = false;
    if (!kiwiDisplayActive && forceRadioCenter && m_radioModel.isConnected()) {
        centerDeferred = !m_radioModel.requestPanCenter(s->panId(), targetMhz);
    }

    // Keep the local spectrum centered immediately so the active slice marker is
    // visible before the radio's status echo arrives — unless the center was
    // deferred, in which case the pan must keep showing truthful spectrum for the
    // span the radio still has.
    if (!centerDeferred) {
        sw->setFrequencyRange(viewCenterMhz, bandwidthMhz);
    }
    pushSliceFrequencyToOverlays(s, targetMhz);

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
    auto* popup = new DspParamPopup(this);
    Nr2SettingsModel& model = Nr2SettingsModel::instance();
    const Nr2SettingsModel::Config initial = model.config();
    const bool thresholdAvailable = initial.gainMethod == 2
        || (!initial.legacyGeometryAndGainMapping
            && initial.gainMethod == 0);

    const DspParamPopup::SliderControl reduction = popup->addSlider(
        "Reduction", 50, 200,
        static_cast<int>(std::lround(initial.gainMax * 100.0f)),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            const float val = v / 100.0f;
            Nr2SettingsModel::instance().setGainMax(val);
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2GainMax(val); });
        });

    const DspParamPopup::SliderControl naturalness = popup->addSlider(
        "Naturalness", 0, 15,
        static_cast<int>(std::lround(initial.gainFloor * 100.0f)),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            const float val = v / 100.0f;
            Nr2SettingsModel::instance().setGainFloor(val);
            QMetaObject::invokeMethod(m_audio, [this, val]() {
                m_audio->setNr2GainFloor(val);
            });
        });

    const DspParamPopup::SliderControl smoothing = popup->addSlider(
        "Smoothing", 50, 98,
        static_cast<int>(std::lround(initial.gainSmooth * 100.0f)),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            const float val = v / 100.0f;
            Nr2SettingsModel::instance().setGainSmooth(val);
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2GainSmooth(val); });
        });

    DspParamPopup::SliderControl threshold = popup->addSlider(
        "Voice Threshold", 5, 50,
        static_cast<int>(std::lround(initial.qspp * 100.0f)),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            const float val = v / 100.0f;
            Nr2SettingsModel::instance().setQspp(val);
            QMetaObject::invokeMethod(m_audio, [this, val]() { m_audio->setNr2Qspp(val); });
        },
        thresholdAvailable,
        thresholdAvailable
            ? QStringLiteral("Speech-presence threshold used by this gain method.")
            : QStringLiteral(
                "Voice Threshold does not affect the selected gain method."));

    QCheckBox* aeFilter = popup->addCheckbox("AE Filter", initial.aeFilter,
        [this](bool on) {
            Nr2SettingsModel::instance().setAeFilter(on);
            QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setNr2AeFilter(on); });
        });

    QCheckBox* legacy = popup->addCheckbox(
        "Original NR2 (geometry + gain mapping)",
        initial.legacyGeometryAndGainMapping,
        [this](bool useOriginal) {
            Nr2SettingsModel::instance()
                .setLegacyGeometryAndGainMapping(useOriginal);
            QMetaObject::invokeMethod(m_audio, [this, useOriginal]() {
                m_audio->setNr2UseOriginalGeometry(useOriginal);
            });
        });

    connect(&model, &Nr2SettingsModel::configChanged, popup,
            [reduction, naturalness, smoothing, threshold, aeFilter, legacy]() {
        const Nr2SettingsModel::Config config =
            Nr2SettingsModel::instance().config();
        reduction.slider->setValue(static_cast<int>(
            std::lround(config.gainMax * 100.0f)));
        naturalness.slider->setValue(static_cast<int>(
            std::lround(config.gainFloor * 100.0f)));
        smoothing.slider->setValue(static_cast<int>(
            std::lround(config.gainSmooth * 100.0f)));
        threshold.slider->setValue(static_cast<int>(
            std::lround(config.qspp * 100.0f)));
        aeFilter->setChecked(config.aeFilter);
        legacy->setChecked(config.legacyGeometryAndGainMapping);

        const bool available = config.gainMethod == 2
            || (!config.legacyGeometryAndGainMapping
                && config.gainMethod == 0);
        threshold.setEnabled(available);
        threshold.setToolTip(available
            ? QStringLiteral(
                "Speech-presence threshold used by this gain method.")
            : QStringLiteral(
                "Voice Threshold does not affect the selected gain method."));
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
            // Route through removePanadapter so a layout-shrink tears down the
            // waterfall too ("display pan remove" + "display panafall remove"),
            // not just the panadapter stream. (#3843)
            m_radioModel.removePanadapter(panId);
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

    // A backend that owns its own receivers creates them at the SEAM.
    //
    // The Flex wire text below goes nowhere on such a radio, and — the part that
    // actually breaks this — its completion callback never runs either. The
    // recursion is driven FROM that callback, so it stopped dead after the first
    // iteration: no pans, no error, no log line past "creating N more".
    //
    // That is what made "Add Panadapter → pick a layout" silently do nothing on
    // a Hermes-Lite 2 while the bridge's `pan create` worked, because that goes
    // through RadioModel::createPanadapter() and this path talks to the wire
    // directly.
    //
    // The seam create is synchronous, so there is no reply to wait for. Which
    // pan is new is found by DIFFING rather than parsing a create reply: the
    // backend numbers its own pans and skips retired numbers after a close.
    if (!m_radioModel.usesFlexCommandPlane()) {
        auto before = std::make_shared<QSet<QString>>();
        for (auto* p : m_panStack->allApplets())
            before->insert(p->panId());
        for (auto* p : m_radioModel.panadapters())
            if (p) before->insert(p->panId());

        m_radioModel.createPanadapter();

        // The seam create is NOT synchronous for every backend (red-team
        // M4): HL2's is, but the demo mints its pan over the synthetic wire
        // — two queued thread hops — so an immediate diff found nothing,
        // declared "capacity full", and aborted the recursion while the pan
        // materialised a moment later.  Diff after the create has settled;
        // 300 ms covers both wire hops with margin and is indistinguishable
        // from the existing 200 ms inter-create pacing for a synchronous
        // backend.
        QTimer::singleShot(300, this,
                           [this, layoutId, total, panIds, created, before]() {
            if (m_shuttingDown || !m_panStack) {
                return;
            }
            QString createdId;
            for (auto* p : m_radioModel.panadapters()) {
                if (p && !before->contains(p->panId())) {
                    createdId = p->panId();
                    break;
                }
            }
            if (createdId.isEmpty()) {
                // The backend refused — receiver count, or the link budget
                // at this span. It has already logged which; surface it and
                // stop rather than recursing against a limit that will
                // refuse every remaining pan.
                qWarning() << "applyPanLayout: backend declined pan"
                           << (created + 1) << "of" << total;
                showPanadapterSliceCapacityMessage();
                return;
            }
            panIds->append(createdId);
            qDebug() << "applyPanLayout: created pan" << (created + 1)
                     << "of" << total << "id:" << createdId;
            // Same inter-create pacing rationale as the Flex path: adding a
            // receiver restarts the EP6 stream, so creates stay spaced.
            QTimer::singleShot(200, this,
                               [this, layoutId, total, panIds, created]() {
                createPansSequentially(layoutId, total, panIds, created + 1);
            });
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
        snap.agcMode       = s->receiveAgcMode();
        snap.agcThreshold  = s->receiveAgcThreshold();
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

// SWR sweep methods live in MainWindow_SwrSweep.cpp (#3351 Phase 1e).
// RADE / FreeDV / DAX methods live in MainWindow_DigitalModes.cpp (#3351 Phase 1e).

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

} // namespace AetherSDR
