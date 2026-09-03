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

    // Amateur bands this radio covers, as canonical BandDefs names, comma
    // separated -- the same "bands=" vocabulary a gateway declares, validated
    // model-side by parseDeclaredBands() before anything renders it.
    //
    // What it buys is the band BUTTONS. With no declaration the band menu falls
    // back to its built-in HF grid plus FlexLib's ModelCapabilities has4Meters/
    // has2Meters flags -- and an IC-705 matches nothing in that Flex model
    // table, so a radio that reaches 2 m and 70 cm natively had no button for
    // either, and 70 cm has no entry in that grid at any radio (#5041).
    //
    // EMPTY MEANS "the built-in HF grid is already right", not "unknown". Every
    // HF-only row below is served correctly by that grid, and tuningMaxHz
    // already disables whatever it cannot reach. So declare only where the grid
    // cannot express the radio -- i.e. it covers VHF/UHF -- and only within the
    // coverage the row itself already claims in tuningMinHz/tuningMaxHz, which
    // keeps this from becoming a second, drifting statement about the same
    // hardware. icom_family_test pins that containment.
    //
    // A name outside BandDefs is dropped at the boundary (Principle VII), so a
    // typo here costs a missing button, never a bogus one.
    std::string_view bands;

    [[nodiscard]] bool isKnown() const noexcept { return civAddress != 0; }
};

enum ModulationSource : unsigned {
    ModSourceNone      = 0,
    ModSourceMic       = 1U << 0,
    ModSourceUsb       = 1U << 1,
    ModSourceAccessory = 1U << 2,
    ModSourceNetwork   = 1U << 3,
};

struct ModulationInputChoice {
    std::uint8_t value = 0;
    std::string_view label;
    unsigned sources = ModSourceNone;
};

// Model-specific 1A 05 SET-menu map. Icom does not keep these item numbers or
// enum values stable between radios: the IC-705 calls its network source WLAN
// at value 03, while the IC-7300MK2 calls it LAN at value 05.
struct ModulationProfile {
    int usbLevelItem = -1;
    int accessoryLevelItem = -1;
    int networkLevelItem = -1;
    int dataOffInputItem = -1;
    int dataInputItem = -1;
    std::uint8_t networkOnlyValue = 0;
    // What PC Audio "off" falls back to when there is no captured selection to
    // put back — the hand microphone, which every Icom has. It lives in the
    // table rather than at the call site for the same reason networkOnlyValue
    // does: the enum is model-specific, and a future radio whose MIC is not
    // 0x00 must not silently inherit this one's.
    std::uint8_t micValue = 0;
    std::span<const ModulationInputChoice> choices;
    // Some network radios expose the level of the selected LAN modulation
    // path separately from 14 0B (the physical microphone gain).  When true,
    // the shared Phone level control follows that radio-owned LAN register
    // while LAN is the active modulation source.
    bool phoneLevelFollowsNetworkInput = false;
};

// The two PRs that motivated RFC #4984 expose different UI depths over a
// largely shared CI-V register family. Keep that distinction in the model
// profile: Basic is the IC-705 tone/offset/XFC surface from #5140; Extended is
// the access-selector, separate TX/RX tones and DTCS surface from #5149.
enum class FmRepeaterDialect : std::uint8_t {
    None,
    Basic,
    Extended,
};

struct FmRepeaterProfile {
    FmRepeaterDialect dialect = FmRepeaterDialect::None;
    std::span<const std::string_view> accessModes;
    bool hasDuplex = false;
    bool hasTxCtcss = false;
    bool hasRxCtcss = false;
    bool hasDtcs = false;
    bool hasXfc = false;
    bool hasTxFrequencyReadback = false;
};

// Empty when this model's own official CI-V guide has not been checked. A
// caller must not borrow another model's SET-menu map as a fallback.
[[nodiscard]] std::optional<ModulationProfile>
modulationProfileFor(const IcomModel& model);

// THE SSB TRANSMIT PASSBAND, and why it cannot be two sliders.
//
// AetherSDR's seam carries setTxFilter(lowHz, highHz) — two continuous
// numbers, because that is what a Flex takes. An Icom does not have that
// control at all. It has:
//
//   * a SHORT LIST of low edges and a SHORT LIST of high edges, and nothing in
//     between is reachable;
//   * FOUR STORED SLOTS holding one (low, high) pair each — WIDE, MID, NAR for
//     voice SSB and one more for SSB-DATA;
//   * 16 58, which picks WHICH voice slot is live — and the radio also swaps
//     slots on its own depending on whether the speech compressor is on.
//
// So a request lands by SNAPPING both edges to the nearest the model has and
// writing them into the slot currently in circuit. What the operator then sees
// must be the snapped pair read back from the radio, never the pair they asked
// for: an IC-705 asked for 150 Hz gives 100 or 200, and a Phone applet that
// kept showing 150 would be reporting a passband that does not exist.
//
// THE MODELS GENUINELY DIFFER, which is the reason this is per-model metadata
// and not a shared constant. The IC-7300MK2 added two low edges the IC-705 does
// not have (120 and 150 Hz), and the two radios keep the four slots at
// completely different SET-menu item numbers.
struct TxBandwidthProfile {
    // Ascending. Snapping assumes it.
    std::span<const int> lowEdgesHz;
    std::span<const int> highEdgesHz;
    // 1A 05 item numbers, decimal as the guide prints them.
    int wideItem = -1;
    int midItem = -1;
    int narrowItem = -1;
    int dataItem = -1;
};

// Empty for a model whose own guide has not been read. The caller must then
// leave setTxFilter() unimplemented and say so through capabilities rather than
// borrowing another radio's item numbers — writing a TX bandwidth into whatever
// SET item happens to live at that number on an unread model is a silent
// misconfiguration of the transmitter.
[[nodiscard]] std::optional<TxBandwidthProfile>
txBandwidthProfileFor(const IcomModel& model);

// The nearest value in an ascending table. Ties take the LOWER index, which for
// a low edge is the wider passband and for a high edge is the narrower one —
// both the conservative direction for a transmitter.
[[nodiscard]] int nearestEdgeHz(std::span<const int> table, int hz) noexcept;
[[nodiscard]] int edgeIndexFor(std::span<const int> table, int hz) noexcept;

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

// One RF deck: a range this model can tune, and the PA rating inside it.
//
// A model needs this only when its tunable range is NOT the single continuous
// interval [tuningMinHz, tuningMaxHz] — which, today, means the IC-9700 alone.
struct IcomBand {
    std::string_view name;
    std::uint64_t lowHz = 0;
    std::uint64_t highHz = 0;
    double maxWatts = 0.0;
};

// This model's discontinuous band table, or an EMPTY span when its tuning
// range is the one continuous tuningMinHz..tuningMaxHz interval.
//
// THE SINGLE SOURCE OF TRUTH for both halves of a banded model: the tune
// guard (supportsFrequency/nearestSupportedFrequency) and the capability
// ceilings (IcomCivBackend::capabilities) both read this one table, so a
// corrected edge or PA rating lands in every consumer at once. Two hand-kept
// copies would have let the guard and the power scale disagree silently —
// exactly the shape of drift that only shows up on the air.
//
// Emptiness is also the predicate the tune path keys on: no table means no
// holes to refuse, so continuous models keep their untouched command path.
[[nodiscard]] std::span<const IcomBand> bandsFor(const IcomModel& model) noexcept;

// Rated PA ceiling for the RF deck containing hz. Empty when the model has no
// per-band ratings or hz is outside every documented deck.
[[nodiscard]] std::optional<double> bandRatedPowerWatts(
    const IcomModel& model, std::uint64_t hz) noexcept;

// True when hz lies in a band this model can tune. Unknown models remain
// permissive because they have no verified range to enforce.
[[nodiscard]] bool supportsFrequency(const IcomModel& model,
                                     std::uint64_t hz) noexcept;

// Resolve an arbitrary request to the nearest frequency this model supports.
// Continuous-range and unknown models preserve their existing min/max policy;
// the IC-9700 snaps across the two holes between its three RF decks.
[[nodiscard]] std::uint64_t nearestSupportedFrequency(const IcomModel& model,
                                                      std::uint64_t hz) noexcept;

// Decode the reply to CI-V 0x19 0x00. Returns the reported address, or nullopt
// if this is not that reply.
[[nodiscard]] std::optional<std::uint8_t> parseModelIdReply(const CivFrame& frame);

// The S9 reference this model+frequency combination should use. Band-dependent,
// not model-dependent — see sMeterDbm().
[[nodiscard]] double s9ReferenceFor(std::uint64_t hz) noexcept;

// Model-owned curve for the Po meter. The output domain is declared by the
// profile's MeterCalibrationProfile::powerConversion: normally native watts;
// IC-9700 uniquely supplies relative percent for a below-seam derived estimate.
//
// EMPTY means no evidence-backed curve exists and the caller must report the
// generic relative indication rather than borrowing another radio's curve.
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

// The demodulator modes this model offers, in AetherSDR's NEUTRAL vocabulary —
// the same strings SliceModel carries and the mode combo displays.
//
// EMPTY means we have no verified mode table for this model, and the caller must
// publish NOTHING rather than borrow another radio's — the rule powerCurveFor
// and preampLabelsFor already state, for the same reason. An empty list leaves
// the UI on its compiled-in FlexRadio default, which is today's behaviour.
//
// NEUTRAL, not wire values, and every entry must ROUND-TRIP through
// modeFromNeutral/modeToNeutral. A name the radio can be put into but never
// reports back (RTTY, which comes home as DIGL) would make the combo jump on the
// confirmation read; a name modeFromNeutral refuses (SAM) would silently revert.
// Both read as a broken control, which is what this list exists to stop.
[[nodiscard]] std::span<const std::string_view> modeListFor(const IcomModel& model);

// True when this model's `mode` is RECEIVE-ONLY — the radio will not transmit in
// it whatever the client asks. Keyed on the neutral name, so it answers the same
// question the mode combo poses.
[[nodiscard]] bool modeIsReceiveOnly(const IcomModel& model, std::string_view neutralMode);

// Attenuator positions. The label is what the operator reads; the dB is what
// goes on the wire (BCD — see cmdSetAttenuator), so the two must not drift.
struct AttenStep {
    std::string_view label;
    int db;
};
[[nodiscard]] std::span<const AttenStep> attenStepsFor(const IcomModel& model);

// A control family, not a UI capability. ControlSpec rows name one of these so
// the automation registry can report the EFFECTIVE model-specific surface
// rather than claiming every CI-V constant on every Icom.
enum class IcomFeature : std::uint8_t {
    Core,
    Scope,
    VfoMode,
    ModulationInput,
    TxBandwidth,
    CwTextKeyer,
    RxAntenna,
    FmRepeaterBasic,
    FmRepeaterExtended,
    FmRepeaterExtendedReadback,
    FmRepeaterCtcssRx,
    TxFrequencyCheck,
    DialLock,
    CivDataRestart,
    MemoryChannels,
    AntennaTuner,
};

enum class MemoryDialect : std::uint8_t {
    Ic705,
    Ic7300Mk2,
    Ic9700,
};

struct MemoryProfile {
    MemoryDialect dialect;
    int firstGroup = -1;
    int lastGroup = -1;
    int firstChannel = 1;
    int lastChannel = 99;
    bool requiresGroupSelection = false;
    std::string_view groupColumnTitle = "Group";
};

enum class EvidenceKind : std::uint8_t {
    None,
    CrossReferenced,
    OfficialGuide,
    LiveHardware,
    OfficialGuideAndLiveHardware,
};

struct FeatureEvidence {
    IcomFeature feature = IcomFeature::Core;
    EvidenceKind evidence = EvidenceKind::None;
    std::string_view source;
};

struct SetMenuProfile {
    int voxDelayItem = -1;
    int civTransceiveItem = -1;
};

struct ScopeCommandProfile {
    bool center = false;
    bool fixed = false;
    bool scrollCenter = false;
    bool scrollFixed = false;
    bool hasSweepSpeed = false;
};

struct CwTextKeyerProfile {
    int minWpm = 6;
    int maxWpm = 48;
    int maxMessageChars = 30;
};

struct RxAntennaProfile {
    bool selectable = false;
    bool readbackAvailable = false;
};

struct MeterCalibrationProfile {
    enum class PowerConversion : std::uint8_t {
        NativeWatts,
        RelativePercentOfBandRating,
    };

    MeterCalibration calibration = MeterCalibration::Uncalibrated;
    double currentFullScaleAmps = 4.0;
    PowerConversion powerConversion = PowerConversion::NativeWatts;
    // Opt into a forward-power face derived from this model's published
    // txPowerMaxWatts even when it has one continuous tuning range. Keep this
    // model-specific: a low-power face must not leak to sibling Icom profiles.
    bool scaleForwardPowerToRatedOutput = false;
    // UI exposure is narrower than wire decoding. Several Icom profiles have
    // an Id calibration, but each model must be approved independently before
    // Radio Vitals offers that instrument.
    bool hasPaCurrentTelemetry = false;
    // Live IC-705 and IC-7300MK2 evidence: SWR/ALC can return an isolated
    // minimum between real keyed samples. Never lend that interpretation to a
    // model whose own meter stream has not demonstrated it.
    bool holdIsolatedTxMinimums = false;
    // True only after this model profile both documents and implements a PA
    // temperature meter. Kept model-specific so one Icom cannot lend an
    // unverified instrument to another merely because they share CI-V.
    bool hasPaTemperatureTelemetry = false;
};

// Recovery policy is model capability, not shared Icom scheduler policy.  The
// RS-BA1 data-start envelope and its retry timing are enabled only for models
// whose public/live evidence supports this exact recovery path.
struct CivRecoveryProfile {
    int retryIntervalMs = 1000;
    int maxAttempts = 3;
};

// Model-owned 1A 05 register addresses for radio-authoritative network state.
// These differ across Icom command tables and are absent from the IC-705 guide.
struct NetworkConfigurationProfile {
    int effectiveIpItem = -1;
    int subnetMaskItem = -1;
    int gatewayItem = -1;
    int networkNameItem = -1;
};

// The immutable, backend-private capability profile from RFC #4984. IcomModel
// remains transport/identity geometry; every command-table difference lives
// here. Adding a radio is intentionally metadata-first and conservative: code
// migrated to a facet must treat its absence as unsupported and must never
// borrow another model's command shape or calibration.
struct IcomModelProfile {
    bool supportedBringup = false;
    bool hasGpsHardware = false;
    int speechProcessorLevelMaximum = 2;
    std::string_view speechProcessorLabel = "PROC";
    std::string_view guideRevision;
    std::span<const FeatureEvidence> features;

    std::span<const IcomBand> bands;
    std::optional<ModulationProfile> modulation;
    std::optional<TxBandwidthProfile> txBandwidth;
    std::optional<FmRepeaterProfile> fmRepeater;
    std::optional<CwTextKeyerProfile> cwTextKeyer;
    std::optional<RxAntennaProfile> rxAntenna;
    SetMenuProfile setMenu;
    ScopeCommandProfile scope;
    MeterCalibrationProfile meters;
    std::optional<CivRecoveryProfile> civRecovery;
    std::optional<MemoryProfile> memory;
    std::optional<NetworkConfigurationProfile> networkConfiguration;
    std::span<const std::string_view> preampLabels;
    std::span<const AttenStep> attenuatorSteps;
    std::span<const std::string_view> modes;
    std::span<const std::string_view> receiveOnlyModes;

    [[nodiscard]] const FeatureEvidence* evidenceFor(IcomFeature feature) const noexcept;
    [[nodiscard]] bool supports(IcomFeature feature) const noexcept;
};

[[nodiscard]] const IcomModelProfile& profileFor(const IcomModel& model) noexcept;
[[nodiscard]] std::string_view featureName(IcomFeature feature) noexcept;
[[nodiscard]] std::string_view evidenceName(EvidenceKind evidence) noexcept;

}  // namespace AetherSDR::icom
