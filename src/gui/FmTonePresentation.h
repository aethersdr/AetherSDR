#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QString>

namespace AetherSDR {

enum class FmToneRole {
    Tx,
    Rx,
};

[[nodiscard]] inline QString fmToneDisplayLabel(FmTonePresentation presentation,
                                                FmToneRole role,
                                                const QString& legacyLabel)
{
    if (presentation != FmTonePresentation::Ctcss) {
        return legacyLabel;
    }
    return role == FmToneRole::Tx
        ? QStringLiteral("TX: %1").arg(legacyLabel)
        : QStringLiteral("RX: %1").arg(legacyLabel);
}

} // namespace AetherSDR
