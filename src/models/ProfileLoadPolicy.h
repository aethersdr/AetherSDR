#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

inline constexpr qint64 kProfileLoadStateWriteHoldMs = 10000;
inline constexpr int kProfileLoadDeferredPanFlushDelayMs =
    static_cast<int>(kProfileLoadStateWriteHoldMs + 1000);
inline constexpr int kProfileLoadPostHoldRecoveryDelayMs =
    static_cast<int>(kProfileLoadStateWriteHoldMs + 1250);

inline bool profileLoadMayRebuildRadioTopology(const QString& profileType)
{
    return profileType == QStringLiteral("global");
}

} // namespace AetherSDR
