#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

namespace AetherSDR {

inline QString formatRfGainIndicator(int value, QStringView unitSuffix)
{
    const QString unit = unitSuffix.trimmed().toString();
    const QString sign = (value > 0 && unit.compare(QStringLiteral("dB"),
                                                    Qt::CaseInsensitive) == 0)
                             ? QStringLiteral("+")
                             : QString();
    const QString separator = unit.startsWith(QLatin1Char('%'))
                                  ? QString()
                                  : QStringLiteral(" ");
    const QString valueText = QStringLiteral("%1%2%3%4")
                                  .arg(sign).arg(value).arg(separator, unit);
    return unit == QLatin1String("%")
               ? QStringLiteral("RFG %1").arg(valueText)
               : valueText;
}

inline QString normalizedRfGainUnitSuffix(QStringView unitSuffix)
{
    const QString normalized = unitSuffix.trimmed().toString();
    return normalized.isEmpty() ? QStringLiteral("dB") : normalized;
}

inline bool shouldShowRfGainIndicator(int value, int neutralValue)
{
    return value != neutralValue;
}

inline QString formatPreampIndicator(const QStringList& labels, int step)
{
    if (step <= 0 || step >= labels.size()) {
        return {};
    }
    return labels.at(step);
}

} // namespace AetherSDR
