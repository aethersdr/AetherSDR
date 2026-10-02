#pragma once

#include <optional>

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized antenna-tuner status delta (aetherd 2.4, #4092), present-only.
// FlexBackend::decodeTunerStatus translates SmartSDR "atu"/"amplifier"
// (model=TunerGeniusXL); TunerModel::applyChanges applies it change-gated.
// pttA/pttB drive TunerApplet's per-port keying lamps. Commands go back via
// FlexBackend::invokeExtension("flex", "tuner.*", …); the port-9010 fast path and
// direct-connection fwd-power/SWR meters are set outside this decode.
struct TunerDelta {
    std::optional<QString> handle;       // normalized tuner identity
    std::optional<QString> serialNum;    // "serial_num"
    std::optional<QString> model;
    std::optional<QString> ip;
    // Which radio antenna each RF port is wired to, from the "ant" field
    // ("ANT1,ANT2"). This is what says which port carries transmit: the port
    // whose antenna matches the TX slice's. Nothing in the tuner's own direct
    // status distinguishes them — with one radio cabled to both ports it
    // reports both as live.
    std::optional<QString> portAAnt;
    std::optional<QString> portBAnt;
    std::optional<bool>    pttA;         // "ptta" — port A keyed
    std::optional<bool>    pttB;         // "pttb" — port B keyed
    std::optional<bool>    operate;      // "1"
    std::optional<bool>    bypass;       // "1"
    std::optional<bool>    tuning;       // "1"
    std::optional<int>     relayC1;
    std::optional<int>     relayC2;
    std::optional<int>     relayL;
    std::optional<int>     antennaA;     // "antA", 0-indexed
    std::optional<bool>    oneByThree;   // "one_by_three" — TGXL 3x1 model
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TunerDelta)
