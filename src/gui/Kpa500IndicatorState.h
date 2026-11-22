// KPA500 status indicator labels and theme tokens (RFC #7092).

#pragma once
#include "core/Kpa500Protocol.h"
#include <QString>

namespace AetherSDR {

struct Kpa500IndicatorState {
    QString label;      // "FAULT" | "TX" | "OPERATE" | "STANDBY" | "OFF" | "–"
    QString colorToken; // ThemeManager token name
};

// Priority order (RFC #7092):
//   FAULT (faultCode > 0)
//   TX    (liveTxMeters && forwardPowerW >= 5 W)
//   OPERATE (powerOn && operate)
//   STANDBY (powerOn && !operate)
//   OFF   (powerOn == false)
//   "–"   (not responding, or any required field still nullopt after expireTelemetry)
//
// live         = Kpa500Connection::isConnected() && Kpa500Connection::isResponding()
// liveTxMeters = Kpa500Connection::hasLiveTxMeters()
inline Kpa500IndicatorState kpa500IndicatorState(
    const Kpa500::Status& s, bool live, bool liveTxMeters)
{
    if (!live) {
        return {QStringLiteral("–"), QStringLiteral("color.text.secondary")};
    }

    const bool fault = s.faultCode.value_or(0) > 0;
    const bool txing = liveTxMeters && s.forwardPowerW.value_or(0.0f) >= 5.0f;
    const bool oper  = s.powerOn == true && s.operate == true;
    const bool stby  = s.powerOn == true && s.operate == false;
    const bool off   = s.powerOn == false;

    if (fault) {
        return {QStringLiteral("FAULT"), QStringLiteral("color.accent.danger")};
    }
    if (txing) {
        return {QStringLiteral("TX"), QStringLiteral("color.accent.danger")};
    }
    if (oper) {
        return {QStringLiteral("OPERATE"), QStringLiteral("color.accessory.indicator.operate.foreground")};
    }
    if (stby) {
        return {QStringLiteral("STANDBY"), QStringLiteral("color.accessory.key.standby.foreground")};
    }
    if (off) {
        return {QStringLiteral("OFF"), QStringLiteral("color.text.secondary")};
    }
    return            {QStringLiteral("–"),       QStringLiteral("color.text.secondary")};
}

} // namespace AetherSDR
