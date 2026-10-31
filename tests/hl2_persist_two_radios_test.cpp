// Two Hermes-Lite 2 radios, one profile: what each remembers is its own.
//
// radio_state_memory_test shows the store keeps two scopes apart with a
// synthetic document. This drives two real backends with different operator
// choices (tuning, mode, passband, per-band drive and per-band LNA gain),
// stores each under its own MAC, and shows what a restart on each radio, a
// restart on a third that was never stored, and a swap from one radio to the
// other on the SAME backend instance all find.
//
// The MAC is the scope's radio id, as Hl2Discovery::macToSerial() produces it.
// The mapping from a discovered radio to that id happens in RadioModel and is
// not exercised here.
//
// Socket-free in the sense of the default graph: nothing answers at TEST-NET-1,
// the link-up push is run by hand, nothing is keyed. The profile is isolated.

#include "Hl2PersistHarness.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>

using namespace AetherSDR;
using namespace AetherSDR::persist;

namespace {

constexpr double k20m = 14'074'000.0;
constexpr double k40m = 7'074'000.0;
constexpr double k80m = 3'573'000.0;

const QString kMacA = QStringLiteral("AA:BB:CC:00:20:01");
const QString kMacB = QStringLiteral("AA:BB:CC:00:20:02");
const QString kMacNew = QStringLiteral("AA:BB:CC:00:20:03");

RadioSettingsScope scopeFor(const QString& mac)
{
    return RadioSettingsScope(QStringLiteral("hl2"), mac);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-persist-two-radios"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    RadioCapabilities caps;
    RestoredRadioState capA, capB;

    // Radio A: 20 m USB, narrowed; drive 30 / gain -6 on 20 m, 40 m visited.
    {
        Session s(kMacA, RestoredRadioState{}, k20m);
        caps = s.backend.capabilities();
        s.backend.setSliceMode(0, QStringLiteral("USB"));
        s.backend.setSliceFilter(0, 300, 2700);
        s.backend.setTxPower(30);
        s.backend.setPanRfGain(s.panId, -6);
        s.backend.setSliceFrequency(0, k40m);
        s.backend.setTxPower(45);
        s.backend.setPanRfGain(s.panId, 3);
        s.backend.setSliceFrequency(0, k20m);
        capA = s.backend.currentOperatingState();
    }
    // Radio B: 80 m LSB, a different passband; 80 m drive 80 / gain 9, and 20 m
    // visited with values that differ from A's 20 m.
    {
        Session s(kMacB, RestoredRadioState{}, k20m);
        s.backend.setTxPower(70);
        s.backend.setPanRfGain(s.panId, 9);
        s.backend.setSliceFrequency(0, k80m);
        s.backend.setTxPower(80);
        s.backend.setPanRfGain(s.panId, 12);
        s.backend.setSliceMode(0, QStringLiteral("LSB"));
        s.backend.setSliceFilter(0, -2500, -200);
        capB = s.backend.currentOperatingState();
    }
    check(capA.rfFrequencyHz == k20m && capB.rfFrequencyHz == k80m && capA.mode == "USB"
              && capB.mode == "LSB",
          "setup: the two radios were left in different places");

    bool storedA = false, storedB = false;
    const RestoredRadioState diskA = throughDisk(scopeFor(kMacA), caps, capA, &storedA);
    const RestoredRadioState diskB = throughDisk(scopeFor(kMacB), caps, capB, &storedB);
    check(storedA && storedB, "both radios' operating states are stored");

    // ---- each radio restarts on its own state ----------------------------
    {
        Session a(kMacA, RadioStateMemory::load(scopeFor(kMacA), caps), k80m);
        const RestoredRadioState up = a.backend.currentOperatingState();
        check(up.rfFrequencyHz == k20m && up.mode == QLatin1String("USB")
                  && up.filterLowHz == 300 && up.filterHighHz == 2700,
              "radio A restarts on 20 m, USB, 300..2700");
        check(Session::driveOf(up, "20m") == 30 && Session::driveOf(up, "40m") == 45
                  && Session::lnaOf(up, "20m") == -6 && Session::lnaOf(up, "40m") == 3,
              "radio A keeps its own per-band drive and gain");
        check(!Session::hasDrive(up, "80m") && !Session::hasLna(up, "80m"),
              "radio A carries nothing for radio B's 80 m");
        check(a.liveLnaDb() == -6, "radio A's start band comes up on A's gain, live");
    }
    {
        Session b(kMacB, RadioStateMemory::load(scopeFor(kMacB), caps), k20m);
        const RestoredRadioState up = b.backend.currentOperatingState();
        check(up.rfFrequencyHz == k80m && up.mode == QLatin1String("LSB")
                  && up.filterLowHz == -2500 && up.filterHighHz == -200,
              "radio B restarts on 80 m, LSB, -2500..-200");
        check(Session::driveOf(up, "20m") == 70 && Session::driveOf(up, "80m") == 80
                  && Session::lnaOf(up, "20m") == 9 && Session::lnaOf(up, "80m") == 12,
              "radio B keeps its own per-band drive and gain");
        check(!Session::hasDrive(up, "40m") && !Session::hasLna(up, "40m"),
              "radio B carries nothing for radio A's 40 m");
        // The same band on both radios holds different values: the negative
        // control for "each keeps its own" above.
        check(Session::driveOf(up, "20m") != Session::driveOf(diskA, "20m")
                  && Session::lnaOf(up, "20m") != Session::lnaOf(diskA, "20m"),
              "negative control: 20 m differs between the two radios");
        b.backend.setSliceFrequency(0, k40m);
        check(b.liveDriveFor("40m") == 70,
              "a band radio B never visited takes B's own baseline (70), not A's 40 m (45)");
    }

    // ---- a radio that was never stored finds nothing ---------------------
    {
        const RestoredRadioState none = RadioStateMemory::load(scopeFor(kMacNew), caps);
        check(none.isEmpty(), "a third radio with no stored state loads as empty");
        Session c(kMacNew, none, k40m);
        const RestoredRadioState up = c.backend.currentOperatingState();
        check(up.rfFrequencyHz == k40m, "a third radio comes up on the start frequency it was given");
        check(!Session::hasDrive(up, "20m") && !Session::hasLna(up, "20m")
                  && !Session::hasDrive(up, "80m") && !Session::hasLna(up, "80m"),
              "a third radio inherits neither A's nor B's per-band memory");
        check(up.mode != QLatin1String("LSB") || up.filterLowHz != -2500,
              "a third radio inherits neither B's mode nor passband");
    }

    // ---- storing A again does not touch B --------------------------------
    {
        RestoredRadioState changed = capA;
        changed.rfFrequencyHz = k40m;
        RadioStateMemory::store(scopeFor(kMacA), caps, changed);
        const RestoredRadioState b = RadioStateMemory::load(scopeFor(kMacB), caps);
        check(b.rfFrequencyHz == k80m && Session::driveOf(b, "80m") == 80,
              "a later store for radio A leaves radio B's document alone");
        const RestoredRadioState a = RadioStateMemory::load(scopeFor(kMacA), caps);
        check(a.rfFrequencyHz == k40m, "and radio A's own document did change");
        RadioStateMemory::store(scopeFor(kMacA), caps, capA);   // put A back
    }

    // ---- the same backend, switched from A to B --------------------------
    // RadioModel restores before every connect, so the previous radio's state
    // must be replaced whole, not merged: A's 40 m must not survive into B.
    {
        Session s(kMacA, RadioStateMemory::load(scopeFor(kMacA), caps), k80m);
        check(Session::driveOf(s.backend.currentOperatingState(), "40m") == 45,
              "setup: the shared backend starts as radio A");
        s.backend.disconnectRadio();
        s.backend.applyRestoredState(RadioStateMemory::load(scopeFor(kMacB), caps));
        QSignalSpy finished(&s.backend, &hl2::Hl2Backend::dspSetupFinished);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");   // TEST-NET-1, never routable
        request.port = 1024;
        request.serial = kMacB;
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        s.backend.connectRadio(request);
        test::awaitDspBuild("hl2_persist_two_radios", [&] { return !finished.isEmpty(); });
        hl2::Hl2DspReadbackTestAccess::pushInitialState(s.backend);
        const RestoredRadioState swapped = s.backend.currentOperatingState();
        check(!Session::hasDrive(swapped, "40m") && !Session::hasLna(swapped, "40m"),
              "after the swap nothing of radio A's 40 m remains");
        check(Session::driveOf(swapped, "80m") == 80 && Session::lnaOf(swapped, "80m") == 12
                  && Session::driveOf(swapped, "20m") == 70 && Session::lnaOf(swapped, "20m") == 9,
              "after the swap the backend holds radio B's per-band drive and gain");
    }

    std::printf("hl2_persist_two_radios_test: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
