// TCI signaling on a backend that has neither a Flex command plane nor a DAX
// data plane (Hermes-Lite 2), which is what WSJT-X needs to work.
//
// Three separate silent failures made TCI useless on such a radio. Each was
// silent in its own way -- the WebSocket stayed up, the handshake completed,
// and WSJT-X showed a connected rig throughout:
//
//   TX confirmation  TciServer treats RadioModel::radioTransmittingChanged as
//                    the authoritative "the radio really keyed" edge and gives
//                    up 1250 ms after a TCI key request that never sees one.
//                    That signal is decoded from Flex `interlock` status, so on
//                    an HL2 it never arrived: every WSJT-X transmission was
//                    unkeyed a second and a quarter in, mid-tone.
//
//   TX audio         AudioEngine::feedDaxTxAudio returned immediately unless a
//                    Flex TX stream id was set. A host-modulating backend has
//                    none and never will -- its modulator is local -- so every
//                    TCI audio frame was dropped and the radio transmitted
//                    silence while reporting a perfectly normal transmit.
//
//   Command plane    Anything routed through sendCmdPublic() (the VFO tune) is
//                    swallowed, AND its completion callback never runs, so
//                    WSJT-X's 2 s vfo-echo timeout expired on every band hop.
//
// These assertions pin the seam, not the symptom: a future backend that reports
// hostModulates or a non-Flex family inherits the same guarantees.

#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/RadioDiscovery.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#ifdef HAVE_WEBSOCKETS
#include <QWebSocket>
#endif

#include <cstdio>

namespace AetherSDR {

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static RadioInfo hl2Info()
{
    RadioInfo i;
    i.family  = QStringLiteral("hl2");
    i.serial  = QStringLiteral("00:1C:C0:00:00:01");
    i.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    i.port    = 1024;
    return i;
}

static RadioInfo flexInfo()
{
    RadioInfo i;
    i.family  = QStringLiteral("flex");
    i.serial  = QStringLiteral("1234-5678-9012-3456");
    i.address = QHostAddress(QStringLiteral("192.0.2.2"));
    i.port    = 4992;
    return i;
}

// Grants access to the private predicate that gates every DAX arrangement in
// TciServer. Declared as a friend in TciServer.h.
//
// Guarded because TciServer.h is `#pragma once` followed immediately by
// `#ifdef HAVE_WEBSOCKETS`, so it expands to NOTHING in a build configured
// without Qt6WebSockets — naming TciServer out here failed to compile such a
// build entirely. Everything else in this file (the transmit edge, the command
// plane, host-modulated TX audio, the per-mode passband) is WebSocket-free and
// deliberately still runs there.
#ifdef HAVE_WEBSOCKETS
class Hl2TciSignalingTest {
public:
    static bool hostModulating(TciServer& server) {
        return server.hostModulatingBackend();
    }

    // The route-transition latch and the deferral queue it feeds. A transition
    // that is opened and never closed is the failure mode under test, and it is
    // invisible from outside: the socket stays up and the handshake still works.
    static bool routeTransitionInFlight(TciServer& s) {
        return s.m_routeTransitionInFlight;
    }
    static bool hasPendingTrx(TciServer& s) { return s.m_pendingTrxRequest.has_value(); }
    static bool splitRequested(TciServer& s) { return s.m_routingState.splitRequested(); }

    static void trx(TciServer& s, QWebSocket* c, const TciProtocol::TrxRequest& r) {
        s.handleTrxRequest(c, r);
    }

    // The VFO-B route builder itself. Driven directly rather than through
    // handleSplitRequest()/handleVfoRequest(), because those resolve their RX
    // slice via TciProtocol::resolveSliceForTrx(), which returns null on a model
    // that is not connected to real hardware — every assertion below it would
    // then pass without the code under test ever running.
    static void createVfoB(TciServer& s, QWebSocket* c,
                           const TciProtocol::VfoRequest& r, SliceModel* rx,
                           const QString& routeConfirmation, bool splitOnly) {
        s.createTxSliceForVfoB(c, r, rx, routeConfirmation, splitOnly);
    }
    static void setSplitRequested(TciServer& s, bool on) {
        s.m_routingState.setSplitRequested(on);
    }
    static bool pendingVfoBCreate(TciServer& s) {
        return s.m_pendingVfoBCreate.has_value();
    }

    // Runs promoteTxSliceAndContinue and reports whether the continuation ran at
    // all — the distinction that matters, since a continuation that never runs
    // strands its caller's route transition rather than failing it.
    static void promote(TciServer& s, int sliceId, bool* answered, bool* selected) {
        *answered = false;
        *selected = false;
        s.promoteTxSliceAndContinue(sliceId, [answered, selected](bool ok) {
            *answered = true;
            *selected = ok;
        });
    }
};
#endif

// ── The raw-TX edge TciServer waits on ────────────────────────────────────
static void testTransmitEdgeIsPublished()
{
    RadioModel model;
    model.connectToRadio(hl2Info());

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);

    // Exactly the call TciServer::handleTrxRequest makes for a WSJT-X key.
    // PttSource::Dax is what lets it through the local interlock preflight with
    // no slice assigned, so this reaches the seam on a link-less model.
    model.setTransmit(true, TransmitModel::PttSource::Dax);
    check(edges.size() == 1, "HL2 key publishes one radioTransmittingChanged");
    check(!edges.isEmpty() && edges.first().first().toBool(),
          "HL2 key publishes radioTransmittingChanged(true)");
    check(model.isRadioTransmitting(), "HL2 key leaves isRadioTransmitting() true");

    // Idempotent: a repeat request is not a new edge, or clients would see a
    // fresh transmit session for every duplicate WSJT-X trx:true.
    model.setTransmit(true, TransmitModel::PttSource::Dax);
    check(edges.size() == 1, "a repeated HL2 key does not republish the edge");

    model.setTransmit(false, TransmitModel::PttSource::Dax);
    check(edges.size() == 2, "HL2 unkey publishes a second edge");
    check(edges.size() == 2 && !edges.at(1).first().toBool(),
          "HL2 unkey publishes radioTransmittingChanged(false)");
    check(!model.isRadioTransmitting(), "HL2 unkey leaves isRadioTransmitting() false");
}

// The MOX button and the PTT coordinator key through moxCommandIssued, NOT
// through setTransmit(). A fix applied to only one of the two leaves a TCI
// client blind to operator-initiated transmits -- which is the shape of bug
// that passes every bridge test and fails on the real button.
static void testMoxPathPublishesTheSameEdge()
{
    RadioModel model;
    model.connectToRadio(hl2Info());

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);

    model.transmitModel().setMox(true);
    check(edges.size() == 1 && edges.first().first().toBool(),
          "HL2 MOX-on publishes radioTransmittingChanged(true)");

    model.transmitModel().setMox(false);
    check(edges.size() == 2 && !edges.at(1).first().toBool(),
          "HL2 MOX-off publishes radioTransmittingChanged(false)");
}

// TUNE is the third keying path, and the one a fix applied to the other two
// silently misses. Hl2Backend::setTune() calls setKeying(), so a tune carrier
// IS a transmission — and TciServer's "already transmitting" guard reads
// isRadioTransmitting(), so leaving this edge unpublished let a TCI client key
// on top of a live tune carrier and drop the key out from under it on unkey.
static void testTunePathPublishesTheSameEdge()
{
    RadioModel model;
    model.connectToRadio(hl2Info());

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);

    // PttSource::Dax for the same reason the key test uses it: it is the one
    // source localPttInterlockMessage() lets through with no TX slice assigned,
    // and TCI/DAX-initiated tune is a real path (see TransmitModel::startTune).
    model.transmitModel().startTune(TransmitModel::PttSource::Dax);
    check(edges.size() == 1 && edges.first().first().toBool(),
          "HL2 TUNE-on publishes radioTransmittingChanged(true)");
    check(model.isRadioTransmitting(),
          "a tune carrier counts as the radio transmitting");

    model.transmitModel().stopTune();
    check(edges.size() == 2 && !edges.at(1).first().toBool(),
          "HL2 TUNE-off publishes radioTransmittingChanged(false)");
    check(!model.isRadioTransmitting(), "tune release clears the TX state");
}

// On Flex the edge is decoded from `interlock` status. A second publisher here
// would race the authoritative one and could report TX before the radio agreed.
static void testFlexEdgeStaysInterlockOwned()
{
    RadioModel model;
    model.connectToRadio(flexInfo());

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);
    model.setTransmit(true, TransmitModel::PttSource::Dax);
    model.transmitModel().setMox(true);
    model.transmitModel().startTune(TransmitModel::PttSource::Dax);
    check(edges.isEmpty(),
          "Flex publishes no raw-TX edge from a command; interlock owns it");
}

// ── Which command plane the radio speaks ──────────────────────────────────
static void testCommandPlanePredicate()
{
    RadioModel model;
    check(model.usesFlexCommandPlane(),
          "a default (Flex) model speaks the Flex command plane");

    model.connectToRadio(hl2Info());
    check(!model.usesFlexCommandPlane(),
          "HL2 does not speak the Flex command plane");
    check(model.panStream() == nullptr,
          "HL2 has no PanadapterStream, hence no DAX data plane");

    model.connectToRadio(flexInfo());
    check(model.usesFlexCommandPlane(),
          "round-trip: Flex speaks the Flex command plane again");
}

#ifdef HAVE_WEBSOCKETS
// The predicate that keeps prepareTxAudio() from arranging a radio-side DAX TX
// route on a radio that has no such thing.
static void testTciSeesHostModulation()
{
    RadioModel model;
    TciServer server(&model);
    check(!Hl2TciSignalingTest::hostModulating(server),
          "Flex modulates on-radio: TCI arranges the DAX TX route");

    model.connectToRadio(hl2Info());
    check(Hl2TciSignalingTest::hostModulating(server),
          "HL2 host-modulates: TCI skips the Flex DAX TX route entirely");
}
#endif

// ── Refusing to key, without faking a transmit ────────────────────────────
//
// setTransmit() refuses a key on a backend reporting canTransmit=false. MOX and
// TUNE do not go through it -- they reach the seam through mox/tuneCommandIssued
// -- and had no such test, so with the HL2 transmit gate closed the backend
// logged "key refused" and returned while this side published the raw-TX edge
// anyway. TciServer then broadcast trx:...,true; for a transmission that never
// happened, and its "already transmitting" guard rejected the next genuine TCI
// key: nothing on the air, and TCI keying dead until the flag cleared.
static void testRefusedKeyPublishesNoTransmitEdge()
{
    // Reproduce the gate the way the product closes it: an automation run with
    // no ALLOW_TX. Hl2Backend decides m_txAllowed once, in its constructor, so
    // the environment has to be set before the backend is built.
    qputenv("AETHER_AUTOMATION", "1");
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");

    RadioModel model;
    model.connectToRadio(hl2Info());
    check(!model.backendCapabilities().canTransmit,
          "fixture precondition: the HL2 transmit gate is closed");

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);

    model.transmitModel().setMox(true);
    check(edges.isEmpty(), "a refused MOX publishes no raw-TX edge");
    check(!model.isRadioTransmitting(),
          "a refused MOX leaves isRadioTransmitting() false");
    check(!model.transmitModel().isTransmitting(),
          "a refused MOX rolls back the optimistic MOX state");

    // TUNE is the path a fix applied only to MOX would miss, and the one that
    // latches: TransmitModel sets m_tune before the seam ever refuses.
    model.transmitModel().startTune(TransmitModel::PttSource::Dax);
    check(edges.isEmpty(), "a refused TUNE publishes no raw-TX edge");
    check(!model.transmitModel().isTuning(),
          "a refused TUNE does not leave the TUNE button latched on");

    // The TCI hardware-PTT path. It keys through requestPttOn() -> setMox(),
    // so it is refused in the preflight before any optimistic state is built.
    model.transmitModel().requestPttOn(TransmitModel::PttSource::TciHardware);
    check(edges.isEmpty(), "a refused TCI hardware PTT publishes no raw-TX edge");
    check(!model.transmitModel().isTransmitting(),
          "a refused TCI hardware PTT does not report a transmit");

    qunsetenv("AETHER_AUTOMATION");
}

#ifdef HAVE_WEBSOCKETS
// ── VFO B / split on a radio that has neither ─────────────────────────────
//
// WSJT-X with Split = Rig (or Fake It) sends split_enable:0,true; before it
// transmits. On a single-slice radio that resolves to RouteAction::Create,
// which issued a Flex `slice create` -- swallowed by a radio that speaks HPSDR,
// with a completion callback that therefore never ran. The route transition
// opened around it never closed, and handleTrxRequest() defers every trx:true
// while one is in flight, so WSJT-X could not transmit again for the rest of
// the connection with the rig still showing connected throughout.
//
// The capacity check ahead of the create does NOT catch this: maxSlices() is
// the model-string-derived Flex estimate, so a one-slice HL2 looks like it has
// room to make a second.
static void testSeamBackendCannotWedgeOnVfoB()
{
    RadioModel model;
    model.connectToRadio(hl2Info());

    QString error;
    if (!model.automationApplySliceFixture(0, QString(), &error)) {
        check(false, "fixture precondition: a slice exists to route from");
        std::fprintf(stderr, "  (%s)\n", qPrintable(error));
        return;
    }
    // The HL2's single slice IS its transmitter (Hl2Backend publishes
    // txSlice=true); the fixture seeds tx=0, so say so explicitly.
    SliceDelta txFlag;
    txFlag.txSlice = true;
    model.slice(0)->applyChanges(txFlag);
    // maxSlices() now reports what the BACKEND says, not the Flex model-string
    // estimate — so on a backend that is not connected (this fixture) it is 1,
    // and the CAPACITY guard is what refuses rather than the command-plane one.
    //
    // Which guard refuses is not the subject. The subject is that whichever one
    // does, it must not strand the route transition — so the precondition now
    // pins the honest capacity instead of an over-report, and every invariant
    // below is unchanged.
    check(model.maxSlices() == 1,
          "fixture precondition: maxSlices() reports the backend's own capacity");

    TciServer server(&model);
    QWebSocket client;

    // What handleSplitRequest() does for WSJT-X's split_enable:0,true; once
    // resolveVfoB() has returned RouteAction::Create: split already latched
    // optimistically, the confirmation held back until the route exists.
    Hl2TciSignalingTest::setSplitRequested(server, true);
    Hl2TciSignalingTest::createVfoB(server, &client,
                                    TciProtocol::VfoRequest{0, 1, 14074000},
                                    model.slice(0),
                                    QStringLiteral("split_enable:0,true;"), true);

    check(!Hl2TciSignalingTest::routeTransitionInFlight(server),
          "a refused VFO-B create leaves no route transition in flight");
    check(!Hl2TciSignalingTest::pendingVfoBCreate(server),
          "a refused VFO-B create leaves no create pending on a reply that "
          "will never arrive");
    check(!Hl2TciSignalingTest::splitRequested(server),
          "a refused split is rolled back, not left half-armed");

    // The whole point: the key that follows must not be deferred forever.
    Hl2TciSignalingTest::trx(server, &client,
                             TciProtocol::TrxRequest{0, true, QStringLiteral("tci")});
    check(!Hl2TciSignalingTest::hasPendingTrx(server),
          "the key after a refused split is handled, not queued behind a "
          "transition that never ends");

    // The plain channel-1 VFO route (WSJT-X's Fake It band hop) takes the same
    // path with no split confirmation to withdraw.
    Hl2TciSignalingTest::createVfoB(server, &client,
                                    TciProtocol::VfoRequest{0, 1, 7074000},
                                    model.slice(0), QString(), false);
    check(!Hl2TciSignalingTest::routeTransitionInFlight(server),
          "a refused VFO-B tune leaves no route transition in flight");
}

// promoteTxSliceAndContinue() has the same shape of trap: `slice set N tx=1` is
// Flex text with no seam counterpart, and every caller opens a route transition
// around it. A continuation that never runs strands that transition, so the
// contract on a seam backend is to ANSWER -- false is fine, silence is not.
static void testSeamBackendPromoteAlwaysAnswers()
{
    RadioModel model;
    model.connectToRadio(hl2Info());

    QString error;
    if (!model.automationApplySliceFixture(0, QString(), &error)) {
        check(false, "fixture precondition: a slice exists to promote");
        return;
    }
    check(!model.slice(0)->isTxSlice(),
          "fixture precondition: the FIXTURE seeded tx=0 on this slice");

    TciServer server(&model);
    bool answered = false, selected = false;
    Hl2TciSignalingTest::promote(server, 0, &answered, &selected);
    check(answered, "promoteTxSliceAndContinue answers on a seam backend");
    // There IS a seam verb now (IRadioBackend::setTxSlice), so this no longer
    // answers a flat "not selected". It asks the backend and reports what the
    // backend says — and the backend's answer for slice 0 is that it already
    // owns transmit, which it CONFIRMS by republishing rather than returning
    // silently. The fixture's seeded tx=0 was the disagreement, and the
    // backend's answer is the one that survives it.
    //
    // The contract this test exists for is unchanged and is the line above:
    // ANSWER. False is fine, silence strands the route transition.
    check(selected, "it reports the backend's answer: slice 0 already owns transmit");
    check(model.slice(0)->isTxSlice(),
          "and the backend's republish corrected the model's seeded tx=0");

    // A slice that is ALREADY the TX slice still succeeds without a command:
    // this is the path every HL2 TCI key takes, and it must not regress.
    SliceDelta txFlag;
    txFlag.txSlice = true;
    model.slice(0)->applyChanges(txFlag);
    Hl2TciSignalingTest::promote(server, 0, &answered, &selected);
    check(answered && selected,
          "an already-TX slice is selected with no command at all");
}
#endif

// ── TCI TX audio reaching a local modulator ───────────────────────────────
static void testHostModulatedTxAudio()
{
    AudioEngine audio;

    // The exact frame TciServer hands over: float32 interleaved stereo at
    // 24 kHz, already gain- and overflow-processed, L == R (WSJT-X duplicates).
    constexpr int kFrames = 8;
    QByteArray in(kFrames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(in.data());
    for (int n = 0; n < kFrames; ++n) {
        const float v = 0.25f * static_cast<float>(n - 4);   // spans negative and positive
        f[2 * n]     = v;
        f[2 * n + 1] = v;
    }

    // ---- Flex, no TX stream: nothing goes anywhere (unchanged behavior) ----
    {
        QSignalSpy monitor(&audio, &AudioEngine::txFinalMonitorPcmReady);
        QSignalSpy packets(&audio, &AudioEngine::txPacketReady);
        audio.setHostModulation(false);
        audio.feedDaxTxAudio(in);
        check(monitor.isEmpty() && packets.isEmpty(),
              "no TX stream and no host modulation: TCI audio is dropped");
    }

    // ---- Host-modulating backend: the local modulator gets it ----
    QSignalSpy monitor(&audio, &AudioEngine::txFinalMonitorPcmReady);
    QSignalSpy packets(&audio, &AudioEngine::txPacketReady);
    audio.setHostModulation(true);
    audio.feedDaxTxAudio(in);

    check(monitor.size() == 1,
          "host modulation: TCI audio reaches the final-monitor tap");
    check(packets.isEmpty(),
          "host modulation: no VITA-49 packet is built (there is no stream)");
    if (monitor.isEmpty())
        return;

    // TCI audio must arrive marked client-leveled: the sender owns its level
    // (WSJT-X's Pwr slider is a digital attenuator on this very stream), so
    // the HL2 modulator bypasses its ALC for it (#4796). The mic chain emits
    // this flag false; feedDaxTxAudio is the external-client path.
    check(monitor.first().size() >= 2 && monitor.first().at(1).toBool(),
          "host modulation: TCI audio is marked client-leveled at the tap");

    // MainWindow routes that tap to RadioModel::submitTxAudio(), whose HL2
    // implementation requires int16 interleaved stereo at 24 kHz and averages
    // L/R. Stereo must survive, or the averaging halves every sample.
    const QByteArray out = monitor.first().first().toByteArray();
    check(out.size() == kFrames * 2 * static_cast<int>(sizeof(qint16)),
          "host modulation: output is int16 STEREO, one sample per input sample");

    const auto* o = reinterpret_cast<const qint16*>(out.constData());
    bool converted = true;
    for (int n = 0; n < kFrames; ++n) {
        const qint16 want = static_cast<qint16>(f[2 * n] * 32768.0f);
        if (o[2 * n] != want || o[2 * n + 1] != want)
            converted = false;
    }
    check(converted, "host modulation: float32 -> int16 is a plain full-scale map");
}

// ── Per-mode default passband ─────────────────────────────────────────────
// WSJT-X selects DIGU. Before this table the HL2's passband was simply sticky,
// so arriving at DIGU from CW handed the decoder a ~500 Hz window and it
// decoded nothing -- with the mode visibly correct, which is what made it hard
// to see. A radio that owns its DSP echoes a mode-appropriate filter and heals
// this; we own the DSP, so nothing does.
static void testModeDefaultPassband()
{
    hl2::Hl2Backend backend;

    int low = 0, high = 0;
    int deltas = 0;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int, const SliceDelta& d) {
        if (d.filterLow)  low  = *d.filterLow;
        if (d.filterHigh) high = *d.filterHigh;
        ++deltas;
    });

    // Arrive at DIGU from CW, the case that broke: a CW-width filter must not
    // survive into the mode the decoder runs in.
    backend.setSliceMode(0, QStringLiteral("CWU"));
    const int cwWidth = high - low;
    check(cwWidth > 0 && cwWidth <= 900,
          "CWU gets a CW-width passband, not the SSB default");

    // "CW" is the spelling TciProtocol produces for TCI's `cw` and the one a
    // Flex reports; only "CWU" was recognised, so plain CW silently landed on
    // the USB fallback -- right mode indicator, wrong detector and passband.
    backend.setSliceMode(0, QStringLiteral("USB"));
    backend.setSliceMode(0, QStringLiteral("CW"));
    check(high - low <= 900, "plain \"CW\" is CW, not the USB fallback");

    backend.setSliceMode(0, QStringLiteral("DIGU"));
    check(low > 0 && high > 0, "DIGU passband is upper-sideband (both bounds positive)");
    check(high - low >= 2500,
          "DIGU passband is wide enough for WSJT-X's 3 kHz decode window");

    // DIGL is the mirror. A positive passband here is the bug SliceModel's
    // polarity normalizer would silently paper over, so assert the sign.
    backend.setSliceMode(0, QStringLiteral("DIGL"));
    check(low < 0 && high < 0, "DIGL passband is lower-sideband (both bounds negative)");
    check(high - low >= 2500, "DIGL passband is as wide as DIGU");

    // AM straddles the carrier -- an envelope detector fed a one-sided passband
    // is detecting a suppressed-carrier signal and sounds distorted, not silent.
    backend.setSliceMode(0, QStringLiteral("AM"));
    check(low < 0 && high > 0, "AM passband straddles the carrier");

    // An operator's own filter edit must survive until the NEXT mode change,
    // or every deliberate narrowing would be undone by a repeated mode set.
    backend.setSliceMode(0, QStringLiteral("DIGU"));
    backend.setSliceFilter(0, 500, 2000);
    backend.setSliceMode(0, QStringLiteral("DIGU"));
    check(low == 500 && high == 2000,
          "re-selecting the SAME mode leaves an operator filter edit alone");
}

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    // Before QCoreApplication: AudioEngine and RadioModel both read AppSettings,
    // and this test must not touch the operator's real configuration.
    TestSettingsProfile profile(QStringLiteral("hl2-tci-signaling-test"));
    QCoreApplication app(argc, argv);

    AetherSDR::testTransmitEdgeIsPublished();
    AetherSDR::testMoxPathPublishesTheSameEdge();
    AetherSDR::testTunePathPublishesTheSameEdge();
    AetherSDR::testFlexEdgeStaysInterlockOwned();
    AetherSDR::testCommandPlanePredicate();
#ifdef HAVE_WEBSOCKETS
    AetherSDR::testTciSeesHostModulation();
    AetherSDR::testSeamBackendCannotWedgeOnVfoB();
    AetherSDR::testSeamBackendPromoteAlwaysAnswers();
#endif
    AetherSDR::testHostModulatedTxAudio();
    AetherSDR::testModeDefaultPassband();
    // Last: it closes the HL2 transmit gate through the environment, and every
    // test above needs it open.
    AetherSDR::testRefusedKeyPublishesNoTransmitEdge();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "hl2_tci_signaling_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
