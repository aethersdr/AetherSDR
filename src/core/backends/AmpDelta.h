#pragma once

#include <optional>

#include <QMap>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized power-amplifier status delta (aetherd 2.4 — AmpModel decode split,
// #4094). FlexBackend::decodeAmplifierStatus translates the SmartSDR "amplifier"
// wire (the "model"/"ip"/"state" keys, the TunerGeniusXL discriminator) into
// this typed, present-only shape; AmpModel::applyChanges owns the state machine
// (presence latch, operate change-gating, handle matching). Same contract as the
// sub-model deltas — a backend with no amp analog simply never emits one.
//
// The decode is stateless: it carries what the wire reported (a detected power-amp
// model, its ip, the derived operate flag, the raw telemetry). The model decides
// what to do with it. Command/encode is NOT here — AmpModel::setOperate emits the
// neutral operateRequested intent, translated back to the wire by
// FlexBackend::invokeExtension("flex", "amp.operate", …) (#4094).
struct AmpDelta {
    QString handle;                        // amplifier handle from the status object
    bool    removed{false};                // "amplifier <handle> removed"

    // Set only when the wire reported a non-empty, non-TGXL amp model — i.e. a
    // power amp (PGXL) as opposed to the tuner, which routes to TunerModel.
    std::optional<QString> detectedModel;  // e.g. "PowerGeniusXL"
    std::optional<QString> ip;             // for the direct PgxlConnection

    // Operate/standby derived from the wire "state" (IDLE/OPERATE/TRANSMIT* → on,
    // STANDBY → off). Absent when the status carried no "state".
    std::optional<bool> operate;

    // Raw amp telemetry (id/vac/vdd/meffa/temp/state/…) the GUI renders as-is.
    QMap<QString, QString> telemetry;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::AmpDelta)
