#pragma once

#include <QStringView>
#include <array>
#include <limits>

namespace AetherSDR::TuneOutputPolicy {

inline bool firmwareSupportsSuppression(QStringView version)
{
    std::array<unsigned int, 4> segments{};
    int count = 0;
    unsigned int value = 0;
    bool haveDigit = false;
    for (const QChar ch : version) {
        if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) {
            const unsigned int digit = ch.unicode() - '0';
            if (value > (static_cast<unsigned int>(std::numeric_limits<int>::max()) - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            haveDigit = true;
        } else if (ch == QLatin1Char('.') && haveDigit && count < 3) {
            segments[count++] = value;
            value = 0;
            haveDigit = false;
        } else {
            return false;
        }
    }
    if (!haveDigit || count < 2 || count > 3) {
        return false;
    }
    segments[count] = value;
    return segments[0] > 4 || (segments[0] == 4 && segments[1] >= 3);
}

inline bool clientManagesRca(QStringView version)
{
    return !firmwareSupportsSuppression(version);
}

inline bool validBandId(int bandId)
{
    return bandId >= 0 && bandId <= 255;
}

} // namespace AetherSDR::TuneOutputPolicy
