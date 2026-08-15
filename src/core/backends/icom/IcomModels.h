#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/backends/icom/IcomMeters.h"

// Phase 5 — model identity and per-model capability.
//
// The CI-V address IS the model identity, and command 0x19 0x00 asks the radio
// for it. QUERY IT; never assume. The address is user-changeable, several Icom
// models speak this same RS-BA1 transport, and Icom's own RS-BA1 server can
// front a USB-only radio over the network — so a backend that hardcodes 0xA4
// will happily mis-decode an IC-9700 someone pointed it at, and the failure
// looks like corrupt spectrum rather than a wrong model.
//
// Qt-free; icom_models_test drives it.
//
// PROVENANCE. Everything here is a HARDWARE FACT — a CI-V address, a spectrum
// point count, a receiver count — not anyone's creative work. The IC-705's
// numbers are tier 1, confirmed against Icom's own CI-V Reference Guide (475
// points, 0..160 range, one receiver, one scope). The other models are
// cross-referenced but NOT verified against their own guides, and every one of
// them says so in `verified`. A backend must treat an unverified model as a
// reason to be careful, not as licence to stream.

namespace AetherSDR::icom {

struct IcomModel {
    std::uint8_t civAddress = 0;
    std::string_view name;

    int receivers = 1;
    int vfos = 2;

    // Speaks the RS-BA1 UDP transport. False means CI-V only — reachable over
    // a local serial port, or over the network via Icom's own RS-BA1 server
    // acting as a front end.
    bool hasNetwork = false;
    bool hasWifi = false;

    bool hasScope = false;
    int scopePoints = 0;
    int scopeMaxAmplitude = 0;
    // Divisions the sweep is split into over USB. Over WLAN it is always 1 —
    // the whole sweep arrives in one packet.
    int scopeDivisionsUsb = 11;

    // 5 on every current model; the IC-905 uses 6 above 10 GHz. A frequency
    // codec written against a hardcoded 5 misaligns by two bytes there and
    // decodes a plausible-looking wrong frequency.
    std::size_t freqBytes = kFreqBytes;

    bool hasTransmit = true;
    double txPowerMaxWatts = 0.0;

    std::uint64_t tuningMinHz = 0;
    std::uint64_t tuningMaxHz = 0;

    // False when the numbers above are cross-referenced but NOT confirmed
    // against this model's own CI-V Reference Guide. Load-bearing: a backend
    // should decline to advertise capabilities it cannot stand behind.
    bool verified = false;

    // Speaks the selected-VFO form of command 0x26 — mode, DATA state and IF
    // filter in one 26 00 frame. The IC-705 and IC-7300MK2 guides in
    // sources/icom-official/ document this exact form.
    //
    // FALSE UNTIL ATTESTED FOR EACH MODEL. Some Icoms expose a different 0x26
    // shape, and a mode change sent in the wrong form is silently unapplied.
    // The conservative fallback is plain 0x06 with no DATA control or claim.
    //
    // ATTESTED IS NOT `verified` ABOVE, deliberately. `verified` is a claim
    // about this whole row — geometry, amplitude range, tuning limits, all
    // confirmed against the model's own guide. The 0x26 shape is one narrow
    // question that a measured round trip answers on its own, and the IC-9700
    // is exactly that case: geometry still assumed, 26 00 read off the radio.
    // Coupling the two would force either an overclaimed row or a discarded
    // trace. So record the evidence in a comment beside the flag AND add the
    // address to kAttestedVfoMode in icom_meters_test.cpp — that list is what
    // stops a new row copied from the IC-705 inheriting a shape nobody checked.
    bool hasVfoModeCommand = false;

    [[nodiscard]] bool isKnown() const noexcept { return civAddress != 0; }
};

// Look up by the address the radio reported. Returns nullptr for an address we
// do not recognise — which is a real and expected outcome, not an error: Icom
// has ~130 CI-V addresses and this table has a handful.
[[nodiscard]] const IcomModel* modelForCivAddress(std::uint8_t addr);

// Look up by the name the radio reports in its RS-BA1 capabilities packet
// ("IC-705"). That name arrives during the HANDSHAKE — before the session is
// connected — whereas the CI-V address needs a 0x19 0x00 round trip on a
// stream that does not exist yet. So this is what resolves the model in time
// for the connect-edge capability publication; the address corrects it after.
//
// Matched case-insensitively and ignoring '-' so "IC705" and "ic-705" both
// land, since the field is free text set on the radio.
[[nodiscard]] const IcomModel* modelForName(std::string_view name);

// Every model in the table.
[[nodiscard]] std::span<const IcomModel> knownModels();

// The safe fallback for a radio we do not recognise.
//
// Deliberately CONSERVATIVE rather than optimistic: no scope, no transmit, one
// receiver. An unknown radio that gets advertised as scope-capable produces a
// panadapter wired to a command the radio may not implement; an unknown radio
// advertised as transmit-capable produces a TX button on something we cannot
// characterise. Both are worse than a reduced feature set, and the operator can
// still tune and listen.
[[nodiscard]] const IcomModel& unknownModel();

// Decode the reply to CI-V 0x19 0x00. Returns the reported address, or nullopt
// if this is not that reply.
[[nodiscard]] std::optional<std::uint8_t> parseModelIdReply(const CivFrame& frame);

// The S9 reference this model+frequency combination should use. Band-dependent,
// not model-dependent — see sMeterDbm().
[[nodiscard]] double s9ReferenceFor(std::uint64_t hz) noexcept;

// raw -> watts for this model's Po meter.
//
// EMPTY means we have no measured curve for this model, and the caller must
// report PERCENT rather than inventing watts. That distinction is the whole
// point: a power meter showing "50 W" derived from another radio's curve is a
// number an operator will act on.
[[nodiscard]] std::span<const CurvePoint> powerCurveFor(const IcomModel& model);

// The front-end stages this model offers, in register order (index 0 is OFF).
//
// EMPTY means we have no verified ladder for this model, and the caller must
// publish NOTHING rather than fall back to another radio's — the same rule
// powerCurveFor states above, for the same reason. The stages are genuinely
// per-model: an IC-7610's attenuator has several steps where the IC-705 has
// one, and the IC-9700's preamp ladder is not the HF ladder. A button labelled
// "20 dB" on a radio whose register means something else is exactly the
// misdescription the control registry exists to make visible.
//
// A control that does not appear is a better answer than one that appears and
// lies, so an empty span means the operator simply does not get the button.
[[nodiscard]] std::span<const std::string_view> preampLabelsFor(const IcomModel& model);

// Attenuator positions. The label is what the operator reads; the dB is what
// goes on the wire (BCD — see cmdSetAttenuator), so the two must not drift.
struct AttenStep {
    std::string_view label;
    int db;
};
[[nodiscard]] std::span<const AttenStep> attenStepsFor(const IcomModel& model);

}  // namespace AetherSDR::icom
