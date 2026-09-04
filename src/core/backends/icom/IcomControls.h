#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "core/backends/icom/IcomModels.h"

// THE CONTROL REGISTRY — every CI-V message this backend knows about, declared
// once, in a form something other than a human can read.
//
// WHY THIS EXISTS. The backend's knowledge of the radio used to live entirely in
// switch statements: `cmd::kLevel` + `level::kNrLevel` decoded in one switch,
// encoded in another, its 0..255-to-percent conversion inlined at both ends, and
// the fact that a control is READ at connect but never SENT visible only by
// grepping for the constant and finding one call site instead of two.
//
// That shape has one failure mode and the bring-up hit it repeatedly: a control
// that is half-wired looks exactly like a control that works. `level::kRf` was
// declared, named, and never sent or decoded — the RF Gain slider drove the
// PREAMP instead, and nothing in the code said so. `filterForWidthHz` snapped
// every mode against the SSB thresholds, so three filter buttons reached one
// filter in AM and one in CW. Both were found by an operator noticing a wrong
// number on screen, one control at a time.
//
// A table cannot prevent those. What it can do is make them ENUMERABLE: with the
// wire address, the scale and the seam verb declared next to each other, an
// agent can ask "which controls do we claim, which of those reach the radio, and
// which have a UI that reaches them" and get an answer for all of them at once
// instead of one at a time.
//
// THIS IS A CLAIM, NOT A PROOF. Every field here is what the code INTENDS. The
// `controls.scrub` extension is what checks the intent against the wire — see
// IcomCivBackend::invokeExtension. A row saying `seamVerb = "setSliceNoiseReduction"`
// is a promise that something calls it; only the scrub can tell you it does.

namespace AetherSDR::icom {

// How the payload bytes carry the value.
enum class Encoding : std::uint8_t {
    None,        // no payload — a read, or a bare trigger
    Level255,    // two BCD bytes, 0000..0255, mapped to a percentage
    OnOff,       // one byte, 00 / 01
    Enum,        // one byte, a small set of named positions
    BcdByte,     // one BCD byte carrying its own decimal value (the attenuator's dB)
    BcdFreq,     // five BCD bytes, little-endian, Hz
    ModeFilter,  // mode byte + filter slot byte
    Bcd4,        // four BCD digits (a scope span, a SET-menu item)
    Bcd6,        // six BCD digits (repeater offset / CTCSS frequency)
    Dtcs,        // polarity byte + three displayed BCD code digits
    Ascii,       // bounded ASCII text after the command-specific address
    GpsPosition, // latitude/longitude plus optional altitude/course/speed/UTC
};

// Which model the value belongs to once it is across the seam. Says where to
// look for it, and is what makes "declared but nothing consumes it" visible.
enum class Plane : std::uint8_t {
    Slice,
    Transmit,
    Pan,
    Radio,
    Meter,
    None,     // known to the protocol, deliberately not surfaced
};

// How far a control actually got. The whole point of the table: a control that
// is decoded but never sent, or sent but never decoded, is a real state and must
// not look like a working one.
enum class Wiring : std::uint8_t {
    Both,        // we send it AND decode the radio's report
    SendOnly,    // we can set it; nothing reads it back
    DecodeOnly,  // we adopt what the radio says; the operator cannot set it
    Declared,    // the constant exists and neither happens — a stub
};

struct ControlSpec {
    std::string_view id;        // stable slug, safe to key a report on
    std::uint8_t cmd = 0;       // 0x14, 0x16, 0x11, ...
    std::uint8_t sub = 0;       // subcommand, when hasSub
    bool hasSub = false;
    std::string_view label;     // what an operator would call it

    Plane plane = Plane::None;
    Encoding encoding = Encoding::None;
    Wiring wiring = Wiring::Declared;

    // WHAT THE RADIO TAKES, in its own units — the numbers from the CI-V
    // reference guide, not ours.
    int rawLow = 0;
    int rawHigh = 0;

    // WHAT THE SEAM CARRIES, after conversion. The pair exists because the
    // conversion is exactly where a control goes wrong quietly: a 0..255
    // register presented as 0..100 is fine, presented as dB is a fabrication.
    std::string_view neutralUnit;   // "%", "dB", "Hz", "step", "on/off", ""
    int neutralLow = 0;
    int neutralHigh = 0;

    // The IRadioBackend method this maps to, or empty when the control does not
    // cross the seam at all. Empty with wiring != Declared means the backend
    // handles it internally (a poll, a startup push).
    std::string_view seamVerb;

    // The objectName an operator's control carries, when there is one. Empty
    // means the radio has the feature and AetherSDR exposes no way to reach it —
    // which is a finding, not an omission from this table.
    std::string_view uiTarget;

    // Read at connect, so the UI opens where the radio actually is rather than
    // at our own defaults. A settable control that is NOT read at connect will
    // show a wrong value until the operator touches it.
    bool readAtConnect = false;

    // Free-text caveat carried into the report. Where a row is more complicated
    // than its fields — the mode-dependent filter ladder, the attenuator's
    // band limits — this is where that lives, so the report explains itself.
    std::string_view note;

    // The model-profile facet required before this row is effective. Core is
    // the backend's model-neutral CI-V floor; Scope follows the discovered
    // model's transport geometry. Their evidence can still be "none" so the
    // registry distinguishes reachability from guide/live attestation.
    IcomFeature requiredFeature = IcomFeature::Core;
};

[[nodiscard]] std::span<const ControlSpec> controlSpecs();
[[nodiscard]] bool controlSupported(const IcomModel& model,
                                    const IcomModelProfile& profile,
                                    const ControlSpec& spec) noexcept;
[[nodiscard]] int speechProcessorRawLevel(int maximum, int level) noexcept;

[[nodiscard]] std::string_view encodingName(Encoding e);
[[nodiscard]] std::string_view planeName(Plane p);
[[nodiscard]] std::string_view wiringName(Wiring w);

}  // namespace AetherSDR::icom
