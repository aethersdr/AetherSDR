// Lifecycle test for the demo-mode SimBackend (RFC #4288, Phase 1 skeleton).
// Verifies the IRadioBackend contract a demo radio must honor: connect emits
// connected + an initial radio/slice snapshot, capabilities are RX-only, a
// second connect is idempotent, slice intents echo back (radio-authoritative),
// disconnect removes the slice + reconnect works, and TX stays fail-closed.

#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using AetherSDR::RadioCapabilities;
using AetherSDR::RadioConnectRequest;
using AetherSDR::RadioDelta;
using AetherSDR::SimBackend;
using AetherSDR::SliceDelta;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

void testConnectEmitsInitialState()
{
    SimBackend sim;
    QSignalSpy connectedSpy(&sim, &SimBackend::connected);
    QSignalSpy radioSpy(&sim, &SimBackend::radioChanged);
    QSignalSpy sliceSpy(&sim, &SimBackend::sliceChanged);

    report("starts disconnected", !sim.isConnected());

    sim.connectRadio(RadioConnectRequest{});

    report("connect() emits connected once", connectedSpy.count() == 1);
    report("connect() reports connected", sim.isConnected());
    report("connect() emits a radio-global snapshot", radioSpy.count() == 1);
    report("connect() emits an initial slice", sliceSpy.count() == 1);

    if (radioSpy.count() == 1) {
        const auto delta = radioSpy.first().at(0).value<RadioDelta>();
        report("radio snapshot carries the demo model",
               delta.model.has_value()
                   && delta.model.value() == SimBackend::demoModelName());
    }
    if (sliceSpy.count() == 1) {
        const int sliceId = sliceSpy.first().at(0).toInt();
        const auto delta = sliceSpy.first().at(1).value<SliceDelta>();
        report("initial slice is slice 0", sliceId == 0);
        report("initial slice is active with a frequency + mode",
               delta.active.value_or(false) && delta.frequency.has_value()
                   && delta.mode.has_value());
    }
}

void testCapabilitiesAreReceiveOnly()
{
    SimBackend sim;
    const RadioCapabilities caps = sim.capabilities();
    report("capabilities family is 'sim'", caps.family == QStringLiteral("sim"));
    report("a demo radio cannot transmit (Principle VI)", !caps.canTransmit);
    report("TX power is zero when RX-only", caps.txPowerMaxWatts == 0.0);
    report("advertises at least one slice", caps.maxSlices >= 1);
}

void testSecondConnectIsIdempotent()
{
    SimBackend sim;
    sim.connectRadio(RadioConnectRequest{});
    QSignalSpy connectedSpy(&sim, &SimBackend::connected);
    QSignalSpy sliceSpy(&sim, &SimBackend::sliceChanged);

    sim.connectRadio(RadioConnectRequest{});   // already connected

    report("second connect emits no extra connected signal",
           connectedSpy.count() == 0);
    report("second connect does not re-emit the initial slice",
           sliceSpy.count() == 0);
    report("still connected after redundant connect", sim.isConnected());
}

void testSliceIntentEchoesBack()
{
    SimBackend sim;
    sim.connectRadio(RadioConnectRequest{});
    QSignalSpy sliceSpy(&sim, &SimBackend::sliceChanged);

    sim.setSliceFrequency(0, 7'074'000.0);   // 7.074 MHz, in Hz
    report("frequency intent echoes a slice update", sliceSpy.count() == 1);
    if (sliceSpy.count() == 1) {
        const auto delta = sliceSpy.first().at(1).value<SliceDelta>();
        report("echoed frequency is the requested value (MHz)",
               delta.frequency.has_value()
                   && qFuzzyCompare(delta.frequency.value(), 7.074));
    }

    sim.setSliceMode(0, QStringLiteral("CW"));
    report("mode intent echoes a slice update", sliceSpy.count() == 2);

    sim.setSliceFrequency(1, 14'000'000.0);   // unknown slice id
    report("intent on an unknown slice id is ignored", sliceSpy.count() == 2);
}

void testDisconnectAndReconnect()
{
    SimBackend sim;
    sim.connectRadio(RadioConnectRequest{});
    QSignalSpy removedSpy(&sim, &SimBackend::sliceRemoved);
    QSignalSpy disconnectedSpy(&sim, &SimBackend::disconnected);

    sim.disconnectRadio();
    report("disconnect removes the slice", removedSpy.count() == 1);
    report("disconnect emits disconnected", disconnectedSpy.count() == 1);
    report("reports disconnected", !sim.isConnected());

    // Intents while disconnected are inert.
    QSignalSpy sliceSpy(&sim, &SimBackend::sliceChanged);
    sim.setSliceFrequency(0, 10'000'000.0);
    report("slice intent while disconnected is ignored", sliceSpy.count() == 0);

    // Reconnect brings the radio back to life.
    QSignalSpy connectedSpy(&sim, &SimBackend::connected);
    sim.connectRadio(RadioConnectRequest{});
    report("reconnect works", sim.isConnected() && connectedSpy.count() == 1);
    report("reconnect re-emits the initial slice", sliceSpy.count() == 1);
}

void testKeyingIsAlwaysInert()
{
    SimBackend sim;
    sim.connectRadio(RadioConnectRequest{});
    // No transmit-related signal exists to fire; the contract is simply that
    // setKeying never drives a transmit path. This asserts it does not crash or
    // change connection state — the real TX guard lives above the seam.
    sim.setKeying(true);
    sim.setKeying(false);
    report("setKeying is a safe no-op on an RX-only sim", sim.isConnected());
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // QSignalSpy stores emitted args as QVariant; register the delta types so
    // .value<T>() round-trips (belt-and-braces alongside Q_DECLARE_METATYPE).
    qRegisterMetaType<RadioDelta>();
    qRegisterMetaType<SliceDelta>();

    testConnectEmitsInitialState();
    testCapabilitiesAreReceiveOnly();
    testSecondConnectIsIdempotent();
    testSliceIntentEchoesBack();
    testDisconnectAndReconnect();
    testKeyingIsAlwaysInert();

    if (g_failed == 0) {
        std::printf("All SimBackend lifecycle checks passed\n");
    }
    return g_failed == 0 ? 0 : 1;
}
