#include "TestSettingsProfile.h"
#include "core/backends/TransmitDelta.h"
#include "core/TxKeyingMarker.h"
#include "core/aprs/AprsMessenger.h"
#include "gui/AtuPreTuneDialog.h"
#include "gui/HGauge.h"
#include "gui/MidiTxDispatch.h"
#include "gui/ModemReceiveAction.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QHelpEvent>
#include <QMenu>
#include <QToolTip>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {

class TxAppletPowerReconciliationTestAccess {
public:
    static TxController::Input prepareSweep(AtuPreTuneDialog& dialog,
                                            const std::shared_ptr<TxController>& controller)
    {
        dialog.m_programController = controller;
        dialog.m_programInput = controller->captureProgram(TxController::Activity::Atu);
        dialog.m_sweepActive = true;
        dialog.m_currentIndex = 0;
        dialog.m_points = {{QStringLiteral("20m"), 14.100, 1, 1, 14.0, 14.35}};
        dialog.m_tuneBtn->setEnabled(true);
        dialog.requestTuneNow();
        return dialog.m_programInput;
    }
    static void settle(AtuPreTuneDialog& dialog)
    {
        dialog.m_settleTimer->stop();
        QMetaObject::invokeMethod(dialog.m_settleTimer, "timeout", Qt::DirectConnection);
    }
    static void cancel(AtuPreTuneDialog& dialog) { dialog.cancelProgram(); }
    static QPushButton* tune(AtuPreTuneDialog& dialog) { return dialog.m_tuneBtn; }
};

} // namespace AetherSDR

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

QSlider* powerSlider(TxApplet& applet, const QString& accessibleName)
{
    const QList<QSlider*> sliders = applet.findChildren<QSlider*>();
    for (QSlider* slider : sliders) {
        if (slider->accessibleName() == accessibleName) {
            return slider;
        }
    }
    return nullptr;
}

QWidget* namedWidget(TxApplet& applet, const QString& accessibleName)
{
    const QList<QWidget*> widgets = applet.findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        if (widget->accessibleName() == accessibleName)
            return widget;
    }
    return nullptr;
}

QLabel* labelWithText(TxApplet& applet, const QString& text)
{
    const QList<QLabel*> labels = applet.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == text) {
            return label;
        }
    }
    return nullptr;
}

void testReleaseReconcilesAuthoritativePower(const QString& accessibleName,
                                             bool rfPower)
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    QSlider* slider = powerSlider(applet, accessibleName);
    const QByteArray sliderName = accessibleName.toUtf8();
    report(qPrintable(sliderName + " slider exists"), slider != nullptr);
    if (!slider) {
        return;
    }

    QSignalSpy commandSpy(&model, &TransmitModel::commandReady);

    slider->setSliderDown(true);
    slider->setValue(66);
    report(qPrintable(sliderName + " drag updates model"),
           rfPower ? model.rfPower() == 66 : model.tunePower() == 66);

    TransmitDelta clamp;
    if (rfPower) {
        clamp.rfPower = 40;
    } else {
        clamp.tunePower = 40;
    }
    model.applyChanges(clamp);

    report(qPrintable(sliderName + " defers clamp during drag"),
           slider->value() == 66,
           QString::number(slider->value()));
    commandSpy.clear();

    slider->setSliderDown(false);

    report(qPrintable(sliderName + " reconciles on release"),
           slider->value() == 40,
           QString::number(slider->value()));
    report(qPrintable(sliderName + " release sends no command"),
           commandSpy.isEmpty(),
           QString::number(commandSpy.count()));
}

void testTxMetersAreLiveOnly()
{
    TxApplet applet;
    QWidget* power = namedWidget(applet, QStringLiteral("Forward power gauge"));
    QWidget* swr = namedWidget(applet, QStringLiteral("SWR gauge"));
    report("TX gauges exist", power && swr);
    if (!power || !swr)
        return;

    applet.updateMeters(37.0f, 1.7f, true);
    auto* powerGauge = static_cast<HGauge*>(power);
    auto* swrGauge = static_cast<HGauge*>(swr);
    report("startup ignores a stale RF power sample", powerGauge->value() == 0.0f);

    applet.setTransmitting(true);
    applet.updateMeters(37.0f, 1.7f, true);
    report("active TX displays RF power",
           powerGauge->value() == 37.0f);
    report("active TX displays SWR",
           swrGauge->value() == 1.7f);

    applet.setTransmitting(false);
    report("un-key clears RF power immediately",
           powerGauge->value() == 0.0f);
    report("un-key parks SWR immediately",
           swrGauge->value() == 1.0f);

    applet.updateMeters(52.0f, 2.1f, true); // reply already in flight at un-key
    applet.updatePeakPower(60.0f);
    report("late meter replies cannot repaint idle RF power",
           powerGauge->value() == 0.0f);
}

void testCapabilityPowerScaleHonoursBandCeiling()
{
    RadioModel radio;
    TxApplet applet;
    applet.setRadioModel(&radio);
    auto* powerGauge = static_cast<HGauge*>(
        namedWidget(applet, QStringLiteral("Forward power gauge")));
    report("forward-power gauge exists for scale test", powerGauge != nullptr);
    if (!powerGauge) {
        return;
    }

    applet.setPowerScale(75, false);
    report("unverified lower-power radio preserves established face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);

    RadioCapabilities caps;
    caps.txPowerBands.append(TxPowerBand{430'000'000.0, 450'000'000.0, 75.0});
    emit radio.capabilitiesChanged(true, caps);
    applet.setPowerScale(75, false);
    report("75 W capability sets 90 W face",
           powerGauge->property("gaugeMax").toFloat() == 90.0f);
    report("75 W capability sets red threshold at rating",
           powerGauge->property("gaugeRedStart").toFloat() == 75.0f);

    applet.setPowerScale(10, false);
    report("10 W capability sets 12 W face",
           powerGauge->property("gaugeMax").toFloat() == 12.0f);
    report("10 W capability sets red threshold at rating",
           powerGauge->property("gaugeRedStart").toFloat() == 10.0f);

    applet.setPowerScale(100, false);
    report("100 W capability preserves established Flex face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);

    applet.setPowerScale(0, false);
    report("unknown power ceiling preserves safe established face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);
}

void testForwardPowerResponseCapabilityIsConsumed()
{
    report("default capability preserves established power smoothing",
           !RadioCapabilities{}.forwardPowerRequiresSmoothing
               && RadioCapabilities{}.txPowerBands.isEmpty());

    RadioModel radio;
    TxApplet applet;
    applet.setRadioModel(&radio);
    auto* powerGauge = static_cast<HGauge*>(
        namedWidget(applet, QStringLiteral("Forward power gauge")));
    report("forward-power gauge exists for response test", powerGauge != nullptr);
    if (!powerGauge) {
        return;
    }

    RadioCapabilities caps;
    caps.forwardPowerRequiresSmoothing = false;
    emit radio.capabilitiesChanged(true, caps);
    applet.setTransmitting(true);
    applet.updateMeters(60.0f, 1.0f, true);
    report("backend response capability snaps the displayed power sample",
           std::fabs(powerGauge->filledFraction() - 0.5f) < 0.001f);
}

void testAtuSuccessTogglesToBypass()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);
    auto* atu = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU tune")));
    report("ATU button exists", atu != nullptr);
    if (!atu)
        return;

    QSignalSpy commandSpy(&model, &TransmitModel::atuCommandIssued);
    TransmitDelta matched;
    matched.transmitFreq = 14.100;
    matched.atuEnabled = true;
    matched.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
    model.applyChanges(matched);
    atu->click();
    report("successful same-frequency ATU click requests bypass",
           !commandSpy.isEmpty()
               && !commandSpy.takeLast().at(0).toBool());

    TransmitDelta bypassed;
    bypassed.atuEnabled = false;
    bypassed.atuStatusRaw = QStringLiteral("TUNE_BYPASS");
    model.applyChanges(bypassed);
    commandSpy.clear();
    atu->click();
    report("bypassed ATU click starts a fresh tune",
           !commandSpy.isEmpty()
               && commandSpy.takeLast().at(0).toBool());
}

// Inject the production backend seam, not a firmware peer. No sockets or RF.
class TxActionBackend final : public IRadioBackend {
public:
    QStringList commands;
    bool connected{false};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.canTransmit = true;
        caps.hasTuner = true;
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool on, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "mox:on" : "mox:off"); }
    void setAtu(bool on, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "atu:on" : "atu:off"); }
    void setTune(bool on, int, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "tune:on" : "tune:off"); }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void testMidiProducerDispatch()
{
    RadioModel radio;
    auto backend = std::make_unique<TxActionBackend>();
    TxActionBackend* recorder = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
    const bool sliceReady = radio.automationApplySliceFixture(0, QStringLiteral("A"));
    report("MIDI dispatch disconnected slice fixture", sliceReady);
    if (!sliceReady) {
        return;
    }
    SliceDelta slice;
    slice.txSlice = true;
    slice.mode = QStringLiteral("USB");
    slice.panId = QStringLiteral("0x40000000");
    radio.slice(0)->applyChanges(slice);
    recorder->connected = true;
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    QObject device;
    const auto controller = TxController::forNativeDevice(&radio, &device);
    const TxCoordinator::Request raw = controller->captureRawInput();
    const auto captured = TxController::captureInputScope(controller, raw);
    controller->discardDeviceInputs();
    int cwCalls = 0;
    const auto cw = [&cwCalls](const QString&, bool, const std::shared_ptr<TxController>&) { ++cwCalls; };
    const QStringList keyingIds{"tx.mox", "global.txButton", "cw.ptt", "tx.tune",
                               "global.twoToneTune", "tx.atuStart", "cwkey", "cwdit", "cwdah"};
    for (const QString& id : keyingIds) {
        const bool revokedConsumed = dispatchMidiTxInput(id, 1.0f, radio, captured, cw);
        const bool missingConsumed = dispatchMidiTxInput(id, 1.0f, radio, {}, cw);
        report("expired MIDI TX input cannot fall through to native setter",
               revokedConsumed && missingConsumed && recorder->commands.isEmpty() && cwCalls == 0, id);
    }
    report("non-TX MIDI action retains generic dispatch",
           !dispatchMidiTxInput("rx.afGain", 0.5f, radio, {}, cw));

    const auto fresh = TxController::captureInputScope(controller, controller->captureRawInput());
    report("TX Button alias uses original device producer",
           dispatchMidiTxInput("global.txButton", -1.0f, radio, fresh, cw)
               && fresh->current(TxController::Activity::Mox).active()
               && radio.transmitModel().isTransmitting());
    const auto native = std::make_shared<TxController>(&radio);
    const auto nativeHold = native->capture(TxController::Activity::Mox);
    report("independent contributor can join device MOX", nativeHold.start());
    recorder->commands.clear();
    controller->discardDeviceInputs();
    controller->cleanupDeviceInputs();
    report("device cleanup preserves independent MOX contributor",
           recorder->commands.isEmpty() && nativeHold.active() && radio.transmitModel().isTransmitting());
    nativeHold.stop();
    report("remaining contributor can end its own MOX",
           recorder->commands == QStringList{"mox:off"} && !radio.transmitModel().isTransmitting());

    const auto tuneController = TxController::captureInputScope(controller, controller->captureRawInput());
    QSignalSpy modeCommands(&radio.transmitModel(), &TransmitModel::commandReady);
    recorder->commands.clear();
    dispatchMidiTxInput("global.twoToneTune", 1.0f, radio, tuneController, cw);
    report("first MIDI two-tone press starts its own tune",
           recorder->commands == QStringList{"tune:on"} && radio.transmitModel().isTuning()
               && modeCommands.count() == 1
               && modeCommands.at(0).at(0).toString() == "transmit set tune_mode=two_tone");
    const auto stranger = std::make_shared<TxController>(&radio);
    recorder->commands.clear();
    modeCommands.clear();
    dispatchMidiTxInput("global.twoToneTune", 1.0f, radio, stranger, cw);
    report("another contributor cannot stop or change this two-tone tune",
           recorder->commands.isEmpty() && modeCommands.isEmpty() && radio.transmitModel().isTuning());
    dispatchMidiTxInput("global.twoToneTune", 1.0f, radio, tuneController, cw);
    report("second MIDI two-tone press stops and restores single tone",
           recorder->commands == QStringList{"tune:off"} && !radio.transmitModel().isTuning()
               && modeCommands.count() == 1
               && modeCommands.at(0).at(0).toString() == "transmit set tune_mode=single_tone");
}

void testScopedControllerActions()
{
    RadioModel radio;
    auto backend = std::make_unique<TxActionBackend>();
    TxActionBackend* recorder = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
    const bool sliceReady = radio.automationApplySliceFixture(0, QStringLiteral("A"));
    report("scoped control disconnected slice fixture", sliceReady);
    if (!sliceReady) {
        return;
    }
    SliceDelta slice;
    slice.txSlice = true;
    slice.mode = QStringLiteral("USB");
    slice.panId = QStringLiteral("0x40000000");
    radio.slice(0)->applyChanges(slice);
    recorder->connected = true;
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });

    TxApplet applet;
    applet.setRadioModel(&radio);
    applet.setTransmitModel(&radio.transmitModel());
    QWidget* mox = namedWidget(applet, QStringLiteral("MOX transmit"));
    QWidget* atu = namedWidget(applet, QStringLiteral("ATU tune"));
    report("scoped applet controls exist", mox && atu);
    if (!mox || !atu) {
        return;
    }
    const auto bridge = std::make_shared<TxController>(&radio);
    TxKeyingAction::Prepared action = prepareTxKeyingAction(mox, bridge, "setChecked", "true");
    report("MOX supplies a prepared producer action", bool(action));
    if (!action) {
        return;
    }
    recorder->commands.clear();
    bridge->invalidate();
    action();
    report("revoked prepared UI action never keys", recorder->commands.isEmpty());

    const auto controller = std::make_shared<TxController>(&radio);
    action = prepareTxKeyingAction(atu, controller, "click", {});
    report("ATU supplies a prepared producer action", bool(action));
    if (!action) {
        return;
    }
    action();
    report("prepared ATU action starts actual model request", recorder->commands.contains("atu:on"));
    TransmitDelta matched;
    matched.transmitFreq = 14.100;
    matched.atuEnabled = true;
    matched.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
    radio.transmitModel().applyChanges(matched);
    recorder->commands.clear();
    action = prepareTxKeyingAction(atu, controller, "click", {});
    if (action) {
        action();
    }
    report("scoped ATU preserves same-frequency bypass logic", recorder->commands == QStringList{"atu:off"});
    TransmitDelta moved;
    moved.transmitFreq = 14.200;
    radio.transmitModel().applyChanges(moved);
    recorder->commands.clear();
    action = prepareTxKeyingAction(atu, controller, "click", {});
    if (action) {
        action();
    }
    report("scoped ATU retunes after a frequency change", recorder->commands == QStringList{"atu:on"});
    controller->invalidate();

    const auto operatorInput = std::make_shared<TxController>(&radio);
    (void)operatorInput->capture(TxController::Activity::Mox).start();
    const auto observer = std::make_shared<TxController>(&radio);
    action = prepareTxKeyingAction(mox, observer, "setChecked", "false");
    recorder->commands.clear();
    if (action) {
        action();
    }
    report("UI release cannot consume another controller's MOX", recorder->commands.isEmpty()
           && radio.transmitModel().isTransmitting());
    RadioModel other;
    const auto foreign = std::make_shared<TxController>(&other);
    report("scoped action rejects a different radio model",
           !prepareTxKeyingAction(mox, foreign, "click", {}));
}

void testScopedPointerActivation()
{
    RadioModel radio;
    auto backend = std::make_unique<TxActionBackend>();
    TxActionBackend* recorder = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
    report("pointer test slice fixture", radio.automationApplySliceFixture(0, QStringLiteral("A")));
    if (!radio.slice(0)) { return; }
    SliceDelta delta;
    delta.txSlice = true;
    delta.mode = QStringLiteral("USB");
    radio.slice(0)->applyChanges(delta);
    recorder->connected = true;
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    const auto controller = std::make_shared<TxController>(&radio);
    QPushButton button(QStringLiteral("Transmit"));
    button.resize(160, 30);
    int preparations = 0;
    registerTxKeyingAction(&button, [&](const std::shared_ptr<TxController>& source,
        const QString&, const QString&) -> TxKeyingAction::Prepared {
        ++preparations;
        const auto input = source->capture(TxController::Activity::Mox);
        return [&, input] {
            if (radio.transmitModel().isTransmitting()) { input.stop(); }
            else { (void)input.start(); }
        };
    });
    const QPoint inside = button.mapToGlobal(button.rect().center());
    const QPoint outside = button.mapToGlobal(QPoint(-20, -20));
    auto pointer = TxPointerAction::prepare(&button, controller);
    report("pointer action prepares", bool(pointer));
    if (!pointer) { return; }
    pointer->press(inside);
    report("TX pointer press changes down state but never keys", button.isDown() && recorder->commands.isEmpty());
    pointer->cancel();
    report("TX gesture cancel never activates a button", !button.isDown() && recorder->commands.isEmpty());
    pointer = TxPointerAction::prepare(&button, controller);
    pointer->press(inside);
    pointer->release(outside);
    report("release outside does not key", recorder->commands.isEmpty());
    pointer = TxPointerAction::prepare(&button, controller);
    pointer->press(inside);
    pointer->release(inside);
    report("release inside invokes original controller action", recorder->commands.contains("mox:on"));
    const auto compound = pointer->controller();
    pointer.reset();
    report("released pointer destruction preserves its hold", radio.transmitModel().isTransmitting());
    pointer = TxPointerAction::prepare(&button, compound);
    pointer->press(inside);
    pointer->release(inside);
    report("second click reads updated toggle state under same input", recorder->commands.last() == "mox:off");
    radio.cancelLocalTransmit();
    report("cancelled compound gesture cannot prepare another activation", !TxPointerAction::prepare(&button, compound));
    report("every activation captured at preparation", preparations == 4);
}

void testOptionalReceiveControlActivation()
{
    QPushButton transmit(QStringLiteral("Transmit"));
    int txPreparations = 0;
    registerTxKeyingAction(&transmit, [&](const std::shared_ptr<TxController>&,
        const QString&, const QString&) -> TxKeyingAction::Prepared {
        ++txPreparations;
        return [] {};
    });
    report("ordinary TX invoke and pointer reject absent authority",
           txActionRequiresPermission(&transmit)
               && !prepareTxKeyingAction(&transmit, {}, "click", {})
               && !TxPointerAction::prepare(&transmit, {}) && txPreparations == 0);

    QCheckBox receive(QStringLiteral("Enable receive"));
    receive.resize(receive.sizeHint());
    QSignalSpy nativeClicked(&receive, &QCheckBox::clicked);
    QSignalSpy nativeToggled(&receive, &QCheckBox::toggled);
    int rxActions = 0;
    int nullPreparations = 0;
    registerReceiveControlAction(&receive, [&](const std::shared_ptr<TxController>& source,
        const QString&, const QString&) -> TxKeyingAction::Prepared {
        if (!source) { ++nullPreparations; }
        return [&] {
            ++rxActions;
            const QSignalBlocker blocker(&receive);
            receive.setChecked(!receive.isChecked());
        };
    });
    TxKeyingAction::Prepared action = prepareTxKeyingAction(&receive, {}, "click", {});
    report("explicit receive endpoint accepts absent TX authority",
           !txActionRequiresPermission(&receive) && bool(action));
    if (!action) { return; }
    action();
    report("receive invoke never calls native clicked or toggled slots",
           rxActions == 1 && receive.isChecked() && nativeClicked.isEmpty() && nativeToggled.isEmpty());

    const QPoint inside = receive.mapToGlobal(receive.rect().center());
    const QPoint outside = receive.mapToGlobal(QPoint(-20, -20));
    auto pointer = TxPointerAction::prepare(&receive, {});
    report("receive pointer accepts absent TX authority", bool(pointer) && !pointer->controller());
    if (!pointer) { return; }
    pointer->press(inside);
    report("receive pointer press only changes down state", receive.isDown() && rxActions == 1);
    pointer->release(inside);
    report("receive pointer invokes scoped callback without native signals",
           rxActions == 2 && !receive.isChecked() && nativeClicked.isEmpty() && nativeToggled.isEmpty());
    pointer = TxPointerAction::prepare(&receive, {});
    pointer->press(inside);
    pointer->release(outside);
    pointer = TxPointerAction::prepare(&receive, {});
    pointer->press(inside);
    pointer->cancel();
    report("receive outside release and cancellation cannot activate",
           rxActions == 2 && !receive.isDown() && nativeClicked.isEmpty() && nativeToggled.isEmpty());

    RadioModel radio;
    const auto controller = std::make_shared<TxController>(&radio);
    action = prepareTxKeyingAction(&receive, controller, "click", {});
    pointer = TxPointerAction::prepare(&receive, controller);
    report("receive control can retain supplied valid authority", bool(action) && bool(pointer));
    if (!action || !pointer) { return; }
    pointer->press(inside);
    const int previousNullPreparations = nullPreparations;
    controller->invalidate();
    action();
    pointer->release(inside);
    report("supplied revoked authority is never downgraded to receive-only",
           rxActions == 2 && nullPreparations == previousNullPreparations
               && !prepareTxKeyingAction(&receive, controller, "click", {})
               && !TxPointerAction::prepare(&receive, controller));
}

void testModemReceiveProgramAuthority()
{
    for (const QString& label : {QStringLiteral("Enable Modem"), QStringLiteral("TNC Enable Modem")}) {
        RadioModel radio;
        AprsMessenger messenger;
        messenger.setMyAddress(*ax25::Address::parse(QStringLiteral("N0CALL-9")));
        QVector<TxCoordinator::Request> ackInputs;
        QObject::connect(&messenger, &AprsMessenger::transmitFrame,
            [&](const QByteArray&, const TxCoordinator::Request& input) { ackInputs.append(input); });
        const auto receiveMessage = [&](int number) {
            const ax25::Frame frame = ax25::Frame::makeUI(
                *ax25::Address::parse(QStringLiteral("APRS")),
                *ax25::Address::parse(QStringLiteral("W1AW")), {},
                QStringLiteral(":N0CALL-9 :QSL?{%1").arg(number).toLatin1());
            const auto packet = aprs::parseFrame(frame);
            if (packet) { messenger.onPacket(*packet); }
        };
        QCheckBox box(label);
        QSignalSpy nativeClicked(&box, &QCheckBox::clicked);
        QSignalSpy nativeToggled(&box, &QCheckBox::toggled);
        int applied = 0;
        registerModemReceiveAction(&box, [&](bool enabled,
            const std::shared_ptr<TxController>& source, const TxController::Input& input) {
            ++applied;
            const QSignalBlocker blocker(&box);
            box.setChecked(enabled);
            messenger.setReceiveProgram(enabled && source && input.valid()
                ? input.request() : TxCoordinator::Request{});
        });
        TxKeyingAction::Prepared action = prepareTxKeyingAction(&box, {}, "setChecked", "true");
        report("modem RX-only action prepares without TX authority", bool(action), label);
        if (!action) { continue; }
        action();
        receiveMessage(101);
        report("RX-only modem message cannot acquire ACK authority",
               applied == 1 && box.isChecked() && ackInputs.size() == 1
                   && !ackInputs.last().valid() && nativeClicked.isEmpty() && nativeToggled.isEmpty(), label);

        const auto controller = std::make_shared<TxController>(&radio);
        action = prepareTxKeyingAction(&box, controller, "setChecked", "true");
        report("authorized modem receive program prepares", bool(action), label);
        if (!action) { continue; }
        action();
        receiveMessage(102);
        report("authorized modem ACK carries its original valid program",
               applied == 2 && ackInputs.size() == 2 && ackInputs.last().valid(), label);
        controller->invalidate();
        receiveMessage(103);
        report("revoked modem receive program cannot authorize a later ACK",
               ackInputs.size() == 3 && !ackInputs.last().valid(), label);

        const auto source = std::make_shared<TxController>(&radio);
        const auto scope = TxController::captureInputScope(source);
        const TxController::Input root = scope->captureProgram(TxController::Activity::Mox);
        action = prepareTxKeyingAction(&box, scope, "setChecked", "false");
        report("modem toggle captures its program before delivery", bool(action) && root.valid(), label);
        root.stop();
        if (action) { action(); }
        report("revoked queued modem program suppresses callback and native slots",
               source->valid() && !root.valid() && applied == 2 && box.isChecked()
                   && nativeClicked.isEmpty() && nativeToggled.isEmpty(), label);
    }
}

void testSweepRetainsOriginalAuthority()
{
    RadioModel radio;
    auto backend = std::make_unique<TxActionBackend>();
    TxActionBackend* recorder = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
    recorder->connected = true;
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    AtuPreTuneDialog dialog(&radio, nullptr);
    const auto controller = std::make_shared<TxController>(&radio);
    const auto stranger = std::make_shared<TxController>(&radio);
    const TxController::Input root =
        TxAppletPowerReconciliationTestAccess::prepareSweep(dialog, controller);
    report("sweep continuation rejects another controller",
           !prepareTxKeyingAction(TxAppletPowerReconciliationTestAccess::tune(dialog),
                                 stranger, "click", {}));
    TxKeyingAction::Prepared continuation = prepareTxKeyingAction(
        TxAppletPowerReconciliationTestAccess::tune(dialog), controller, "click", {});
    report("sweep continuation captures the original program", bool(continuation));
    recorder->commands.clear();
    controller->invalidate();
    TxAppletPowerReconciliationTestAccess::settle(dialog);
    if (continuation) { continuation(); }
    report("revoked sweep cannot key after its settling delay", recorder->commands.isEmpty());

    const auto replacement = std::make_shared<TxController>(&radio);
    const TxController::Input currentRoot =
        TxAppletPowerReconciliationTestAccess::prepareSweep(dialog, replacement);
    TxAppletPowerReconciliationTestAccess::settle(dialog);
    report("fresh sweep starts its own scoped ATU point", recorder->commands == QStringList{"atu:on"});
    report("point admission does not consume the original sequence input", currentRoot.valid());
    recorder->commands.clear();
    root.stop();
    report("old sweep cancellation cannot stop replacement ATU", recorder->commands.isEmpty());
    TxAppletPowerReconciliationTestAccess::cancel(dialog);
    report("sweep abort stops its point and closes future derivation",
           recorder->commands == QStringList{"atu:off"} && !currentRoot.derive().valid());

    const TxController::Input manual = stranger->capture(TxController::Activity::Atu);
    report("manual ATU can start after sweep cleanup", manual.start());
    recorder->commands.clear();
    TxAppletPowerReconciliationTestAccess::cancel(dialog);
    report("repeated sweep cleanup cannot stop unrelated manual ATU", recorder->commands.isEmpty());
    manual.stop();

    const auto delayed = std::make_shared<TxController>(&radio);
    (void)TxAppletPowerReconciliationTestAccess::prepareSweep(dialog, delayed);
    radio.cancelLocalTransmit();
    recorder->commands.clear();
    TxAppletPowerReconciliationTestAccess::settle(dialog);
    report("operator cancellation fences an ATU point not admitted yet", recorder->commands.isEmpty());
}

void testAtuCapabilityUsesThreeVisibleStates()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    auto* atu = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU tune")));
    auto* mem = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU memories")));
    QLabel* success = labelWithText(applet, QStringLiteral("Success"));
    QLabel* bypass = labelWithText(applet, QStringLiteral("Byp"));
    QLabel* memory = labelWithText(applet, QStringLiteral("Mem"));
    report("ATU capability widgets exist",
           atu && mem && success && bypass && memory);
    if (!atu || !mem || !success || !bypass || !memory) {
        return;
    }
    const QString inactiveSuccessStyle = success->styleSheet();
    const QString inactiveBypassStyle = bypass->styleSheet();
    const QString inactiveMemoryStyle = memory->styleSheet();

    report("available inactive ATU controls remain enabled",
           atu->isEnabled() && mem->isEnabled()
               && !atu->isCheckable() && mem->isCheckable()
               && !mem->isChecked());
    report("available inactive indicators are greyed",
           success->isEnabled() && bypass->isEnabled() && memory->isEnabled()
               && !inactiveSuccessStyle.isEmpty()
               && inactiveSuccessStyle == inactiveBypassStyle
               && inactiveSuccessStyle == inactiveMemoryStyle);

    QSignalSpy commandSpy(&model, &TransmitModel::commandReady);
    QSignalSpy atuIntents(&model, &TransmitModel::atuCommandIssued);
    model.setHasTuner(false);
    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("unavailable tuner controls remain visible",
           !atu->isHidden() && !mem->isHidden()
               && !success->isHidden() && !bypass->isHidden() && !memory->isHidden());
    report("unavailable tuner controls are dimmed and inert",
           !atu->isEnabled() && !mem->isEnabled());
    report("unavailable tuner indicators are dimmed",
           !success->isEnabled() && !bypass->isEnabled() && !memory->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && bypass->styleSheet() != inactiveBypassStyle
               && memory->styleSheet() != inactiveMemoryStyle);
    atu->click();
    mem->click();
    report("unavailable tuner controls emit no commands", commandSpy.isEmpty() && atuIntents.isEmpty());
    report("unavailable tuner controls explain the state",
           atu->toolTip()
                   == QStringLiteral("Antenna tuner controls are unavailable for this radio")
               && mem->toolTip()
                   == QStringLiteral("ATU memory controls are unavailable for this radio"));

    model.setHasTuner(true);
    model.setHasTunerMemories(true);
    QApplication::processEvents();
    report("available tuner controls return to inactive state",
           atu->isEnabled() && mem->isEnabled()
               && !mem->isChecked()
               && success->styleSheet() == inactiveSuccessStyle
               && bypass->styleSheet() == inactiveBypassStyle
               && memory->styleSheet() == inactiveMemoryStyle);

    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("Icom-style tuner availability keeps memory surfaces unavailable",
           atu->isEnabled() && !mem->isEnabled()
               && atu->contextMenuPolicy() == Qt::NoContextMenu
               && success->isEnabled() && success->styleSheet() == inactiveSuccessStyle
               && !memory->isEnabled() && memory->styleSheet() != inactiveMemoryStyle);
    mem->click();
    report("unavailable tuner-memory control emits no command", commandSpy.isEmpty());

    model.setHasTunerMemories(true);
    QApplication::processEvents();
    report("tuner-memory capability restores memory-only menu actions",
           atu->contextMenuPolicy() == Qt::CustomContextMenu);

    TransmitDelta active;
    active.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
    active.atuEnabled = true;
    active.memoriesEnabled = true;
    active.usingMemory = true;
    model.applyChanges(active);
    QApplication::processEvents();
    report("available active tuner indicators are enabled",
           success->isEnabled() && memory->isEnabled() && bypass->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && memory->styleSheet() != inactiveMemoryStyle
               && bypass->styleSheet() == inactiveBypassStyle);
    report("available active tuner memory control follows radio readback",
           !atu->isCheckable() && mem->isChecked());
    const QString activeSuccessStyle = success->styleSheet();
    const QString activeMemoryStyle = memory->styleSheet();

    model.setHasTuner(false);
    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("active tuner controls dim when capability disappears",
           mem->isChecked()
               && !atu->isEnabled() && !mem->isEnabled()
               && !success->isEnabled() && !memory->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && memory->styleSheet() != inactiveMemoryStyle
               && success->styleSheet() != activeSuccessStyle
               && memory->styleSheet() != activeMemoryStyle);
}

void testTuneAvailability()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);
    auto* tune = qobject_cast<QPushButton*>(namedWidget(applet, QStringLiteral("Tune")));
    report("Tune button exists", tune != nullptr);
    if (!tune) {
        return;
    }
    QSignalSpy commands(&model, &TransmitModel::commandReady);
    QSignalSpy tuneIntents(&model, &TransmitModel::tuneCommandIssued);
    model.setTuneAvailable(false);
    report("unsupported Tune button is disabled", !tune->isEnabled());
    model.startTune();
    model.startTwoToneTune();
    report("both Tune paths refuse without commands or optimistic state",
           commands.isEmpty() && tuneIntents.isEmpty() && !model.isTuning());
    model.setTuneAvailable(true);
    report("capable mode restores Tune", tune->isEnabled());
    model.startTune();
    model.setTuneAvailable(false);
    report("active Tune retains an enabled stop control", tune->isEnabled() && model.isTuning());
    tune->click();
    report("stop remains usable and restores disabled state", !model.isTuning() && !tune->isEnabled());
}

} // namespace

// #5510 — a disabled menu entry must be able to say WHY it is disabled.
//
// The ATU right-click menu has always set an explanatory tooltip on a disabled
// "Pre-tune bands…", but Qt has suppressed per-action tooltips since 5.1 unless
// the menu opts in, and a disabled QAction does not highlight on hover either.
// The operator therefore saw an inert item with no feedback at all and reported
// it as a broken control. These assertions pin the opt-in and the reason text;
// deleting menu.setToolTipsVisible(true) fails the first one.
void testAtuContextMenuExplainsWhyPreTuneIsDisabled()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    // The reporter's state: the radio HAS an ATU memory database, but memories
    // are switched off, so the sweep is legitimately unavailable.
    model.setHasTunerMemories(true);
    TransmitDelta memoriesOff;
    memoriesOff.memoriesEnabled = false;
    model.applyChanges(memoriesOff);

    QMenu menu;
    applet.buildAtuContextMenu(menu);

    report("ATU menu opts into per-action tooltips",
           menu.toolTipsVisible(),
           menu.toolTipsVisible()
               ? QString()
               : QStringLiteral("QMenu::toolTipsVisible() is false, so every "
                                "disabled-item explanation is unreachable"));

    QAction* preTune = nullptr;
    for (QAction* action : menu.actions()) {
        if (action->text().startsWith(QStringLiteral("Pre-tune"))) {
            preTune = action;
            break;
        }
    }

    report("ATU menu offers a Pre-tune entry", preTune != nullptr);
    if (preTune == nullptr) {
        return;
    }

    report("Pre-tune is disabled while ATU memories are off",
           !preTune->isEnabled());
    report("Disabled Pre-tune carries a reason",
           !preTune->toolTip().isEmpty(),
           preTune->toolTip());

    // Assert the RENDER, not the property. toolTipsVisible() being true only means
    // the menu opted in; what the operator needs is Qt actually painting the text
    // over a DISABLED entry. Qt's QMenu::actionAt() does not filter on enabled
    // state, but that is Qt's behaviour to demonstrate here, not ours to assume.
    menu.popup(QPoint(50, 50));
    QCoreApplication::processEvents();
    const QRect tipRect = menu.actionGeometry(preTune);
    QHelpEvent tipEvent(QEvent::ToolTip, tipRect.center(),
                        menu.mapToGlobal(tipRect.center()));
    QApplication::sendEvent(&menu, &tipEvent);
    QCoreApplication::processEvents();
    const QString shownTip = QToolTip::text();
    report("Disabled Pre-tune actually renders its reason",
           shownTip == preTune->toolTip(),
           shownTip.isEmpty() ? QStringLiteral("<nothing rendered>") : shownTip);
    QToolTip::hideText();
    menu.close();
    QCoreApplication::processEvents();

    // Control: the SAME construction with memories on must enable the entry, so
    // the assertion above cannot pass just because the item is always disabled.
    TransmitDelta memoriesOn;
    memoriesOn.memoriesEnabled = true;
    model.applyChanges(memoriesOn);

    QMenu enabledMenu;
    applet.buildAtuContextMenu(enabledMenu);
    QAction* enabledPreTune = nullptr;
    for (QAction* action : enabledMenu.actions()) {
        if (action->text().startsWith(QStringLiteral("Pre-tune"))) {
            enabledPreTune = action;
            break;
        }
    }
    report("Pre-tune is enabled once ATU memories are on",
           enabledPreTune != nullptr && enabledPreTune->isEnabled());
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-tx-applet-power-reconciliation-test"));
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    qputenv("AETHER_AUTOMATION", "1");

    QApplication app(argc, argv);

    std::printf("TxApplet power reconciliation test harness\n\n");

    testReleaseReconcilesAuthoritativePower(QStringLiteral("RF power"), true);
    testReleaseReconcilesAuthoritativePower(QStringLiteral("Tune power"), false);
    testTxMetersAreLiveOnly();
    testCapabilityPowerScaleHonoursBandCeiling();
    testForwardPowerResponseCapabilityIsConsumed();
    testAtuSuccessTogglesToBypass();
    testMidiProducerDispatch();
    testScopedControllerActions();
    testScopedPointerActivation();
    testOptionalReceiveControlActivation();
    testModemReceiveProgramAuthority();
    testSweepRetainsOriginalAuthority();
    testAtuCapabilityUsesThreeVisibleStates();
    testTuneAvailability();
    testAtuContextMenuExplainsWhyPreTuneIsDisabled();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
