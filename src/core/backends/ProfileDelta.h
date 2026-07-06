#pragma once

#include <optional>

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Normalized profile-status delta (aetherd RFC 2.3 — RadioModel residual). The
// backend parses the vendor "profile <type> …" status (list/current, plus the
// database importing/exporting flags) into this present-only shape;
// RadioModel::applyProfileChanges routes it to the right target (TransmitModel
// tx/mic profiles, the global-profile list, or the import/export flags).
//
// `type` is "tx" | "mic" | "global" for a list/current update, or empty for a
// flags-only update (importing/exporting arrive without a profile type).
struct ProfileDelta {
    QString type;
    std::optional<QStringList> list;      // '^'-separated names, split by the backend
    std::optional<QString>     current;
    std::optional<bool>        importing;
    std::optional<bool>        exporting;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ProfileDelta)
