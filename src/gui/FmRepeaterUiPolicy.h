#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "core/backends/RadioCapabilities.h"

namespace AetherSDR {

struct FmRepeaterUiPolicy {
    bool extendedControls = false;
    bool surfaceVisible = true;
    bool duplexVisible = true;
    bool reverseVisible = true;
    QStringList accessModes{QStringLiteral("off"), QStringLiteral("ctcss_tx")};
};

// Pat's #5120 ruling: an unattested Icom surface is hidden, not disabled.
// Other families keep their established surface; in particular Flex retains
// its exact Off/CTCSS TX and client-side REV behavior.
inline FmRepeaterUiPolicy fmRepeaterUiPolicy(FmRepeaterPresentation presentation,
                                              const QStringList& accessModes,
                                              bool hasDuplex, bool hasReverse)
{
    FmRepeaterUiPolicy policy;
    if (presentation == FmRepeaterPresentation::Legacy) {
        return policy;
    }
    policy.extendedControls = presentation == FmRepeaterPresentation::Extended;
    policy.surfaceVisible = policy.extendedControls
        && (!accessModes.isEmpty() || hasDuplex || hasReverse);
    policy.duplexVisible = policy.extendedControls && hasDuplex;
    policy.reverseVisible = policy.extendedControls && hasDuplex && hasReverse;
    policy.accessModes = policy.extendedControls ? accessModes : QStringList{};
    return policy;
}

inline bool fmRepeaterReverseEnabled(bool extendedControls, bool reverseAvailable,
                                     const QString& duplexDirection)
{
    if (!reverseAvailable) {
        return false;
    }
    return !extendedControls || duplexDirection == QLatin1String("down")
        || duplexDirection == QLatin1String("up");
}

inline const QStringList& fmRepeaterDirectionLabels()
{
    static const QStringList labels{
        QString::fromUtf8("\xe2\x88\x92"), QStringLiteral("Simplex"),
        QStringLiteral("+"), QStringLiteral("REV")};
    return labels;
}

inline std::optional<double> flexFmRepeaterTxOffset(const QString& direction,
                                                    double magnitude,
                                                    bool reverse)
{
    if (direction == QLatin1String("up")) {
        return reverse ? -magnitude : magnitude;
    }
    if (direction == QLatin1String("down")) {
        return reverse ? magnitude : -magnitude;
    }
    return std::nullopt;
}

} // namespace AetherSDR
