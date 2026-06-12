// MainWindow_Controllers.cpp — external-controller methods of MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1a). This translation
// unit holds the method bodies for every physical-controller subsystem:
//
//   • FlexControl (serial knob): dialog, indicator sync, tune/button/wheel
//     handlers
//   • USB HID encoders (Icom RC-28, TMate 2, Ulanzi Dial, PowerMate,
//     Contour Shuttle): defaults, LED/display/overlay state, action dispatch
//   • StreamDeck+ label refresh
//   • MIDI parameter registry (registerMidiParams, HAVE_MIDI)
//   • The control-devices support-bundle snapshot
//
// Pure code motion from MainWindow.cpp — same class, no header changes; a
// C++ class may define its members across any number of TUs. Constructor
// wiring for these subsystems still lives in MainWindow.cpp and moves in a
// later phase.

#include "MainWindow.h"

#include "FlexControlDialog.h"
#include "MainWindowHelpers.h"
#include "core/AppSettings.h"
#include "core/CwTrace.h"
#include "core/LogManager.h"
#include "core/MidiSettings.h"
#include "core/UlanziDialBackend.h"
#include "AppletPanel.h"
#include "core/IambicKeyer.h"
#include "PanadapterStack.h"
#include "RC28MappingDialog.h"
#include "RadioSetupDialog.h"
#include "UlanziDialMapperDialog.h"
#include "RxApplet.h"
#include "SpectrumWidget.h"
#include "TitleBar.h"
#include "models/SliceModel.h"

#include <QColor>
#include <QPainter>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kTMate2DefaultOverlayDurationMs = 1500;

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

static bool isCwMomentaryActionId(const QString& id)
{
    return id == QLatin1String(kCwStraightKeyActionId)
        || id == QLatin1String(kCwLeftPaddleActionId)
        || id == QLatin1String(kCwRightPaddleActionId);
}


// FlexControl wheel/button action decoding — moved with their only
// callers from MainWindow.cpp's anonymous namespace (#3351 Phase 1a).
bool flexWheelModeForAction(const QString& actionName, FlexWheelMode& mode)
{
    if (actionName == QLatin1String("WheelFrequency")) {
        mode = FlexWheelMode::Frequency;
    } else if (actionName == QLatin1String("WheelVolume")) {
        mode = FlexWheelMode::Volume;
    } else if (actionName == QLatin1String("WheelPower")) {
        mode = FlexWheelMode::Power;
    } else if (actionName == QLatin1String("WheelRit")) {
        mode = FlexWheelMode::Rit;
    } else if (actionName == QLatin1String("WheelXit")) {
        mode = FlexWheelMode::Xit;
    } else if (actionName == QLatin1String("WheelMasterAf")) {
        // Back-compat for saved FlexControl bindings made before #2986
        // consolidation.  Routes to the same Volume mode (master volume).
        mode = FlexWheelMode::Volume;
    } else if (actionName == QLatin1String("WheelSliceAudio")) {
        mode = FlexWheelMode::SliceAudio;
    } else if (actionName == QLatin1String("WheelHeadphoneVolume")) {
        mode = FlexWheelMode::HeadphoneVolume;
    } else if (actionName == QLatin1String("WheelAgcT")) {
        mode = FlexWheelMode::AgcT;
    } else if (actionName == QLatin1String("WheelApf")) {
        mode = FlexWheelMode::Apf;
    } else if (actionName == QLatin1String("WheelCwSpeed")) {
        mode = FlexWheelMode::CwSpeed;
    } else {
        return false;
    }
    return true;
}

QString flexControlButtonAction(int button, int action)
{
    static const char* defaults[4][3] = {
        {"StepUp",     "StepDown",     "None"},
        {"ToggleMox",  "ToggleTune",   "None"},
        {"ToggleMute", "ToggleLock",   "None"},
        {"StepUp",     "StepDown",     "None"},
    };
    const char* fallback = (button >= 1 && button <= 4 && action >= 0 && action <= 2)
                               ? defaults[button - 1][action] : "None";
    return AppSettings::instance()
        .value(QString("FlexControlBtn%1Action%2").arg(button).arg(action), fallback)
        .toString();
}

} // namespace

void MainWindow::showFlexControlDialog()
{
    const bool wasFresh = !m_flexControlDialog;
    showOrRaisePersistent(m_flexControlDialog);
    if (wasFresh && m_flexControlDialog) {
        connect(m_flexControlDialog, &FlexControlDialog::virtualWheelSteps,
                this, &MainWindow::handleVirtualFlexControlWheel);
        connect(m_flexControlDialog, &FlexControlDialog::virtualButtonPressed,
                this, &MainWindow::handleFlexControlButton);
        connect(m_flexControlDialog, &FlexControlDialog::virtualButtonPressed,
                this, [this](int button, int action) {
            if (m_flexControlDialog)
                m_flexControlDialog->reflectButtonPress(button, action);
        });
        connect(m_flexControlDialog, &FlexControlDialog::flexControlSettingsChanged,
                this, [this] {
            if (m_radioSetupDialog)
                m_radioSetupDialog->refreshFlexControlButtonActions();
            syncFlexControlIndicatorForSettings();
#ifdef HAVE_SERIALPORT
            const bool invert = AppSettings::instance()
                .value("FlexControlInvertDir", "False").toString() == "True";
            QMetaObject::invokeMethod(m_flexControl, [this, invert] {
                m_flexControl->setInvertDirection(invert);
            });
#endif
        });
        connect(m_flexControlDialog, &FlexControlDialog::physicalDetectRequested,
                this, [this] {
#ifdef HAVE_SERIALPORT
            const QString port = FlexControlManager::detectPort();
            if (port.isEmpty()) {
                m_flexControlConnected = false;
                if (m_flexControlDialog)
                    m_flexControlDialog->setPhysicalReady(false);
                if (m_radioSetupDialog)
                    m_radioSetupDialog->setFlexControlConnectionStatus(false);
                return;
            }

            auto& settings = AppSettings::instance();
            settings.setValue("FlexControlPort", port);
            settings.setValue("FlexControlOpen", "True");
            settings.save();

            const bool invert = settings.value("FlexControlInvertDir", "False").toString() == "True";
            QMetaObject::invokeMethod(m_flexControl, [this, port, invert] {
                m_flexControl->setInvertDirection(invert);
                m_flexControl->open(port);
            });
#else
            if (m_flexControlDialog)
                m_flexControlDialog->setPhysicalReady(false);
#endif
        });
        connect(m_flexControlDialog, &FlexControlDialog::physicalDisconnectRequested,
                this, [this] {
#ifdef HAVE_SERIALPORT
            auto& settings = AppSettings::instance();
            settings.setValue("FlexControlOpen", "False");
            settings.save();
            m_flexControlConnected = false;
            if (m_flexControlDialog)
                m_flexControlDialog->setPhysicalReady(false);
            if (m_radioSetupDialog)
                m_radioSetupDialog->setFlexControlConnectionStatus(false);
            QMetaObject::invokeMethod(m_flexControl, [this] {
                if (m_flexControl->isOpen())
                    m_flexControl->close();
            });
#endif
        });
    }
    syncFlexControlDialog();
}

void MainWindow::syncFlexControlDialog()
{
    if (!m_flexControlDialog)
        return;

    auto* s = activeSlice();
    m_flexControlDialog->setSlice(s);
#ifdef HAVE_SERIALPORT
    m_flexControlDialog->setPhysicalReady(
        m_flexControlConnected,
        m_flexControlConnected && m_flexControl ? m_flexControl->portName() : QString());
#else
    m_flexControlDialog->setPhysicalReady(false);
#endif
    int stepHz = 100;
    if (auto* sw = spectrumForSlice(s))
        stepHz = sw->stepSize();
    else if (s && s->stepHz() > 0)
        stepHz = s->stepHz();
    m_flexControlDialog->setStepSize(stepHz);
    m_flexControlDialog->setActiveAuxButton(m_flexActiveLedButton);
}

void MainWindow::syncFlexControlIndicatorForSettings()
{
    if (m_flexActiveLedButton < 1 || m_flexActiveLedButton > 3) {
        syncFlexControlDialog();
        return;
    }

    FlexWheelMode mode = FlexWheelMode::Frequency;
    if (flexWheelModeForAction(flexControlButtonAction(m_flexActiveLedButton, 0), mode)) {
        m_flexWheelMode = mode;
        setFlexControlHardwareIndicator(m_flexActiveLedButton);
    } else {
        m_flexWheelMode = FlexWheelMode::Frequency;
        setFlexControlHardwareIndicator(0);
    }
    syncFlexControlDialog();
}

void MainWindow::setFlexControlHardwareIndicator(int button)
{
    if (button < 1 || button > 3) {
        button = 0;
    }
    m_flexActiveLedButton = button;
#ifdef HAVE_SERIALPORT
    if (m_flexControl) {
        QMetaObject::invokeMethod(m_flexControl, [this, button] {
            m_flexControl->setActiveLedButton(button);
        });
    }
#else
    Q_UNUSED(button);
#endif
}

void MainWindow::handleFlexControlTuneSteps(int steps)
{
    switch (m_flexWheelMode) {
    case FlexWheelMode::Frequency:
        applyFlexControlWheelAction(QStringLiteral("WheelFrequency"), steps);
        break;
    case FlexWheelMode::Volume:
        applyFlexControlWheelAction(QStringLiteral("WheelVolume"), steps);
        break;
    case FlexWheelMode::Power:
        applyFlexControlWheelAction(QStringLiteral("WheelPower"), steps);
        break;
    case FlexWheelMode::Rit:
        applyFlexControlWheelAction(QStringLiteral("WheelRit"), steps);
        break;
    case FlexWheelMode::Xit:
        applyFlexControlWheelAction(QStringLiteral("WheelXit"), steps);
        break;
    case FlexWheelMode::SliceAudio:
        applyFlexControlWheelAction(QStringLiteral("WheelSliceAudio"), steps);
        break;
    case FlexWheelMode::HeadphoneVolume:
        applyFlexControlWheelAction(QStringLiteral("WheelHeadphoneVolume"), steps);
        break;
    case FlexWheelMode::AgcT:
        applyFlexControlWheelAction(QStringLiteral("WheelAgcT"), steps);
        break;
    case FlexWheelMode::Apf:
        applyFlexControlWheelAction(QStringLiteral("WheelApf"), steps);
        break;
    case FlexWheelMode::CwSpeed:
        applyFlexControlWheelAction(QStringLiteral("WheelCwSpeed"), steps);
        break;
    }
}

void MainWindow::handleFlexControlButton(int button, int action)
{
    // Knob press while a wheel function is active returns to frequency mode (#1354).
    if (button == 4 && action == 0 && m_flexWheelMode != FlexWheelMode::Frequency) {
        m_flexWheelMode = FlexWheelMode::Frequency;
        setFlexControlHardwareIndicator(0);
        syncFlexControlDialog();
        return;
    }

    const QString actionName = flexControlButtonAction(button, action);
    FlexWheelMode requestedWheelMode = FlexWheelMode::Frequency;
    const bool actionControlsWheel = flexWheelModeForAction(actionName, requestedWheelMode);
    if (button >= 1 && button <= 3 && action == 0 && !actionControlsWheel) {
        m_flexWheelMode = FlexWheelMode::Frequency;
        setFlexControlHardwareIndicator(0);
    }

    if (actionName == "StepUp") {
        if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepUp();
    } else if (actionName == "StepDown") {
        if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepDown();
    } else if (actionName == "ToggleMox") {
        m_radioModel.setTransmit(!m_radioModel.transmitModel().isTransmitting());
    } else if (actionName == "ToggleTune") {
        if (m_radioModel.transmitModel().isTuning())
            m_radioModel.transmitModel().stopTune();
        else
            m_radioModel.transmitModel().startTune();
    } else if (actionName == "ToggleMute") {
        if (m_audio) m_audio->setMuted(!m_audio->isMuted());
    } else if (actionName == "ToggleLock") {
        if (auto* s = activeSlice()) s->setLocked(!s->isLocked());
    } else if (actionName == "ClearRit") {
        if (auto* s = activeSlice()) s->setRit(s->ritOn(), 0);
    } else if (actionName == "ClearXit") {
        if (auto* s = activeSlice()) s->setXit(s->xitOn(), 0);
    } else if (actionName == "ToggleApf") {
        if (auto* s = activeSlice()) s->setApf(!s->apfOn());
    } else if (actionName == "BandZoom") {
        auto* s = activeSlice();
        if (!s) return;
        const QString panId = !s->panId().isEmpty()
            ? s->panId()
            : (m_panStack ? m_panStack->activePanId() : m_radioModel.panId());
        if (panId.isEmpty()) return;
        m_flexVirtualBandZoomOn = !m_flexVirtualBandZoomOn;
        m_radioModel.sendCommand(QString("display pan set %1 band_zoom=%2")
            .arg(panId).arg(m_flexVirtualBandZoomOn ? 1 : 0));
    } else if (actionName == "SegmentZoom") {
        auto* s = activeSlice();
        if (!s) return;
        const QString panId = !s->panId().isEmpty()
            ? s->panId()
            : (m_panStack ? m_panStack->activePanId() : m_radioModel.panId());
        if (panId.isEmpty()) return;
        m_flexVirtualSegmentZoomOn = !m_flexVirtualSegmentZoomOn;
        m_radioModel.sendCommand(QString("display pan set %1 segment_zoom=%2")
            .arg(panId).arg(m_flexVirtualSegmentZoomOn ? 1 : 0));
    } else if (actionName == "NextSlice") {
        const auto& slices = m_radioModel.slices();
        if (slices.size() > 1) {
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i) {
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            }
            setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
        }
    } else if (actionName == "PrevSlice") {
        const auto& slices = m_radioModel.slices();
        if (slices.size() > 1) {
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i) {
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            }
            setActiveSlice(slices[(idx - 1 + slices.size()) % slices.size()]->sliceId());
        }
    } else if (actionName == "ToggleAgc") {
        if (auto* s = activeSlice()) {
            static const char* modes[] = {"off", "slow", "med", "fast"};
            const QString cur = s->agcMode().toLower();
            int idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (cur == modes[i]) { idx = i; break; }
            }
            s->setAgcMode(modes[(idx + 1) % 4]);
        }
    } else if (actionName == "VolumeUp") {
        // Route to master volume to match SmartSDR behavior (#2921).
        const int current = AppSettings::instance().value("MasterVolume", "100").toInt();
        const int next = std::clamp(current + 5, 0, 100);
        if (m_titleBar)
            m_titleBar->setMasterVolume(next);
        applyMasterVolume(next);
    } else if (actionName == "VolumeDown") {
        const int current = AppSettings::instance().value("MasterVolume", "100").toInt();
        const int next = std::clamp(current - 5, 0, 100);
        if (m_titleBar)
            m_titleBar->setMasterVolume(next);
        applyMasterVolume(next);
    } else if (actionControlsWheel) {
        m_flexWheelMode = requestedWheelMode;
        setFlexControlHardwareIndicator(button);
    } else if (actionName == "SplitActiveSlice") {
        if (!m_splitActive) {
            if (m_radioModel.slices().size() >= m_radioModel.maxSlices()) return;
            auto* s = activeSlice();
            if (!s) return;
            QString panId = s->panId();
            if (panId.isEmpty())
                panId = m_panStack ? m_panStack->activePanId() : m_radioModel.panId();
            const bool isCw = s->mode() == "CW" || s->mode() == "CWL";
            const double txFreq = s->frequency() + (isCw ? 0.001 : 0.005);
            m_splitActive = true;
            m_splitRxSliceId = s->sliceId();
            m_radioModel.sendCommand(
                QString("slice create pan=%1 freq=%2").arg(panId).arg(txFreq, 0, 'f', 6));
        } else {
            disableSplit();
        }
    } else if (actionName.startsWith("CwxF")) {
        bool ok = false;
        const int idx = actionName.mid(4).toInt(&ok);
        if (ok && idx >= 1 && idx <= 12)
            m_radioModel.cwxModel().sendMacro(idx);
    }

    syncFlexControlDialog();
}

void MainWindow::handleVirtualFlexControlWheel(const QString& actionId, int steps)
{
    applyFlexControlWheelAction(actionId, steps);
}

#ifdef HAVE_HIDAPI
// static
QString MainWindow::hidEncoderDefaultAction(int encoderIndex)
{
    switch (encoderIndex) {
    case 0:  return QStringLiteral("WheelFrequency");
    case 1:  return QStringLiteral("WheelRit");
    case 2:  return QStringLiteral("WheelXit");
    case 3:  return QStringLiteral("WheelVolume");
    default: return QStringLiteral("WheelFrequency");
    }
}

// static
QString MainWindow::hidEncoderDefaultPushAction(int encoderIndex)
{
    switch (encoderIndex) {
    case 0:  return QStringLiteral("StepCycle");  // tuning encoder push → cycle step size
    case 1:  return QStringLiteral("ToggleRit");
    case 2:  return QStringLiteral("ToggleXit");
    case 3:  return QStringLiteral("None");
    default: return QStringLiteral("None");
    }
}

// True when the hold action assigned to an RC-28 F-key is currently engaged, so
// that key's LED should be lit. Lets any toggleable hold action (mute, RIT, XIT,
// slice lock, fast/fine tune) drive its own button's LED — not just the tune
// modes. Returns false for non-stateful actions, which simply leave the LED off.
bool MainWindow::rc28HoldActionActive(const QString& action) const
{
    if (action == "TuneFast")   return m_hidFastTune;
    if (action == "FineTune")   return m_hidFineTune;
    if (action == "ToggleMute") return m_audio && m_audio->isMuted();
    if (auto* s = activeSlice()) {
        if (action == "ToggleRit")  return s->ritOn();
        if (action == "ToggleXit")  return s->xitOn();
        if (action == "ToggleLock") return s->isLocked();
    }
    return false;
}

// Compute the current RC-28 LED byte from radio + RC-28 action state and send
// it to the device on its ExtControllers thread. (#3323)
// Active-low: bit0=TX, bit1=F1, bit2=F2, bit3=LINK. 0=LED on, 1=LED off.
void MainWindow::updateRC28Leds()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isRC28Compatible()) return;
    uint8_t b = HidEncoderManager::RC28_LEDS_OFF;  // 0x0F — start all off
    b &= ~0x08u;  // LINK always on while connected
    if (m_radioModel.transmitModel().isTransmitting()) b &= ~0x01u;  // TX
    // Each F-key's LED reflects the on/off state of the hold action assigned to
    // that key: F1 → bit1, F2 → bit2. (FlexRC-28 maps functions to LEDs the same
    // way and drives bit2 freely, confirming there is no per-button lock.)
    const QString f1Hold = HidEncoderManager::rc28MappingField("f1Hold", "TuneFast");
    const QString f2Hold = HidEncoderManager::rc28MappingField("f2Hold", "ModeCycle");
    if (rc28HoldActionActive(f1Hold)) b &= ~0x02u;  // F1 LED
    if (rc28HoldActionActive(f2Hold)) b &= ~0x04u;  // F2 LED
    const uint8_t ledByte = b;
    QMetaObject::invokeMethod(m_hidEncoder, [this, ledByte] {
        m_hidEncoder->setRC28Leds(ledByte);
    });
}

bool MainWindow::tmate2OverlayActive() const
{
    return m_tmate2Overlay != TMate2Overlay::None
        && QDateTime::currentMSecsSinceEpoch() <= m_tmate2OverlayUntilMs;
}

QString MainWindow::tmate2OverlayName() const
{
    switch (m_tmate2Overlay) {
    case TMate2Overlay::Volume: return QStringLiteral("volume");
    case TMate2Overlay::Power:  return QStringLiteral("power");
    case TMate2Overlay::Speed:  return QStringLiteral("speed");
    case TMate2Overlay::Wpm:    return QStringLiteral("wpm");
    case TMate2Overlay::Rit:    return QStringLiteral("rit");
    case TMate2Overlay::None:   break;
    }
    return QString();
}

int MainWindow::tmate2IdleTimeoutMs() const
{
    auto& settings = AppSettings::instance();
    const QVariant raw = settings.value("TMate2UserInteractionTimeoutMs");
    if (!raw.isValid())
        return 0;
    const int timeoutMs = raw.toInt();
    if (timeoutMs <= 0)
        return 0;
    return std::clamp(timeoutMs, 100, 60000);
}

void MainWindow::restartTMate2IdleTimer()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    const int idleMs = tmate2IdleTimeoutMs();
    if (idleMs <= 0) {
        m_tmate2IdleTimer.stop();
        return;
    }
    m_tmate2IdleTimer.start(idleMs);
}

void MainWindow::noteTMate2Interaction()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    m_tmate2LastUserInteractionMs = QDateTime::currentMSecsSinceEpoch();
    if (m_tmate2DisplayBlanked) {
        m_tmate2DisplayBlanked = false;
        updateTMate2Display();
        updateTMate2Indicators();
    }
    restartTMate2IdleTimer();
}

void MainWindow::blankTMate2Display()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    if (tmate2OverlayActive()) {
        restartTMate2IdleTimer();
        return;
    }
    const int idleMs = tmate2IdleTimeoutMs();
    if (idleMs <= 0)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_tmate2LastUserInteractionMs > 0 &&
        now - m_tmate2LastUserInteractionMs < idleMs) {
        m_tmate2IdleTimer.start(static_cast<int>(idleMs - (now - m_tmate2LastUserInteractionMs)));
        return;
    }
    m_tmate2DisplayBlanked = true;
    QMetaObject::invokeMethod(m_hidEncoder, [this] {
        m_hidEncoder->setTMate2Display(0, 0);
        m_hidEncoder->clearTMate2Indicators();
    });
}

void MainWindow::triggerTMate2Overlay(TMate2Overlay overlay, int value)
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    auto& settings = AppSettings::instance();
    const int durationMs = std::clamp(
        settings.value("TMate2OverlayDurationMs",
                       QString::number(kTMate2DefaultOverlayDurationMs)).toInt(),
        100, 10000);
    m_tmate2Overlay = overlay;
    m_tmate2OverlayValue = value;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_tmate2LastUserInteractionMs = now;
    m_tmate2DisplayBlanked = false;
    m_tmate2OverlayUntilMs = now + durationMs;
    m_tmate2OverlayTimer.start(durationMs);
    updateTMate2Display();
    updateTMate2Indicators();
    restartTMate2IdleTimer();
}

// Push the current frequency and S-meter/power reading to the TMate 2 LCD.
// Called whenever the active-slice frequency, S-meter level, or device
// connection state changes.  Frequency comes from activeSlice(); S-meter uses
// the last value cached in m_tmate2SmeterDbm.
//
// small_val mapping:
//   RX: linear dBm offset from S9, clamped 0-999.
//       S9 (-73 dBm) → 90; each dB above S9 adds 1 (S9+10 dB → 100);
//       each dB below S9 subtracts 1 (S8 → 84, S5 → 66, S1 → 42).
//   TX: forward power in watts from the last txMetersChanged sample.
void MainWindow::updateTMate2Display()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    if (m_tmate2DisplayBlanked) return;

    if (tmate2OverlayActive()) {
        const int32_t mainVal = m_tmate2OverlayValue;
        QMetaObject::invokeMethod(m_hidEncoder, [this, mainVal] {
            m_hidEncoder->setTMate2Display(static_cast<uint32_t>(std::abs(mainVal)), 0);
        });
        return;
    }
    if (m_tmate2Overlay != TMate2Overlay::None) {
        m_tmate2Overlay = TMate2Overlay::None;
        m_tmate2OverlayUntilMs = 0;
    }

    // Frequency: active slice in Hz, 0 if no slice.
    uint32_t freqHz = 0;
    if (auto* s = activeSlice())
        freqHz = static_cast<uint32_t>(std::round(s->frequency() * 1e6));

    // S-meter small display: show the actual signal strength in dBm so the
    // number matches the AetherSDR UI reading.  The DBM and "-" indicator
    // segments (lit in setTMate2Indicators during RX) label it, so e.g.
    // -95 dBm reads as "-95".  The S-unit itself is conveyed by the 15-bar
    // bargraph, so the numeric and the bargraph are complementary, not
    // redundant.
    const int dbm       = static_cast<int>(std::round(m_tmate2SmeterDbm));
    const uint32_t sVal = static_cast<uint32_t>(std::clamp(std::abs(dbm), 0, 999));

    const uint32_t smallVal = m_radioModel.transmitModel().isTransmitting()
        ? static_cast<uint32_t>(m_tmate2TxWatts + 0.5f)
        : sVal;

    QMetaObject::invokeMethod(m_hidEncoder, [this, freqHz, smallVal] {
        m_hidEncoder->setTMate2Display(freqHz, smallVal);
    });
}

// Push the LED status byte to the TMate 2.
//   bit0 = radio connected; bit1 = VFO locked (active slice).
void MainWindow::updateTMate2Status()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    uint8_t led = 0;
    if (m_radioModel.isConnected())                              led |= 0x01u;
    if (auto* s = activeSlice(); s && s->isLocked())            led |= 0x02u;
    QMetaObject::invokeMethod(m_hidEncoder, [this, led] {
        m_hidEncoder->setTMate2Status(led);
    });
}

// Push all indicator segments (RX/TX, mode, S-meter bargraph, RIT/XIT) to
// the TMate 2.  Called whenever any of these state items changes.
void MainWindow::updateTMate2Indicators()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isTMate2()) return;
    if (m_tmate2DisplayBlanked) return;
    auto* s = activeSlice();
    const bool tx     = m_radioModel.transmitModel().isTransmitting();
    const QString mode = s ? s->mode() : QStringLiteral("USB");
    const bool rit    = s && s->ritOn();
    const bool xit    = s && s->xitOn();
    const float dbm   = m_tmate2SmeterDbm;
    auto& settings = AppSettings::instance();
    const uint8_t r = static_cast<uint8_t>(settings.value(
        tx ? "TMate2TxBacklightR" : "TMate2BacklightR",
        tx ? "255" : "0").toInt());
    const uint8_t g = static_cast<uint8_t>(settings.value(
        tx ? "TMate2TxBacklightG" : "TMate2BacklightG",
        tx ? "30" : "50").toInt());
    const uint8_t b = static_cast<uint8_t>(settings.value(
        tx ? "TMate2TxBacklightB" : "TMate2BacklightB",
        tx ? "0" : "255").toInt());
    if (tmate2OverlayActive()) {
        const QString overlay = tmate2OverlayName();
        const int overlayValue = m_tmate2OverlayValue;
        QMetaObject::invokeMethod(m_hidEncoder, [this, r, g, b, overlay, overlayValue, mode] {
            m_hidEncoder->setTMate2Backlight(r, g, b);
            m_hidEncoder->setTMate2OverlayIndicators(overlay, overlayValue, mode);
        });
        return;
    }
    QMetaObject::invokeMethod(m_hidEncoder, [this, tx, mode, dbm, rit, xit, r, g, b] {
        m_hidEncoder->setTMate2Backlight(r, g, b);
        m_hidEncoder->setTMate2Indicators(tx, mode, dbm, rit, xit);
    });
}

// Dispatch a resolved HID action name and optionally log it to the mapping
// dialog if it is open. Called for both F1/F2 hold (from the timer) and
// short-press (on release). (#3323)
void MainWindow::dispatchHidAction(const QString& actionName,
                                   const QString& gestureLabel)
{
    if (m_rc28MappingDialog && m_hidEncoder->isRC28Compatible())
        m_rc28MappingDialog->appendButtonEvent(gestureLabel, actionName);

    if (actionName == "StepCycle" || actionName == "StepUp") {
        if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepUp();
        if (auto* s = activeSlice()) {
            if (auto* sw = spectrumForSlice(s))
                triggerTMate2Overlay(TMate2Overlay::Speed, sw->stepSize());
        }
    } else if (actionName == "StepDown") {
        if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepDown();
        if (auto* s = activeSlice()) {
            if (auto* sw = spectrumForSlice(s))
                triggerTMate2Overlay(TMate2Overlay::Speed, sw->stepSize());
        }
    } else if (actionName == "ToggleRit") {
        if (auto* s = activeSlice()) {
            s->setRit(!s->ritOn(), s->ritFreq());
            triggerTMate2Overlay(TMate2Overlay::Rit, s->ritOn() ? s->ritFreq() : 0);
        }
    } else if (actionName == "ToggleXit") {
        if (auto* s = activeSlice()) s->setXit(!s->xitOn(), s->xitFreq());
    } else if (actionName == "ClearRit") {
        if (auto* s = activeSlice()) {
            s->setRit(s->ritOn(), 0);
            triggerTMate2Overlay(TMate2Overlay::Rit, 0);
        }
    } else if (actionName == "ClearXit") {
        if (auto* s = activeSlice()) s->setXit(s->xitOn(), 0);
    } else if (actionName == "ToggleMox") {
        m_radioModel.setTransmit(!m_radioModel.transmitModel().isTransmitting());
    } else if (actionName == "ToggleTune") {
        if (m_radioModel.transmitModel().isTuning())
            m_radioModel.transmitModel().stopTune();
        else
            m_radioModel.transmitModel().startTune();
    } else if (actionName == "ToggleMute") {
        if (m_audio) m_audio->setMuted(!m_audio->isMuted());
    } else if (actionName == "ToggleLock") {
        if (auto* s = activeSlice()) s->setLocked(!s->isLocked());
    } else if (actionName == "ToggleApf") {
        if (auto* s = activeSlice()) s->setApf(!s->apfOn());
    } else if (actionName == "ToggleAgc") {
        if (auto* s = activeSlice()) {
            static const char* modes[] = {"off","slow","med","fast"};
            const QString cur = s->agcMode().toLower();
            int idx = 0;
            for (int i = 0; i < 4; ++i) if (cur == modes[i]) { idx = i; break; }
            s->setAgcMode(modes[(idx + 1) % 4]);
        }
    } else if (actionName == "BandZoom") {
        auto* s = activeSlice();
        if (s) {
            const QString panId = !s->panId().isEmpty() ? s->panId()
                : (m_panStack ? m_panStack->activePanId() : m_radioModel.panId());
            if (!panId.isEmpty()) {
                m_flexVirtualBandZoomOn = !m_flexVirtualBandZoomOn;
                m_radioModel.sendCommand(QString("display pan set %1 band_zoom=%2")
                    .arg(panId).arg(m_flexVirtualBandZoomOn ? 1 : 0));
            }
        }
    } else if (actionName == "SegmentZoom") {
        auto* s = activeSlice();
        if (s) {
            const QString panId = !s->panId().isEmpty() ? s->panId()
                : (m_panStack ? m_panStack->activePanId() : m_radioModel.panId());
            if (!panId.isEmpty()) {
                m_flexVirtualSegmentZoomOn = !m_flexVirtualSegmentZoomOn;
                m_radioModel.sendCommand(QString("display pan set %1 segment_zoom=%2")
                    .arg(panId).arg(m_flexVirtualSegmentZoomOn ? 1 : 0));
            }
        }
    } else if (actionName == "NextSlice") {
        const auto& slices = m_radioModel.slices();
        if (slices.size() > 1) {
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
        }
    } else if (actionName == "PrevSlice") {
        const auto& slices = m_radioModel.slices();
        if (slices.size() > 1) {
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            setActiveSlice(slices[(idx - 1 + slices.size()) % slices.size()]->sliceId());
        }
    } else if (actionName == "VolumeUp") {
        const int next = std::clamp(
            AppSettings::instance().value("MasterVolume","100").toInt() + 5, 0, 100);
        if (m_titleBar) m_titleBar->setMasterVolume(next);
        applyMasterVolume(next);
    } else if (actionName == "VolumeDown") {
        const int next = std::clamp(
            AppSettings::instance().value("MasterVolume","100").toInt() - 5, 0, 100);
        if (m_titleBar) m_titleBar->setMasterVolume(next);
        applyMasterVolume(next);
    } else if (actionName == "SplitActiveSlice") {
        if (!m_splitActive) {
            auto* s = activeSlice();
            if (s && m_radioModel.slices().size() < m_radioModel.maxSlices()) {
                QString panId = s->panId().isEmpty()
                    ? (m_panStack ? m_panStack->activePanId() : m_radioModel.panId())
                    : s->panId();
                const bool isCw = s->mode() == "CW" || s->mode() == "CWL";
                m_splitActive    = true;
                m_splitRxSliceId = s->sliceId();
                m_radioModel.sendCommand(
                    QString("slice create pan=%1 freq=%2")
                    .arg(panId).arg(s->frequency() + (isCw ? 0.001 : 0.005), 0, 'f', 6));
            }
        } else {
            disableSplit();
        }
    // ── RC-28 extended actions (#3323) ─────────────────────────────────────
    } else if (actionName == "TuneFast") {
        m_hidFastTune = !m_hidFastTune;
        if (m_hidFastTune) m_hidFineTune = false;
        // Do NOT write the LED here: this runs from the 600 ms hold timer while
        // the button is still physically held, and writing the LED mid-hold is
        // what broke the F1 LED. The post-release timer in the button handler
        // updates it once the button is up — same as FlexRC-28.
    } else if (actionName == "FineTune") {
        m_hidFineTune = !m_hidFineTune;
        if (m_hidFineTune) m_hidFastTune = false;
        // LED handled by the post-release timer, same as TuneFast above.
    } else if (actionName == "ModeCycle") {
        if (auto* s = activeSlice()) {
            static const char* kModes[] = {"LSB", "USB", "CW", "AM"};
            constexpr int kModeCount = static_cast<int>(std::size(kModes));
            const QString cur = s->mode().toUpper();
            int idx = -1;
            for (int i = 0; i < kModeCount; ++i)
                if (cur == kModes[i]) { idx = i; break; }
            // Unknown/extended mode (CWL, FM, DIGU…) → idx stays -1 so the cycle
            // starts cleanly at the first entry (LSB) rather than skipping it.
            s->setMode(kModes[(idx + 1) % kModeCount]);
        }
    } else if (actionName == "BandCycle") {
        if (auto* s = activeSlice()) {
            struct BandEntry { double freqMhz; const char* mode; };
            static const BandEntry kBands[] = {
                {1.900,  "LSB"}, {3.750,  "LSB"}, {5.3715, "USB"},
                {7.150,  "LSB"}, {10.120, "USB"}, {14.225, "USB"},
                {18.128, "USB"}, {21.285, "USB"}, {24.940, "USB"},
                {28.500, "USB"}, {50.150, "USB"},
            };
            static constexpr int kBandCount = static_cast<int>(std::size(kBands));
            const double cur = s->frequency();
            int idx = 0;
            double closest = std::numeric_limits<double>::max();
            for (int i = 0; i < kBandCount; ++i) {
                const double d = std::abs(cur - kBands[i].freqMhz);
                if (d < closest) { closest = d; idx = i; }
            }
            const auto& next = kBands[(idx + 1) % kBandCount];
            s->setMode(next.mode);
            applyTuneRequest(s, next.freqMhz, TuneIntent::CommandedTargetCenter, "rc28");
        }
    } else if (actionName == "SnapKHz") {
        if (auto* s = activeSlice()) {
            const double snapped = std::round(s->frequency() * 1000.0) / 1000.0;
            // 1e-9 MHz (1 mHz) tolerance: skip a redundant tune when already on
            // grid without being fooled by floating-point rounding noise.
            if (std::abs(snapped - s->frequency()) > 1e-9)
                applyTuneRequest(s, snapped, TuneIntent::CommandedTargetCenter, "rc28");
        }
    } else if (actionName == "Snap100kHz") {
        if (auto* s = activeSlice()) {
            const double snapped = std::round(s->frequency() * 10.0) / 10.0;
            if (std::abs(snapped - s->frequency()) > 1e-9)
                applyTuneRequest(s, snapped, TuneIntent::CommandedTargetCenter, "rc28");
        }
    } else if (actionName == "Snap500kHz") {
        if (auto* s = activeSlice()) {
            const double snapped = std::round(s->frequency() * 2.0) / 2.0;
            if (std::abs(snapped - s->frequency()) > 1e-9)
                applyTuneRequest(s, snapped, TuneIntent::CommandedTargetCenter, "rc28");
        }
    } else if (actionName == "Snap100Hz") {
        if (auto* s = activeSlice()) {
            const double snapped = std::round(s->frequency() * 10000.0) / 10000.0;
            if (std::abs(snapped - s->frequency()) > 1e-9)
                applyTuneRequest(s, snapped, TuneIntent::CommandedTargetCenter, "rc28");
        }
    } else if (actionName == "Snap500Hz") {
        if (auto* s = activeSlice()) {
            const double snapped = std::round(s->frequency() * 2000.0) / 2000.0;
            if (std::abs(snapped - s->frequency()) > 1e-9)
                applyTuneRequest(s, snapped, TuneIntent::CommandedTargetCenter, "rc28");
        }
    }
}
#endif

#ifdef HAVE_HIDAPI
// Render the full 800x100 touchscreen strip for the StreamDeck+.
// Four equal 200x100 sections, one per encoder. Each section shows the turn
// action on the top half and the push action + state on the bottom half.
static QByteArray renderTouchscreenJpeg(
    const std::array<QString,4>& turnLabels,
    const std::array<QString,4>& pushLabels,
    const std::array<QString,4>& stateTexts,
    const std::array<bool,4>&    active)
{
    constexpr int W = 800, H = 100, COLS = 4, COL_W = W / COLS;

    QImage img(W, H, QImage::Format_RGB32);
    img.fill(QColor(15, 15, 20));

    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);

    for (int i = 0; i < COLS; ++i) {
        const int x = i * COL_W;

        // Top half: turn action — dark blue tint
        p.fillRect(x, 0, COL_W - 1, 49, QColor(18, 28, 52));

        // Bottom half: push action — color indicates state
        QColor pushBg;
        if (active[i])                                       pushBg = QColor(10, 70, 10);
        else if (pushLabels[i].isEmpty())                    pushBg = QColor(12, 12, 16);
        else                                                 pushBg = QColor(45, 18, 18);
        p.fillRect(x, 51, COL_W - 1, H - 51, pushBg);

        // Divider line (1px, slightly lighter)
        p.fillRect(x + COL_W - 1, 0, 1, H, QColor(40, 40, 50));
        // Horizontal divider between top/bottom halves
        p.fillRect(x, 49, COL_W - 1, 2, QColor(35, 35, 45));

        // Turn label
        if (!turnLabels[i].isEmpty()) {
            QFont f;
            f.setPixelSize(20);
            f.setBold(true);
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(QRect(x + 2, 0, COL_W - 4, 49), Qt::AlignCenter, turnLabels[i]);
        }

        // Push label + state
        if (!pushLabels[i].isEmpty()) {
            QFont f;
            f.setPixelSize(16);
            f.setBold(active[i]);
            p.setFont(f);
            const QString line = stateTexts[i].isEmpty()
                ? pushLabels[i]
                : pushLabels[i] + QLatin1String("  ") + stateTexts[i];
            p.setPen(active[i] ? QColor(120, 255, 120) : QColor(200, 200, 200));
            p.drawText(QRect(x + 2, 51, COL_W - 4, H - 51), Qt::AlignCenter, line);
        }
    }

    p.end();

    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 92);
    return bytes;
}

// Render a 120x120 JPEG label for a single StreamDeck+ LCD key.
static QByteArray renderKeyImageJpeg(const QString& label, const QColor& bg)
{
    QImage img(120, 120, QImage::Format_RGB32);
    img.fill(bg);

    if (!label.isEmpty()) {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f;
        f.setPixelSize(label.length() > 6 ? 22 : 28);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRect(4, 4, 112, 112), Qt::AlignCenter | Qt::TextWordWrap, label);
    }

    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    return bytes;
}

void MainWindow::refreshStreamDeckLabels()
{
    if (!m_hidEncoder || !m_hidEncoder->isOpen() || !m_hidEncoder->isStreamDeckPlus())
        return;

    static const QHash<QString, QString> kShortLabels{
        {QStringLiteral("WheelFrequency"),      QStringLiteral("TUNE")},
        {QStringLiteral("WheelRit"),            QStringLiteral("RIT")},
        {QStringLiteral("WheelXit"),            QStringLiteral("XIT")},
        {QStringLiteral("WheelVolume"),         QStringLiteral("VOL")},
        {QStringLiteral("WheelSliceAudio"),     QStringLiteral("S.VOL")},
        {QStringLiteral("WheelHeadphoneVolume"),QStringLiteral("H.VOL")},
        {QStringLiteral("WheelAgcT"),           QStringLiteral("AGC-T")},
        {QStringLiteral("WheelApf"),            QStringLiteral("APF")},
        {QStringLiteral("WheelCwSpeed"),        QStringLiteral("CW SPD")},
        {QStringLiteral("WheelPower"),          QStringLiteral("RF PWR")},
        {QStringLiteral("StepCycle"),           QStringLiteral("STEP")},
        {QStringLiteral("ToggleRit"),           QStringLiteral("RIT")},
        {QStringLiteral("ToggleXit"),           QStringLiteral("XIT")},
        {QStringLiteral("ToggleMox"),           QStringLiteral("MOX")},
        {QStringLiteral("ToggleMute"),          QStringLiteral("MUTE")},
        {QStringLiteral("ToggleLock"),          QStringLiteral("LOCK")},
        {QStringLiteral("None"),                {}},
    };

    static const char* kTurnDflt[4] = {"WheelFrequency","WheelRit","WheelXit","WheelVolume"};
    static const char* kPushDflt[4] = {"StepCycle","ToggleRit","ToggleXit","None"};

    auto& settings = AppSettings::instance();
    auto* slice    = activeSlice();
    const bool ritOn = slice && slice->ritOn();
    const bool xitOn = slice && slice->xitOn();

    std::array<QString,4> turnLabels, pushLabels, stateTexts;
    std::array<bool,4> activeFlags{};

    for (int i = 0; i < 4; ++i) {
        const QString turnId = settings.value(QString("HidEncoderAction%1").arg(i),
                                              QString::fromLatin1(kTurnDflt[i])).toString();
        turnLabels[i] = kShortLabels.value(turnId, turnId.left(6).toUpper());

        const QString pushId = settings.value(QString("HidEncoderPushAction%1").arg(i),
                                              QString::fromLatin1(kPushDflt[i])).toString();
        pushLabels[i] = kShortLabels.value(pushId, pushId.left(6).toUpper());

        if (pushId == QLatin1String("ToggleRit")) {
            activeFlags[i] = ritOn;
            stateTexts[i]  = ritOn ? QStringLiteral("ON") : QStringLiteral("OFF");
        } else if (pushId == QLatin1String("ToggleXit")) {
            activeFlags[i] = xitOn;
            stateTexts[i]  = xitOn ? QStringLiteral("ON") : QStringLiteral("OFF");
        }
        // StepCycle and others: no state text, not "active"
    }

    QByteArray tsImg = renderTouchscreenJpeg(turnLabels, pushLabels, stateTexts, activeFlags);

    // Build labeled 120x120 images for the 8 LCD keys
    static const QHash<QString, QColor> kKeyBgColors{
        {QStringLiteral("ToggleMox"),        QColor(70, 20, 20)},
        {QStringLiteral("ToggleTune"),       QColor(70, 20, 20)},
        {QStringLiteral("ToggleRit"),        QColor(20, 55, 20)},
        {QStringLiteral("ToggleXit"),        QColor(20, 55, 20)},
        {QStringLiteral("ClearRit"),         QColor(20, 40, 20)},
        {QStringLiteral("ClearXit"),         QColor(20, 40, 20)},
        {QStringLiteral("VolumeUp"),         QColor(40, 20, 60)},
        {QStringLiteral("VolumeDown"),       QColor(40, 20, 60)},
        {QStringLiteral("SplitActiveSlice"), QColor(60, 40, 10)},
    };
    static const QHash<QString, QString> kKeyShortLabels{
        {QStringLiteral("None"),             {}},
        {QStringLiteral("ToggleMox"),        QStringLiteral("MOX")},
        {QStringLiteral("ToggleTune"),       QStringLiteral("TUNE")},
        {QStringLiteral("ToggleRit"),        QStringLiteral("RIT")},
        {QStringLiteral("ToggleXit"),        QStringLiteral("XIT")},
        {QStringLiteral("ClearRit"),         QStringLiteral("CLR\nRIT")},
        {QStringLiteral("ClearXit"),         QStringLiteral("CLR\nXIT")},
        {QStringLiteral("StepUp"),           QStringLiteral("STEP\nUP")},
        {QStringLiteral("StepDown"),         QStringLiteral("STEP\nDN")},
        {QStringLiteral("ToggleMute"),       QStringLiteral("MUTE")},
        {QStringLiteral("ToggleLock"),       QStringLiteral("LOCK")},
        {QStringLiteral("ToggleApf"),        QStringLiteral("APF")},
        {QStringLiteral("ToggleAgc"),        QStringLiteral("AGC")},
        {QStringLiteral("BandZoom"),         QStringLiteral("BAND\nZOOM")},
        {QStringLiteral("SegmentZoom"),      QStringLiteral("SEG\nZOOM")},
        {QStringLiteral("NextSlice"),        QStringLiteral("NEXT\nSLICE")},
        {QStringLiteral("PrevSlice"),        QStringLiteral("PREV\nSLICE")},
        {QStringLiteral("VolumeUp"),         QStringLiteral("VOL +")},
        {QStringLiteral("VolumeDown"),       QStringLiteral("VOL -")},
        {QStringLiteral("SplitActiveSlice"), QStringLiteral("SPLIT")},
    };
    const QColor kDefaultKeyBg(20, 28, 45);

    QVector<QByteArray> keyImages(8);
    for (int i = 0; i < 8; ++i) {
        const QString actionId = settings.value(QString("HidKeyAction%1").arg(i),
                                                QStringLiteral("None")).toString();
        const QString lbl = kKeyShortLabels.value(actionId, actionId.left(8).toUpper());
        const QColor  bg  = kKeyBgColors.value(actionId, kDefaultKeyBg);
        keyImages[i] = renderKeyImageJpeg(lbl, bg);
    }

    QMetaObject::invokeMethod(m_hidEncoder,
        [enc=m_hidEncoder, ts=tsImg, keys=keyImages]() {
            enc->setTouchscreenImage(ts);
            enc->setKeyImages(keys);
        }, Qt::QueuedConnection);
}
#endif

void MainWindow::applyFlexControlWheelAction(const QString& actionId, int steps)
{
    if (steps == 0)
        return;

    if (actionId == "WheelFrequency") {
        auto* s = activeSlice();
        if (!s) return;
        if (s->isLocked()) {
            s->notifyTuneBlockedByLock();
            // Drop queued tuning so unlock does not replay stale wheel input.
            m_flexTargetMhz = -1.0;
            return;
        }
        auto* sw = spectrumForSlice(s);
        const int stepHz = sw ? sw->stepSize()
                              : (s->stepHz() > 0 ? s->stepHz() : 100);
        if (m_flexTargetMhz < 0.0 ||
            (!m_flexCoalesceTimer.isActive() &&
             std::abs(m_flexTargetMhz - s->frequency()) > 0.001)) {
            // Snap the base to the active step grid so encoder ticks land on
            // clean multiples even when the slice frequency is off-grid
            // (e.g. after a step-size change or a typed entry). Mirrors the
            // MIDI tune-knob path at the top of this file (#3260).
            const long long curHz =
                static_cast<long long>(std::round(s->frequency() * 1e6));
            const long long snapped = stepHz > 0
                ? ((curHz + stepHz / 2) / stepHz) * stepHz
                : curHz;
            m_flexTargetMhz = snapped / 1e6;
        }
        m_flexTargetMhz += steps * stepHz / 1e6;
        if (sw) sw->setVfoFrequency(m_flexTargetMhz);
        if (!m_flexCoalesceTimer.isActive())
            m_flexCoalesceTimer.start();
    } else if (actionId == "WheelRit") {
        if (auto* s = activeSlice()) {
            const int hz = std::clamp(s->ritFreq() + steps * 10, -9999, 9999);
            s->setRit(true, hz);
#ifdef HAVE_HIDAPI
            triggerTMate2Overlay(TMate2Overlay::Rit, hz);
#endif
        }
    } else if (actionId == "WheelXit") {
        if (auto* s = activeSlice()) {
            const int hz = std::clamp(s->xitFreq() + steps * 10, -9999, 9999);
            s->setXit(true, hz);
        }
    } else if (actionId == "WheelVolume" || actionId == "WheelMasterAf") {
        // Route to master volume to match SmartSDR behavior (#2921).
        // "WheelMasterAf" is the legacy action name from #2888; accepted
        // here for back-compat with saved FlexControl bindings made
        // before the #2986 consolidation but routes to the same code path.
        const int current = AppSettings::instance().value("MasterVolume", "100").toInt();
        const int next = std::clamp(current + steps * 2, 0, 100);
        if (m_titleBar)
            m_titleBar->setMasterVolume(next);
        applyMasterVolume(next);
#ifdef HAVE_HIDAPI
        triggerTMate2Overlay(TMate2Overlay::Volume, next);
#endif
    } else if (actionId == "WheelSliceAudio") {
        if (auto* s = activeSlice()) {
            const float next = std::clamp(s->audioGain() + steps * 2.0f, 0.0f, 100.0f);
            s->setAudioGain(next);
#ifdef HAVE_HIDAPI
            triggerTMate2Overlay(TMate2Overlay::Volume, static_cast<int>(next));
#endif
        }
    } else if (actionId == "WheelHeadphoneVolume") {
        const int next = std::clamp(m_radioModel.headphoneGain() + steps * 2, 0, 100);
        if (m_titleBar)
            m_titleBar->setHeadphoneVolume(next);
        m_radioModel.setHeadphoneGain(next);
#ifdef HAVE_HIDAPI
        triggerTMate2Overlay(TMate2Overlay::Volume, next);
#endif
    } else if (actionId == "WheelAgcT") {
        if (auto* s = activeSlice())
            s->setAgcThreshold(std::clamp(s->agcThreshold() + steps, 0, 100));
    } else if (actionId == "WheelApf") {
        if (auto* s = activeSlice())
            s->setApfLevel(std::clamp(s->apfLevel() + steps, 0, 100));
    } else if (actionId == "NextSlice" || actionId == "PrevSlice") {
        const auto& slices = m_radioModel.slices();
        if (slices.size() <= 1) return;
        int idx = 0;
        for (int i = 0; i < slices.size(); ++i) {
            if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
        }
        const int direction = actionId == "PrevSlice" ? -1 : 1;
        int next = (idx + steps * direction) % slices.size();
        if (next < 0)
            next += slices.size();
        setActiveSlice(slices[next]->sliceId());
    } else if (actionId == "WheelPower") {
        auto& tx = m_radioModel.transmitModel();
        const int next = std::clamp(tx.rfPower() + steps, 0, 100);
        tx.setRfPower(next);
#ifdef HAVE_HIDAPI
        triggerTMate2Overlay(TMate2Overlay::Power, next);
#endif
    } else if (actionId == "WheelCwSpeed") {
        auto& tx = m_radioModel.transmitModel();
        const int next = std::clamp(tx.cwSpeed() + steps, 5, 100);
        tx.setCwSpeed(next);
#ifdef HAVE_HIDAPI
        triggerTMate2Overlay(TMate2Overlay::Wpm, next);
#endif
    }
}

QJsonObject MainWindow::buildControlDevicesSnapshot() const
{
    auto stringArray = [](const QStringList& values) {
        QJsonArray array;
        for (const QString& value : values)
            array.append(value);
        return array;
    };

    auto buttonBindings = [](const QString& prefix, int buttonCount) {
        static const char* kActionNames[] = {"tap", "double_tap", "hold"};
        QJsonArray bindings;
        auto& settings = AppSettings::instance();
        for (int button = 1; button <= buttonCount; ++button) {
            for (int action = 0; action < 3; ++action) {
                const QString key = QString("%1Btn%2Action%3").arg(prefix).arg(button).arg(action);
                const QString mappedAction = settings.value(key, "None").toString();
                if (mappedAction == QStringLiteral("None"))
                    continue;
                QJsonObject obj;
                obj["button"] = button;
                obj["gesture"] = kActionNames[action];
                obj["action"] = mappedAction;
                bindings.append(obj);
            }
        }
        return bindings;
    };

    auto addTarget = [this](QJsonObject* obj) {
        if (m_activeSliceId >= 0)
            (*obj)["target_slice_id"] = m_activeSliceId;
        else
            (*obj)["target_slice_id"] = QJsonValue();
        (*obj)["target_scope"] = "active_slice";
    };

    auto flexWheelModeName = [](FlexWheelMode mode) {
        switch (mode) {
        case FlexWheelMode::Frequency: return QStringLiteral("Frequency");
        case FlexWheelMode::Volume:    return QStringLiteral("Volume");
        case FlexWheelMode::Power:     return QStringLiteral("Power");
        case FlexWheelMode::Rit:       return QStringLiteral("Rit");
        case FlexWheelMode::Xit:       return QStringLiteral("Xit");
        case FlexWheelMode::SliceAudio:
            return QStringLiteral("SliceAudio");
        case FlexWheelMode::HeadphoneVolume:
            return QStringLiteral("HeadphoneVolume");
        case FlexWheelMode::AgcT:      return QStringLiteral("AgcT");
        case FlexWheelMode::Apf:       return QStringLiteral("Apf");
        case FlexWheelMode::CwSpeed:   return QStringLiteral("CwSpeed");
        }
        return QStringLiteral("Unknown");
    };

    const bool activeSliceAvailable = activeSlice() != nullptr;
    QJsonArray devices;
    int activeDeviceCount = 0;

    auto appendDevice = [&devices, &activeDeviceCount](QJsonObject device) {
        if (device["active"].toBool())
            ++activeDeviceCount;
        devices.append(device);
    };

#ifdef HAVE_SERIALPORT
    {
        const bool active = m_flexControl && m_flexControl->isOpen();
        QJsonObject flex;
        flex["type"] = "FlexControl";
        flex["available"] = true;
        flex["active"] = active;
        flex["active_for_current_slice"] = active && activeSliceAvailable;
        flex["bus_type"] = "USB";
        flex["transport"] = "USB serial";
        flex["wheel_mode"] = flexWheelModeName(m_flexWheelMode);
        flex["port_name"] = (m_flexControl && active)
            ? m_flexControl->portName()
            : AppSettings::instance().value("FlexControlPort").toString();
        flex["auto_detect"] = AppSettings::instance().value("FlexControlAutoDetect", "True").toString() == "True";
        flex["invert_direction"] = AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True";
        flex["button_bindings"] = buttonBindings(QStringLiteral("FlexControl"), 4);
        addTarget(&flex);
        flex["detail"] = active
            ? QString("Flex wheel controls %1 on the active slice").arg(flex["wheel_mode"].toString())
            : QStringLiteral("FlexControl is not connected");
        appendDevice(flex);
    }
#else
    appendDevice(QJsonObject{
        {"type", "FlexControl"},
        {"available", false},
        {"active", false},
        {"active_for_current_slice", false},
        {"bus_type", "USB"},
        {"detail", "Qt SerialPort support is not compiled in"}
    });
#endif

#ifdef HAVE_HIDAPI
    {
        const bool active = m_hidEncoder && m_hidEncoder->isOpen();
        QJsonObject hid;
        hid["type"] = "USB HID Wheel";
        hid["available"] = true;
        hid["active"] = active;
        hid["active_for_current_slice"] = active && activeSliceAvailable;
        hid["bus_type"] = "USB";
        hid["transport"] = "hidapi";
        hid["device_name"] = m_hidEncoder ? m_hidEncoder->deviceName() : QString();
        hid["vendor_id"] = (m_hidEncoder && m_hidEncoder->vendorId() != 0)
            ? QString("0x%1").arg(m_hidEncoder->vendorId(), 4, 16, QChar('0'))
            : QString();
        hid["product_id"] = (m_hidEncoder && m_hidEncoder->productId() != 0)
            ? QString("0x%1").arg(m_hidEncoder->productId(), 4, 16, QChar('0'))
            : QString();
        hid["auto_detect"] = AppSettings::instance().value("HidEncoderAutoDetect", "True").toString() == "True";
        hid["invert_direction"] = AppSettings::instance().value("HidEncoderInvertDir", "False").toString() == "True";
        hid["button_bindings"] = buttonBindings(QStringLiteral("HidEncoder"), 16);
        addTarget(&hid);
        hid["detail"] = active
            ? QString("HID wheel `%1` tunes the active slice").arg(hid["device_name"].toString())
            : QStringLiteral("No supported HID wheel is connected");
        appendDevice(hid);
    }
#else
    appendDevice(QJsonObject{
        {"type", "USB HID Wheel"},
        {"available", false},
        {"active", false},
        {"active_for_current_slice", false},
        {"bus_type", "USB"},
        {"detail", "hidapi support is not compiled in"}
    });
#endif

#ifdef HAVE_MIDI
    {
        auto messageTypeName = [](MidiBinding::MsgType type) {
            switch (type) {
            case MidiBinding::CC:        return QStringLiteral("CC");
            case MidiBinding::NoteOn:    return QStringLiteral("NoteOn");
            case MidiBinding::NoteOff:   return QStringLiteral("NoteOff");
            case MidiBinding::PitchBend: return QStringLiteral("PitchBend");
            }
            return QStringLiteral("Unknown");
        };

        auto bindingScope = [](const QString& paramId, const QString& category) {
            if (paramId.startsWith(QStringLiteral("rx."))
                || paramId == QStringLiteral("global.nextSlice")
                || paramId == QStringLiteral("global.prevSlice")) {
                return QStringLiteral("active_slice");
            }
            if (category == QStringLiteral("Global"))
                return QStringLiteral("global");
            if (category == QStringLiteral("TX") || category == QStringLiteral("Phone/CW"))
                return QStringLiteral("transmit");
            return category.toLower();
        };

        const bool active = m_midiControl && m_midiControl->isOpen();
        QJsonArray bindings;
        int sliceBindingCount = 0;
        if (m_midiControl) {
            for (const MidiBinding& binding : m_midiControl->bindings()) {
                const MidiParam* param = m_midiControl->findParam(binding.paramId);
                const QString category = param ? param->category : QString();
                const QString scope = bindingScope(binding.paramId, category);
                if (scope == QStringLiteral("active_slice"))
                    ++sliceBindingCount;

                QJsonObject obj;
                obj["source"] = binding.sourceDisplayName();
                obj["channel"] = binding.channel < 0
                    ? QStringLiteral("any")
                    : QString::number(binding.channel + 1);
                obj["message_type"] = messageTypeName(binding.msgType);
                obj["number"] = binding.number;
                obj["param_id"] = binding.paramId;
                obj["param_name"] = param ? param->displayName : binding.paramId;
                obj["category"] = category;
                obj["relative"] = binding.relative;
                obj["inverted"] = binding.inverted;
                obj["scope"] = scope;
                bindings.append(obj);
            }
        }

        QJsonObject midi;
        midi["type"] = "MIDI Controller";
        midi["available"] = true;
        midi["active"] = active;
        midi["active_for_current_slice"] = active && activeSliceAvailable && sliceBindingCount > 0;
        midi["bus_type"] = "Unknown";
        midi["transport"] = "RtMidi";
        midi["port_name"] = m_midiControl ? m_midiControl->currentPortName() : QString();
        midi["available_ports"] = m_midiControl ? stringArray(m_midiControl->availablePorts()) : QJsonArray{};
        midi["binding_count"] = bindings.size();
        midi["slice_binding_count"] = sliceBindingCount;
        midi["bindings"] = bindings;
        addTarget(&midi);
        midi["detail"] = active
            ? QString("MIDI port `%1` has %2 active-slice binding(s)")
                  .arg(midi["port_name"].toString())
                  .arg(sliceBindingCount)
            : QStringLiteral("MIDI controller is not connected");
        appendDevice(midi);
    }
#else
    appendDevice(QJsonObject{
        {"type", "MIDI Controller"},
        {"available", false},
        {"active", false},
        {"active_for_current_slice", false},
        {"bus_type", "Unknown"},
        {"detail", "MIDI support is not compiled in"}
    });
#endif

    QJsonObject snapshot;
    snapshot["available"] = true;
    snapshot["target_scope"] = "active_slice";
    snapshot["active_slice_available"] = activeSliceAvailable;
    if (m_activeSliceId >= 0)
        snapshot["target_slice_id"] = m_activeSliceId;
    else
        snapshot["target_slice_id"] = QJsonValue();
    snapshot["active_device_count"] = activeDeviceCount;
    snapshot["devices"] = devices;
    snapshot["note"] = "External wheel controls operate on the current active slice unless a binding changes slices.";
    return snapshot;
}

#ifdef HAVE_MIDI
void MainWindow::registerMidiParams()
{
    using P = MidiParamType;
    // Setters/getters stored on MainWindow for main-thread dispatch (#502).
    // MidiControlManager gets metadata only (no lambdas that capture main-thread objects).
    auto reg = [this](const char* id, const char* name, const char* cat,
                      MidiParamType type, float lo, float hi,
                      std::function<void(float)> setter,
                      std::function<float()> getter = {}) {
        m_midiSetters[id] = setter;
        if (getter) m_midiGetters[id] = getter;
        m_midiControl->registerParam({id, name, cat, type, lo, hi, std::move(setter), std::move(getter)});
    };

    // ── RX ──────────────────────────────────────────────────────────────
    reg("rx.afGain", "AF Gain", "RX", P::Slider, 0, 200,
        [this](float v) { if (auto* s = activeSlice()) s->setAudioGain(v); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->audioGain() : 0; });

    reg("rx.squelch", "Squelch Level", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = activeSlice()) s->setSquelch(s->squelchOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->squelchLevel() : 0; });

    reg("rx.agcThreshold", "AGC Threshold", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = activeSlice()) s->setAgcThreshold(static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->agcThreshold() : 0; });

    reg("rx.audioPan", "Audio Pan", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = activeSlice()) s->setAudioPan(static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->audioPan() : 50; });

    reg("rx.nbEnable", "Noise Blanker", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setNb(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->nbOn() ? 1 : 0; });

    reg("rx.nrEnable", "Noise Reduction", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setNr(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->nrOn() ? 1 : 0; });

    reg("rx.anfEnable", "Auto Notch", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setAnf(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->anfOn() ? 1 : 0; });

    reg("rx.squelchEnable", "Squelch Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setSquelch(v > 0.5f, s->squelchLevel()); },
        [this]() -> float { auto* s = activeSlice(); return s && s->squelchOn() ? 1 : 0; });

    reg("rx.mute", "Audio Mute", "RX", P::Toggle, 0, 1,
        [this](float v) { m_audio->setMuted(v > 0.5f); },
        [this]() -> float { return m_audio->isMuted() ? 1 : 0; });

    reg("rx.tuneLock", "Tune Lock", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setLocked(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->isLocked() ? 1 : 0; });

    reg("rx.ritEnable", "RIT Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setRit(v > 0.5f, s->ritFreq()); },
        [this]() -> float { auto* s = activeSlice(); return s && s->ritOn() ? 1 : 0; });

    reg("rx.xitEnable", "XIT Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = activeSlice()) s->setXit(v > 0.5f, s->xitFreq()); },
        [this]() -> float { auto* s = activeSlice(); return s && s->xitOn() ? 1 : 0; });

    reg("rx.nr2Enable", "NR2 (Spectral)", "RX", P::Toggle, 0, 1,
        [this](float v) {
            if (v > 0.5f) {
                enableNr2WithWisdom();
            } else {
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(false); });
            }
        },
        [this]() -> float { return m_audio->nr2Enabled() ? 1 : 0; });

    reg("rx.rn2Enable", "RN2 (RNNoise)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setRn2Enabled(v > 0.5f); }); },
        [this]() -> float { return m_audio->rn2Enabled() ? 1 : 0; });

    reg("rx.nr4Enable", "NR4 (Spectral Bleach)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4Enabled(v > 0.5f); }); },
        [this]() -> float { return m_audio->nr4Enabled() ? 1 : 0; });

    reg("rx.dfnrEnable", "DFNR (DeepFilter)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setDfnrEnabled(v > 0.5f); }); },
        [this]() -> float { return m_audio->dfnrEnabled() ? 1 : 0; });

    reg("rx.stepUp", "Step Size Up", "RX", P::Trigger, 0, 1,
        [this](float) { if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepUp(); });

    reg("rx.stepDown", "Step Size Down", "RX", P::Trigger, 0, 1,
        [this](float) { if (auto* rx = m_appletPanel->rxApplet()) rx->cycleStepDown(); });

    // rx.tuneKnob: bind a relative MIDI knob for VFO tuning.
    // Set the binding to "relative" mode in MIDI Mapping dialog.
    // Steps are coalesced every 20ms, but each controller detent remains one
    // radio step to avoid jumpy jog-wheel behavior.
    reg("rx.tuneKnob", "VFO Tune Knob", "RX", P::Slider, 0, 127,
        [this](float v) {
            // Absolute fallback (non-relative bindings): center=64
            auto* s = activeSlice();
            if (!s) return;
            int steps = static_cast<int>(v) - 64;
            if (steps == 0) return;
            if (s->isLocked()) {
                s->notifyTuneBlockedByLock();
                return;
            }
            int stepHz = spectrum() ? spectrum()->stepSize() : 100;
            double newMhz = s->frequency() + steps * stepHz / 1e6;
            applyTuneRequest(s, newMhz, TuneIntent::IncrementalTune, "midi-absolute");
        });

    // ── TX ──────────────────────────────────────────────────────────────
    reg("tx.rfPower", "RF Power", "TX", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setRfPower(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().rfPower(); });

    reg("tx.tunePower", "Tune Power", "TX", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setTunePower(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().tunePower(); });

    reg("tx.mox", "MOX", "TX", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.setTransmit(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().isTransmitting() ? 1 : 0; });

    reg("tx.tune", "TUNE", "TX", P::Toggle, 0, 1,
        [this](float v) {
            if (v > 0.5f)
                m_radioModel.transmitModel().startTune();
            else
                m_radioModel.transmitModel().stopTune();
        },
        [this]() -> float { return m_radioModel.transmitModel().isTuning() ? 1 : 0; });

    reg("tx.atuStart", "ATU Start", "TX", P::Trigger, 0, 1,
        [this](float) { m_radioModel.sendCommand("atu start"); });

    // ── Phone/CW ────────────────────────────────────────────────────────
    reg("phone.micLevel", "Mic Level", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setMicLevel(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().micLevel(); });

    reg("phone.monGain", "Monitor Volume", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setMonGainSb(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().monGainSb(); });

    reg("phone.procEnable", "Speech Processor", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setSpeechProcessorEnable(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().companderOn() ? 1 : 0; });

    reg("phone.daxEnable", "DAX", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setDax(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().daxOn() ? 1 : 0; });

    reg("phone.monEnable", "Monitor", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setSbMonitor(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().sbMonitor() ? 1 : 0; });

    reg("phone.voxEnable", "VOX Enable", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setVoxEnable(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().voxEnable() ? 1 : 0; });

    reg("phone.voxLevel", "VOX Level", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setVoxLevel(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().voxLevel(); });

    reg("phone.amCarrier", "AM Carrier", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_radioModel.transmitModel().setAmCarrierLevel(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().amCarrierLevel(); });

    reg("cw.speed", "CW Speed", "Phone/CW", P::Slider, 5, 100,
        [this](float v) { m_radioModel.transmitModel().setCwSpeed(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().cwSpeed(); });

    reg("cw.delayMs", "CW Break-In Delay", "Phone/CW", P::Slider, 0, 2000,
        [this](float v) { m_radioModel.transmitModel().setCwDelay(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().cwDelay(); });

    reg("cw.sidetoneEnable", "CW Sidetone", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwSidetone(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().cwSidetone() ? 1 : 0; });

    reg("cw.iambicEnable", "CW Iambic", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwIambic(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().cwIambic() ? 1 : 0; });

    reg("cw.iambicMode", "CW Iambic Mode (0=A, 1=B)", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwIambicMode(v > 0.5f ? 1 : 0); },
        [this]() -> float { return m_radioModel.transmitModel().cwIambicMode() ? 1 : 0; });

    reg("cw.swapPaddles", "CW Swap Paddles", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwSwapPaddles(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().cwSwapPaddles() ? 1 : 0; });

    reg("cw.cwlEnable", "CWL Frequency Offset", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwlEnabled(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().cwlEnabled() ? 1 : 0; });

    reg("cw.breakInEnable", "CW Break-In (QSK)", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.transmitModel().setCwBreakIn(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().cwBreakIn() ? 1 : 0; });

    reg(kCwStraightKeyActionId, kCwStraightKeyActionName, "Phone/CW", P::Gate, 0, 1,
        [this](float v) {
            setCwStraightKeyState(v > 0.5f, QStringLiteral("midi:cwkey"),
                                  m_currentMidiTrace.traceId,
                                  m_currentMidiTrace.callbackMs);
        });

    // Iambic paddle: left and right are separate momentary actions.
    // When the local iambic keyer is running, paddle states feed into it
    // (drives sidetone with sub-5 ms latency, then forwards to radio).
    // Otherwise pass straight to the radio's RF iambic engine.
    reg(kCwLeftPaddleActionId, kCwLeftPaddleActionName, "Phone/CW", P::Gate, 0, 1,
        [this](float v) {
            setCwLeftPaddleState(v > 0.5f, QStringLiteral("midi:cwdit"),
                                 m_currentMidiTrace.traceId,
                                 m_currentMidiTrace.callbackMs);
        },
        [this]() -> float { return m_cwLeftPaddleActive ? 1.0f : 0.0f; });

    reg(kCwRightPaddleActionId, kCwRightPaddleActionName, "Phone/CW", P::Gate, 0, 1,
        [this](float v) {
            setCwRightPaddleState(v > 0.5f, QStringLiteral("midi:cwdah"),
                                  m_currentMidiTrace.traceId,
                                  m_currentMidiTrace.callbackMs);
        },
        [this]() -> float { return m_cwRightPaddleActive ? 1.0f : 0.0f; });

    reg("cw.ptt", "PTT (hold)", "Phone/CW", P::Gate, 0, 1,
        [this](float v) {
            const bool on = v > 0.5f;
            if (lcCw().isDebugEnabled()) {
                const quint64 now = cwTraceNowMs();
                qCDebug(lcCw).noquote().nospace()
                    << "CW MIDI ptt trace=" << m_currentMidiTrace.traceId
                    << " t=" << now << "ms"
                    << " sinceSourceMs=" << (m_currentMidiTrace.callbackMs
                        ? static_cast<qint64>(now - m_currentMidiTrace.callbackMs) : -1)
                    << " mox=" << on;
            }
            m_radioModel.setTransmit(on);
        });

    // ── EQ ──────────────────────────────────────────────────────────────
    reg("eq.txEnable", "TX EQ Enable", "EQ", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.equalizerModel().setTxEnabled(v > 0.5f); },
        [this]() -> float { return m_radioModel.equalizerModel().txEnabled() ? 1 : 0; });

    reg("eq.rxEnable", "RX EQ Enable", "EQ", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.equalizerModel().setRxEnabled(v > 0.5f); },
        [this]() -> float { return m_radioModel.equalizerModel().rxEnabled() ? 1 : 0; });

    {
        using B = EqualizerModel::Band;
        static const B bands[] = {B::B63, B::B125, B::B250, B::B500, B::B1k, B::B2k, B::B4k, B::B8k};
        static const int freqs[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
        static const char* names[] = {"63 Hz", "125 Hz", "250 Hz", "500 Hz",
                                       "1 kHz", "2 kHz", "4 kHz", "8 kHz"};
        for (int i = 0; i < 8; ++i) {
            B band = bands[i];
            QString id = QString("eq.band%1").arg(freqs[i]);
            reg(id.toUtf8().constData(), names[i], "EQ", P::Slider, -10, 10,
                [this, band](float v) { m_radioModel.equalizerModel().setTxBand(band, static_cast<int>(v)); },
                [this, band]() -> float { return m_radioModel.equalizerModel().txBand(band); });
        }
    }

    // ── Global ──────────────────────────────────────────────────────────
    reg("global.masterVolume", "Master Volume", "Global", P::Slider, 0, 100,
        [this](float v) { m_radioModel.sendCommand(QString("mixer lineout gain %1").arg(static_cast<int>(v))); },
        [this]() -> float { return m_radioModel.lineoutGain(); });

    reg("global.hpVolume", "Headphone Volume", "Global", P::Slider, 0, 100,
        [this](float v) { m_radioModel.sendCommand(QString("mixer headphone gain %1").arg(static_cast<int>(v))); },
        [this]() -> float { return m_radioModel.headphoneGain(); });

    reg("global.masterMute", "Master Mute", "Global", P::Toggle, 0, 1,
        [this](float v) { m_audio->setMuted(v > 0.5f); },
        [this]() -> float { return m_audio->isMuted() ? 1 : 0; });

    reg("global.txButton", "TX Button", "Global", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.setTransmit(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().isTransmitting() ? 1 : 0; });

    reg("global.tnfEnable", "TNF Global", "Global", P::Toggle, 0, 1,
        [this](float v) { m_radioModel.sendCommand(QString("radio set tnf_enabled=%1").arg(v > 0.5f ? 1 : 0)); });

    // Helper — reuse keyboard-shortcut handlers so MIDI bindings don't
    // duplicate any logic.  Each MIDI Trigger/Toggle that mirrors a
    // shortcut just looks up the action by id and fires its handler.
    auto fireShortcut = [this](const char* shortcutId) {
        if (auto* a = m_shortcutManager.action(shortcutId)) {
            if (a->handler) a->handler();
        }
    };

    // ── Mode triggers (mirror Mode/* keyboard shortcuts) ───────────────
    static const char* kModes[] = {"USB", "LSB", "CW", "CWL",
                                    "AM", "SAM", "FM", "NFM",
                                    "DFM", "DIGU", "DIGL", "RTTY"};
    for (const char* m : kModes) {
        const QString idShort = QString("mode_%1").arg(QString(m).toLower());
        const QString idMidi  = QString("global.mode%1").arg(m);
        const QString name    = QString("Mode %1").arg(m);
        reg(idMidi.toUtf8().constData(),
            name.toUtf8().constData(),
            "Mode", P::Trigger, 0, 1,
            [fireShortcut, idShort](float) {
                fireShortcut(idShort.toUtf8().constData());
            });
    }

    // ── Band triggers (mirror Band/* keyboard shortcuts) ───────────────
    struct MidiBand { const char* idMidi; const char* idShort; const char* label; };
    static const MidiBand kMidiBands[] = {
        {"global.band160m","band_160m","Band 160m"},
        {"global.band80m", "band_80m", "Band 80m"},
        {"global.band60m", "band_60m", "Band 60m"},
        {"global.band40m", "band_40m", "Band 40m"},
        {"global.band30m", "band_30m", "Band 30m"},
        {"global.band20m", "band_20m", "Band 20m"},
        {"global.band17m", "band_17m", "Band 17m"},
        {"global.band15m", "band_15m", "Band 15m"},
        {"global.band12m", "band_12m", "Band 12m"},
        {"global.band10m", "band_10m", "Band 10m"},
        {"global.band6m",  "band_6m",  "Band 6m"},
        {"global.band2m",  "band_2m",  "Band 2m"},
    };
    for (const auto& b : kMidiBands) {
        reg(b.idMidi, b.label, "Band", P::Trigger, 0, 1,
            [fireShortcut, idShort = QString(b.idShort)](float) {
                fireShortcut(idShort.toUtf8().constData());
            });
    }

    // ── Band Up / Down (cycle through the band list above) ────────────
    // Replaces the earlier placeholders that only logged a debug line.
    static constexpr int kBandCount =
        static_cast<int>(sizeof(kMidiBands) / sizeof(MidiBand));
    auto cycleBand = [this, fireShortcut](int direction) {
        // Find the current band by matching the active slice's frequency
        // against canonical band centres.  If no slice or no match,
        // start at index 0 / -1 so the first cycle still does something.
        static const double freqs[kBandCount] = {
            1.900, 3.800, 5.357, 7.200, 10.125, 14.225,
            18.118, 21.300, 24.940, 28.400, 50.125, 146.000
        };
        int currentIdx = -1;
        if (auto* s = activeSlice()) {
            const double fMhz = s->frequency();
            double bestDelta = 1e9;
            for (int i = 0; i < kBandCount; ++i) {
                const double d = std::abs(fMhz - freqs[i]);
                if (d < bestDelta) { bestDelta = d; currentIdx = i; }
            }
        }
        const int next = ((currentIdx < 0 ? 0 : currentIdx) + direction
                          + kBandCount) % kBandCount;
        fireShortcut(kMidiBands[next].idShort);
    };
    reg("global.bandUp", "Band Up", "Global", P::Trigger, 0, 1,
        [cycleBand](float) { cycleBand(+1); });
    reg("global.bandDown", "Band Down", "Global", P::Trigger, 0, 1,
        [cycleBand](float) { cycleBand(-1); });

    // ── Mode Up / Down (cycle through the mode list above) ───────────
    static constexpr int kModeCount =
        static_cast<int>(sizeof(kModes) / sizeof(const char*));
    auto cycleMode = [this, fireShortcut](int direction) {
        // Find current mode index from the active slice; if no match, start at 0.
        int currentIdx = 0;
        if (auto* s = activeSlice()) {
            const QString curMode = s->mode().toUpper();
            for (int i = 0; i < kModeCount; ++i) {
                if (curMode == QLatin1String(kModes[i])) { currentIdx = i; break; }
            }
        }
        const int next = (currentIdx + direction + kModeCount) % kModeCount;
        const QString idShort = QString("mode_%1").arg(QString(kModes[next]).toLower());
        fireShortcut(idShort.toUtf8().constData());
    };
    reg("global.modeUp", "Mode Up", "Global", P::Trigger, 0, 1,
        [cycleMode](float) { cycleMode(+1); });
    reg("global.modeDown", "Mode Down", "Global", P::Trigger, 0, 1,
        [cycleMode](float) { cycleMode(-1); });

    // ── Slice / display / filter / DSP triggers (mirror keyboard) ──────
    reg("global.splitToggle", "Split Toggle", "Slice", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("split_toggle"); });
    reg("global.filterWiden", "Filter Widen", "Filter", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("filter_widen"); });
    reg("global.filterNarrow", "Filter Narrow", "Filter", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("filter_narrow"); });
    reg("global.tuneUp1mhz", "Tune Up 1 MHz", "Frequency", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("tune_up_1mhz"); });
    reg("global.tuneDown1mhz", "Tune Down 1 MHz", "Frequency", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("tune_down_1mhz"); });
    reg("global.bandZoom", "Band Zoom", "Display", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("band_zoom"); });
    reg("global.segmentZoom", "Segment Zoom", "Display", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("segment_zoom"); });
    reg("global.panZoomIn", "Panadapter Zoom In", "Display", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("pan_zoom_in"); });
    reg("global.panZoomOut", "Panadapter Zoom Out", "Display", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("pan_zoom_out"); });
    reg("global.openMemories", "Open Memories", "Display", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("open_memories"); });
    reg("global.nrCycle", "NR Cycle", "RX", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("nr_cycle"); });
    reg("global.agcCycle", "AGC Cycle", "RX", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("agc_cycle"); });
    reg("global.twoToneTune", "Two-Tone Tune", "TX", P::Trigger, 0, 1,
        [fireShortcut](float) { fireShortcut("two_tone_tune"); });

    reg("global.nextSlice", "Next Slice", "Global", P::Trigger, 0, 1,
        [this](float) {
            const auto& slices = m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
                }
                setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
            }
        });

    reg("global.prevSlice", "Previous Slice", "Global", P::Trigger, 0, 1,
        [this](float) {
            const auto& slices = m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
                }
                int prev = (idx - 1 + slices.size()) % slices.size();
                setActiveSlice(slices[prev]->sliceId());
            }
        });

    // ── QSO Recorder ────────────────────────────────────────────────────
    // Mirror the exact dual routing used by the VFO ⏺/▶ buttons
    // (MainWindow.cpp:11413-11443): RecordingMode=="Client" → QsoRecorder,
    // otherwise → SliceModel::setRecordOn / setPlayOn (radio-side).
    reg("global.qsoRecord", "QSO Record", "Global", P::Toggle, 0, 1,
        [this](float v) {
            const bool on = v > 0.5f;
            const bool clientSide =
                AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
            if (clientSide) {
                if (on) m_qsoRecorder->startRecording();
                else    m_qsoRecorder->stopRecording();
            } else if (auto* s = activeSlice()) {
                s->setRecordOn(on);
            }
        },
        [this]() -> float {
            const bool clientSide =
                AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
            if (clientSide)
                return (m_qsoRecorder && m_qsoRecorder->isRecording()) ? 1.0f : 0.0f;
            auto* s = activeSlice();
            return (s && s->recordOn()) ? 1.0f : 0.0f;
        });

    reg("global.qsoPlay", "QSO Playback", "Global", P::Toggle, 0, 1,
        [this](float v) {
            const bool on = v > 0.5f;
            const bool clientSide =
                AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
            if (clientSide) {
                if (on) m_qsoRecorder->startPlayback();
                else    m_qsoRecorder->stopPlayback();
            } else if (auto* s = activeSlice()) {
                s->setPlayOn(on);
            }
        },
        [this]() -> float {
            const bool clientSide =
                AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
            if (clientSide)
                return (m_qsoRecorder && m_qsoRecorder->isPlaying()) ? 1.0f : 0.0f;
            auto* s = activeSlice();
            return (s && s->playOn()) ? 1.0f : 0.0f;
        });
}
#endif

// ─── One-shot controller wiring (#3351 Phase 2a) ────────────────────────────
//
// Extracted verbatim from the constructor: the external-controllers worker
// thread (#502), FlexControl / MIDI / HID manager construction and signal
// routing, and the RC-28 deferred press/hold logic (#3323).

void MainWindow::wireExternalControllers()
{
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
}

} // namespace AetherSDR
