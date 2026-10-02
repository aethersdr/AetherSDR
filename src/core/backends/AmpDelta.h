#pragma once

#include <optional>

#include <QMap>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized power-amplifier status delta (aetherd 2.4, #4094), present-only.
// FlexBackend::decodeAmplifierStatus translates the SmartSDR "amplifier" wire
// ("model"/"ip"/"state", the TunerGeniusXL discriminator) statelessly;
// AmpModel::applyChanges owns the state machine. Command/encode is not here:
// AmpModel::setOperate's operateRequested intent goes back via
// FlexBackend::invokeExtension("flex", "amp.operate", …).
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
