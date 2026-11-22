#include "TciTxPowerExtension.h"

#include <QtGlobal>

#include <cmath>

namespace AetherSDR::TciTxPowerExtension {

RadioCapabilities limits(const RadioCapabilities& caps)
{
    RadioCapabilities out;
    out.canTransmit = caps.canTransmit;
    out.txPowerMaxWatts = caps.txPowerMaxWatts;
    out.txPowerBands = caps.txPowerBands;
    return out;
}

double wattsAt(const RadioCapabilities& caps, double frequencyHz)
{
    if (!caps.canTransmit) {
        return 0.0;
    }
    const double watts = caps.txPowerMaxWattsAt(frequencyHz);
    return std::isfinite(watts) && watts > 0.0 ? watts : 0.0;
}

bool offered(bool connected, bool canTransmit, bool hasTxSlice, double watts)
{
    // Below a tenth of a watt the line would read 0.0.
    return connected && canTransmit && hasTxSlice && tenths(watts) > 0;
}

int tenths(double watts)
{
    return std::isfinite(watts) && watts > 0.0 ? qRound(watts * 10.0) : 0;
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

QString line(int trx, double watts)
{
    return QStringLiteral("%1:%2,%3;")
        .arg(QLatin1String(kCommand))
        .arg(trx)
        .arg(QString::number(tenths(watts) / 10.0, 'f', 1));
}

} // namespace AetherSDR::TciTxPowerExtension
