#pragma once

#include <QByteArray>
#include <QStringList>
#include <QtTypes>

#include <optional>

namespace AetherSDR::kenwood {
struct PowerLimit { int type; int band; int watts; };
// PC Command Reference (2019-01-30), p.40. Indices identify configuration
// rows/columns, not frequency permissions. Frequency binding is separate below.
QStringList powerLimitTypes();
QStringList powerLimitBands();
std::optional<QByteArray> queryPowerLimit(int type, int band);
std::optional<QByteArray> encodePowerLimit(PowerLimit limit);
std::optional<PowerLimit> decodePowerLimit(const QByteArray& message);
// Documented transmitter ranges identify configuration columns only. This is
// neither a regional TX permission nor a mapping for general-coverage gaps.
std::optional<int> powerLimitBandForFrequency(qint64 hz);
} // namespace AetherSDR::kenwood
