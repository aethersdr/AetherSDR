#pragma once

#include <optional>

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized antenna-tuner status delta (aetherd 2.4 — TunerModel decode split,
// #4092). FlexBackend::decodeTunerStatus translates the SmartSDR "atu"/"amplifier"
// (model=TunerGeniusXL) wire into this typed, present-only shape; TunerModel::
// applyChanges applies exactly the reported fields (change-gated, with the
// tuning/antenna edge signals). Same contract as the sub-model deltas.
//
// Command/encode (operate/bypass/autotune) is NOT here — TunerModel emits neutral
// intents, translated back to the wire by FlexBackend::invokeExtension("flex",
// "tuner.*", …) (#4092). The direct port-9010 relay/antenna fast-path and the
// direct-connection fwd-power/SWR meters are set outside this decode.
struct TunerDelta {
    std::optional<QString> serialNum;    // "serial_num"
    std::optional<QString> model;
    std::optional<QString> ip;
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
