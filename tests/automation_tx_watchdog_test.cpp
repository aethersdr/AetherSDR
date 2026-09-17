// Socket-free watchdog tests: real engine admission, injected terminal recorder.
// No radio, listener, discovery, audio device or RF.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/RadioCertification.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& command)
    {
        return server.handleLine(command, nullptr);
    }
    static void setMaxKeyMs(AutomationServer& server, int ms) { server.m_txMaxKeyMs = ms; }
    static qint64 elapsed(const AutomationServer& server) { return server.m_txKeyClock.elapsed(); }
    static bool claimed(const AutomationServer& server) { return server.m_txBridgeInitiated; }
    static bool owns(const AutomationServer& server) { return server.txBridgeOwnsCurrentTransmit(); }
    static void poll(AutomationServer& server) { server.onTxWatchdog(); }
    static void release(AutomationServer& server) { server.releaseEdgeHandsBackPolicing(); }
    static void observeKey(AutomationServer& server, bool on,
                           const TxCoordinator::Operation&, bool)
    {
        if (on) {
            server.markTxBridgeInitiated();
        } else {
            server.releaseEdgeHandsBackPolicing();
        }
    }
    static std::shared_ptr<TxController> controller(AutomationServer& server)
    {
        return server.txController();
    }
    static void defer(AutomationServer& server, std::function<void(TxController::Input)> action,
                       TxController::Activity activity = TxController::Activity::Mox)
    {
        const TxController::Input input = server.txController()->capture(activity);
        server.deferInvokeAction([input, action = std::move(action)] { action(input); }, true);
    }
};
class RadioCertificationTestAccess {
public:
    static bool key(RadioCertification& cert, bool on) { return cert.keyViaOperatorPath(on); }
};
} // namespace AetherSDR

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
void pump(int ms)
{
    QElapsedTimer clock;
    clock.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    } while (clock.elapsed() < ms);
}

class RecordingBackend final : public IRadioBackend {
public:
    QStringList& commands;
    bool connected{false};
    bool canTransmit{true};
    std::function<void(bool)> keyingWriter;
    std::function<void(bool)> tuneWriter;
    TxCoordinator::Operation lastKeyOn;
    explicit RecordingBackend(QStringList& record) : commands(record) {}
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.canTransmit = canTransmit;
        caps.hasRadioSideCwKeyer = true;
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
    void setKeying(bool on, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion&) override
    {
        if (on) {
            lastKeyOn = operation;
        }
        commands << (on ? "mox:on" : "mox:off");
        if (keyingWriter) {
            keyingWriter(on);
        }
    }
    void setTune(bool on, int, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override
    {
        commands << (on ? "tune:on" : "tune:off");
        if (tuneWriter) {
            tuneWriter(on);
        }
    }
    void setAtu(bool on, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { commands << (on ? "atu:on" : "atu:off"); }
    void setCwKeying(bool on, bool, int, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { commands << (on ? "cw:on" : "cw:off"); }
    void abortCwText(const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { commands << "cwx:abort"; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    QStringList commands;
    RadioModel radio;
    AutomationServer bridge;
    RecordingBackend* backend;
    Fixture()
    {
        auto owned = std::make_unique<RecordingBackend>(commands);
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            qFatal("Could not install disconnected slice fixture");
        }
        SliceDelta delta;
        delta.txSlice = true;
        delta.mode = QStringLiteral("USB");
        delta.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(delta);
        backend->connected = true;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        bridge.setRadioModel(&radio);
        bridge.setTxAllowed(true);
        commands.clear();
    }
    QJsonObject request(const QByteArray& command)
    {
        return AutomationServerTestAccess::request(bridge, command);
    }
};

void manualTransmitIsNotPoliced()
{
    Fixture f;
    f.radio.setTransmit(true);
    AutomationServerTestAccess::setMaxKeyMs(f.bridge, 0);
    f.commands.clear();
    AutomationServerTestAccess::poll(f.bridge);
    f.bridge.setTxAllowed(false);
    check(f.commands.isEmpty() && f.radio.transmitModel().isTransmitting(),
          "enabled watchdog and permission revocation leave operator TX alone");
}

void actionsCannotAdoptExistingOperation()
{
    for (const QByteArray command : {QByteArray("txtest twotone"), QByteArray("atu start")}) {
        Fixture f;
        f.radio.setTransmit(true);
        check(!f.request(command).value("ok").toBool(),
              "carrier request cannot borrow a different producer's PTT latch");
        check(!AutomationServerTestAccess::claimed(f.bridge),
              "typed bridge action cannot adopt an existing operator operation");
        f.commands.clear();
        f.bridge.setTxAllowed(false);
        check(f.commands.isEmpty(), "revocation does not stop an unclaimed operation");
    }
    Fixture f;
    // Keep the engine operation through an RX readback gap, as during QSK.
    f.radio.setTransmit(true);
    f.radio.transmitModel().setTransmitting(false);
    check(f.request("key ptt on").value("ok").toBool(),
          "compatible bridge PTT holds its own contribution during a readback gap");
    f.commands.clear();
    f.bridge.setTxAllowed(false);
    check(f.commands.isEmpty(), "revocation leaves the earlier operator contribution intact");
}

void deadlineDoesNotRenew()
{
    Fixture f;
    f.request("key ptt on");
    check(AutomationServerTestAccess::owns(f.bridge), "accepted bridge key owns its original operation");
    pump(35);
    const qint64 before = AutomationServerTestAccess::elapsed(f.bridge);
    f.request("key ptt on");
    f.request("txtest twotone");
    f.request("atu start");
    check(AutomationServerTestAccess::elapsed(f.bridge) >= before,
          "repeated key, two-tone and ATU cannot renew the monotonic deadline");
    AutomationServerTestAccess::setMaxKeyMs(f.bridge, 1);
    f.commands.clear();
    AutomationServerTestAccess::poll(f.bridge);
    check(f.commands.contains("mox:off") && f.commands.contains("tune:off")
              && f.commands.contains("atu:off") && !f.commands.contains("cwx:abort"),
          "expired original operation receives watchdog cleanup");
}

void productionTimerExpiresOriginalOperation()
{
    for (bool reenable : {false, true}) {
        Fixture f;
        if (reenable) {
            f.bridge.setTxAllowed(false);
            f.bridge.setTxAllowed(true);
        }
        AutomationServerTestAccess::setMaxKeyMs(f.bridge, 10);
        f.request("key ptt on");
        f.commands.clear();
        QElapsedTimer elapsed;
        elapsed.start();
        // Exercise the production 500 ms timer, not the direct poll seam.
        // A bounded wait makes a missing/disconnected timer fail promptly.
        while (!f.commands.contains("mox:off") && elapsed.elapsed() < 2'000) {
            pump(10);
        }
        check(f.commands.contains("mox:off")
                  && !AutomationServerTestAccess::claimed(f.bridge),
              "production timer expires owned TX after initial enable or re-enable");
    }
}

void staleWatchdogCannotStopReplacement()
{
    for (bool revoke : {false, true}) {
        Fixture f;
        f.request("key ptt on");
        const TxCoordinator::Operation original = f.radio.transmitOperation();
        // Entire handoff happens between polls; old bridge claim is still set.
        f.request("key ptt off");
        f.radio.setTransmit(true);
        check(!original.sameOperation(f.radio.transmitOperation()), "replacement has a distinct operation");
        f.commands.clear();
        AutomationServerTestAccess::setMaxKeyMs(f.bridge, 0);
        if (revoke) {
            f.bridge.setTxAllowed(false);
        } else {
            AutomationServerTestAccess::poll(f.bridge);
        }
        check(f.commands.isEmpty() && f.radio.transmitModel().isTransmitting(),
              "stale watchdog/revocation cannot stop a replacement operator over");
    }
}

void releaseAndReadbackGaps()
{
    Fixture f;
    f.request("key ptt on");
    AutomationServerTestAccess::release(f.bridge);
    check(AutomationServerTestAccess::claimed(f.bridge), "refused release keeps policing");
    f.radio.transmitModel().setTransmitting(false);
    AutomationServerTestAccess::poll(f.bridge);
    check(AutomationServerTestAccess::claimed(f.bridge), "active operation survives readback RX gap");
    f.request("key ptt off");
    AutomationServerTestAccess::release(f.bridge);
    check(!AutomationServerTestAccess::claimed(f.bridge), "completed release hands policing back");
}

void refusedAndNestedRequests()
{
    Fixture f;
    f.backend->canTransmit = false;
    f.request("key ptt on");
    check(!AutomationServerTestAccess::claimed(f.bridge), "engine-refused key cannot arm watchdog");
    f.bridge.setTxAllowed(false);
    f.request("key ptt on");
    check(!AutomationServerTestAccess::claimed(f.bridge), "refused key-on cannot arm watchdog");

    Fixture nested;
    nested.backend->keyingWriter = [&](bool on) {
        if (on) {
            nested.request("not-a-verb");
        }
    };
    nested.request("key ptt on");
    check(AutomationServerTestAccess::owns(nested.bridge),
          "nested request restores outer pre-key sample before claim");
}

void deferredActions()
{
    Fixture f;
    AutomationServerTestAccess::defer(f.bridge, [](TxController::Input input) { (void)input.start(); });
    check(!AutomationServerTestAccess::claimed(f.bridge), "queued widget action does not claim TX early");
    pump(1);
    check(AutomationServerTestAccess::owns(f.bridge), "deferred action claims after engine admission");
    f.request("key ptt off");
    AutomationServerTestAccess::poll(f.bridge);

    int calls = 0;
    AutomationServerTestAccess::defer(f.bridge, [&](TxController::Input) { ++calls; });
    f.bridge.setTxAllowed(false);
    f.bridge.setTxAllowed(true);
    pump(1);
    check(calls == 0, "revoked queued action stays cancelled after permission re-enabled");

    AutomationServerTestAccess::defer(f.bridge, [&](TxController::Input) { ++calls; });
    f.bridge.stop();
    pump(1);
    check(calls == 0, "bridge stop fences deferred TX actions");

    AutomationServerTestAccess::defer(f.bridge, [&](TxController::Input) { ++calls; });
    f.bridge.setReadOnly(true);
    f.bridge.setReadOnly(false);
    pump(1);
    check(calls == 0, "observe-only transition permanently fences queued TX action");

    AutomationServerTestAccess::defer(f.bridge, [](TxController::Input input) { (void)input.start(); });
    f.radio.setTransmit(true);
    pump(1);
    f.commands.clear();
    f.bridge.setTxAllowed(false);
    check(f.commands.isEmpty() && f.radio.transmitModel().isTransmitting(),
          "deferred scoped action never releases an intervening operator contribution");

    Fixture revoked;
    AutomationServerTestAccess::defer(revoked.bridge, [&](TxController::Input input) {
        revoked.bridge.setTxAllowed(false);
        (void)input.start();
    });
    pump(1);
    check(!revoked.radio.transmitModel().isTransmitting()
              && !AutomationServerTestAccess::claimed(revoked.bridge),
          "revocation during a deferred callback cannot leave later keying unpoliced");
}

void cwAndAtuAreStopped()
{
    Fixture cw;
    AutomationServerTestAccess::defer(cw.bridge, [](TxController::Input input) { (void)input.start(); },
                                      TxController::Activity::CwKey);
    pump(1);
    check(AutomationServerTestAccess::owns(cw.bridge), "CW key with no MOX flag is policed");
    AutomationServerTestAccess::setMaxKeyMs(cw.bridge, 0);
    cw.commands.clear();
    AutomationServerTestAccess::poll(cw.bridge);
    check(cw.commands.contains("cw:off"), "watchdog releases straight-key carrier");

    Fixture atu;
    atu.request("atu start");
    check(AutomationServerTestAccess::owns(atu.bridge), "ATU operation is policed without MOX flag");
    AutomationServerTestAccess::setMaxKeyMs(atu.bridge, 0);
    atu.commands.clear();
    AutomationServerTestAccess::poll(atu.bridge);
    check(atu.commands.contains("atu:off"), "watchdog bypasses an owned internal ATU cycle");
}

void reentrantReplacementDuringCleanup()
{
    Fixture f;
    f.request("txtest twotone");
    f.backend->tuneWriter = [&](bool on) {
        if (!on) {
            // Backend completion still retains the old contribution here.
            // Post fresh operator input after that terminal write returns.
            QMetaObject::invokeMethod(&f.radio, [&] { f.radio.setTransmit(true); }, Qt::QueuedConnection);
        }
    };
    AutomationServerTestAccess::setMaxKeyMs(f.bridge, 0);
    f.commands.clear();
    AutomationServerTestAccess::poll(f.bridge);
    pump(1);
    check(f.commands.contains("mox:on") && !f.commands.contains("mox:off"),
          "watchdog cleanup rechecks identity after synchronous replacement");
}

void voiceTimeoutPreservesTuner()
{
    Fixture f;
    f.request("key ptt on");
    AutomationServerTestAccess::setMaxKeyMs(f.bridge, 0);
    f.commands.clear();
    AutomationServerTestAccess::poll(f.bridge);
    check(f.commands.contains("mox:off") && !f.commands.contains("atu:off"),
          "voice timeout does not bypass an operator tuner it did not start");
}

void diagnosticObserverCapturesAdmission()
{
    Fixture f;
    RadioCertification cert(&f.radio, nullptr, AutomationServerTestAccess::controller(f.bridge));
    int acceptedEdges = 0;
    cert.setKeyObserver([&](bool on, const TxCoordinator::Operation& previous, bool keyedBefore) {
        AutomationServerTestAccess::observeKey(f.bridge, on, previous, keyedBefore);
        if (on) {
            ++acceptedEdges;
            check(!keyedBefore && !previous.sameOperation(f.radio.transmitOperation())
                      && AutomationServerTestAccess::owns(f.bridge),
                  "diagnostic observer receives pre-key identity after engine admission");
        }
    });
    check(RadioCertificationTestAccess::key(cert, true), "injected diagnostic key changes local intent");
    check(RadioCertificationTestAccess::key(cert, false), "injected diagnostic release ends local intent");
    check(acceptedEdges == 1 && !AutomationServerTestAccess::claimed(f.bridge),
          "diagnostic per-key observer brackets one operation, not an entire run");
}

void laterCompatibleProducerSurvives()
{
    for (bool release : {false, true}) {
        Fixture f;
        f.request("key ptt on");
        const TxCoordinator::Operation original = f.radio.transmitOperation();
        const TxCoordinator::Producer cat = f.radio.registerTxProducer();
        const TxCoordinator::Request input = cat.request();
        check(f.radio.requestProducerPttOn(input, TransmitModel::PttSource::Dax)
                  && original.sameOperation(f.radio.transmitOperation()),
              "later CAT-equivalent input has its own hold in the shared operation");
        f.commands.clear();
        if (release) {
            f.request("key ptt off");
        } else {
            AutomationServerTestAccess::setMaxKeyMs(f.bridge, 0);
            AutomationServerTestAccess::poll(f.bridge);
        }
        check(f.commands.isEmpty() && f.radio.transmitModel().isTransmitting()
                  && f.backend->lastKeyOn.permitsDispatch(TxCoordinator::monotonicMs()),
              "bridge release/watchdog does not unkey or cancel a later CAT contribution");
        check(!AutomationServerTestAccess::owns(f.bridge)
                  && !AutomationServerTestAccess::claimed(f.bridge),
              "completed bridge hold cannot retain watchdog ownership of another producer's carrier");
        f.radio.requestProducerPttOff(input, TransmitModel::PttSource::Dax);
        check(f.commands.contains("mox:off"), "last compatible producer still performs normal release");
    }
}

void authorizationLifetimeFencesCapturedWork()
{
    for (bool readOnly : {false, true}) {
        Fixture f;
        const std::shared_ptr<TxController> original = AutomationServerTestAccess::controller(f.bridge);
        const TxController::Input queued = original->capture(TxController::Activity::Mox);
        int calls = 0;
        AutomationServerTestAccess::defer(f.bridge, [&](TxController::Input) { ++calls; });
        if (readOnly) {
            f.bridge.setReadOnly(true);
            f.bridge.setReadOnly(false);
        } else {
            f.bridge.setAuthToken(QStringLiteral("rotated-test-authorization"));
            f.bridge.setAuthToken({});
        }
        pump(1);
        check(calls == 0 && !queued.start() && !original->valid() && f.commands.isEmpty(),
              "authorization rotation/observe-only fences input permanently, even before admission");
        check(f.request("key ptt on").value("ok").toBool(),
              "a fresh request can use the new authorization lifetime");
        const TxCoordinator::Operation queuedCommand = f.backend->lastKeyOn;
        f.bridge.setReadOnly(true);
        check(!queuedCommand.permitsDispatch(TxCoordinator::monotonicMs())
                  && f.commands.contains("mox:off"),
              "revocation fences the original queued command before scoped cleanup");
    }
}

void policingPrecedesBackendReentry()
{
    Fixture f;
    f.backend->keyingWriter = [&](bool on) {
        if (on) {
            check(AutomationServerTestAccess::owns(f.bridge),
                  "watchdog is armed before backend dispatch can enter a nested event loop");
            f.bridge.setReadOnly(true);
        }
    };
    f.request("key ptt on");
    check(!f.backend->lastKeyOn.permitsDispatch(TxCoordinator::monotonicMs())
              && f.commands.contains("mox:off") && !AutomationServerTestAccess::claimed(f.bridge),
          "reentrant authorization revocation releases and fences the original input");
}

void diagnosticCannotRenewAuthorization()
{
    Fixture f;
    const std::shared_ptr<TxController> controller = AutomationServerTestAccess::controller(f.bridge);
    RadioCertification cert(&f.radio, nullptr, controller);
    check(RadioCertificationTestAccess::key(cert, true), "diagnostic uses its captured controller");
    f.bridge.setReadOnly(true);
    f.bridge.setReadOnly(false);
    f.commands.clear();
    check(!RadioCertificationTestAccess::key(cert, true) && f.commands.isEmpty(),
          "a continuing diagnostic cannot adopt replacement authorization between stages");
}

void cleanupCanDestroyBridge()
{
    for (int action = 0; action < 7; ++action) {
        Fixture f;
        auto bridge = std::make_unique<AutomationServer>();
        bridge->setRadioModel(&f.radio);
        bridge->setTxAllowed(true);
        const auto input = AutomationServerTestAccess::controller(*bridge)->capture(TxController::Activity::Mox);
        check(input.start(), "lifetime fixture owns an admitted bridge input");
        f.backend->keyingWriter = [&](bool on) {
            if (!on) { bridge.reset(); }
        };
        switch (action) {
        case 0: bridge->setTxAllowed(false); break;
        case 1: bridge->setReadOnly(true); break;
        case 2: bridge->setAuthToken(QStringLiteral("replacement-test-token")); break;
        case 3: bridge->setRadioModel(nullptr); break;
        case 4: bridge->stop(); break;
        case 5:
            AutomationServerTestAccess::setMaxKeyMs(*bridge, 0);
            AutomationServerTestAccess::poll(*bridge);
            break;
        case 6:
            // Leave the controller with its original, now cancelled session
            // input. Refreshing it still retires its unconfirmed local tail.
            f.backend->keyingWriter = {};
            f.radio.cancelLocalTransmit();
            f.backend->keyingWriter = [&](bool on) {
                if (!on) { bridge.reset(); }
            };
            check(!AutomationServerTestAccess::controller(*bridge),
                  "controller refresh stops if retiring its predecessor destroys the bridge");
            break;
        }
        check(!bridge && !input.valid(), "unkey callback may destroy bridge during authorization cleanup");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("automation-tx-watchdog"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    manualTransmitIsNotPoliced();
    actionsCannotAdoptExistingOperation();
    deadlineDoesNotRenew();
    productionTimerExpiresOriginalOperation();
    staleWatchdogCannotStopReplacement();
    releaseAndReadbackGaps();
    refusedAndNestedRequests();
    deferredActions();
    cwAndAtuAreStopped();
    reentrantReplacementDuringCleanup();
    voiceTimeoutPreservesTuner();
    diagnosticObserverCapturesAdmission();
    laterCompatibleProducerSurvives();
    authorizationLifetimeFencesCapturedWork();
    policingPrecedesBackendReentry();
    diagnosticCannotRenewAuthorization();
    cleanupCanDestroyBridge();
    return failures == 0 ? 0 : 1;
}
