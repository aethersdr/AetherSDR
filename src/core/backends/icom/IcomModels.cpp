#include "core/backends/icom/IcomModels.h"

#include <array>
#include <cctype>
#include <string>

namespace AetherSDR::icom {
namespace {

// The table.
//
// TWO rows are `verified`, meaning their numbers were read out of that model's
// OWN Icom CI-V Reference Guide (both are in sources/icom-official/):
//
//   IC-705      475 points, range 0..160, division max 1 over WLAN / 11 over
//               USB, one receiver, one scope (0x27 0x12 and 0x27 0x13 are both
//               fixed at 00), 10 W.
//   IC-7300MK2  CI-V address 0xB6 (the guide's own frame diagram reads
//               FE FE E0 B6), 475 points, range 0..160, LAN data length 490,
//               0.03-74.8 MHz, and all FOUR scope modes.
//
// The rest are cross-referenced hardware facts and are marked unverified. Each
// one needs its own model's CI-V guide read before this backend advertises
// support for it — the shape of the transport is shared, the command table and
// scope geometry are not.
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
        /*hasVfoModeCommand*/ true,
    },
    {
        // IC-9700 — scope geometry MEASURED on a live radio 2026-08-05 (G0JKN),
        // not read from the guide, so `verified` stays false: that flag means
        // "confirmed against this model's own CI-V Reference Guide" and this
        // evidence is a different kind.
        //
        // 618 consecutive scope frames off an IC-9700 at 10.0.0.7, every one
        // 475 pixels wide, decoded through the RS-BA1 CI-V data stream. The
        // frame's own bounds header cross-checks: centre 439.864060 MHz (where
        // the radio was tuned) and a 500 kHz span, giving 1052.6 Hz per pixel.
        //
        // So the 475/160/11 inherited from the IC-705 turn out to be RIGHT for
        // this model — worth recording precisely because it could not be
        // assumed. The IC-7610 row below is the counter-example: 689 points and
        // a 0..200 range.
        0xA2, "IC-9700", 2, 2,
        /*hasNetwork*/ true, /*hasWifi*/ false,
        /*hasScope*/ true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        144'000'000ULL, 1'300'000'000ULL,
        /*verified*/ false,
        // 0x26 MEASURED on the live radio at 10.0.0.7, 2026-08-14 (G0JKN), the
        // same standard of evidence as the scope geometry above:
        //
        //   tx: fe fe a2 e0 26 00 fd
        //   rx: 26 00 05 00 01          (vfo 00, mode 05=FM, DATA 00, FIL1)
        //
        // A structured reply, not FA. Without this the 9700 fell to the legacy
        // 06 branch in setSliceMode(), which forces m_dataMode = false and
        // sends a body of {mode, filter} with NO DATA byte — so selecting FM-D
        // wrote plain FM and the mode reverted in the UI. On air that left
        // transmit audio on the MICROPHONE, so a 2 m AX.25 frame keyed the
        // radio and put room noise out instead of the modem's AFSK.
        /*hasVfoModeCommand*/ true,
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
    },
    {
        0x8E, "IC-785x", 2, 1,
        true, false,
        true, 689, 200, 15,
        kFreqBytes,
        true, 200.0,
        30'000ULL, 60'000'000ULL,
        false,
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
    },
    {
        // IC-7300MK2 — VERIFIED against Icom's own CI-V Reference Guide
        // (IC-7300MK2_ENG_CI-V_0), which is in sources/icom-official/.
        //
        // The big difference from the original IC-7300: it has a LAN PORT, so
        // it speaks the RS-BA1 transport and this backend reaches it directly
        // rather than needing Icom's server as a front end. The guide's own
        // words: over LAN "it is sent all at once", over USB "divided into 11
        // segments" — the same division-max 01/11 split as the IC-705.
        //
        // Its guide also states the LAN data length as 490 bytes, which
        // independently confirms the 15-byte first-division header this
        // decoder computes (3 + 1 + 5*2 + 1 == 15, and 15 + 475 == 490).
        0xB6, "IC-7300MK2", 1, 2,
        /*hasNetwork*/ true, /*hasWifi*/ false,   // Ethernet, not WiFi
        /*hasScope*/ true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 74'800'000ULL,
        /*verified*/ true,
        /*hasVfoModeCommand*/ true,
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
    // No 0x26: see the field's comment. An unknown radio gets the mode command
    // every Icom has had for decades and no DATA control.
    /*hasVfoModeCommand*/ false,
};

constexpr std::array<ModulationInputChoice, 4> kIc705ModInputs{{
    {0x00, "MIC",     ModSourceMic},
    {0x01, "USB",     ModSourceUsb},
    {0x02, "MIC+USB", ModSourceMic | ModSourceUsb},
    {0x03, "WLAN",    ModSourceNetwork},
}};

constexpr std::array<ModulationInputChoice, 6> kIc7300Mk2ModInputs{{
    {0x00, "MIC",     ModSourceMic},
    {0x01, "USB",     ModSourceUsb},
    {0x02, "ACC",     ModSourceAccessory},
    {0x03, "MIC+USB", ModSourceMic | ModSourceUsb},
    {0x04, "MIC+ACC", ModSourceMic | ModSourceAccessory},
    {0x05, "LAN",     ModSourceNetwork},
}};

}  // namespace

const IcomModel* modelForCivAddress(std::uint8_t addr)
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

std::optional<ModulationProfile> modulationProfileFor(const IcomModel& model)
{
    // IC-705 CI-V guide: USB/WLAN levels 0116/0117, DATA OFF/DATA MOD
    // selections 0118/0119.
    if (model.civAddress == 0xA4) {
        return ModulationProfile{116, -1, 117, 118, 119, 0x03, 0x00,
                                 kIc705ModInputs};
    }
    // IC-7300MK2 CI-V guide: USB/ACC/LAN levels 0081/0082/0083, DATA OFF/DATA
    // MOD selections 0084/0085. LAN is value 05, not the IC-705's WLAN 03.
    if (model.civAddress == 0xB6) {
        return ModulationProfile{81, 82, 83, 84, 85, 0x05, 0x00,
                                 kIc7300Mk2ModInputs};
    }
    return std::nullopt;
}

std::optional<std::uint8_t> parseModelIdReply(const CivFrame& frame)
{
    if (frame.cmd != cmd::kReadId || !frame.hasSub || frame.sub != 0x00)
        return std::nullopt;
    if (frame.data.empty())
        return std::nullopt;
    return frame.data[0];
}

std::span<const CurvePoint> powerCurveFor(const IcomModel& model)
{
    // Only models with their own verified conversion are named here. Every
    // other model returns EMPTY so the caller reports percent rather than a
    // watts figure derived from a different radio's PA.
    if (model.civAddress == 0xA4)
        return powerCurveIc705();
    if (model.civAddress == 0xB6)
        return powerCurveIc7300Mk2();
    return {};
}

std::span<const std::string_view> preampLabelsFor(const IcomModel& model)
{
    // The IC-705's HF ladder. Above 50 MHz the radio collapses to OFF/P.AMP1
    // and refuses P.AMP2, then reports what it actually did — which is why this
    // is published once rather than rewritten on every band change under an
    // operator who may be mid-adjustment.
    static constexpr std::array<std::string_view, 3> kIc705{"OFF", "P.AMP1", "P.AMP2"};
    if (model.civAddress == 0xA4 || model.civAddress == 0xB6)
        return kIc705;
    return {};
}

std::span<const AttenStep> attenStepsFor(const IcomModel& model)
{
    // ONE step on the IC-705, and 20 dB is its real figure — nameable in dB
    // where the preamp positions are not, because the guide publishes it. HF
    // and 50 MHz only; higher bands ignore the request and report OFF.
    static constexpr std::array<AttenStep, 2> kIc705{{{"OFF", 0}, {"20 dB", 20}}};
    if (model.civAddress == 0xA4 || model.civAddress == 0xB6)
        return kIc705;
    return {};
}

double s9ReferenceFor(std::uint64_t hz) noexcept
{
    // BAND-dependent, not model-dependent. IARU Region 1: S9 is -73 dBm below
    // 30 MHz and -93 dBm above. Using -73 everywhere reports VHF signals 20 dB
    // hot, which on a 2 m weak-signal band is the entire usable range.
    return usesVhfSReference(hz) ? kS9DbmVhf : kS9DbmHf;
}

}  // namespace AetherSDR::icom
