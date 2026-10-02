#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "core/backends/icom/IcomModels.h"

// The control registry: every CI-V message this backend knows, with wire
// address, scale and seam verb declared together so tools can enumerate which
// controls are claimed, which reach the radio, and which have a UI.
// This is a CLAIM, not a proof: the `controls.scrub` extension
// (IcomCivBackend::invokeExtension) checks the intent against the wire.

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
