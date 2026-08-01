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
    };
    Q_DECLARE_FLAGS(ClientSettingsDomains, ClientSettingsDomain)
    ClientSettingsDomains clientSettingsDomains;   // default: empty — restore nothing

    // Peripherals / features every family may or may not have
    bool canReboot = false;        // supports a client-triggered radio reboot
    bool hasTuner = false;         // antenna tuner / ATU
    bool hasAmplifier = false;     // integrated or controllable PA
    bool hasExtendedDsp = false;   // extended firmware DSP filters (NRS/RNN/NRF)

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
