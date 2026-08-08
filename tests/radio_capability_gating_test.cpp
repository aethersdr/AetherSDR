// Capability-gated UI surfaces: hasProfiles, hasDaxStreams, hasExtendedDsp,
// hasSupplyVoltageTelemetry, and the three status-bar toggles
// (hasRadioSideCwKeyer / hasVoiceKeyer / hasFullDuplex).
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
//   status bar    the CWX / DVK / FDX labels are HIDDEN, not dimmed, on a radio
//                 whose firmware runs none of those verbs. Three separate flags
//                 rather than one ride on hasRadioSideDsp, because a family
//                 could plausibly have a voice keyer without full duplex. TWO
//                 neighbours in that row are NOT gated: ASR, because Copy
//                 Assist runs whisper on this host off the engine's post-DSP RX
//                 audio, and TNF, because a host-side notch is landing and the
//                 TNF / +TNF surfaces are what it will drive — gating either
//                 would remove a control that works or is about to (the
//                 EQ-applet mistake, one row over). The keyer F1-F12 SHORTCUTS
//                 carry the same two flags as their buttons: an
//                 ApplicationShortcut stays armed whether or not its label is
//                 on screen. hasVoiceKeyer is evaluated AHEAD of the SmartSDR+
//                 entitlement gate, which fails open on an unknown license
//                 (#4210) and would otherwise leave DVK live on every radio
//                 that reports none at all. Both flags are read through
//                 RadioModel::hasRadioSideCwKeyer() / hasVoiceKeyer(), because
//                 the `cwx` verb has six more entry points than the buttons —
//                 the FlexControl/Ulanzi macro action, the MQTT cw/transmit
//                 topic, TCI cw_msg / cw_macros, rigctl send_morse / stop_morse,
//                 SmartCAT KY and the bridge's `cwx` verb — and all of them ask
//                 the accessor.
//
// A NOTE ON WHAT THE HELPERS BELOW DO AND DO NOT PIN. This target links
// aethercore, not the GUI (CMakeLists.txt), so nothing here can call
// MainWindow::applyCapabilitiesToUi() or updateKeyerAvailability() directly.
// uiWouldShow() is therefore a MIRROR of the visibility expression and pins
// nothing on the MainWindow side — deleting a `caps.x &&` there leaves this
// green. What IS pinned for real: every backend's declaration, and, for the two
// keyers, the RadioModel accessor the gate is built on, which the shortcut
// helpers call rather than paraphrase.
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
#include "gui/DvkAvailabilityGate.h"
#include "core/RadioDiscovery.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/sim/SimBackend.h"

#include "TestEventLoop.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// This file is where the shared wait came from: it spun on an iteration count
// (`for (200x) processEvents(AllEvents, 10)`), which waits approximately zero
// because processEvents() strips WaitForMoreEvents and returns immediately on an
// empty queue. Both edge assertions below therefore passed on luck — on whether
// the emission happened to be queued already — and lost that luck on a loaded
// box (#4693). AetherTest::waitForSignal() is that fix, generalised; the trap
// itself is pinned as a negative case in tests/test_event_loop_test.cpp so it
// cannot quietly stop being a trap. See tests/TestEventLoop.h for the full
// account and for which helper to reach for.

// The exact expression MainWindow::applyCapabilitiesToUi() applies to every
// capability-gated surface. Stated once here so the assertions below read as
// "what would the UI do", not as a paraphrase of it.
static bool uiWouldShow(bool connected, bool declared)
{
    return !connected || declared;
}

// The expressions MainWindow::updateKeyerAvailability() applies to the two
// keyers' F1-F12 arming. Separate from uiWouldShow() because the BUTTON and the
// SHORTCUT are two gates: hiding the label leaves an ApplicationShortcut armed,
// which is how a keypress ends up silently doing nothing.
//
// These take the MODEL rather than a bool, so the capability half is the
// PRODUCTION accessor and not a paraphrase of it: RadioModel::hasRadioSideCwKeyer()
// / hasVoiceKeyer() carry the permissive disconnected rule themselves, and they
// are the same call MainWindow makes — and the same one the FlexControl macro
// action, the MQTT cw/transmit topic, TCI's cw_msg / cw_macros, rigctl's
// send_morse, SmartCAT's KY and the automation bridge's `cwx` verb make. Only the mode half is restated, because MainWindow is
// not linkable from this target (it links aethercore, not the GUI).
static bool cwxShortcutsWouldArm(const RadioModel& model, bool txModeIsCw)
{
    return model.hasRadioSideCwKeyer() && txModeIsCw;
}

static bool dvkShortcutsWouldArm(const RadioModel& model, bool txModeIsVoice)
{
    // The entitlement gate is the second input on a radio that HAS the feature;
    // with the mode true and no license reported it answers None (fails open),
    // so this expression isolates the capability, which is the new half.
    return model.hasVoiceKeyer()
           && dvkIndicatorBlocker(txModeIsVoice, /*licenseSeen=*/false,
                                  /*licenseEnabled=*/false)
                  == DvkIndicatorBlocker::None;
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
        // The three status-bar toggles. Same regression shape as the supply-rail
        // field above and worse in kind: these are shipping SmartSDR features
        // whose only implementation is a command-plane verb, so a field added
        // without touching FlexBackend deletes CWX, DVK or FDX from every Flex.
        check(caps.hasRadioSideCwKeyer,
              "Flex declares hasRadioSideCwKeyer (the `cwx` text buffer)");
        check(caps.hasVoiceKeyer,
              "Flex declares hasVoiceKeyer (the `dvk` recorder)");
        check(caps.hasFullDuplex,
              "Flex declares hasFullDuplex (radio set full_duplex_enabled=)");
        // Three flags, not one ride on hasRadioSideDsp. All three are true on a
        // Flex and false on both other backends, so the shipped set cannot
        // demonstrate that they are separable — assert it on the struct, which
        // is where a future merge would start. A family could plausibly have a
        // voice keyer without full duplex, or full duplex without either.
        RadioCapabilities statusBar;
        statusBar.hasRadioSideDsp = true;
        check(!statusBar.hasRadioSideCwKeyer
                  && !statusBar.hasVoiceKeyer
                  && !statusBar.hasFullDuplex,
              "hasRadioSideDsp implies none of the three status-bar capabilities");
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
        // The three status-bar toggles. The HL2 has no CW text buffer, no voice
        // recorder and no full-duplex setting, so all three labels go away
        // entirely rather than sitting permanently dim.
        check(!caps.hasRadioSideCwKeyer,
              "HL2 declares hasRadioSideCwKeyer=false (no text buffer)");
        check(!caps.hasVoiceKeyer,
              "HL2 declares hasVoiceKeyer=false (no on-radio recorder)");
        check(!caps.hasFullDuplex,
              "HL2 declares hasFullDuplex=false (exclusive T/R changeover)");
        // The keyer F1-F12 shortcuts, evaluated as updateKeyerAvailability()
        // evaluates them. These are ApplicationShortcuts that stay armed whether
        // or not their button is on screen, so hiding the labels is not enough:
        // without the capability in this expression an HL2 in CW keeps F1-F12
        // firing `cwx send` into a backend with no such verb.
        // DECLARED, on the struct. The accessors cannot be exercised here: the
        // HL2 fixture reaches the post-swap state without hardware, so
        // isConnected() is false and hasRadioSideCwKeyer() answers permissively
        // whatever the backend declares. The Sim block below is genuinely
        // connected and is where the accessor path is pinned.
        // hasVoiceKeyer is ANDed AHEAD of the SmartSDR+ entitlement gate, whose
        // unknown-entitlement rule fails OPEN (#4210 — the radio must say no
        // before the UI does). Right for a Flex mid-handshake, and exactly why
        // it cannot be the only gate: an HL2 never reports a license at all, so
        // on the entitlement alone the DVK button stays live forever.
        check(dvkIndicatorBlocker(/*txModeIsVoice=*/true,
                                  /*licenseSeen=*/false,
                                  /*licenseEnabled=*/false)
                  == DvkIndicatorBlocker::None,
              "the DVK entitlement gate alone fails open on a radio that reports "
              "no license — hasVoiceKeyer is what closes it");
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
        const bool relayFired = AetherTest::waitForSignal(spy);

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
        check(!caps.hasRadioSideCwKeyer,
              "Sim declares hasRadioSideCwKeyer=false");
        check(!caps.hasVoiceKeyer, "Sim declares hasVoiceKeyer=false");
        check(!caps.hasFullDuplex, "Sim declares hasFullDuplex=false");
        // The two keyer ACCESSORS, on the one backend in this file that really
        // connects — so this is the only place the permissive rule inside them
        // can be shown to be off rather than assumed. Five surfaces ask through
        // here: the status-bar gate, the FlexControl/Ulanzi CwxF1..F12 macro
        // action, the MQTT cw/transmit topic, TCI cw_msg / cw_macros, rigctl
        // send_morse / stop_morse, SmartCAT KY and the automation bridge's
        // `cwx` verb. All but the first reach CwxModel without passing the
        // status bar at all, and each would otherwise emit `cwx send` at a radio
        // with no such verb.
        check(!model.hasRadioSideCwKeyer(),
              "connected Sim: hasRadioSideCwKeyer() is false through the accessor");
        check(!model.hasVoiceKeyer(),
              "connected Sim: hasVoiceKeyer() is false through the accessor");
        check(!cwxShortcutsWouldArm(model, /*txModeIsCw=*/true),
              "connected + hasRadioSideCwKeyer=false disarms F1-F12 even in CW");
        check(!dvkShortcutsWouldArm(model, /*txModeIsVoice=*/true),
              "connected + hasVoiceKeyer=false disarms F1-F12 even in a voice mode");
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
        check(!uiWouldShow(model.isConnected(), caps.hasRadioSideCwKeyer),
              "connected + hasRadioSideCwKeyer=false hides CWX and its panel");
        check(!uiWouldShow(model.isConnected(), caps.hasVoiceKeyer),
              "connected + hasVoiceKeyer=false hides DVK and its panel");
        check(!uiWouldShow(model.isConnected(), caps.hasFullDuplex),
              "connected + hasFullDuplex=false hides FDX");
        // ASR sits in the same status-bar row and is deliberately NOT in that
        // list, which is why no assertion here hides it: AsrAudioTap feeds
        // whisper from the engine's post-DSP RX audio on THIS host, so Copy
        // Assist works on every family. There is no capability to gate it on,
        // and adding one would delete a working control — the mistake the EQ
        // applet already made and reverted
        // (docs/architecture/radio-capabilities-map.md).

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
        const bool relayFired = AetherTest::waitForSignal(spy);
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
        check(uiWouldShow(model.isConnected(), caps.hasRadioSideCwKeyer),
              "disconnected: the CWX indicator is restored");
        check(uiWouldShow(model.isConnected(), caps.hasVoiceKeyer),
              "disconnected: the DVK indicator is restored");
        check(uiWouldShow(model.isConnected(), caps.hasFullDuplex),
              "disconnected: the FDX indicator is restored");
        // The keyer shortcuts follow the buttons back. Disarmed while an HL2 was
        // attached, armed again with nothing attached and the TX slice in the
        // right mode — the same permissive rule, applied to the gate that is not
        // a widget.
        check(model.hasRadioSideCwKeyer() && model.hasVoiceKeyer(),
              "disconnected: both keyer accessors go permissive");
        check(cwxShortcutsWouldArm(model, /*txModeIsCw=*/true),
              "disconnected: F1-F12 rearm for CWX in a CW TX mode");
        check(dvkShortcutsWouldArm(model, /*txModeIsVoice=*/true),
              "disconnected: F1-F12 rearm for DVK in a voice TX mode");
    }

    // ---- The TX-intent callbacks are installed once too --------------------
    //
    // Same ownership rule as the connection-edge relay checked further down, on
    // the other three connections setupBackend() used to make with RadioModel on
    // both ends: rfPowerChanged, moxCommandIssued and tuneCommandIssued (sender
    // &m_transmitModel, a RadioModel value member; receiver `this`). They are
    // installed as one block, so counting one of them counts all three.
    // setupBackend() has run three times by here — ctor Flex, HL2, Sim — so a
    // per-backend installation leaves three live copies.
    //
    // Counted through the refusal cascade, the one publicly observable
    // consequence of a duplicate: the sim declares canTransmit=false, so a TUNE
    // intent is refused, and the refusal calls TransmitModel::stopTune(), which
    // re-emits tuneCommandIssued(false) unconditionally. One live callback =>
    // one unlatch. Nothing is keyed and nothing reaches a radio: the model is
    // disconnected, the backend is the simulator, and the path under test is the
    // one that REFUSES to transmit.
    //
    // The signal is invoked directly for the same reason the relay check does
    // it below — this asserts the WIRING, not TransmitModel's PTT preflight.
    // (If stopTune() ever stops re-emitting unconditionally the count changes;
    // this comment is the place to start reading when it does.)
    {
        check(!model.isConnected() && model.family() == QLatin1String("sim"),
              "TX-intent ownership check runs disconnected, on the sim backend");
        check(!model.backendCapabilities().canTransmit,
              "TX-intent ownership check runs against an RX-only backend");
        QSignalSpy spy(&model.transmitModel(), &TransmitModel::tuneCommandIssued);
        const bool invoked = QMetaObject::invokeMethod(
            &model.transmitModel(), "tuneCommandIssued", Qt::DirectConnection,
            Q_ARG(bool, true));
        check(invoked, "tuneCommandIssued can be invoked for the ownership check");
        check(spy.count() == 2,
              "family swaps leave exactly one TX-intent callback (one TUNE "
              "intent, one refusal unlatch)");
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
        check(caps.hasRadioSideCwKeyer && caps.hasVoiceKeyer
                  && caps.hasFullDuplex,
              "round-trip: Flex regains CWX / DVK / FDX after sim -> Flex");
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

    // ---- Family swaps do not duplicate the connection-edge relay -----------
    //
    // setupBackend() ran once for each of Flex -> HL2 -> Sim -> Flex above.
    // A RadioModel-owned connection installed there survives teardownBackend(),
    // so one connectionStateChanged edge would fan out once per family visited.
    // Invoke the signal synchronously to isolate this ownership invariant from
    // any backend's asynchronous connect lifecycle or capabilitiesChanged signal.
    //
    // The TX-power push installed beside the relay has no observable of its own
    // (it calls IRadioBackend::setTxPower and nothing else), but the two are one
    // adjacent block in the constructor, so this count stands for both — the
    // same reasoning the TX-intent check above uses for its three.
    {
        QSignalSpy spy(&model, &RadioModel::capabilitiesChanged);
        const bool invoked = QMetaObject::invokeMethod(
            &model, "connectionStateChanged", Qt::DirectConnection,
            Q_ARG(bool, true));
        check(invoked, "connectionStateChanged can be invoked for the ownership check");
        check(spy.count() == 1,
              "family swaps leave exactly one connection-edge capability relay");
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
