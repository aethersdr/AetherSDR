// A RECEIVER'S S-METER MUST NOT OUTLIVE THE RECEIVER.
//
// #5852 gave every receiver above the first its own "SLC"/"LEVEL" definition,
// declared in openReceiverDsp() as the chain opens and withdrawn in
// removePanadapter() as it closes. Those are the two ORDINARY ends of the
// lifetime. They are not the only ends, and the others were all missing their
// withdrawal -- a shape nothing goes red for, because a meter left declared
// does not throw, does not warn and does not stop rendering. It renders the
// last value it ever received, forever.
//
// WHAT THIS FILE OBSERVES IS THE CONSEQUENCE, NOT THE CALL. There is no spy on
// withdrawSliceLevelMeter() here. The backend's meterDefined/meterRemoved
// signals are wired into a REAL MeterModel, exactly as RadioModel wires them,
// and every assertion is a MeterModel::findMeter() -- the same lookup the
// meter list and the S-meter widget go through. A meter this test can still
// find is a meter a consumer can still see.
//
// NO RADIO IS NEEDED, and nothing here asserts anything about the wire. The
// per-receiver meters are declared by buildReceivers() -> openReceiverDsp(),
// which runs during the DSP build and long before the link comes up; that is
// also why defineMeters()' fixed 1..9 catalogue never appears below, since it
// is emitted from the linkUp handler that a socket-free test never reaches.
//
// TWO OF THE PATHS ARE DRIVABLE FROM HERE, and they are the two that never emit
// disconnected() -- which is what makes them the ones worth driving.
// RadioModel::onDisconnected()'s MeterModel::clear() is the backstop that hides
// every one of these defects on the ordinary disconnect, and on neither path
// below does it run at all.
//
//   THE SUPERSEDED BUILD. A connect arriving while the previous one's WDSP
//   chains are still opening supersedes it; finishDspSetup() releases the
//   chains it built and re-drives the queued connect, leaving the catalogue
//   holding whatever the abandoned build declared.
//
//   THE REFUSED CONNECT. Receiver 0's configure() failing is a failed connect:
//   finishDspSetup() reports it and returns with no teardown at all, so every
//   receiver above the first keeps a definition for a radio that never came up.
//   Reached by asking for a sample rate Hl2RxDsp::configure() refuses -- see
//   requestAtRefusedRate() for why that is a real route and not a stub.
//
// The sibling sites share the fix but not the reachability; see the commit
// message for which ones are covered only by inspection and why.

#include "core/backends/hl2/Hl2Backend.h"
#include "models/MeterModel.h"

#include "TestDspBuildWait.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QObject>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// boardMaxRx is what skips the unicast discovery probe; TEST-NET-1 alone would
// still wait it out. numRx is only inserted above 1 because connectRadio()
// treats the key's ABSENCE as "no preference", not as 1.
RadioConnectRequest request(int numRx)
{
    RadioConnectRequest req;
    req.host = QStringLiteral("192.0.2.1");
    req.port = 1024;
    req.params.insert(QStringLiteral("boardMaxRx"), 4);
    if (numRx > 1) {
        req.params.insert(QStringLiteral("numRx"), numRx);
    }
    return req;
}

// THE SAME REQUEST WITH A SAMPLE RATE THE DSP WILL REFUSE.
//
// Hl2RxDsp::configure() guards its rate inputs -- "input/audio sample rate and
// DSP block size must all be positive" -- explicitly because a params override
// is where a malformed "sampleRateHz" gets in (QVariant::toInt of a missing or
// junk value is 0). connectRadio() takes the override verbatim, so this is a
// supported way to make the FIRST chain's configure() fail without touching
// production code or standing up a fake DSP. Nothing else about the connect
// changes: the receiver count survives (effectiveNumRx() reads numRx and
// boardMaxRx, and maxReceiversAtRate() admits everything at a zero rate,
// because a zero rate is zero bits per second) and both chains are opened and
// their meters declared before the I/O thread tries to configure either.
RadioConnectRequest requestAtRefusedRate(int numRx)
{
    RadioConnectRequest req = request(numRx);
    req.params.insert(QStringLiteral("sampleRateHz"), 0);
    return req;
}

// The meter catalogue as a CONSUMER holds it.
//
// These three connections are RadioModel's, reproduced rather than referenced
// so this test needs no GUI: meterDefined -> defineMeter, meterRemoved ->
// removeMeter, and -- the one that matters for what is being asserted --
// disconnected -> clear(), which is RadioModel::onDisconnected()'s wholesale
// wipe. Modelling that wipe faithfully is what keeps the test honest: it is
// precisely the reason the ordinary disconnect path needs no per-receiver
// withdrawal, and precisely what does NOT run on the path below.
class Catalogue {
public:
    MeterModel model;

    explicit Catalogue(Hl2Backend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::meterDefined, &backend,
                         [this](const MeterDef& def) { model.defineMeter(def); });
        QObject::connect(&backend, &IRadioBackend::meterRemoved, &backend,
                         [this](int index) { model.removeMeter(index); });
        QObject::connect(&backend, &IRadioBackend::disconnected, &backend,
                         [this] { model.clear(); });
    }

    // The consumer's own lookup. The explicit sourceIndex is not optional:
    // findMeter() treats a negative index as MATCH-ANY and would answer with
    // receiver 0's meter for every receiver, turning a real absence into a
    // false present.
    int sliceMeter(int uiNumber) const
    {
        return model.findMeter(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                               uiNumber);
    }
};

// Counts completed DSP builds. dspSetupFinished fires once per build on EVERY
// exit from finishDspSetup(), the superseded one included, so a supersede plus
// its re-driven queued connect is two.
struct BuildCounter {
    int finished = 0;
    explicit BuildCounter(Hl2Backend& backend)
    {
        QObject::connect(&backend, &Hl2Backend::dspSetupFinished, &backend,
                         [this] { ++finished; });
    }
};

// POSITIVE CONTROL FOR THE HARNESS ITSELF. Every assertion below is an
// ABSENCE, and an absence passes for free against a catalogue that was never
// populated -- a mis-wired signal, a build that never ran, a receiver count
// that silently came back as 1. This leg is what makes the others evidence.
void theSecondReceiverGetsAMeterWhenItsChainOpens()
{
    TestSettingsProfile profile(QStringLiteral("hl2-slice-meter-define"));
    Hl2Backend backend;
    Catalogue catalogue(backend);
    BuildCounter builds(backend);

    backend.connectRadio(request(2));
    check(test::awaitDspBuild(__func__, [&builds] { return builds.finished >= 1; }),
          "define: the two-receiver DSP build finishes");
    check(catalogue.sliceMeter(1) >= 0,
          "define: receiver 1's S-meter is declared when its chain opens");
    backend.disconnectRadio();
}

// THE DEFECT. Two receivers are built, the build is superseded by a connect for
// ONE, and the abandoned build's chains are released. Receiver 1 no longer
// exists and never will in this session, so nothing will ever publish
// "SLC1:LEVEL" again -- but without the withdrawal its definition is still in
// the catalogue, still keyed into MeterModel's per-slice cache, still listed in
// allMeters(), showing the last reading it happened to take.
void aSupersededBuildTakesItsMetersWithIt()
{
    TestSettingsProfile profile(QStringLiteral("hl2-slice-meter-supersede"));
    Hl2Backend backend;
    Catalogue catalogue(backend);
    BuildCounter builds(backend);

    backend.connectRadio(request(2));   // declares receiver 1's meter
    backend.connectRadio(request(1));   // queued; the build above is superseded

    check(test::awaitDspBuild(__func__, [&builds] { return builds.finished >= 2; }),
          "supersede: both the abandoned build and the queued one finish");
    check(catalogue.sliceMeter(1) < 0,
          "supersede: the abandoned build's receiver-1 meter goes with its chain");
    backend.disconnectRadio();
}

// THE OTHER HALF, and it is not decoration. The assertion above is satisfied by
// any change that withdraws MORE -- including one that withdraws on every
// teardown and never re-declares, which would delete the second receiver's
// S-meter for the whole application. Same supersede, same release, but the
// queued connect asks for TWO again, so the meter must come BACK.
void aSupersededRebuildAtTheSameCountKeepsItsMeters()
{
    TestSettingsProfile profile(QStringLiteral("hl2-slice-meter-rebuild"));
    Hl2Backend backend;
    Catalogue catalogue(backend);
    BuildCounter builds(backend);

    backend.connectRadio(request(2));
    backend.connectRadio(request(2));

    check(test::awaitDspBuild(__func__, [&builds] { return builds.finished >= 2; }),
          "rebuild: both the abandoned build and the queued one finish");
    check(catalogue.sliceMeter(1) >= 0,
          "rebuild: the re-driven build declares receiver 1's meter again");
    backend.disconnectRadio();
}

// THE REFUSED CONNECT -- the exit that leaves the MOST behind, and the one with
// no teardown in front of it.
//
// Receiver 0's DSP failing is a failed connect, so finishDspSetup() reports it
// and RETURNS: no tearDownReceivers(), no wire, no connected(), and therefore
// no disconnected() either. RadioModel's onDisconnected() clear() -- the
// backstop that hides every one of these on the ordinary path -- is not merely
// late here, it never runs at all. Meanwhile buildReceivers() declared a meter
// for every receiver above the first before the I/O thread configured any of
// them, so the catalogue is left describing receivers of a radio that never
// came up, for the life of the application if nothing reconnects.
//
// The control is on the SAME backend rather than in a separate leg: the meter
// is looked up once while the build is still in flight, where it must be
// present, and again after the refusal, where it must be gone. An absence that
// follows an observed presence cannot be the absence of a build that never ran.
void aRefusedConnectTakesTheLaterReceiversMetersWithIt()
{
    TestSettingsProfile profile(QStringLiteral("hl2-slice-meter-refused"));
    Hl2Backend backend;
    Catalogue catalogue(backend);
    BuildCounter builds(backend);

    int errors = 0;
    QObject::connect(&backend, &IRadioBackend::connectionError, &backend,
                     [&errors](const QString&) { ++errors; });

    backend.connectRadio(requestAtRefusedRate(2));
    // connectRadio() returns once the opens are handed to the I/O thread, and
    // openReceiverDsp() -- declarations included -- has already run on this
    // thread by then. This is the positive control.
    check(catalogue.sliceMeter(1) >= 0,
          "refused: receiver 1's S-meter is declared before the configure fails");

    check(test::awaitDspBuild(__func__, [&builds] { return builds.finished >= 1; }),
          "refused: the build finishes");
    // Names the branch. Any other exit from finishDspSetup() -- the supersede,
    // the failed socket bind, the trim -- would leave this at zero or reach it
    // by a route whose teardown is already covered elsewhere in this file.
    check(errors == 1, "refused: receiver 0's DSP failure refuses the connect");
    check(catalogue.sliceMeter(1) < 0,
          "refused: the refused connect's receiver-1 meter goes with it");
    backend.disconnectRadio();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    theSecondReceiverGetsAMeterWhenItsChainOpens();
    aSupersededBuildTakesItsMetersWithIt();
    aSupersededRebuildAtTheSameCountKeepsItsMeters();
    aRefusedConnectTakesTheLaterReceiversMetersWithIt();
    std::fprintf(stderr, "%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
