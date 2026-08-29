// Capability-gated UI surfaces: hasProfiles, hasDaxStreams, hasExtendedDsp,
// hasSupplyVoltageTelemetry, hasMainFanTelemetry,
// hasTransmitFrequencyCheck, and the three status-bar toggles
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
//   main fan      hasMainFanTelemetry gates the Radio Vitals Main Fan gauge.
//                 Unsupported radios omit the instrument rather than showing
//                 an empty scale, while disconnect restores the permissive
//                 surface for the next session.
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
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "models/ModelCapabilities.h"
#include "gui/DvkAvailabilityGate.h"
#include "gui/VoiceModeGate.h"
#include "core/RadioDiscovery.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/sim/SimBackend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"

#include "TestEventLoop.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR::icom {

// Select model-table data without opening an IcomSession or any socket. The
// backend already grants this focused test accessor for deterministic state
// injection; each test binary supplies only the operations it needs.
struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }

    static void deliverDialLock(IcomCivBackend& backend, int value)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = 1;
        CivFrame frame;
        frame.cmd = cmd::kFunction;
        frame.hasSub = true;
        frame.sub = func::kDialLock;
        frame.data = {static_cast<std::uint8_t>(value)};
        backend.onCivFrame(frame, 1);
    }

    static void prepareDialLockWrite(IcomCivBackend& backend,
                                     const IcomModel& model)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_session = std::make_unique<IcomSession>();
        backend.m_session->setCivAddress(model.civAddress);
    }

    static QString lastOutboundCiv(const IcomCivBackend& backend)
    {
        return backend.m_lastOutboundCiv;
    }

    static std::uint8_t activeCivAddress(const IcomCivBackend& backend)
    {
        return backend.m_session ? backend.m_session->civAddress() : 0;
    }
};

}  // namespace AetherSDR::icom

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
    TestSettingsProfile profile(QStringLiteral("radio-capability-gating-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

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
        check(caps.receiveOnlyModes.isEmpty(),
              "Flex declares no receive-only modes (it transmits in everything "
              "it demodulates), so the mode key guard is inert on it");
        check(caps.txPowerBands.isEmpty(),
              "Flex explicitly leaves per-band TX power limits empty");
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
        check(caps.hasPaTemperatureTelemetry,
              "Flex declares hasPaTemperatureTelemetry (the PATEMP meter)");
        check(caps.hasMainFanTelemetry,
              "Flex declares hasMainFanTelemetry (the MAINFAN meter)");
        // The two DSP flags are independent statements, not synonyms: the base
        // set and the extra 8000-series filters. A default Flex model string is
        // unknown to the platform table, so the narrower one is false here while
        // the base one is true — which is exactly why they cannot be merged.
        check(caps.hasRadioSideDsp && !caps.hasExtendedDsp,
              "hasRadioSideDsp and hasExtendedDsp are independent");
        check(caps.hasRadioSideWaterfallAutoBlack,
              "Flex declares hasRadioSideWaterfallAutoBlack (per-tile auto_black)");
        check(!caps.hasTransmitFrequencyCheck,
              "Flex declares hasTransmitFrequencyCheck=false (REV is local state)");
        check(caps.fmTonePresentation == FmTonePresentation::Legacy
                  && caps.fmToneModes.isEmpty(),
              "Flex retains its legacy FM-tone presentation and labels");
        // The three status-bar toggles. Same regression shape as the supply-rail
        // field above and worse in kind: these are shipping SmartSDR features
        // whose only implementation is a command-plane verb, so a field added
        // without touching FlexBackend deletes CWX, DVK or FDX from every Flex.
        check(caps.hasRadioSideCwKeyer,
              "Flex declares hasRadioSideCwKeyer (the `cwx` text buffer)");
        check(caps.hasVoiceKeyer,
              "Flex declares hasVoiceKeyer (the `dvk` recorder)");
        check(caps.hasDownwardExpander,
              "Flex preserves its authoritative DEXP compander surface");
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
        check(caps.receiveOnlyModes.isEmpty(),
              "HL2 declares no receive-only modes — it modulates on this host, "
              "so there is no mode it hears and cannot send");
        check(caps.txPowerBands.isEmpty(),
              "HL2 explicitly leaves per-band TX power limits empty");
        check(!caps.hasDaxStreams,
              "HL2 declares hasDaxStreams=false (one raw IQ feed, no stream plane)");
        check(!caps.hasExtendedDsp,
              "HL2 declares hasExtendedDsp=false");
        check(!caps.hasRadioSideDsp,
              "HL2 declares hasRadioSideDsp=false (host runs every noise module)");
        check(!caps.hasRadioSideWaterfallAutoBlack,
              "HL2 declares hasRadioSideWaterfallAutoBlack=false (no display engine)");
        check(!caps.hasTransmitFrequencyCheck,
              "HL2 declares hasTransmitFrequencyCheck=false");
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
        check(caps.hasPaTemperatureTelemetry,
              "HL2 declares hasPaTemperatureTelemetry (host-decoded PATEMP)");
        check(!caps.hasMainFanTelemetry,
              "HL2 declares hasMainFanTelemetry=false");
        // The three status-bar toggles. The HL2 has no CW text buffer, no voice
        // recorder and no full-duplex setting, so all three labels go away
        // entirely rather than sitting permanently dim.
        check(!caps.hasRadioSideCwKeyer,
              "HL2 declares hasRadioSideCwKeyer=false (no text buffer)");
        check(!caps.hasVoiceKeyer,
              "HL2 declares hasVoiceKeyer=false (no on-radio recorder)");
        check(!caps.hasDownwardExpander,
              "HL2 declares hasDownwardExpander=false (no command path)");
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

        // CW-down is a transmit start even though the carrier and PTT envelope
        // are separate below the seam. Pin both public element entry points:
        // a receive-only pan must stop them before Hl2Backend sees the edge,
        // while release remains unconditional. The final uninhibited edge is
        // the control proving this fixture really can cross the backend seam.
        QString fixtureError;
        check(model.automationApplySliceFixture(0, QString(), &fixtureError),
              "CW inhibit fixture creates a slice");
        SliceModel* cwSlice = model.slice(0);
        check(cwSlice != nullptr, "CW inhibit fixture resolves its slice");
        if (cwSlice) {
            SliceDelta txOn;
            txOn.txSlice = true;
            txOn.panId = QStringLiteral("0");
            cwSlice->applyChanges(txOn);
            const QString panId = cwSlice->panId();
            check(model.txSlice() == cwSlice,
                  "CW inhibit fixture assigns the transmit slice");
            check(!panId.isEmpty(), "CW inhibit fixture has a pan identity");
            model.setPanTransmitInhibited(
                panId, true, QStringLiteral("receive-only CW regression"));
            check(model.panTransmitInhibited(panId),
                  "CW inhibit fixture marks its pan receive-only");
            QSignalSpy keyEdgeSpy(&model, &RadioModel::cwKeyDownChanged);
            QSignalSpy forwardedSpy(&model,
                                    &RadioModel::backendCwKeyingForwarded);

            model.sendCwKey(true);
            check(forwardedSpy.count() == 0,
                  "inhibited straight-key down never crosses the backend seam");
            model.sendCwKey(false);  // release must always be accepted
            check(forwardedSpy.count() == 1
                      && !forwardedSpy.last().value(0).toBool(),
                  "straight-key release crosses the seam despite the inhibit");

            model.sendCwKeyEdge(true);
            check(forwardedSpy.count() == 1,
                  "inhibited iambic down never crosses the backend seam");
            model.sendCwKeyEdge(false);  // release must always be accepted
            check(forwardedSpy.count() == 2
                      && !forwardedSpy.last().value(0).toBool(),
                  "iambic release crosses the seam despite the inhibit");
            check(keyEdgeSpy.count() == 0,
                  "refused CW downs do not publish false key-active state");

            model.setPanTransmitInhibited(panId, false);
            check(!model.panTransmitInhibited(panId),
                  "CW inhibit fixture can clear the receive-only state");
            model.sendCwKeyEdge(true);
            check(forwardedSpy.count() == 3
                      && forwardedSpy.last().value(0).toBool(),
                  "the same uninhibited down crosses the backend seam");
            check(keyEdgeSpy.count() == 1,
                  "an accepted down publishes one key-active edge");
            model.sendCwKeyEdge(false);
        }
    }

    // ---- Icom model capabilities and CI-V dial lock without a socket -------
    {
        using namespace AetherSDR::icom;
        const IcomModel* ic9700 = modelForName("IC-9700");
        check(ic9700 != nullptr, "the IC-9700 resolves from the Icom model table");
        // Guarded as a block: without the guard a failed lookup would run the
        // six checks below against the constructor's unknownModel() seed and
        // report six misleading capability failures for one missing table row.
        if (ic9700) {
            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *ic9700);
            const RadioCapabilities caps = backend.capabilities();
            check(caps.txPowerBands.size() == 3,
                  "Icom declares the three IC-9700 per-band TX power limits");
            check(caps.txPowerMaxWattsAt(146'000'000.0) == 100.0,
                  "the IC-9700 2 m capability clamps TX power to 100 W");
            check(caps.txPowerMaxWattsAt(432'000'000.0) == 75.0,
                  "the IC-9700 70 cm capability clamps TX power to 75 W");
            check(caps.txPowerMaxWattsAt(1'296'000'000.0) == 10.0,
                  "the IC-9700 23 cm capability clamps TX power to 10 W");
            check(caps.hasTransmitFrequencyCheck,
                  "Icom declares the profiled IC-9700 momentary XFC command");
            check(caps.hasSupplyVoltageTelemetry,
                  "Icom declares the profiled IC-9700 supply-voltage telemetry");
            check(!caps.hasMainFanTelemetry,
                  "Icom declares no Main Fan telemetry family-wide");
            check(caps.speechProcessorLevelMaximum == 100
                      && caps.speechProcessorLabel == QStringLiteral("COMP"),
                  "IC-9700 alone declares the continuous COMP presentation");
            check(caps.fmDtcsCodes.size() == 104
                      && caps.fmToneModes.contains(QStringLiteral("dtcs_txrx")),
                  "IC-9700 declares the complete DTCS operator vocabulary");
        }

        {
            const IcomModel* ic705 = modelForName("IC-705");
            check(ic705 != nullptr, "the IC-705 model resolves");
            if (ic705) {
                IcomCivBackend backend;
                IcomCivBackendTestAccess::selectModel(backend, *ic705);
                const RadioCapabilities caps = backend.capabilities();
                check(caps.speechProcessorLevelMaximum == 2
                          && caps.speechProcessorLabel == QStringLiteral("PROC"),
                      "IC-705 retains the legacy PROC presentation");
                check(caps.fmDtcsCodes.size() == 104
                          && caps.fmToneModes.contains(QStringLiteral("dtcs_txrx")),
                      "IC-705 declares its documented DTCS operator vocabulary");
            }
        }

        {
            const IcomModel* sibling = modelForName("IC-7300MK2");
            check(sibling != nullptr, "the protected IC-7300MK2 model resolves");
            if (sibling) {
                IcomCivBackend backend;
                IcomCivBackendTestAccess::selectModel(backend, *sibling);
                const RadioCapabilities caps = backend.capabilities();
                check(caps.speechProcessorLevelMaximum == 2
                          && caps.speechProcessorLabel == QStringLiteral("PROC"),
                      "IC-7300MK2 retains the legacy PROC presentation");
                check(caps.fmDtcsCodes.isEmpty(),
                      "Icom models without documented DTCS do not activate controls");
            }
        }

        struct DialLockCase {
            const char* modelName;
            std::uint8_t civAddress;
        };
        constexpr DialLockCase dialLockCases[] = {
            {"IC-705", 0xA4},
            {"IC-7300MK2", 0xB6},
            {"IC-9700", 0xA2},
        };
        for (const DialLockCase& lockCase : dialLockCases) {
            const IcomModel* lockModel = modelForName(lockCase.modelName);
            check(lockModel != nullptr,
                  "the profiled dial-lock Icom model resolves");
            if (!lockModel) {
                continue;
            }
            check(lockModel->civAddress == lockCase.civAddress,
                  "the dial-lock model retains its official CI-V address");
            check(profileFor(*lockModel).supports(IcomFeature::DialLock),
                  "the model profile attests CI-V dial-lock support");

            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *lockModel);
            check(backend.capabilities().hasRadioDialLock,
                  "the profiled model publishes radio-authoritative dial lock");

            IcomCivBackend lockWriter;
            IcomCivBackendTestAccess::prepareDialLockWrite(lockWriter, *lockModel);
            check(IcomCivBackendTestAccess::activeCivAddress(lockWriter)
                      == lockCase.civAddress,
                  "dial lock targets the selected model's CI-V address");
            lockWriter.setRadioDialLock(true);
            check(IcomCivBackendTestAccess::lastOutboundCiv(lockWriter)
                      == QStringLiteral("16 50 01"),
                  "dial lock sends CI-V 16 50 01");

            IcomCivBackend unlockWriter;
            IcomCivBackendTestAccess::prepareDialLockWrite(unlockWriter, *lockModel);
            unlockWriter.setRadioDialLock(false);
            check(IcomCivBackendTestAccess::lastOutboundCiv(unlockWriter)
                      == QStringLiteral("16 50 00"),
                  "dial unlock sends CI-V 16 50 00");

            QSignalSpy lockSpy(&backend, &IRadioBackend::radioDialLockChanged);
            IcomCivBackendTestAccess::deliverDialLock(backend, 1);
            check(lockSpy.count() == 1 && lockSpy.takeFirst().at(0).toBool(),
                  "dial-lock readback publishes the locked state");
            IcomCivBackendTestAccess::deliverDialLock(backend, 1);
            check(lockSpy.isEmpty(),
                  "unchanged dial-lock polling does not republish state");
            IcomCivBackendTestAccess::deliverDialLock(backend, 0);
            check(lockSpy.count() == 1 && !lockSpy.takeFirst().at(0).toBool(),
                  "front-panel unlock readback publishes the unlocked state");
        }

        const IcomModel* unprofiled = modelForName("IC-7610");
        check(unprofiled != nullptr, "the unprofiled Icom model resolves");
        if (unprofiled) {
            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *unprofiled);
            check(!backend.capabilities().hasRadioDialLock,
                  "an unattested Icom model does not inherit dial lock");
            IcomCivBackendTestAccess::prepareDialLockWrite(backend, *unprofiled);
            backend.setRadioDialLock(true);
            check(IcomCivBackendTestAccess::lastOutboundCiv(backend).isEmpty(),
                  "an unattested Icom model emits no dial-lock command");
        }
    }

    // ---- A radio-global dial lock reaches every slice surface ------------
    {
        RadioModel fanoutModel;
        QString fixtureError;
        check(fanoutModel.automationApplySliceFixture(0, QString(), &fixtureError),
              "dial-lock fan-out fixture creates slice 0");
        check(fanoutModel.automationApplySliceFixture(1, QString(), &fixtureError),
              "dial-lock fan-out fixture creates slice 1");
        SliceModel* first = fanoutModel.slice(0);
        SliceModel* second = fanoutModel.slice(1);
        check(first && second, "dial-lock fan-out fixture resolves both slices");
        if (first && second) {
            const bool invoked = QMetaObject::invokeMethod(
                fanoutModel.backend(), "radioDialLockChanged",
                Qt::DirectConnection, Q_ARG(bool, true));
            check(invoked, "backend dial-lock signal is invokable through the seam");
            check(first->isLocked() && second->isLocked(),
                  "radio-authoritative lock fans out to every slice surface");
            QMetaObject::invokeMethod(fanoutModel.backend(), "radioDialLockChanged",
                                      Qt::DirectConnection, Q_ARG(bool, false));
            check(!first->isLocked() && !second->isLocked(),
                  "radio-authoritative unlock clears every slice surface");
        }
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
        check(caps.txPowerBands.isEmpty(),
              "Sim explicitly leaves per-band TX power limits empty");
        check(!caps.hasDaxStreams, "Sim declares hasDaxStreams=false");
        check(!caps.hasExtendedDsp, "Sim declares hasExtendedDsp=false");
        check(!caps.hasRadioSideDsp, "Sim declares hasRadioSideDsp=false");
        check(!caps.hasRadioSideWaterfallAutoBlack,
              "Sim declares hasRadioSideWaterfallAutoBlack=false");
        check(!caps.hasTransmitFrequencyCheck,
              "Sim declares hasTransmitFrequencyCheck=false");
        check(!caps.hasWaveforms, "Sim declares hasWaveforms=false");
        check(!caps.hasMultiClientSessions,
              "Sim declares hasMultiClientSessions=false");
        check(!caps.hasGpsLocation, "Sim declares hasGpsLocation=false");
        check(!caps.hasSupplyVoltageTelemetry,
              "Sim declares hasSupplyVoltageTelemetry=false");
        check(!caps.hasPaTemperatureTelemetry,
              "Sim declares hasPaTemperatureTelemetry=false");
        check(!caps.hasMainFanTelemetry,
              "Sim declares hasMainFanTelemetry=false");
        check(!caps.hasRadioSideCwKeyer,
              "Sim declares hasRadioSideCwKeyer=false");
        check(!caps.hasVoiceKeyer, "Sim declares hasVoiceKeyer=false");
        check(!caps.hasDownwardExpander,
              "Sim declares hasDownwardExpander=false");
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
        QSignalSpy cwForwardedSpy(&model,
                                  &RadioModel::backendCwKeyingForwarded);
        model.sendCwKey(true);
        check(cwForwardedSpy.count() == 0,
              "CW down never crosses the seam of a receive-only backend");
        model.sendCwKey(false);
        check(cwForwardedSpy.count() == 1
                  && !cwForwardedSpy.last().value(0).toBool(),
              "CW release still crosses a receive-only backend seam");
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

    // ---- hasLmsNoiseFilters / hasManualNotch: the two newest gates ---------
    //
    // Both exist because hasRadioSideDsp was two claims wearing one name — "the
    // radio runs its own receive DSP" and "the radio runs FLEXRADIO'S PARTICULAR
    // SET of it". An Icom is the first and not the second, so that one flag lit
    // up NRL/ANFL/ANFT buttons reaching no register on the radio at all.
    //
    // THE TWO FIELDS TAKE OPPOSITE DISCONNECTED DEFAULTS, and that asymmetry is
    // the thing worth pinning. hasLmsNoiseFilters is PERMISSIVE — the buttons
    // ship today and must not vanish from an empty window. hasManualNotch is
    // NOT — MN is a NEW button, and a permissive default would put it on every
    // Flex on screen before any backend had claimed it. An inverted default is
    // exactly what a later refactor "tidies" in the wrong direction, and the
    // failure is silent: a control that appears, wired to nothing.
    {
        // The struct's own default, which is what a backend that forgets the
        // field gets. False for both — absent unless declared.
        const RadioCapabilities fresh;
        check(!fresh.hasManualNotch,
              "RadioCapabilities defaults hasManualNotch to false (absent unless declared)");
        check(!fresh.hasLmsNoiseFilters,
              "RadioCapabilities defaults hasLmsNoiseFilters to false (absent unless declared)");
        check(!fresh.hasPaTemperatureTelemetry,
              "RadioCapabilities defaults PA temperature telemetry to absent");
        check(!fresh.hasMainFanTelemetry,
              "RadioCapabilities defaults Main Fan telemetry to absent");
        check(!fresh.alwaysUseClientSideSpots,
              "RadioCapabilities defaults to the existing operator spot policy");
        check(fresh.speechProcessorLevelMaximum == 2
                  && fresh.speechProcessorLabel == QStringLiteral("PROC"),
              "RadioCapabilities defaults to the legacy PROC presentation");

        // Read from each backend's DECLARATION rather than restating it, so a
        // copy-paste that flips either one reds this suite.
        FlexBackend flex;
        hl2::Hl2Backend hl2;
        SimBackend sim;
        AetherSDR::icom::IcomCivBackend icom;
        const RadioCapabilities flexCaps = flex.capabilities();
        const RadioCapabilities hl2Caps = hl2.capabilities();
        const RadioCapabilities simCaps = sim.capabilities();
        const RadioCapabilities icomCaps = icom.capabilities();

        check(!flexCaps.alwaysUseClientSideSpots,
              "Flex keeps its radio-side spot publication behavior");
        check(!hl2Caps.alwaysUseClientSideSpots,
              "HL2 keeps its existing operator-controlled spot behavior");
        check(!simCaps.alwaysUseClientSideSpots,
              "Sim keeps its existing operator-controlled spot behavior");
        check(icomCaps.alwaysUseClientSideSpots,
              "Icom forces SpotHub spots through the passive client model");
        check(fresh.fmDtcsCodes.isEmpty() && flexCaps.fmDtcsCodes.isEmpty()
                  && hl2Caps.fmDtcsCodes.isEmpty() && simCaps.fmDtcsCodes.isEmpty()
                  && icomCaps.fmDtcsCodes.isEmpty(),
              "DTCS defaults and backends without an active Icom profile stay empty");
        check(flexCaps.speechProcessorLevelMaximum == 2
                  && flexCaps.speechProcessorLabel == QStringLiteral("PROC"),
              "Flex retains the legacy PROC presentation");
        check(hl2Caps.speechProcessorLevelMaximum == 2
                  && hl2Caps.speechProcessorLabel == QStringLiteral("PROC"),
              "HL2 retains the legacy PROC presentation");
        check(simCaps.speechProcessorLevelMaximum == 2
                  && simCaps.speechProcessorLabel == QStringLiteral("PROC"),
              "Sim retains the legacy PROC presentation");
        check(icomCaps.speechProcessorLevelMaximum == 2
                  && icomCaps.speechProcessorLabel == QStringLiteral("PROC"),
              "an unidentified Icom retains the legacy PROC presentation");

        check(!fresh.hasTxFilterControls,
              "RadioCapabilities defaults TX cutoff controls to absent");
        check(flexCaps.hasTxFilterControls,
              "Flex explicitly retains its continuous TX cutoff controls");
        check(hl2Caps.hasTxFilterControls,
              "HL2 explicitly retains its host-modulated TX cutoff controls");
        check(!simCaps.hasTxFilterControls,
              "Sim explicitly omits TX cutoff controls because it is RX-only");
        check(!icomCaps.hasTxFilterControls,
              "an unidentified Icom cannot surface an unverified TX cutoff editor");
        check(uiWouldShow(/*connected=*/false, /*declared=*/false),
              "disconnected: the TX cutoff editor remains permissive");

        check(flexCaps.hasLmsNoiseFilters,
              "Flex declares hasLmsNoiseFilters (NRL/ANFL/ANFT are base firmware)");
        check(!icomCaps.hasLmsNoiseFilters,
              "Icom declares NO hasLmsNoiseFilters (no WDSP LMS/FFT register exists)");

        check(!flexCaps.hasManualNotch,
              "Flex declares NO hasManualNotch (it notches with TNFs — a different instrument)");
        check(icomCaps.hasManualNotch,
              "Icom declares hasManualNotch (16 48 enable, 14 0D position, 16 57 width)");
        check(!icomCaps.hasPaTemperatureTelemetry,
              "Icom declares no PA-temperature telemetry without a model profile");

        // The gates themselves, through the SAME expression the UI applies, so
        // these assert behaviour rather than paraphrase it.
        //
        // Disconnected: the LMS row stays (permissive, it ships), the MN button
        // does not (not permissive, it is new).
        check(uiWouldShow(/*connected=*/false, /*declared=*/false),
              "disconnected: a permissive gate shows (hasLmsNoiseFilters rule)");
        RadioModel disconnected;
        check(disconnected.hasLmsNoiseFilters(),
              "disconnected: hasLmsNoiseFilters() is permissive — the shipping row stays");
        check(!disconnected.hasManualNotch(),
              "disconnected: hasManualNotch() is NOT permissive — no MN button on an "
              "empty window");
    }

    // ---- ASR (Copy Assist) indicator: gated on the ACTIVE slice (#4825) ----
    //
    // The two status-bar keyers and the ASR indicator ask the SAME mode question
    // (isVoiceMode) of DIFFERENT slices, and that difference is the whole fix.
    // CWX and DVK key the TX slice, so an absent TX slice must close them. Copy
    // Assist decodes received audio and is already bound to the ACTIVE slice, so
    // an absent TX slice must NOT close it — it did, which is #4825: an operator
    // who turns TX off (antenna disconnected, bench work) or listens receive-only
    // lost the feature outright.
    //
    // WHAT THE MODE CHECKS BELOW DO NOT PIN, stated plainly because they read
    // like they cover the fix and they do not: every isVoiceMode() check here
    // would pass, green, against the bug — the PRE-FIX code used the same mode
    // list. They pin the mode list, which is worth pinning now that two
    // indicators share it. The slice SOURCE, the actual subject of #4825, is
    // still out of reach: MainWindow is not linkable from this target (it links
    // aethercore, not the GUI), so nothing here can ask
    // updateKeyerAvailability() which slice it read. That half rests on the
    // hardware verification (a FLEX-6500 with TX off, and a two-slice TX=CW /
    // active=USB pair).
    //
    // The teardown decision IS pinned, in the second block below.
    // shouldAutoHideCopyAssist() was moved into VoiceModeGate.h for exactly the
    // reason this comment used to give for not pinning it, so the clause that
    // keeps a band recall from stopping a running transcription has a test that
    // fails when it is removed.
    {
        // TX off: no slice carries isTxSlice(), so txSlice() is null and
        // updateKeyerAvailability() reads an empty TX mode.
        const QString txModeWithTxOff;                          // txSlice() == nullptr
        const QString activeModeUsb = QStringLiteral("USB");

        check(!isVoiceMode(txModeWithTxOff),
              "TX off: the TX-slice mode test is false — CWX/DVK close, which is "
              "right, there is nothing to key");
        check(dvkIndicatorBlocker(isVoiceMode(txModeWithTxOff),
                                  /*licenseSeen=*/false, /*licenseEnabled=*/false)
                  == DvkIndicatorBlocker::TxModeNotVoice,
              "TX off: the DVK gate names the mode as its blocker");
        check(isVoiceMode(activeModeUsb),
              "TX off while listening on USB: the ACTIVE slice's mode is still a "
              "voice mode, so the ASR indicator stays available (#4825)");

        // The row may now disagree with itself, and that is the intended
        // outcome, not a regression: the indicators answer about two slices.
        check(isVoiceMode(activeModeUsb) != isVoiceMode(txModeWithTxOff),
              "the ASR and DVK indicators can disagree — they read different "
              "slices, so a consistent-looking row is not the invariant");

        // Mode gate itself is UNCHANGED: voice only. ASR in CW or DIGx would
        // have nothing intelligible to transcribe.
        check(!isVoiceMode(QStringLiteral("CW")),
              "active slice in CW: ASR indicator stays dimmed (nothing to transcribe)");
        check(!isVoiceMode(QStringLiteral("CWL")),
              "active slice in CWL: ASR indicator stays dimmed");
        check(isCwMode(QStringLiteral("CW")) && isCwMode(QStringLiteral("CWU"))
                  && isCwMode(QStringLiteral("CWL")),
              "CW decoder gate accepts legacy CW plus explicit CWU/CWL names");
        check(!isVoiceMode(QStringLiteral("DIGU")) && !isVoiceMode(QStringLiteral("DIGL")),
              "active slice in DIGU/DIGL: ASR indicator stays dimmed");
        check(!isVoiceMode(QStringLiteral("RTTY")),
              "active slice in RTTY: ASR indicator stays dimmed");
        check(!isVoiceMode(QStringLiteral("FDVU")),
              "active slice in FreeDV: ASR indicator stays dimmed (RADAE-encoded, "
              "not acoustic speech)");

        // The whole voice family, so a later edit cannot quietly drop one.
        for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                    QStringLiteral("AM"),  QStringLiteral("SAM"),
                                    QStringLiteral("FM"),  QStringLiteral("NFM"),
                                    QStringLiteral("DFM")}) {
            check(isVoiceMode(mode),
                  "voice family member is a voice mode for both indicators");
        }

        // No active slice at all: an empty mode is not a voice mode, so the
        // indicator dims. Whether that also CLOSES an open panel is a separate
        // decision, pinned in the next block.
        check(!isVoiceMode(QString()),
              "no active slice: empty mode is not a voice mode — ASR dims");
    }

    // ---- Copy Assist auto-hide: what may tear down a RUNNING transcription ----
    //
    // Hiding the panel calls setAsrEnabled(false) — the audio tap is dropped and
    // Enable is unticked — and updateKeyerAvailability() only ever hides, never
    // shows. So a hide the operator did not ask for cannot be undone by any
    // later refresh, which is why every clause of this predicate exists to say
    // NO. Two of them were regressions caught in review on this PR, and this
    // block is what stops them coming back.
    {
        const QString usb = QStringLiteral("USB");
        const QString cw  = QStringLiteral("CW");

        // The one case that SHOULD tear down: the operator selected a live
        // slice that carries nothing transcribable, with no rebuild in flight.
        check(shouldAutoHideCopyAssist(/*panelVisible=*/true,
                                       /*haveActiveSlice=*/true, cw,
                                       /*bandRecallInFlight=*/false),
              "operator selects a live CW slice: an open panel closes — this is "
              "the auto-hide's whole purpose (#4825)");

        // Nothing open, nothing to tear down.
        check(!shouldAutoHideCopyAssist(false, true, cw, false),
              "panel already closed: nothing to hide");

        // A voice slice never closes it, recall or not.
        check(!shouldAutoHideCopyAssist(true, true, usb, false),
              "live USB slice: the panel stays open");

        // #4158 / single-slice band recall, and disconnect: the slice is ABSENT.
        // band_persistence drops the slice and re-creates it under the same id a
        // moment later; on a single-slice setup that empties slices() and lands
        // here. Closing would stop transcription on every band change and never
        // restore it.
        check(!shouldAutoHideCopyAssist(true, /*haveActiveSlice=*/false,
                                        QString(), false),
              "no active slice (single-slice band recall, or disconnect): the "
              "panel is left alone — an absent slice is not a mode change");

        // #4158 again, one slice further along, and the regression this PR's
        // review caught second: with a SECOND slice on the pan, onSliceRemoved()
        // re-selects it (slices.first()) instead of taking the empty-slices
        // branch, so the gate is handed a live CW slice mid-rebuild and
        // haveActiveSlice no longer protects anything. Only the recall window
        // tells this apart from the operator clicking that CW slice themselves.
        check(!shouldAutoHideCopyAssist(true, /*haveActiveSlice=*/true, cw,
                                        /*bandRecallInFlight=*/true),
              "band recall in flight with a surviving CW slice: the panel is "
              "left alone — Copy Assist survives a band change on a multi-slice "
              "pan too (#4932 review)");

        // The guard is scoped to the recall window, not a blanket suppression:
        // once it expires, selecting that same CW slice closes the panel again.
        check(shouldAutoHideCopyAssist(true, true, cw, false)
                  != shouldAutoHideCopyAssist(true, true, cw, true),
              "the band-recall window is the ONLY thing separating those two "
              "cases — remove it and a band change stops transcription");
    }

    // ---- receiveOnlyModes: the second key-on guard -------------------------
    //
    // A radio that transmits, just not in the mode it is in right now — WFM on
    // an IC-705, which covers 76-108 MHz broadcast and whose transmitter does
    // not follow (#5040). canTransmit cannot express that: it would disable the
    // whole transmit surface on a radio that keys perfectly well one mode away.
    //
    // The DECISION is pinned here, away from the Icom plumbing that produces
    // the list, because it is what every key path asks:
    // RadioModel::refuseKeyInReceiveOnlyMode() is the only thing that can roll
    // back TransmitModel's optimistic MOX/TUNE state, and it makes that decision
    // through this function (#5106 review).
    {
        RadioCapabilities rxOnlyWfm;
        rxOnlyWfm.canTransmit = true;          // the radio DOES transmit...
        rxOnlyWfm.receiveOnlyModes = {QStringLiteral("WFM")};   // ...just not here

        check(modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("WFM")),
              "a listed mode refuses the key");
        check(!modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("USB")),
              "and an unlisted one does not — the radio still transmits");
        // The CW spelling an Icom actually reports. A guard that matched on a
        // prefix, or on the older neutral "CW", would refuse the key in the mode
        // operators spend the most time transmitting in.
        check(!modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("CWU"))
                  && !modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("CWL"))
                  && !modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("CW")),
              "CW keys normally in every spelling — this guard is exact-match, "
              "not a family of modes");
        // The automation bridge upper-cases what it is handed; nothing promises
        // the neutral vocabulary arrives that way at every other seam.
        check(modeIsReceiveOnly(rxOnlyWfm, QStringLiteral("wfm")),
              "the match is case-insensitive");
        // Asked before a slice exists — the guard must stay OPEN. Refusing on
        // an empty mode would deny keying on any path that runs before the TX
        // slice is known.
        check(!modeIsReceiveOnly(rxOnlyWfm, QString()),
              "no slice, no claim: an empty mode is not a receive-only mode");

        // And the default every shipping backend but Icom reports: inert.
        RadioCapabilities plain;
        plain.canTransmit = true;
        check(!modeIsReceiveOnly(plain, QStringLiteral("WFM")),
              "an empty list refuses nothing — adding this field changed no "
              "existing radio's keying");

        RadioModel sim;
        sim.connectToRadio(simInfo());
        check(sim.backendCapabilities().receiveOnlyModes.isEmpty(),
              "the simulator declares no receive-only modes either");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "radio_capability_gating_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
