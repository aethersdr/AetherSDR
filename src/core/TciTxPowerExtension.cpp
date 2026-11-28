#include "TciTxPowerExtension.h"

namespace AetherSDR::TciTxPowerExtension {

bool offered(bool connected, bool canTransmit, bool haveCeiling, int watts)
{
    return connected && canTransmit && haveCeiling && watts > 0;
}

std::optional<int> parseRead(const QStringList& args)
{
    if (args.isEmpty()) {
        return -1;
    }
    if (args.size() != 1) {
        return std::nullopt;
    }
    bool ok = false;
    const int trx = args.at(0).toInt(&ok);
    if (!ok || trx < 0) {
        return std::nullopt;
    }
    return trx;
}

QString line(int trx, int watts)
{
    return QStringLiteral("%1:%2,%3;").arg(QLatin1String(kCommand)).arg(trx).arg(watts);
}

} // namespace AetherSDR::TciTxPowerExtension
