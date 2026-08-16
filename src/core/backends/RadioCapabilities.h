#pragma once

#include <QFlags>
#include <QString>
#include <QVector>
#include <QVariantMap>

namespace AetherSDR {

// The honest, self-declared feature set of a connected radio, produced by an
// IRadioBackend and surfaced to clients (aetherd RFC §4.1 `welcome`). Clients
// render against what the radio *reports* — a control the radio lacks is
// disabled/absent — instead of hard-coding "the radio is a Flex". This is the
// structural replacement for the model-impersonation anti-pattern (RFC §1).
//
// Design (RFC §5.5 open-question Q1): a TYPED struct for the core profile —
// the surface every radio family has — plus a namespaced `extensions` bag for
// vendor-specific capability values that don't belong in the core. Typed where
// it's universal, open where it's vendor.
//
// NOT the same as models/ModelCapabilities: that is model-string-*derived*
// truth (a static FlexLib platform table keyed by the model name, Principle I);
// this is the radio's *reported* self-description produced by a backend and
// surfaced to clients. A FlexBackend may seed this FROM ModelCapabilities, but
// the two are distinct concepts (derived-from-name vs reported-by-backend).
//
// ADDING A FIELD: every field below defaults to false/0/empty, so a backend
// that omits one silently declares the feature ABSENT — set it explicitly in
// FlexBackend, Hl2Backend AND SimBackend. Then record it in
// docs/architecture/radio-capabilities-map.md, which maps every field to the
// code that reads it (and lists the ones nothing reads yet). A capability no
// consumer reads looks identical, from here, to one that works.
struct RadioCapabilities {
    // Identity
    QString family;   // backend id: "flex", "kiwi", … (stable, lowercase)
    QString model;    // radio model string as reported by the hardware

    // Who MADE it, as an operator would say it — "Icom", "FlexRadio". Display
    // only; nothing branches on it. Empty means "not reported", and a consumer
    // then shows the model alone rather than inventing a brand.
    //
    // Separate from `family` because family is a wire-protocol id, not a name.
    // They coincide today only by accident of having one backend per vendor.
    //
    // The status bar shows this ABOVE the model, and only when the model does
    // not already carry it: "FLEX-8400M" says FlexRadio in the string itself,
    // so a brand line above it would be a stutter, while "IC-705" says nothing
    // to anyone who does not already know the radio. That rule lives at the
    // display site — this field just reports the truth and lets the UI decide.
    QString manufacturer;

    // Receive
    int maxSlices = 1;             // independent demod slices the radio supports
    int maxPanadapters = 1;        // simultaneous panadapters
    QVector<int> sampleRatesHz;    // supported per-receiver sample rates (Hz)

    // The frequency range the receiver can actually be tuned to, in Hz.
    //
    // Both zero means "not reported" — clients then keep whatever range they
    // previously assumed, so this is additive for a backend that never sets it.
    //
    // This exists because the band buttons had no way to be honest. They are a
    // fixed grid from 2200 m to 2 m, and every one of them was live on every
    // radio: pressing 6 m on a direct-sampling HF receiver tuned it somewhere
    // it cannot hear, and the operator got a dead band rather than a control
    // that told them it was not available.
    double tuningMinHz = 0.0;
    double tuningMaxHz = 0.0;

    // Manual notch filters (a Flex TNF) the radio can hold at once. ZERO is the
    // load-bearing default: it means "this radio cannot notch", and the UI then
    // omits the +TNF button and the panadapter's add/remove entries entirely.
    //
    // That default matters because the control shipped ungated. The +TNF button
    // and the right-click menu were live on every backend while the commands
    // behind them only meant anything to a Flex, so on an HL2 an operator could
    // place notches all day and hear nothing change.
    int maxNotchFilters = 0;

    // Whether a notch has a DEPTH control as well as a width.
    //
    // A Flex TNF has three depths; a host-DSP notch built on WDSP's notched
    // bandpass is a full null with no depth parameter at all. False hides the
    // depth submenu rather than leaving three settings that all do the same
    // thing. Meaningless when maxNotchFilters is 0.
    bool notchHasDepth = false;

    // The narrowest notch the radio can actually produce, in Hz, and the
    // widest. Zero for either means "not reported" and the UI keeps its own
    // defaults.
    //
    // The minimum is here because on a host-DSP backend it is a real, moving
    // constraint rather than a UI preference: WDSP's floor is a function of
    // filter length and it WIDENS a narrower request instead of refusing it, so
    // a UI offering 50 Hz against a 200 Hz floor draws a notch four times
    // narrower than the one the operator is hearing.
    double notchMinWidthHz = 0.0;
    double notchMaxWidthHz = 0.0;

    // Transmit — the load-bearing capability for TX safety (RFC §6). A backend
    // that cannot key sets canTransmit=false; the engine guard then denies any
    // keying intent regardless of client requests.
    bool canTransmit = false;
    double txPowerMaxWatts = 0.0;  // 0 when RX-only

    // TX audio is modulated on THIS host rather than inside the radio. True for
    // direct-sampling backends (HL2) where the PC runs the modulator and streams
    // baseband to the radio; false for a Flex, which modulates on-radio from its
    // own mic/line jacks. Drives the mic-source list and the PC-audio lock — so
    // it must be a capability, not a family-name special case: an RX-only
    // non-Flex backend must NOT open the mic on connect. (#4449 review)
    bool hostModulates = false;

    // The RADIO owns its display dBm scale and will echo back a range sent to
    // it. True for a Flex, whose `display pan set min_dbm=…` is a real command
    // the radio adopts and reports; false for a backend that decodes its scope
    // at a FIXED calibration it does not accept changes to (Icom CI-V, whose
    // floor/span come from ScopeCalibration and shift only with the radio's own
    // reference level).
    //
    // This gates the noise-floor auto-adjust, and it has to, because that loop
    // is built on the echo: the widget moves its reference level, requests the
    // new range, and waits for the radio to confirm before moving again. With
    // no command plane the request is dropped, the confirmation never arrives,
    // and the auto-floor reads the unchanged floor as "not there yet" and steps
    // again — measured at a linear 24 dB/s, walking off the bottom of the scale
    // (-202, -226, -250 … -1882 dBm) until dbmRangeLooksPlausible() starts
    // rejecting it at -180. Those rejections are the SYMPTOM; the missing echo
    // is the fault, which is why raising the reject floor would not have fixed
    // it. On an IC-9700 this was the visible "waterfall resets ~1 s after the
    // trace fills" and the reconnect churn behind it.
    //
    // A backend with a fixed scale needs no auto-adjust: its floor is already
    // where the calibration puts it.
    bool radioOwnsDbmScale = true;

    // The RADIO stores memory channels and re-dumps them on connect. True for a
    // Flex, whose memory slots live in the radio and are shared by every client
    // attached to it; false for a direct-sampling or receiver-only backend (HL2,
    // Kiwi, demo) that has nowhere to put them.
    //
    // False is the load-bearing default: a backend that says nothing gets the
    // client-side memory bank, so an operator's channels survive rather than
    // being written into a radio that silently drops them. A backend only sets
    // this true when it can prove the radio gives the slots back.
    bool persistsMemories = false;

    // Domains of OPERATING STATE this client persists and restores because the
    // radio cannot (RFC #4603 proposal B). Constitution Principle III assigns
    // persistence authority per value, not per family — so this is a typed set,
    // not a boolean: a family may persist some domains on-radio and rely on the
    // client for others (cf. persistsMemories above, the pattern this follows).
    //
    // EMPTY IS THE LOAD-BEARING DEFAULT: a backend that declares nothing gets
    // NOTHING restored. For a radio that persists its own state (Flex), that is
    // exactly the Constitution II/III rule — the client must never re-assert
    // radio-owned values (#2465/#4126/#4261). A backend only declares a domain
    // when the radio genuinely has no memory of it, making the client the
    // radio's memory (HL2: "the radio reports no VFO, so the app is
    // authoritative and must push").
    //
    // Restore NEVER keys transmit (Principle VI): TxSetpoints covers setpoint
    // values (drive levels) only — the TX gate is untouched by any of this.
    enum class ClientSettingsDomain : quint32 {
        Tuning      = 1u << 0,  // RF frequency + demod mode
        Passband    = 1u << 1,  // filter low/high edges
        SpanRate    = 1u << 2,  // span / IQ sample rate
        RfGain      = 1u << 3,  // LNA/preamp gain (per band — see RFC PR 3)
        TxSetpoints = 1u << 4,  // TX drive setpoints (per band); never keying
        Memories    = 1u << 5,  // host-side memory bank documents (#4590 fold-in)
        Agc         = 1u << 6,  // AGC mode + threshold (client-side WDSP AGC)
    };
    Q_DECLARE_FLAGS(ClientSettingsDomains, ClientSettingsDomain)
    ClientSettingsDomains clientSettingsDomains;   // default: empty — restore nothing

    // The radio tunes off an oscillator it cannot characterise, so the CLIENT
    // owns the frequency-error correction and must offer the operator a way to
    // enter one. True for the HL2: its 76.8 MHz NCO scale is a localparam in
    // the gateware bitstream and no register in the HPSDR map can be told the
    // crystal's real error, so a host-side scalar is the only correction there
    // is. False for a radio that owns its own reference and its own calibration
    // command (Flex: `radio set cal_freq` / `freq_error_ppb`, which is why its
    // calibration UI lives on the Flex path and not behind this flag).
    //
    // NOT "does this radio have a frequency error" — every radio does. What
    // varies is whether correcting it is the client's job.
    bool hostFrequencyCalibration = false;

    // Peripherals / features every family may or may not have
    bool canReboot = false;        // supports a client-triggered radio reboot
    bool hasTuner = false;         // antenna tuner / ATU
    bool hasAmplifier = false;     // integrated or controllable PA
    bool hasExtendedDsp = false;   // extended firmware DSP filters (NRS/RNN/NRF)

    // The WDSP LMS / FFT filter family — NRL (leaky LMS noise reduction), ANFL
    // (LMS notch) and ANFT (FFT notch). A THIRD tier under hasRadioSideDsp,
    // narrower than that flag and orthogonal to hasExtendedDsp.
    //
    // It exists because hasRadioSideDsp turned out to be two claims wearing one
    // name: "the radio runs its own receive DSP" and "the radio runs FlexRadio's
    // particular set of it". An Icom is emphatically the first and not the
    // second — it has noise reduction, a noise blanker, an auto notch and a
    // manual notch, and nothing resembling NRL/ANFL/ANFT. Declaring
    // hasRadioSideDsp on it therefore lit up three buttons whose intents reach
    // no register on that radio: the classic HERMES §17 shape, where the control
    // moves, the setting persists and the audio never changes.
    //
    // Flex only, today. The name is the CONCEPT, not the vendor — a future
    // radio with LMS filters says true and gets the same three buttons.
    bool hasLmsNoiseFilters = false;

    // THIS HOST runs an impulse noise blanker on the radio's IQ, so the NB
    // control is real even on a radio whose own firmware has no DSP.
    //
    // The exact shape of the manual-notch exception, one field over: on a
    // direct-sampling backend the blanker either happens in WDSP on this host
    // or it does not happen at all, so gating NB on hasRadioSideDsp removed a
    // WORKING control rather than an empty one — the mirror image of the
    // HERMES §17 failure that flag exists to prevent.
    //
    // NARROWER THAN "the radio has a noise blanker", deliberately. It says
    // where the blanker runs, because that is what varies and what decides
    // whether this application has anything to do. A Flex leaves it false and
    // gets its NB from hasRadioSideDsp; the two are OR'd at the button.
    //
    // Requires an IQ path this host actually demodulates. A backend that
    // receives finished audio has nothing to blank however much it would like
    // to, and must leave this false.
    bool hasHostNoiseBlanker = false;

    // The radio has ONE operator-placed notch in its own DSP: an enable and a
    // position within the passband. The IC-705 spends 16 48 on the enable and
    // 14 0D on the position (0000..0255 across the passband), with 16 57
    // choosing one of three widths.
    //
    // NOT the tracking notch filters (TnfModel). A TNF is pinned to an absolute
    // frequency, the radio keeps several, and they survive tuning; this is one
    // notch at an offset inside the current passband, which is a different
    // instrument with a different control. Conflating them would have meant a
    // +TNF button that created a second notch by silently moving the first.
    //
    // Distinct from the AUTO notch (hasRadioSideDsp's ANF), which finds its own
    // tone. A radio can have either, both or neither.
    bool hasManualNotch = false;

    // The radio reports the PA supply-voltage rail as telemetry — the value the
    // status bar renders directly under the PA temperature. A radio that never
    // reports the rail declares false and that readout goes away, instead of
    // formatting an initialiser to two decimals so it reads as a measurement.
    //
    // Named for the TELEMETRY, not for the PA and not for the brand. "Does it
    // have a Flex PA" is the wrong axis: an HL2 has a PA and reports no supply
    // rail, and an IC-7610 would be the same. What actually varies between
    // families is whether the radio reports the voltage — which is exactly the
    // question the label needs answered.
    //
    // NOT hasAmplifier, despite the adjacency. That field means "integrated or
    // controllable PA", nothing reads it (see radio-capabilities-map.md, where
    // it sits under the fields no consumer reads — the AMP applet runs off
    // TunerModel::presenceChanged), and the HL2 declares it false while
    // genuinely having a PA. It already means something other than this.
    bool hasSupplyVoltageTelemetry = false;

    // The radio exposes SELECTABLE HARDWARE microphone inputs — the Phone
    // applet's MIC / BAL / LINE / ACC choices, which are FlexRadio's front and
    // rear connectors.
    //
    // False does NOT mean "no microphone". It means a client cannot pick the
    // input: the only source this application can feed is its own host audio,
    // so the dropdown collapses to PC. An Icom takes network audio and chooses
    // its own input from its own menu (MOD Input > DATA MOD); an HL2 is
    // modulated on this host entirely.
    //
    // Offering the Flex connector names on such a radio is the "the control
    // moves and nothing happens" failure the other capability comments warn
    // about — worse here, because picking MIC on a radio that will only ever
    // hear network audio produces a transmission with no modulation, which
    // looks like a hardware fault.
    bool hasSelectableMicInputs = false;

    // Transmit audio reaches this backend through IRadioBackend::submitTxAudio
    // rather than through a Flex DAX/VITA-49 stream.
    //
    // SEPARATE FROM hostModulates, and conflating the two cost a working
    // transmitter. `hostModulates` answers "does the HOST run the modulator" —
    // true for the HL2, false for an Icom, whose own firmware modulates. But
    // the Icom still needs the host to capture, process and SHIP the audio,
    // over its own UDP stream. There are three cases, not two:
    //
    //   Flex   modulates on the radio, audio leaves via DAX      → false
    //   HL2    modulates on the host,  audio leaves via the seam → true
    //   Icom   modulates on the radio, audio leaves via the seam → true
    //
    // AudioEngine gated its entire transmit chain on hostModulates, so an Icom
    // captured nothing, processed nothing and emitted nothing: the radio keyed
    // and transmitted no modulation at all. Meanwhile TCI's transmit path tried
    // to create a DAX stream and failed with "this radio has no command plane",
    // which is the same mistake read from the other end.
    bool takesTxAudioOverSeam = false;

    // The RX filter widths this radio can actually reach, in Hz. EMPTY means
    // "continuous, or unknown" and the UI keeps its own configurable list.
    //
    // Populated by a radio whose IF filters are a fixed, short set: the IC-705
    // has exactly three (FIL1/FIL2/FIL3), so the applet's full FlexRadio width
    // list gives most of its steps the same result and the operator gets a row
    // of buttons that mostly do nothing. Same treatment the RF-gain slider got
    // when it was narrowed to the three preamp detents that physically exist —
    // advertise the real, discrete set rather than let a continuous-looking
    // control sweep over hardware that cannot follow it.
    QList<int> rxFilterWidthsHz;

    // The RADIO stores named configuration profiles (global / TX / mic) that a
    // client can list, load and save. The seam already carries ProfileDelta and
    // profileChanged in both directions; this is the flag that says whether the
    // radio has any such thing to carry. A backend whose hardware has no
    // on-radio profile store reports false and every profile surface — the PROF
    // applet, the Profile Manager, import/export, the Profiles menu — goes away,
    // rather than offering an empty list the operator cannot populate.
    bool hasProfiles = false;

    // The radio can deliver per-slice receive audio and per-panadapter IQ as
    // separate streams, which this host routes to virtual audio devices for
    // external decoders (WSJT-X, fldigi, CW Skimmer).
    //
    // Named for the CONCEPT, not the brand: "DAX" is FlexRadio's name for it,
    // but nothing about routing RX audio to a virtual device is inherently
    // Flex-specific, and a future backend that grows the ability should be able
    // to say so without the field reading as a vendor special case.
    //
    // UI VISIBILITY ONLY. The runtime guard that stops a non-Flex session
    // reaching the bridge is a separate null-check on panStream() in
    // MainWindow::startDax() — a crash guard, deliberately not merged with this.
    bool hasDaxStreams = false;

    // Audio DSP runs INSIDE the radio, driven by command-plane verbs, rather than
    // on this host. True for a Flex, whose firmware owns NR/NB/ANF/NRL/ANFL/ANFT,
    // the APD predistorter, the wideband noise blanker and the 8-band hardware
    // equalizer; false for a direct-sampling backend like the HL2, where the host
    // runs every one of those it has.
    //
    // The test for "does this belong here" is whether the control's only effect is
    // to emit a verb the radio's firmware executes. The hardware EQ qualifies:
    // EqualizerModel emits `eq RXsc`/`eq TXsc`, which reach nothing on a backend
    // with no Flex command plane — the widget moves, the setting persists, and the
    // audio is unchanged (HERMES §17's failure shape).
    //
    // NOT about the client-side equivalents — the AetherDSP noise modules
    // (NR2/NR4/MNR/BNR/DFNR/RN2) and the Aetherial RX/TX EQ. Those run in this
    // application, work on any family, and must never be gated on this. On a
    // radio reporting false they are the ONLY audio DSP the operator has, so
    // hiding them would leave nothing.
    //
    // Distinct from hasExtendedDsp, which is a narrower statement about the
    // extra 8000-series firmware filters (NRS/RNN/NRF) on a radio that already
    // has the base set. A radio with hasRadioSideDsp=false has neither.
    bool hasRadioSideDsp = false;

    // The RADIO computes the waterfall's black level per tile and embeds it in
    // the waterfall stream, so the client can hand the floor decision to the
    // hardware instead of estimating it. True for a Flex, which does this on
    // `display panafall set <id> auto_black=1`.
    //
    // The Display panel's "Black Level" button cycles Off -> SW -> HW; HW is
    // this capability. On a backend without it the cycle is Off <-> SW only,
    // because HW there is a mode that can never produce a level: the enabling
    // command reaches no command plane, no tile ever carries a black level, and
    // the operator is left on a setting that silently does nothing. This is the
    // display-plane sibling of hasRadioSideDsp — same failure shape (HERMES
    // §17): the control moves, the setting persists, the picture is unchanged.
    //
    // NOT about auto-black as a feature. The client-side (SW) estimate works on
    // every family and must never be gated on this — on a radio reporting false
    // it is the only automatic floor the operator has.
    bool hasRadioSideWaterfallAutoBlack = false;

    // NO hasTrackingNotchFilters HERE, deliberately. TNF looks like it belongs
    // beside the three below — TnfModel's whole surface is `tnf create/remove/
    // set` and `sub tnf all`, so it passes the "does the control only emit a
    // verb the firmware executes" test the same way they do.
    //
    // It is left ungated because the CONTROL is about to stop being empty: a
    // host-side notch is landing, and the status-bar TNF indicator and the
    // overlay menu's +TNF button are the surfaces it will drive on a radio with
    // no `tnf` command plane. Hiding them now would mean deleting them and
    // putting them straight back — the exact round trip the hardware EQ already
    // made (see hasRadioSideDsp above, and the map doc).
    //
    // If that host-side notch does not land, this is the first thing to
    // reconsider — but reconsider it as "is the control still empty", not as
    // "is this a Flex feature".

    // The RADIO buffers CW text and sends it on its own keyer, driven by `cwx`
    // verbs and reporting progress through `sub cwx all`. True for a Flex, whose
    // firmware owns the character queue, the send index and the break-in timing;
    // false for a backend where nothing on the far end has a text buffer.
    //
    // Gates the status-bar CWX indicator, the CWX panel and its F1-F12 macro
    // shortcuts together — leaving the keys armed on a radio that refuses every
    // `cwx send` is the "silently does nothing" shape the DVK entitlement gate
    // already exists to prevent. The buttons are not the only surface: the
    // FlexControl/Ulanzi macro action, the MQTT cw/transmit topic, TCI's
    // cw_msg / cw_macros, rigctl's send_morse / stop_morse, SmartCAT's KY and
    // the automation bridge's `cwx` verb all reach CwxModel without passing the
    // status bar, so all of them ask RadioModel::hasRadioSideCwKeyer() — read
    // through the accessor, never inline, so the permissive disconnected rule
    // cannot be forgotten at a site. The three that owe a caller an answer
    // (bridge, rigctl, SmartCAT) return an error rather than a cheerful ok.
    //
    // NOT about CW. A radio reporting false still transmits CW perfectly well
    // from a key, a paddle or the host's own keying path; what it lacks is a
    // place to put the text.
    bool hasRadioSideCwKeyer = false;

    // The RADIO records and plays back voice-keyer messages from its own store
    // (`dvk` verbs). True for a Flex; false for a backend with no recorder.
    //
    // Distinct from, and evaluated BEFORE, the SmartSDR+ DVK entitlement in
    // DvkAvailabilityGate. That gate answers "is this Flex licensed for the
    // feature", which is a question only a radio that HAS the feature can be
    // asked — its fail-open rule for an unknown entitlement (#4210) is correct
    // for a Flex mid-handshake and would otherwise leave a live DVK button on
    // every radio that never reports a license at all.
    bool hasVoiceKeyer = false;

    // The radio can receive and transmit simultaneously on demand, toggled with
    // `radio set full_duplex_enabled=`. True for a Flex; false for a backend
    // where the T/R changeover is exclusive and no such setting exists.
    //
    // Gates the status-bar FDX indicator. On a radio reporting false the button
    // could only ever produce the "FDX not available" interlock notification it
    // already raises on a non-zero response — an error message where a control
    // should be.
    bool hasFullDuplex = false;

    // The radio accepts installable waveform/mode plugins (SmartSDR waveforms),
    // so a client can offer to manage them.
    bool hasWaveforms = false;

    // Several GUI clients can hold independent sessions on the radio at once,
    // each with its own slices and audio streams (SmartSDR multiFLEX). A backend
    // that serves exactly one client reports false and the multi-client
    // configuration UI goes away.
    bool hasMultiClientSessions = false;

    // The RADIO reports its own position/time from an on-board GNSS receiver, so
    // a client can offer a live GPS readout and the station-location dashboard
    // it feeds.
    //
    // This is about the radio as a POSITION SOURCE, not about the client knowing
    // where the station is. A grid square the operator typed into settings is
    // not this capability, and must never be gated on it — a radio with
    // hasGpsLocation=false still has a station location, it just cannot tell you
    // what it is. That distinction is why the flag is named for the receiver
    // rather than for the dashboard it happens to drive today.
    bool hasGpsLocation = false;

    // Vendor-specific capabilities, keyed by extension namespace. Clients that
    // don't understand a namespace ignore it; a backend never puts core-profile
    // fields here. Example: {"flex": {"multiFlex": true, "guiClientId": "…"}}.
    QVariantMap extensions;

    // The vendor-extension namespaces this backend implements (for the
    // capability handshake). A client can pre-check before issuing
    // invokeExtension(ns, …).
    QVector<QString> extensionNamespaces;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(RadioCapabilities::ClientSettingsDomains)

}  // namespace AetherSDR
