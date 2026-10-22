#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR::AutomationIsolationPolicy {

// Process-only admission policy. Presence pins the restriction, including an
// empty value or "0"; settings and later environment changes cannot relax it.
inline bool demoOnly()
{
    static const bool restricted = qEnvironmentVariableIsSet("AETHER_AUTOMATION_DEMO_ONLY");
    return restricted;
}

bool allowsRadio(const QString& family, const QString& serial);

inline QString refusal()
{
    return QStringLiteral("This process is restricted to the AetherSDR demo simulator.");
}

} // namespace AetherSDR::AutomationIsolationPolicy
