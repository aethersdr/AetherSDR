#include "KenwoodPowerLimits.h"

#include <array>

namespace AetherSDR::kenwood {
std::optional<int> powerLimitBandForFrequency(qint64 hz)
{
    // Instruction manual, Specifications, EN-19-2. Use the documented union
    // of K/E/E2 ranges; firmware and the TX coordinator retain all authority.
    struct Range { qint64 low; qint64 high; int band; };
    static constexpr std::array<Range, 13> kRanges{{
        {1800000, 2000000, 0}, {3500000, 4000000, 1}, {5167500, 5167500, 2},
        {5250000, 5450000, 2}, {7000000, 7300000, 3}, {10100000, 10150000, 4},
        {14000000, 14350000, 5}, {18068000, 18168000, 6}, {21000000, 21450000, 7},
        {24890000, 24990000, 8}, {28000000, 29700000, 9}, {50000000, 54000000, 10},
        {70000000, 70500000, 11}}};
    for (const Range& range : kRanges) {
        if (hz >= range.low && hz <= range.high) { return range.band; }
    }
    return std::nullopt;
}

QStringList powerLimitTypes()
{
    return {QStringLiteral("SSB"), QStringLiteral("CW"), QStringLiteral("FSK/PSK"),
        QStringLiteral("FM/AM"), QStringLiteral("DATA"), QStringLiteral("TX tuning")};
}
QStringList powerLimitBands()
{
    return {QStringLiteral("1.8 MHz"), QStringLiteral("3.5 MHz"), QStringLiteral("5 MHz"),
        QStringLiteral("7 MHz"), QStringLiteral("10 MHz"), QStringLiteral("14 MHz"),
        QStringLiteral("18 MHz"), QStringLiteral("21 MHz"), QStringLiteral("24 MHz"),
        QStringLiteral("28 MHz"), QStringLiteral("50 MHz"), QStringLiteral("70 MHz")};
}
std::optional<QByteArray> queryPowerLimit(int type, int band)
{
    if (type < 0 || type >= 6 || band < 0 || band >= 12) { return std::nullopt; }
    return "LP1" + QByteArray::number(type) + QByteArray::number(band).rightJustified(2, '0');
}
std::optional<QByteArray> encodePowerLimit(PowerLimit limit)
{
    const std::optional<QByteArray> prefix = queryPowerLimit(limit.type, limit.band);
    if (!prefix || limit.watts < 5 || limit.watts > 100) { return std::nullopt; }
    return *prefix + QByteArray::number(limit.watts).rightJustified(3, '0');
}
std::optional<PowerLimit> decodePowerLimit(const QByteArray& message)
{
    if (message.size() != 9 || !message.startsWith("LP1")) { return std::nullopt; }
    for (qsizetype index = 3; index < message.size(); ++index) {
        if (message[index] < '0' || message[index] > '9') { return std::nullopt; }
    }
    const PowerLimit limit{message[3] - '0', (message[4] - '0') * 10 + message[5] - '0',
        (message[6] - '0') * 100 + (message[7] - '0') * 10 + message[8] - '0'};
    return encodePowerLimit(limit) ? std::optional<PowerLimit>(limit) : std::nullopt;
}
} // namespace AetherSDR::kenwood
