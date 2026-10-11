#pragma once

#include <QString>

namespace AetherSDR {

// Which RX audio compression to ask the radio for: "opus" or "none".
// `saved` is the AudioCompression setting ("Auto", "None" or "Opus"), empty
// when the operator has never chosen. A radio reached at a tailnet address is
// remote, often over a relayed, rate-limited path, so it gets Opus by default,
// as SmartLink does under Auto (RFC #6271 ruling D4). An explicit choice
// always wins.
inline QString audioCompressionFor(const QString& saved, bool wan, bool tailnet)
{
    if (saved.isEmpty()) {
        // Never chosen: Opus over a tailnet, uncompressed otherwise (the
        // long-standing default, SmartLink included).
        return tailnet ? QStringLiteral("opus") : QStringLiteral("none");
    }
    if (saved == QLatin1String("Opus")) {
        return QStringLiteral("opus");
    }
    if (saved == QLatin1String("None")) {
        return QStringLiteral("none");
    }
    // Auto: Opus when remote (SmartLink or a tailnet), uncompressed on the LAN.
    return (wan || tailnet) ? QStringLiteral("opus") : QStringLiteral("none");
}

}  // namespace AetherSDR
