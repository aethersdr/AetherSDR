#include "core/backends/icom/IcomModels.h"

#include "core/backends/icom/CivCodec.h"   // setting::kNtp* — one source for the SET items

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>

namespace AetherSDR::icom {
namespace {

// `verified` rows were read from that model's OWN Icom CI-V Reference Guide:
//   IC-705      475 points, range 0..160, division max 1 over WLAN / 11 over
//               USB, one receiver, one scope (0x27 0x12 / 0x27 0x13 fixed at
//               00), 10 W.
//   IC-7300MK2  CI-V address 0xB6 (FE FE E0 B6), 475 points, range 0..160, LAN
//               data length 490, 0.03-74.8 MHz, all FOUR scope modes.
// The rest are cross-referenced hardware facts, unverified: the transport is
// shared, but each model's command table and scope geometry need its own guide.
constexpr std::array<IcomModel, 7> kModels{{
    {
        /*civAddress*/ 0xA4, /*name*/ "IC-705",
        /*receivers*/ 1, /*vfos*/ 2,
        /*hasNetwork*/ true, /*hasWifi*/ true,
        /*hasScope*/ true, /*scopePoints*/ 475, /*scopeMaxAmplitude*/ 160,
        /*scopeDivisionsUsb*/ 11,
        /*freqBytes*/ kFreqBytes,
        /*hasTransmit*/ true, /*txPowerMaxWatts*/ 10.0,
        /*tuningMinHz*/ 30'000ULL, /*tuningMaxHz*/ 470'000'000ULL,
        /*verified*/ true,
        // Amateur allocations inside the 30 kHz – 470 MHz range above; 70 cm is "440"
        // as BandDefs names it. 2200m/630m are non-declarable (#4027) and 4 m is
        // RX-only here, so they get no band button; typing the frequency still works.
        /*bands*/ "160m,80m,60m,40m,30m,20m,17m,15m,12m,10m,6m,2m,440",
    },
    {
        // IC-9700 — scope geometry MEASURED on a live radio (618 frames, all 475
        // pixels; bounds header centre 439.864060 MHz, 500 kHz span = 1052.6 Hz/pixel),
        // not read from the guide, so `verified` stays false. 475/160/11 match the
        // IC-705; the IC-7610 (689 points, 0..200) shows this cannot be assumed.
        0xA2, "IC-9700", 2, 2,
        /*hasNetwork*/ true, /*hasWifi*/ false,
        /*hasScope*/ true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        144'000'000ULL, 1'300'000'000ULL,
        /*verified*/ false,
        // The tri-bander's three bands, exactly the 144 MHz – 1.3 GHz the row
        // already claims. Declaring is not a nicety here: with no declaration
        // this radio gets the HF grid, every button of which its tuning range
        // then DISABLES — a band menu with nothing in it that can be pressed.
        /*bands*/ "2m,440,23cm",
    },
    {
        0x98, "IC-7610", 2, 1,
        /*hasNetwork*/ true, /*hasWifi*/ false,
        // 689 points and a 0..200 range — BOTH differ from the IC-705, which is
        // exactly why the scope geometry cannot be a compile-time constant.
        /*hasScope*/ true, 689, 200, 15,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 60'000'000ULL,
        /*verified*/ false,
        /*bands*/ "",
    },
    {
        0x8E, "IC-785x", 2, 1,
        true, false,
        true, 689, 200, 15,
        kFreqBytes,
        true, 200.0,
        30'000ULL, 60'000'000ULL,
        false,
        /*bands*/ "",
    },
    {
        // NO NETWORK. Reachable only over a local serial port, or over the
        // network through Icom's own RS-BA1 server acting as a front end — in
        // which case this same backend reaches it without any extra work.
        0x94, "IC-7300", 1, 2,
        /*hasNetwork*/ false, /*hasWifi*/ false,
        true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 74'800'000ULL,
        false,
        /*bands*/ "",
    },
    {
        // IC-7300MK2 — VERIFIED against IC-7300MK2_ENG_CI-V_0. Unlike the IC-7300 it
        // has a LAN port, so it speaks RS-BA1 directly. Scope over LAN is "sent all at
        // once", over USB "divided into 11 segments" (division max 01/11, as IC-705).
        // The guide's LAN data length of 490 confirms the 15-byte first-division header
        // (3 + 1 + 5*2 + 1 == 15, and 15 + 475 == 490).
        0xB6, "IC-7300MK2", 1, 2,
        /*hasNetwork*/ true, /*hasWifi*/ false,   // Ethernet, not WiFi
        /*hasScope*/ true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 74'800'000ULL,
        /*verified*/ true,
        /*bands*/ "",
    },
    {
        // SIX-BYTE FREQUENCIES above 10 GHz. A codec written against a
        // hardcoded 5 misaligns by two bytes and decodes a plausible-looking
        // wrong frequency — which on transmit is an out-of-band emission.
        0xAC, "IC-905", 1, 2,
        true, false,
        true, 475, 160, 11,
        /*freqBytes*/ 6,
        true, 10.0,
        144'000'000ULL, 10'500'000'000ULL,
        false,
        // NO DECLARATION, on purpose. This radio's bands are not the contiguous
        // span its 144 MHz – 10.5 GHz range suggests — it covers five discrete
        // bands, the top one only with the CX-10G unit fitted — so the band set
        // is a fact to read out of the model's own guide, like the rest of this
        // unverified row, not one to infer from two numbers. Until someone does,
        // frequency entry still reaches every one of them.
        /*bands*/ "",
    },
}};

// Conservative fallback for an unrecognised address. No scope, no transmit.
constexpr IcomModel kUnknown{
    /*civAddress*/ 0x00, /*name*/ "Unknown Icom",
    /*receivers*/ 1, /*vfos*/ 2,
    /*hasNetwork*/ true, /*hasWifi*/ false,
    /*hasScope*/ false, /*scopePoints*/ 0, /*scopeMaxAmplitude*/ 0,
    /*scopeDivisionsUsb*/ 11,
    /*freqBytes*/ kFreqBytes,
    /*hasTransmit*/ false, /*txPowerMaxWatts*/ 0.0,
    /*tuningMinHz*/ 0, /*tuningMaxHz*/ 0,
    /*verified*/ false,
    /*bands*/ "",
};

// The IC-9700's three RF decks, each with its own PA rating — the single source
// for both the tune guard (refuses the holes between decks) and
// IcomCivBackend::capabilities() txPowerBands. Ranges are the US/A version;
// narrower regional radios (EU stops at 146 and 440) refuse the excess
// themselves, which is the safe direction to be wrong in.
constexpr std::array<IcomBand, 3> kIc9700Bands{{
    {"2m",     144'000'000ULL,   148'000'000ULL, 100.0},
    {"440",    430'000'000ULL,   450'000'000ULL,  75.0},
    {"23cm", 1'240'000'000ULL, 1'300'000'000ULL,  10.0},
}};

constexpr std::array<ModulationInputChoice, 4> kIc705ModInputs{{
    {0x00, "MIC",     ModSourceMic},
    {0x01, "USB",     ModSourceUsb},
    {0x02, "MIC+USB", ModSourceMic | ModSourceUsb},
    {0x03, "WLAN",    ModSourceNetwork},
}};

// IC-9700 CI-V Reference Guide 2019, SET > Connectors > MOD Input,
// 1A 05 0115/0116.  The numeric vocabulary is model-owned: it happens to
// match neither the shorter IC-705 table nor every future networked Icom.
constexpr std::array<ModulationInputChoice, 6> kIc9700ModInputs{{
    {0x00, "MIC",     ModSourceMic},
    {0x01, "ACC",     ModSourceAccessory},
    {0x02, "MIC+ACC", ModSourceMic | ModSourceAccessory},
    {0x03, "USB",     ModSourceUsb},
    {0x04, "MIC+USB", ModSourceMic | ModSourceUsb},
    {0x05, "LAN",     ModSourceNetwork},
}};

constexpr std::array<ModulationInputChoice, 6> kIc7300Mk2ModInputs{{
    {0x00, "MIC",     ModSourceMic},
    {0x01, "USB",     ModSourceUsb},
    {0x02, "ACC",     ModSourceAccessory},
    {0x03, "MIC+USB", ModSourceMic | ModSourceUsb},
    {0x04, "MIC+ACC", ModSourceMic | ModSourceAccessory},
    {0x05, "LAN",     ModSourceNetwork},
}};

constexpr std::array<std::string_view, 3> kHfPreampLabels{
    "OFF", "P.AMP1", "P.AMP2"};
// Publish only the IC-9700's internal preamp through the shared front-end
// control. External P.AMP is separately enabled per band in SET menu items
// 0093..0095; treating those persistent settings as ordinary preamp steps
// makes the radio reject the request and restore its authoritative state.
constexpr std::array<std::string_view, 2> kIc9700PreampLabels{
    "OFF", "P.AMP INT"};
constexpr std::array<AttenStep, 2> kHfAttenuatorSteps{{
    {"OFF", 0}, {"20 dB", 20}}};
constexpr std::array<std::string_view, 10> kIc705Modes{
    "USB", "LSB", "CW", "CWL", "AM", "FM", "DFM", "WFM", "DIGU", "DIGL"};
constexpr std::array<std::string_view, 1> kIc705ReceiveOnlyModes{"WFM"};

// The three supported bring-up targets share the complete documented repeater
// vocabulary. Their presentation can still differ (Basic versus Extended), but
// no profile is permitted to pretend these registers are IC-9700-only: both
// official HF-portable/desktop guides list 16 5D and 1B 00/01/02 as well.
constexpr std::array<std::string_view, 8> kExtendedFmAccessModes{
    "off", "ctcss_tx", "ctcss_rx", "ctcss_txrx",
    "dtcs_tx", "dtcs_txrx", "ctcss_tx_dtcs_rx", "dtcs_tx_ctcss_rx"};
constexpr std::array<std::string_view, 4> kToneSquelchFmAccessModes{
    "off", "ctcss_tx", "ctcss_rx", "ctcss_txrx"};

constexpr std::array<FeatureEvidence, 17> kIc705Evidence{{
    {IcomFeature::Core, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020; live IC-705 bring-up"},
    {IcomFeature::Scope, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, command 27"},
    {IcomFeature::VfoMode, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, 26 00"},
    {IcomFeature::ModulationInput, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, SET 0116-0119"},
    {IcomFeature::TxBandwidth, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, SET 0019-0022"},
    {IcomFeature::CwTextKeyer, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, command 17"},
    {IcomFeature::FmRepeaterBasic, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020; live tone/level/offset/XFC proof"},
    {IcomFeature::FmRepeaterExtended, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, 16 5D and 1B 00/01/02"},
    {IcomFeature::FmRepeaterExtendedReadback, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, 16 5D and 1B 01/02"},
    {IcomFeature::FmRepeaterCtcssRx, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, 16 5D and 1B 00/01"},
    {IcomFeature::TxFrequencyCheck, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, 1C 02"},
    {IcomFeature::DialLock, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, 16 50"},
    {IcomFeature::RxAntenna, EvidenceKind::None, "not supported"},
    {IcomFeature::GpsPosition, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, 23 00/01; live position proof 2026-08-21"},
    {IcomFeature::GpsTimeConfiguration, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-705 CI-V Reference Guide 2020, SET 0167-0169 and 1A 07/08; live NTP proof 2026-08-21"},
    {IcomFeature::MemoryChannels, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, command 1A 00 memory-channel records"},
    {IcomFeature::AntennaTuner, EvidenceKind::OfficialGuide,
     "IC-705 CI-V Reference Guide 2020, command 1C 01"},
}};

constexpr std::array<FeatureEvidence, 15> kIc7300Mk2Evidence{{
    {IcomFeature::Core, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-7300MK2 CI-V Reference Guide; live IC-7300MK2 bring-up"},
    {IcomFeature::Scope, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, command 27"},
    {IcomFeature::VfoMode, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, 26 00"},
    {IcomFeature::ModulationInput, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-7300MK2 CI-V Reference Guide, SET 0081-0085"},
    {IcomFeature::TxBandwidth, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, SET 0014-0017"},
    {IcomFeature::CwTextKeyer, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, command 17"},
    {IcomFeature::RxAntenna, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-7300MK2 CI-V Reference Guide, 12 00; live readback returned FB"},
    {IcomFeature::FmRepeaterBasic, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, 0C/0D, 0F, 16 42, 1B 00"},
    {IcomFeature::FmRepeaterExtended, EvidenceKind::None,
     "DTCS and mixed tone access not documented for IC-7300MK2"},
    {IcomFeature::FmRepeaterExtendedReadback, EvidenceKind::None,
     "extended repeater readback is not attested"},
    {IcomFeature::FmRepeaterCtcssRx, EvidenceKind::None,
     "not activated: preserve the proven IC-7300MK2 basic repeater path"},
    {IcomFeature::TxFrequencyCheck, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, 1C 02/03"},
    {IcomFeature::DialLock, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, 16 50"},
    {IcomFeature::MemoryChannels, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, command 1A 00 memory-channel records"},
    {IcomFeature::AntennaTuner, EvidenceKind::OfficialGuide,
     "IC-7300MK2 CI-V Reference Guide, command 1C 01"},
}};

constexpr std::array<FeatureEvidence, 14> kIc9700Evidence{{
    {IcomFeature::Core, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019; live IC-9700 trace"},
    {IcomFeature::Scope, EvidenceKind::LiveHardware,
     "live IC-9700 475-point scope trace, 2026-08-05"},
    {IcomFeature::VfoMode, EvidenceKind::LiveHardware,
     "live IC-9700 26 00 reply, 2026-08-14"},
    {IcomFeature::ModulationInput, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019, SET 0112-0116 (printed p.7); "
     "live IC-9700 LAN MOD read/write proof"},
    {IcomFeature::FmRepeaterBasic, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019; PR #5149 live trace"},
    {IcomFeature::FmRepeaterExtended, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019, pp. 4-5 and 11; PR #5149 live trace"},
    {IcomFeature::FmRepeaterExtendedReadback,
     EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 16 5D, 1B 01/02 and 1C 03; PR #5149 live trace"},
    {IcomFeature::FmRepeaterCtcssRx, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019, 16 5D and 1B 00/01; live IC-9700"},
    {IcomFeature::TxFrequencyCheck, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019, 1C 02/03; PR #5149 live trace"},
    {IcomFeature::DialLock, EvidenceKind::OfficialGuideAndLiveHardware,
     "IC-9700 CI-V Reference Guide 2019, 16 50; PR #5261 live proof"},
    {IcomFeature::CivDataRestart, EvidenceKind::CrossReferenced,
     "wfview RS-BA1 data-start implementation and published physical IC-9700 watchdog log"},
    {IcomFeature::MemoryChannels, EvidenceKind::OfficialGuide,
     "IC-9700 CI-V Reference Guide 2019, command 1A 00 memory-channel records"},
    {IcomFeature::RxAntenna, EvidenceKind::None, "not attested"},
    {IcomFeature::AntennaTuner, EvidenceKind::None,
     "IC-9700 CI-V Reference Guide does not declare an antenna tuner"},
}};

constexpr std::array<FeatureEvidence, 1> kTunerOnlyEvidence{{
    {IcomFeature::AntennaTuner, EvidenceKind::OfficialGuide,
     "IC-7300, IC-7610, and IC-7850/IC-7851 CI-V Reference Guides, command 1C 01"},
}};

}  // namespace

const IcomModel* modelForId(std::uint8_t addr)
{
    for (const auto& m : kModels)
        if (m.civAddress == addr)
            return &m;
    return nullptr;
}

namespace {
std::string canonicalName(std::string_view in)
{
    std::string out;
    for (char c : in) {
        if (c == '-' || c == ' ' || c == '_')
            continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}
}  // namespace

const IcomModel* modelForName(std::string_view name)
{
    if (name.empty())
        return nullptr;
    const std::string wanted = canonicalName(name);
    for (const auto& m : kModels)
        if (canonicalName(m.name) == wanted)
            return &m;
    return nullptr;
}

std::span<const IcomModel> knownModels() { return kModels; }

const IcomModel& unknownModel() { return kUnknown; }

std::span<const IcomBand> bandsFor(const IcomModel& model) noexcept
{
    return profileFor(model).bands;
}

std::optional<double> bandRatedPowerWatts(const IcomModel& model,
                                          std::uint64_t hz) noexcept
{
    const std::span<const IcomBand> bands = bandsFor(model);
    const auto active = std::ranges::find_if(bands, [hz](const IcomBand& band) {
        return hz >= band.lowHz && hz <= band.highHz;
    });
    if (active == bands.end()) {
        return std::nullopt;
    }
    return active->maxWatts;
}

bool supportsFrequency(const IcomModel& model, std::uint64_t hz) noexcept
{
    if (const std::span<const IcomBand> bands = bandsFor(model); !bands.empty()) {
        return std::ranges::any_of(bands, [hz](const IcomBand& band) {
            return hz >= band.lowHz && hz <= band.highHz;
        });
    }
    if (model.tuningMinHz == 0 || model.tuningMaxHz == 0) {
        return true;
    }
    return hz >= model.tuningMinHz && hz <= model.tuningMaxHz;
}

std::uint64_t nearestSupportedFrequency(const IcomModel& model,
                                        std::uint64_t hz) noexcept
{
    if (supportsFrequency(model, hz)) {
        return hz;
    }
    if (const std::span<const IcomBand> bands = bandsFor(model); !bands.empty()) {
        std::uint64_t nearest = bands.front().lowHz;
        std::uint64_t distance = hz > nearest ? hz - nearest : nearest - hz;
        for (const IcomBand& band : bands) {
            for (const std::uint64_t edge : {band.lowHz, band.highHz}) {
                const std::uint64_t edgeDistance = hz > edge ? hz - edge : edge - hz;
                if (edgeDistance < distance) {
                    nearest = edge;
                    distance = edgeDistance;
                }
            }
        }
        return nearest;
    }
    if (model.tuningMinHz == 0 || model.tuningMaxHz == 0) {
        return hz;
    }
    return std::clamp(hz, model.tuningMinHz, model.tuningMaxHz);
}

std::optional<ModulationProfile> modulationProfileFor(const IcomModel& model)
{
    return profileFor(model).modulation;
}

// TX bandwidth edge tables.
//
// HIGH EDGES ARE THE SAME ON BOTH RADIOS; the low edges are not. Transcribed
// from the "SSB/SSB-DATA transmission passband width settings" page of each
// model's own CI-V Reference Guide — IC-7300MK2 p.19 (1A 05 00 14..00 17),
// IC-705 p.19 (1A 05 0019..0022).
constexpr std::array<int, 4> kTbwLowIc705{100, 200, 300, 500};
constexpr std::array<int, 6> kTbwLowIc7300Mk2{100, 120, 150, 200, 300, 500};
constexpr std::array<int, 4> kTbwHigh{2500, 2700, 2800, 2900};

std::optional<TxBandwidthProfile> txBandwidthProfileFor(const IcomModel& model)
{
    return profileFor(model).txBandwidth;
}

int edgeIndexFor(std::span<const int> table, int hz) noexcept
{
    if (table.empty())
        return 0;
    std::size_t best = 0;
    int bestDelta = std::abs(table[0] - hz);
    for (std::size_t i = 1; i < table.size(); ++i) {
        const int delta = std::abs(table[i] - hz);
        if (delta < bestDelta) {
            best = i;
            bestDelta = delta;
        }
    }
    return static_cast<int>(best);
}

int nearestEdgeHz(std::span<const int> table, int hz) noexcept
{
    if (table.empty())
        return hz;
    return table[static_cast<std::size_t>(edgeIndexFor(table, hz))];
}

std::optional<std::uint8_t> parseModelIdReply(const CivFrame& frame)
{
    if (frame.cmd != cmd::kReadId || !frame.hasSub || frame.sub != 0x00)
        return std::nullopt;
    if (frame.data.size() != 1 || frame.from == kBroadcastAddress
        || frame.from >= kControllerAddress
        || (frame.to != kControllerAddress && frame.to != kBroadcastAddress)) {
        return std::nullopt;
    }
    return frame.data[0];
}

std::span<const CurvePoint> powerCurveFor(const IcomModel& model)
{
    const MeterCalibrationProfile& meters = profileFor(model).meters;
    if (meters.powerConversion
        == MeterCalibrationProfile::PowerConversion::RelativePercentOfBandRating) {
        return powerCurveIc9700();
    }
    return powerCurveForCalibration(meters.calibration);
}

std::span<const std::string_view> preampLabelsFor(const IcomModel& model)
{
    // The IC-705's HF ladder. Above 50 MHz the radio collapses to OFF/P.AMP1
    // and refuses P.AMP2, then reports what it actually did — which is why this
    // is published once rather than rewritten on every band change under an
    // operator who may be mid-adjustment.
    return profileFor(model).preampLabels;
}

std::span<const std::string_view> modeListFor(const IcomModel& model)
{
    // The IC-705's 0x06 modes in neutral names. Omitted wire modes:
    //   RTTY / RTTY-R — modeToNeutral() maps them to DIGL/DIGU, so offering "RTTY"
    //                   would make the combo jump on the confirmation read.
    //   DV            — D-STAR is a waveform; modeToNeutral() returns "" for it.
    // DFM/DIGU/DIGL are the DATA-flag forms of FM/USB/LSB (cmdSetVfoMode carries
    // the flag). Publishing this list is what makes WFM reachable (#5040).
    return profileFor(model).modes;
}

bool modeIsReceiveOnly(const IcomModel& model, std::string_view neutralMode)
{
    // WFM is broadcast receive only (IC-705: 76-108 MHz, outside every amateur TX
    // allocation). Answered only for models whose mode table has been read. The
    // withdrawn identity (kUnknown) is still safe even though stale combos may
    // offer WFM (#891): it reports hasTransmit=false, so RadioModel refuses to key
    // it in any mode (#5106).
    const std::span<const std::string_view> modes = profileFor(model).receiveOnlyModes;
    return std::ranges::find(modes, neutralMode) != modes.end();
}

std::span<const AttenStep> attenStepsFor(const IcomModel& model)
{
    // ONE step on the IC-705, and 20 dB is its real figure — nameable in dB
    // where the preamp positions are not, because the guide publishes it. HF
    // and 50 MHz only; higher bands ignore the request and report OFF.
    return profileFor(model).attenuatorSteps;
}

const FeatureEvidence* IcomModelProfile::evidenceFor(IcomFeature feature) const noexcept
{
    const auto it = std::ranges::find(features, feature, &FeatureEvidence::feature);
    return it == features.end() ? nullptr : &*it;
}

bool IcomModelProfile::supports(IcomFeature feature) const noexcept
{
    // Core is the implementation's model-neutral CI-V floor: identity,
    // frequency, mode and the other generic registers that the backend already
    // uses for every discovered Icom. Evidence remains independently absent on
    // an unprofiled model so diagnostics do not turn reachability into an
    // attestation.
    if (feature == IcomFeature::Core) {
        return true;
    }
    const FeatureEvidence* evidence = evidenceFor(feature);
    return evidence && evidence->evidence != EvidenceKind::None;
}

const IcomModelProfile& profileFor(const IcomModel& model) noexcept
{
    // These are the three intentional bring-up profiles. Other identity rows
    // remain discoverable, but receive the conservative empty profile until
    // their own guide is mapped. This is what prevents a copied IC-705 table
    // from silently becoming a write contract for another transmitter.
    static const IcomModelProfile kIc705Profile{
        .supportedBringup = true,
        .hasGpsHardware = true,
        // IC-705 guide: 14 0E "0000=0 ~ 0255=10", the same COMP 0..10 as the
        // MK2. The MK2's bin readback is assumed here, pending a live IC-705.
        .speechProcessorLevelMaximum = 10,
        .speechProcessorLabel = "COMP",
        .guideRevision = "IC-705 CI-V Reference Guide 2020",
        .features = kIc705Evidence,
        .modulation = ModulationProfile{116, -1, 117, 118, 119, 0x03, 0x00,
                                        kIc705ModInputs},
        .txBandwidth = TxBandwidthProfile{kTbwLowIc705, kTbwHigh, 19, 20, 21, 22},
        .fmRepeater = FmRepeaterProfile{FmRepeaterDialect::Extended,
                                       kExtendedFmAccessModes,
                                       true, true, true, true, true, true},
        .cwTextKeyer = CwTextKeyerProfile{},
        .gps = GpsProfile{setting::kNtpEnabled, setting::kNtpServer,
                          setting::kGpsTimeCorrect, true},
        .setMenu = SetMenuProfile{359, 131},
        .scope = ScopeCommandProfile{true, false, false, false, false},
        .meters = MeterCalibrationProfile{
            .calibration = MeterCalibration::Ic705,
            .currentFullScaleAmps = 4.0,
            .scaleForwardPowerToRatedOutput = true,
            .holdIsolatedTxMinimums = true,
        },
        // IC-705 CI-V Reference Guide 2020, p.15 note *4: 18 01 powers
        // on from Standby/Shutdown. Native RS-BA1 carries the standard frame;
        // no model-specific serial preamble is prescribed. Network must be up.
        .powerOn = PowerOnProfile{0, kControllerAddress, 1000},
        .memory = MemoryProfile{MemoryDialect::Ic705, 0, 99, 0, 99, true, "Group"},
        .preampLabels = kHfPreampLabels,
        .attenuatorSteps = kHfAttenuatorSteps,
        .modes = kIc705Modes,
        .receiveOnlyModes = kIc705ReceiveOnlyModes,
    };
    static const IcomModelProfile kIc9700Profile{
        .supportedBringup = true,
        .speechProcessorLevelMaximum = 100,
        .speechProcessorLabel = "COMP",
        .guideRevision = "IC-9700 CI-V Reference Guide 2019",
        .features = kIc9700Evidence,
        .bands = kIc9700Bands,
        // Official guide, printed p.7: ACC/USB/LAN levels are 0112/0113/0114;
        // DATA OFF MOD and DATA MOD are 0115/0116, with LAN encoded as 05.
        .modulation = ModulationProfile{113, 112, 114, 115, 116, 0x05, 0x00,
                                        kIc9700ModInputs, true},
        .fmRepeater = FmRepeaterProfile{FmRepeaterDialect::Extended,
                                       kExtendedFmAccessModes,
                                       true, true, true, true, true, true},
        .scope = ScopeCommandProfile{true, false, false, false, false},
        .meters = MeterCalibrationProfile{
            .calibration = MeterCalibration::Ic9700,
            .currentFullScaleAmps = 20.0,
            .powerConversion = MeterCalibrationProfile::PowerConversion::RelativePercentOfBandRating,
            .hasPaCurrentTelemetry = true,
        },
        // Native LAN sequence measured by W5JWP in #5360: 150 extra FE, E1.
        // This is hardware-derived, not the guide's serial 115200-baud count
        // (approximately 119 FE). Do not copy it to another model/transport.
        .powerOn = PowerOnProfile{150, 0xE1, 10000},
        .civRecovery = CivRecoveryProfile{1000, 3},
        .memory = MemoryProfile{MemoryDialect::Ic9700, 1, 3, 1, 99, false, "Band"},
        // IC-9700 CI-V Reference Guide 2019, printed p. 8.
        .networkConfiguration = NetworkConfigurationProfile{139, 140, 141, 144},
        .preampLabels = kIc9700PreampLabels,
    };
    static const IcomModelProfile kIc7300Mk2Profile{
        .supportedBringup = true,
        // CI-V 14 09 endpoints plus wfview funcCwPitch's 5 Hz decoding.
        .cwPitchStepHz = 5,
        .hasModeIndependentSquelch = true,
        .hasCwTune = false,
        .pollCwSquelchAndTxBandwidth = true,
        // MK2 guide: 14 0E "00 00=0 ~ 02 55=10"; the radio's own COMP control
        // is 0..10. Not NOR/DX/DX+, which is Flex's speech processor.
        .speechProcessorLevelMaximum = 10,
        .speechProcessorLabel = "COMP",
        .guideRevision = "IC-7300MK2 CI-V Reference Guide",
        .features = kIc7300Mk2Evidence,
        .modulation = ModulationProfile{81, 82, 83, 84, 85, 0x05, 0x00,
                                        kIc7300Mk2ModInputs},
        .txBandwidth = TxBandwidthProfile{kTbwLowIc7300Mk2, kTbwHigh,
                                          14, 15, 16, 17},
        // MK2 guide p. 3: 0F is split 00/01 only; 0C/0D and duplex
        // 10/11/12 are absent. Its SET-menu split offset is a different control.
        .fmRepeater = FmRepeaterProfile{FmRepeaterDialect::Basic,
                                       kToneSquelchFmAccessModes,
                                       false, true, true, false, true, true},
        .cwTextKeyer = CwTextKeyerProfile{},
        .rxAntenna = RxAntennaProfile{true, true},
        .setMenu = SetMenuProfile{267, 89},
        .scope = ScopeCommandProfile{true, true, true, true, true},
        .meters = MeterCalibrationProfile{
            .calibration = MeterCalibration::Ic7300Mk2,
            .currentFullScaleAmps = 25.0,
            .holdIsolatedTxMinimums = true,
        },
        // IC-7300MK2 guide, "Turning the transceiver ON": 18 01.
        // Baud-dependent FE fill is explicitly for the REMOTE jack, not LAN.
        // Begin probing after one second; readiness comes from the identity reply.
        .powerOn = PowerOnProfile{0, kControllerAddress, 1000},
        .memory = MemoryProfile{MemoryDialect::Ic7300Mk2, -1, -1, 1, 99, false,
                                "Group"},
        // IC-7300MK2 CI-V Reference Guide, SET > Network, printed p. 10.
        .networkConfiguration = NetworkConfigurationProfile{102, 103, 104, 107},
        .preampLabels = kHfPreampLabels,
        .attenuatorSteps = kHfAttenuatorSteps,
    };
    static const IcomModelProfile kUnprofiled{};
    static const IcomModelProfile kTunerOnlyProfile{
        .guideRevision = "model-specific CI-V Reference Guide",
        .features = kTunerOnlyEvidence,
    };

    switch (model.civAddress) {
    case 0xA4:
        return kIc705Profile;
    case 0xA2:
        return kIc9700Profile;
    case 0xB6:
        return kIc7300Mk2Profile;
    case 0x94: // IC-7300
    case 0x98: // IC-7610
    case 0x8E: // IC-7850 / IC-7851
        return kTunerOnlyProfile;
    default:
        return kUnprofiled;
    }
}

std::string_view featureName(IcomFeature feature) noexcept
{
    switch (feature) {
    case IcomFeature::Core:               return "core";
    case IcomFeature::Scope:              return "scope";
    case IcomFeature::VfoMode:            return "vfo-mode";
    case IcomFeature::ModulationInput:     return "modulation-input";
    case IcomFeature::TxBandwidth:         return "tx-bandwidth";
    case IcomFeature::CwTextKeyer:         return "cw-text-keyer";
    case IcomFeature::RxAntenna:           return "rx-antenna";
    case IcomFeature::FmRepeaterBasic:     return "fm-repeater-basic";
    case IcomFeature::FmRepeaterExtended:  return "fm-repeater-extended";
    case IcomFeature::FmRepeaterExtendedReadback:
        return "fm-repeater-extended-readback";
    case IcomFeature::FmRepeaterCtcssRx:   return "fm-repeater-ctcss-rx";
    case IcomFeature::TxFrequencyCheck:    return "tx-frequency-check";
    case IcomFeature::DialLock:            return "dial-lock";
    case IcomFeature::CivDataRestart:      return "civ-data-restart";
    case IcomFeature::GpsPosition:         return "gps-position";
    case IcomFeature::GpsTimeConfiguration: return "gps-time-configuration";
    case IcomFeature::MemoryChannels:      return "memory-channels";
    case IcomFeature::AntennaTuner:        return "antenna-tuner";
    }
    return "unknown";
}

std::string_view evidenceName(EvidenceKind evidence) noexcept
{
    switch (evidence) {
    case EvidenceKind::None:                         return "none";
    case EvidenceKind::CrossReferenced:              return "cross-referenced";
    case EvidenceKind::OfficialGuide:                return "official-guide";
    case EvidenceKind::LiveHardware:                 return "live-hardware";
    case EvidenceKind::OfficialGuideAndLiveHardware: return "official-guide+live-hardware";
    }
    return "unknown";
}

double s9ReferenceFor(std::uint64_t hz) noexcept
{
    // BAND-dependent, not model-dependent. IARU Region 1: S9 is -73 dBm below
    // 30 MHz and -93 dBm above. Using -73 everywhere reports VHF signals 20 dB
    // hot, which on a 2 m weak-signal band is the entire usable range.
    return usesVhfSReference(hz) ? kS9DbmVhf : kS9DbmHf;
}

}  // namespace AetherSDR::icom
