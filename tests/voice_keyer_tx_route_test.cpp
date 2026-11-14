// Ownership and cancellation for the REAL VoiceKeyerTxRoute — the class
// FakeTxAudioRoute stands in for in generated_audio_transmitter_test, so the
// producer/request/context handling was previously untested.
// Socket-free: no connection is opened; the route is driven directly.
// Run: ./build/voice_keyer_tx_route_test

#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "models/SliceModel.h"
#include "models/TxController.h"
#include "core/VoiceKeyerTxRoute.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace AetherSDR {
class VoiceKeyerTxRouteTestAccess {
public:
    static unsigned activeTxActivities(const RadioModel& radio)
    {
        return radio.activeTxActivities();
    }
};
}  // namespace AetherSDR

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// The production backend seam with no socket: records keying calls.
class KeyingBackend final : public IRadioBackend {
public:
    QStringList commands;
    bool connected{true};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.canTransmit = true;
        caps.hostModulates = true;   // no DAX stream to wait for
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
    void setAtu(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void setTune(bool, int, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

KeyingBackend* installKeyingBackend(RadioModel& radio)
{
    auto backend = std::make_unique<KeyingBackend>();
    KeyingBackend* raw = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
    if (radio.automationApplySliceFixture(0, QStringLiteral("A"))) {
        SliceDelta slice;
        slice.txSlice = true;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(slice);
    }
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    return raw;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A disconnected radio refuses before anything is taken from the
    // coordinator: startRefusal() is the first gate the transmitter consults.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        const QString refusal = route.startRefusal();
        report("disconnected_radio_refuses_start",
               !refusal.isEmpty() && refusal.contains(QLatin1String("Connect")),
               refusal.toStdString());
        report("refusal_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // keyOn() asks for PTT against a request admitted at the input boundary.
    // Without that request there is nothing to key against, and keying anyway
    // would put a carrier up carrying no admitted operation (#5659).
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        report("key_without_admit_is_refused", !route.keyOn());
        report("refused_key_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // The release path blocker 3(b) was about: an admitted request is handed
    // back by keyOff() even though the radio never showed keyed, so a second
    // keyOn() has nothing to key against until it is admitted again.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        const bool admitted = route.admitOperation();
        route.keyOff();
        report("keyoff_hands_back_an_admitted_request",
               admitted && !route.keyOn(),
               admitted ? "admitted" : "admitOperation() refused");
        report("handed_back_request_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // keyOff() with nothing held is harmless: finish() calls it on paths where
    // the key was never asked for.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        route.keyOff();
        route.keyOff();
        report("keyoff_is_idempotent",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // The operator's TX audio state is left as it was found. #5524: DAX left
    // on after a dax_tx producer finishes means the mic is dead on the next
    // PTT, so release must restore the value claim saw — not a fixed one.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        auto& tx = radio.transmitModel();
        tx.setDax(false);
        route.claimAudioPath();
        const bool onForUs = tx.daxOn();
        route.releaseAudioPath();
        report("release_restores_dax_off_as_found",
               onForUs && !tx.daxOn(),
               QString("claimed=%1 after=%2").arg(onForUs).arg(tx.daxOn()).toStdString());
    }

    // The same route on a radio where the operator already had DAX on: release
    // must leave it on, so a fixed restore-to-false is not good enough.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        auto& tx = radio.transmitModel();
        tx.setDax(true);
        route.claimAudioPath();
        route.releaseAudioPath();
        report("release_restores_dax_on_as_found", tx.daxOn(),
               QString("after=%1").arg(tx.daxOn()).toStdString());
    }

    // A "Macro N" from a MIDI or keyboard device admits voice-keyer playback
    // under that device's request. Keying after the settle hop uses it, so
    // the key goes up while the device is live...
    {
        RadioModel radio;
        KeyingBackend* backend = installKeyingBackend(radio);
        QObject device;
        const auto controller = TxController::forNativeDevice(&radio, &device);
        const auto scoped = TxController::captureInputScope(controller, controller->captureRawInput());
        const TxController::Input input = scoped->capture(TxController::Activity::Mox);
        VoiceKeyerTxRoute route(radio, nullptr);
        const bool admitted = route.admitOperationFrom(input.request());
        const bool keyed = route.keyOn();
        report("origin_request_keys_while_device_live",
               admitted && keyed && backend->commands.contains(QStringLiteral("mox:on")),
               backend->commands.join(',').toStdString());
        route.keyOff();
    }

    // ...and closing the device between admission and the delayed key fences
    // it: nothing reaches the transmitter (docs/agents/backends.md, step 4).
    {
        RadioModel radio;
        KeyingBackend* backend = installKeyingBackend(radio);
        QObject device;
        const auto controller = TxController::forNativeDevice(&radio, &device);
        const auto scoped = TxController::captureInputScope(controller, controller->captureRawInput());
        const TxController::Input input = scoped->capture(TxController::Activity::Mox);
        VoiceKeyerTxRoute route(radio, nullptr);
        const bool admitted = route.admitOperationFrom(input.request());
        // What a MIDI port close does: MidiControlManager's discard callback,
        // then MainWindow's portClosed cleanup.
        controller->discardDeviceInputs();
        controller->cleanupDeviceInputs();
        const bool keyed = route.keyOn();
        report("device_close_before_key_fences_the_key",
               admitted && !keyed && !backend->commands.contains(QStringLiteral("mox:on"))
                   && VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0,
               backend->commands.join(',').toStdString());
    }

    // An origin that is already gone is not admitted at all.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        report("dead_origin_is_not_admitted",
               !route.admitOperationFrom(TxCoordinator::Request{}) && !route.keyOn());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
