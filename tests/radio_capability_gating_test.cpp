// Capability-gated UI surfaces: hasProfiles, hasDaxStreams, hasExtendedDsp,
// hasSupplyVoltageTelemetry.
//
// The rule these guard (RadioCapabilities.h header comment, aetherd RFC §1) is
// that no call site asks "is this a Flex". A surface is gated on a DECLARED
// CAPABILITY named for the concept, so every check below reads a capability —
// never caps.family, and never a dynamic_cast to a backend type. A test that
// asserted the family would pass just as happily against the anti-pattern it
// exists to prevent.
//
// What is covered:
//   declaration   every backend sets every gated field EXPLICITLY. The struct
//                 defaults to false, so a backend that merely omits a field
//                 silently loses the feature — for Flex that is a regression,
//                 not a default. Asserting Flex=true is the guard against it.
//   relay         RadioModel::capabilitiesChanged fires on the connect and
//                 disconnect edges carrying both. It is the only signal
//                 MainWindow::applyCapabilitiesToUi() binds to, so a missing
//                 emission means a gated surface never updates.
//   permissive    the `!connected || caps.hasX` rule the GUI applies. With no
//                 radio attached there is nothing to be honest about, and a
//                 PROF applet that stayed hidden after unplugging reads as a
//                 fault. Evaluated here exactly as the GUI evaluates it.
//   supply volts  hasSupplyVoltageTelemetry gates the status bar's PA supply
//                 voltage readout. The label it hides is fed from the
//                 Flex-named "+13.8A" meter but repainted whenever PA
//                 TEMPERATURE changes, so a radio reporting only PA temp
//                 renders the 0.0f initialiser as a two-decimal measurement.
//                 Asserted on the CAPABILITY, so this stays true of any future
//                 family that reports no supply rail.
//   radio DSP     hasRadioSideDsp gates the radio's own NR/NB/ANF/NRL/ANFL/
//                 ANFT, the APD row and the WNB row. It must NOT gate the
//                 host-side equivalents — the AetherDSP modules and the
//                 Aetherial RX/TX EQ — which are the only audio DSP an operator
//                 has on a radio reporting false. The 8-band EQ applet was on
//                 this list until #4609 mapped its octave bands onto ClientEq
//                 for any backend without a Flex command plane; the control is
//                 no longer empty there, so it is no longer gated. WHICH of the
//                 two surfaces onto that shared ClientEq/ClientComp may write is
//                 a separate question, answered by core/HostVoiceChainPolicy.h
//                 and pinned by tests/host_voice_chain_policy_test.cpp.
//                 Asserted independent of
//                 hasExtendedDsp — the base set and the extra 8000-series
//                 filters are two different statements about a radio.
//   wf auto-black hasRadioSideWaterfallAutoBlack gates ONLY the HW position of
//                 the Display > Black Level button — the radio's per-tile level
//                 embedded in the waterfall stream. It must NOT gate the SW
//                 estimate, which is the only automatic floor an operator has
//                 on a radio reporting false, and it must go permissive on
//                 disconnect or the mask could never restore a stashed HW
//                 intent (docs/architecture/radio-capabilities-map.md). (#4606)
//   extended DSP  hasExtendedDspFilters() resolves through the BACKEND while
//                 connected and falls back to the model-name table when not,
//                 with Flex's answer unchanged on both routes.
//
// The connected-backend assertions run against SimBackend over the synthetic
// demo connection (RFC #4288): a demo RadioInfo takes RadioConnection's
// no-socket path, so isConnected() genuinely becomes true with no hardware and
// no network. That matters — the `!connected ||` half of the permissive rule is
// only meaningful if some case actually reaches the connected branch.
//
// Deliberately not covered here: the widget calls themselves
// (AppletPanel::setProfilesVisible / setDaxStreamsVisible, the menu actions).
// Those sit behind the full GUI link, which no test target takes; they are a
// one-line mapping off the values asserted below, proven on the running app
// with the automation bridge.

#include "models/RadioModel.h"
#include "models/ModelCapabilities.h"
#include "core/RadioDiscovery.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Wait for a queued cross-thread signal, or give up at a real deadline.
//
// The obvious `for (200x) processEvents(AllEvents, 10)` does NOT do this. That
// argument is a MAXIMUM, not a wait — processEvents() strips WaitForMoreEvents
// and returns immediately on an empty queue — so all 200 iterations drain in
// microseconds and the loop exits having waited approximately zero. It reads as
// a 2-second budget and is a zero-second one.
//
// Measured with a standalone Qt 6.10.3 probe firing a worker-thread signal
// after N ms. The old loop exited after 0 ms and MISSED it at every N tried
// (5, 50, 200, 1000, 3000) — it never waited at all. This form catches it at
// each of those and returns as the signal arrives (0 ms at N=0, 201 ms at
// N=200), timing out only past the deadline.
//
// So both edge assertions here passed on luck — on whether the emission
// happened to be queued already — which is why this test failed intermittently
// on assertions unrelated to whatever was under review (#4693).
//
// count() is tested FIRST because wait() waits for the NEXT emission and would
// miss one already delivered. 5 s is a timeout, not a duration: the normal path
// returns in well under a millisecond.
//
// Sliced rather than one wait(timeoutMs) call, deliberately. QSignalSpy exits
// its nested loop from the EMITTING thread, so a cross-thread emission racing
// loop entry can in principle lose the wake; the outer re-check of count() then
// recovers it at the cost of one slice, where a single un-sliced wait() would
// sit out the whole timeout. That race is REPORTED on Qt 6.11 (a 0 ms emission
// returning at 102 ms — one slice late, recovered), but it has not reproduced in
// either attempt to measure it: Qt 6.10.3/Windows, 440 trials across four
// emission delays with 220 under 14-way CPU contention, and Qt 6.11.1/Linux, 800
// trials over the same delays with half under load. Both runs put the sliced and
// un-sliced forms within a millisecond of each other at zero misses.
//
// The slicing stays on that asymmetry, then, not on a confirmed race: it costs
// nothing when it does not trigger, and if the report is right an un-sliced
// wait() turns one slice of latency into a full-timeout stall. So do not
// collapse these into a single wait() on the strength of a probe that comes back
// clean — that is the expected result on both versions measured so far.
//
// Retire this helper for AetherTest::waitForSignal() once #4699 lands. That PR
// consolidates the spellings of this wait into tests/TestEventLoop.h but does
// not touch this file, so until it is migrated the test that motivated the
// shared helper is the one place still carrying a private copy of it.
static bool waitForSpy(QSignalSpy& spy, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (spy.count() == 0 && !deadline.hasExpired())
        spy.wait(static_cast<int>(qMin<qint64>(100, deadline.remainingTime())));
    return spy.count() > 0;
}

// The exact expression MainWindow::applyCapabilitiesToUi() applies to every
// capability-gated surface. Stated once here so the assertions below read as
// "what would the UI do", not as a paraphrase of it.
static bool uiWouldShow(bool connected, bool declared)
{
    return !connected || declared;
}

// GPS presence has two layers: the family must support a position source and
// this connected unit must actually report one. Unlike the shared capability
// helper above, either connected-layer false is enough to hide the surface.
static bool gpsUiWouldShow(bool connected, bool familySupportsGps, bool unitHasGps)
{
    return !connected || (familySupportsGps && unitHasGps);
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

// The demo serial is what RadioConnection::isDemoTarget() keys on, so this
// connects for real without a socket.
static RadioInfo simInfo()
{
    RadioInfo i;
    i.family  = SimBackend::familyName();
    i.serial  = SimBackend::demoSerial();
    i.model   = SimBackend::demoModelName();
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Flex declares every gated capability ----------------------------
    //
    // These are the regression guard the traps warn about: RadioCapabilities
    // fields default to FALSE, so a new field added without touching
    // FlexBackend quietly deletes a shipping Flex feature. Each must be true
    // because FlexBackend SAYS so, not because it is the default.
    RadioModel model;
    {
        const RadioCapabilities caps = model.backendCapabilities();
        check(caps.hasProfiles,
              "Flex declares hasProfiles (global/TX/mic profiles are SmartSDR)");
        check(caps.hasDaxStreams,
              "Flex declares hasDaxStreams (DAX audio + DAX IQ)");
        check(caps.hasRadioSideDsp,
              "Flex declares hasRadioSideDsp (firmware NR/NB/ANF, APD, WNB)");
        check(caps.hasWaveforms,
              "Flex declares hasWaveforms (installable SmartSDR waveforms)");
        check(caps.hasMultiClientSessions,
              "Flex declares hasMultiClientSessions (multiFLEX)");
        check(caps.hasGpsLocation,
              "Flex declares hasGpsLocation (GPSDO / on-board GNSS)");
        // The one this field exists to protect: the struct default is false, so
        // adding the field without touching FlexBackend would silently delete a
        // readout that ships and works today.
        check(caps.hasSupplyVoltageTelemetry,
              "Flex declares hasSupplyVoltageTelemetry (the \"+13.8A\" meter)");
        // The two DSP flags are independent statements, not synonyms: the base
        // set and the extra 8000-series filters. A default Flex model string is
        // unknown to the platform table, so the narrower one is false here while
        // the base one is true — which is exactly why they cannot be merged.
        check(caps.hasRadioSideDsp && !caps.hasExtendedDsp,
              "hasRadioSideDsp and hasExtendedDsp are independent");
        check(caps.hasRadioSideWaterfallAutoBlack,
              "Flex declares hasRadioSideWaterfallAutoBlack (per-tile auto_black)");
        // Separate from hasRadioSideDsp on purpose: one is audio DSP driven by
        // command-plane verbs, the other a display-plane computation embedded in
        // the waterfall stream. Both happen to be true on a Flex and false on an
        // HL2, so the shipped backends cannot demonstrate the independence —
        // assert it on the struct, which is where merging them would start.
        RadioCapabilities separate;
        separate.hasRadioSideDsp = true;
        check(!separate.hasRadioSideWaterfallAutoBlack,
              "hasRadioSideDsp does not imply hasRadioSideWaterfallAutoBlack");
        // RFC #4603: the Flex persists its own operating state — an EMPTY
        // domain declaration is the load-bearing value here. If this ever
        // becomes non-empty, RadioStateMemory would start re-asserting
        // radio-owned state on connect (the #2465/#4126/#4261 bug class).
        check(caps.clientSettingsDomains
                  == RadioCapabilities::ClientSettingsDomains{},
              "Flex declares clientSettingsDomains EMPTY (radio-authoritative)");

        check(!gpsUiWouldShow(true, caps.hasGpsLocation, model.hasGpsHardware()),
              "connected GPSDO-less Flex hides the GPS stack");

        const auto applyOscillatorPresence = [&model](const char* key, bool present) {
            const QMap<QString, QString> status{
                {QString::fromLatin1(key), present ? QStringLiteral("1")
                                                   : QStringLiteral("0")}
            };
            const QString object = QStringLiteral("radio oscillator");
            return QMetaObject::invokeMethod(
                &model, "onStatusReceived", Qt::DirectConnection,
                QGenericArgument("QString", &object),
                QGenericArgument("QMap<QString,QString>", &status));
        };

        check(applyOscillatorPresence("gpsdo_present", true),
              "GPSDO presence fixture reached RadioModel");
        check(model.hasGpsHardware(),
              "gpsdo_present=1 marks an optional 6000-series GPSDO present");
        check(gpsUiWouldShow(true, caps.hasGpsLocation, model.hasGpsHardware()),
              "connected optional-GPSDO Flex shows the GPS stack");
        check(applyOscillatorPresence("gpsdo_present", false),
              "GPSDO absence fixture reached RadioModel");
        check(!model.hasGpsHardware(),
              "gpsdo_present=0 clears optional GPSDO presence");

        check(applyOscillatorPresence("gnss_present", true),
              "GNSS presence fixture reached RadioModel");
        check(model.hasGpsHardware(),
              "gnss_present=1 marks on-board GNSS present");
        check(applyOscillatorPresence("gnss_present", false),
              "GNSS absence fixture reached RadioModel");
        check(!model.hasGpsHardware(),
              "gnss_present=0 clears on-board GNSS presence instead of latching");

        check(applyOscillatorPresence("gpsdo_present", true),
              "GPSDO reconnect fixture reached RadioModel");
        check(QMetaObject::invokeMethod(
                  &model, "onDisconnected", Qt::DirectConnection),
              "disconnect fixture reached RadioModel");
        check(!model.hasGpsHardware(),
              "GPS presence does not leak from the previous radio session");
    }

    // ---- HL2 declares none of them ---------------------------------------
    //
    // connectToRadio() rebuilds the backend synchronously before any network
    // I/O, so an unroutable address reaches the post-swap state without
    // hardware.
    model.connectToRadio(hl2Info());
    {
        const RadioCapabilities caps = model.backendCapabilities();
        check(!caps.hasProfiles,
              "HL2 declares hasProfiles=false (no on-radio configuration store)");
        check(!caps.hasDaxStreams,
              "HL2 declares hasDaxStreams=false (one raw IQ feed, no stream plane)");
        check(!caps.hasExtendedDsp,
              "HL2 declares hasExtendedDsp=false");
        check(!caps.hasRadioSideDsp,
              "HL2 declares hasRadioSideDsp=false (host runs every noise module)");
        check(!caps.hasRadioSideWaterfallAutoBlack,
              "HL2 declares hasRadioSideWaterfallAutoBlack=false (no display engine)");
        check(!caps.hasWaveforms,
              "HL2 declares hasWaveforms=false");
        check(!caps.hasMultiClientSessions,
              "HL2 declares hasMultiClientSessions=false (one client owns it)");
        check(!caps.hasGpsLocation,
              "HL2 declares hasGpsLocation=false (no GNSS receiver on the board)");
        // PATEMP yes, "+13.8A" no. The temperature above the volts row is a
        // genuine HL2 reading and must keep working — this flag hides one label,
        // not the stack.
        check(!caps.hasSupplyVoltageTelemetry,
              "HL2 declares hasSupplyVoltageTelemetry=false (PATEMP, no +13.8A)");
        // RFC #4603: the HL2 persists nothing on-radio — the client is its
        // memory for exactly these declared domains (per-band drive/LNA maps
        // ride the extension document, RFC PR 3).
        using Domain = RadioCapabilities::ClientSettingsDomain;
        check(caps.clientSettingsDomains.testFlag(Domain::Tuning)
                  && caps.clientSettingsDomains.testFlag(Domain::Passband)
                  && caps.clientSettingsDomains.testFlag(Domain::SpanRate)
                  && caps.clientSettingsDomains.testFlag(Domain::RfGain)
                  && caps.clientSettingsDomains.testFlag(Domain::TxSetpoints),
              "HL2 declares the five client-owned settings domains");
        check(caps.clientSettingsDomains.testFlag(Domain::Memories),
              "HL2 declares Memories (the #4590 bank's channels are client-"
              "owned; the bank engages on persistsMemories and keeps its own "
              "shared document — RFC #4603 PR 6)");
    }

    // ---- Sim declares none of them, and is genuinely CONNECTED -----------
    {
        QSignalSpy spy(&model, &RadioModel::capabilitiesChanged);
        check(spy.isValid(), "capabilitiesChanged is a connectable signal");

        model.connectToRadio(simInfo());
        // The synthetic connect completes on a zero-delay timer, exactly as a
        // real socket connect would return before `connected` fires. Pump until
        // the RELAY has been seen, not until isConnected() flips: the connection
        // object lives on a worker thread and reaches Connected before its
        // queued signal has crossed to this one, so waiting on isConnected()
        // races the very emission under test.
        //
        // The relay is the anchor for BOTH checks below, so it is waited on and
        // asserted first. isConnected() reads the connection's atomic state, which
        // flips the instant the worker sets Connected — before the queued signal
        // chain has been delivered here. Asserting it before the relay has landed
        // means a worker that has not been scheduled at all inverts every
        // connected-branch assertion in this block, not just this one. (#4693)
        const bool relayFired = waitForSpy(spy);

        // Without it applyCapabilitiesToUi() never runs and every gated surface
        // keeps the previous radio's answer.
        check(relayFired,
              "relay: capabilitiesChanged fired on the connect edge");
        check(model.isConnected(),
              "synthetic demo connect reached the connected state");
        if (spy.count() > 0) {
            const QList<QVariant> args = spy.last();
            check(args.value(0).toBool(),
                  "relay: the connect edge reports connected=true");
        }

        const RadioCapabilities caps = model.backendCapabilities();
        check(!caps.hasProfiles,   "Sim declares hasProfiles=false");
        check(!caps.hasDaxStreams, "Sim declares hasDaxStreams=false");
        check(!caps.hasExtendedDsp, "Sim declares hasExtendedDsp=false");
        check(!caps.hasRadioSideDsp, "Sim declares hasRadioSideDsp=false");
        check(!caps.hasRadioSideWaterfallAutoBlack,
              "Sim declares hasRadioSideWaterfallAutoBlack=false");
        check(!caps.hasWaveforms, "Sim declares hasWaveforms=false");
        check(!caps.hasMultiClientSessions,
              "Sim declares hasMultiClientSessions=false");
        check(!caps.hasGpsLocation, "Sim declares hasGpsLocation=false");
        check(!caps.hasSupplyVoltageTelemetry,
              "Sim declares hasSupplyVoltageTelemetry=false");
        check(caps.clientSettingsDomains
                  == RadioCapabilities::ClientSettingsDomains{},
              "Sim declares clientSettingsDomains EMPTY (synthetic scene "
              "regenerates; nothing to remember)");

        // The surfaces the GUI drives off those flags, evaluated the way the
        // GUI evaluates them. A CONNECTED radio that says no means hidden.
        check(!uiWouldShow(model.isConnected(), caps.hasProfiles),
              "connected + hasProfiles=false hides PROF and the Profiles menu");
        check(!uiWouldShow(model.isConnected(), caps.hasDaxStreams),
              "connected + hasDaxStreams=false hides DAX, DAX-IQ and autostart");
        check(!uiWouldShow(model.isConnected(), caps.hasWaveforms),
              "connected + hasWaveforms=false hides File > Waveforms");
        check(!uiWouldShow(model.isConnected(), caps.hasMultiClientSessions),
              "connected + hasMultiClientSessions=false hides Settings > multiFLEX");
        check(!uiWouldShow(model.isConnected(), caps.hasGpsLocation),
              "connected + hasGpsLocation=false hides the GPS stack and its separator");
        check(!uiWouldShow(model.isConnected(), caps.hasSupplyVoltageTelemetry),
              "connected + hasSupplyVoltageTelemetry=false hides the volts readout");

        // hasRadioSideDsp reaches the UI through its own accessor, which applies
        // the permissive rule itself (there is no model-name table to fall back
        // to, so the fallback is "assume present"). Connected and declared false
        // is the one combination that hides NR/NB/ANF/NRL/ANFL/ANFT, the APD row
        // and the WNB row.
        check(!model.hasRadioSideDsp(),
              "connected Sim: hasRadioSideDsp() is false, radio DSP hidden");
        // Same accessor shape for the display-plane flag. False here is what
        // drops HW from the Black Level cycle; the SW estimate is not gated on
        // it and stays available, which is why nothing asserts it hidden.
        check(!model.hasRadioSideWaterfallAutoBlack(),
              "connected Sim: hasRadioSideWaterfallAutoBlack() is false, HW dropped");
        // The hardware EQ rides the same flag: EqualizerModel emits `eq RXsc`/
        // `eq TXsc`, command-plane verbs that reach nothing here. The Aetherial
        // RX/TX EQ is host-side and deliberately not covered by any capability.
        check(!uiWouldShow(model.isConnected(), caps.hasRadioSideDsp),
              "connected + hasRadioSideDsp=false hides the hardware EQ applet");

        // The reconciliation, on the branch that only exists while connected:
        // the answer now comes from the backend's declaration rather than from
        // capabilitiesFor(model). Both say false here, but only one of them was
        // consulted before this change.
        check(!model.hasExtendedDspFilters(),
              "connected Sim: hasExtendedDspFilters() reads the backend, false");
    }

    // ---- The permissive rule on disconnect --------------------------------
    //
    // Same false capabilities, no radio attached: every surface comes back. A
    // permanently hidden PROF applet after unplugging reads as a fault, not as
    // an accurate report about a radio that is no longer there.
    {
        QSignalSpy spy(&model, &RadioModel::capabilitiesChanged);
        model.disconnectFromRadio();
        // Same ordering as the connect edge above, for the same reason. (#4693)
        const bool relayFired = waitForSpy(spy);
        check(relayFired,
              "relay: capabilitiesChanged fired on the disconnect edge");
        check(!model.isConnected(), "disconnected from the synthetic demo radio");

        const RadioCapabilities caps = model.backendCapabilities();
        check(uiWouldShow(model.isConnected(), caps.hasProfiles),
              "disconnected: PROF is restored even though the last radio said no");
        check(uiWouldShow(model.isConnected(), caps.hasDaxStreams),
              "disconnected: DAX is restored even though the last radio said no");
        check(uiWouldShow(model.isConnected(), caps.hasWaveforms),
              "disconnected: File > Waveforms is restored");
        check(uiWouldShow(model.isConnected(), caps.hasMultiClientSessions),
              "disconnected: Settings > multiFLEX is restored");
        // The GPS button is the ONLY entry point to the location dialog, so a
        // gate that failed to reopen would strand the feature entirely after
        // unplugging a radio that happened to lack GNSS.
        check(uiWouldShow(model.isConnected(), caps.hasGpsLocation),
              "disconnected: the GPS stack is restored");
        check(uiWouldShow(model.isConnected(), caps.hasSupplyVoltageTelemetry),
              "disconnected: the supply-voltage readout is restored");
        check(model.hasRadioSideDsp(),
              "disconnected: hasRadioSideDsp() goes permissive, radio DSP back");
        // Load-bearing for the auto-black MASK, not just for tidiness: the mask
        // never rewrites the stored HW intent, so the only thing that can bring
        // HW back on the button is this flag going permissive again. If it
        // stayed false after unplugging an HL2, a Flex user's stashed HW would
        // be invisible until they reconnected — the exact failure the mask
        // design exists to prevent. (#4606)
        check(model.hasRadioSideWaterfallAutoBlack(),
              "disconnected: hasRadioSideWaterfallAutoBlack() goes permissive, HW back");
    }

    // ---- Round-trip back to Flex ------------------------------------------
    //
    // Capabilities track the LIVE backend, so nothing an earlier family
    // declared may linger. This is the shape that would catch a cached flag.
    model.connectToRadio(flexInfo());
    {
        const RadioCapabilities caps = model.backendCapabilities();
        check(caps.hasProfiles,
              "round-trip: Flex regains hasProfiles after sim -> Flex");
        check(caps.hasDaxStreams,
              "round-trip: Flex regains hasDaxStreams after sim -> Flex");
        check(caps.hasRadioSideDsp,
              "round-trip: Flex regains hasRadioSideDsp after sim -> Flex");
        check(caps.hasRadioSideWaterfallAutoBlack,
              "round-trip: Flex regains hasRadioSideWaterfallAutoBlack");
        check(caps.hasWaveforms,
              "round-trip: Flex regains hasWaveforms after sim -> Flex");
        check(caps.hasMultiClientSessions,
              "round-trip: Flex regains hasMultiClientSessions after sim -> Flex");
        // The one that would catch a cached flag: the sim declares
        // hasGpsLocation=false, so a stale value here hides the GPS stack on a
        // Flex that has a GPSDO.
        check(caps.hasGpsLocation,
              "round-trip: Flex regains hasGpsLocation after sim -> Flex");
        // Flex -> HL2/Sim -> Flex must leave the status bar identical to
        // baseline. A cached flag would show up right here.
        check(caps.hasSupplyVoltageTelemetry,
              "round-trip: Flex regains hasSupplyVoltageTelemetry after sim -> Flex");
    }

    // ---- Flex extended DSP is unchanged by the reconciliation -------------
    //
    // The byte-identical claim: FlexBackend computes caps.hasExtendedDsp as
    // capabilitiesFor(model).hasExtendedDsp(), so for every Flex model the
    // backend route and the table route are the same lookup with the same key.
    // Driven through FlexBackend's own model provider rather than RadioModel,
    // because m_model is only settable from wire status. Checked across an
    // extended-DSP platform, two plain 6000-series radios, and the "S" server
    // variant the old substring form used to miss.
    {
        QString modelName;
        FlexBackend flex;
        flex.setModelProvider([&modelName] { return modelName; });
        for (const char* name : {"AU-510", "MLS-9601", "CLS-9301",
                                 "FLEX-6700", "FLEX-6400", ""}) {
            modelName = QString::fromLatin1(name);
            const bool viaTable   = capabilitiesFor(modelName).hasExtendedDsp();
            const bool viaBackend = flex.capabilities().hasExtendedDsp;
            check(viaTable == viaBackend,
                  "Flex: backend hasExtendedDsp agrees with the model-name table");
        }
        // And the platform table itself still distinguishes them, so the
        // agreement above is not two constant falses agreeing.
        check(capabilitiesFor(QStringLiteral("AU-510")).hasExtendedDsp(),
              "AU-510 is an extended-DSP platform (the check has teeth)");
        check(!capabilitiesFor(QStringLiteral("FLEX-6700")).hasExtendedDsp(),
              "FLEX-6700 is not (the check has teeth)");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "radio_capability_gating_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
