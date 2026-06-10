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
#include "PanadapterStack.h"
#include "RC28MappingDialog.h"
#include "RadioSetupDialog.h"
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

} // namespace AetherSDR
