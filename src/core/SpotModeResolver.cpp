#include "core/SpotModeResolver.h"

#include <QMap>
#include <QSet>
#include <QStringList>

namespace AetherSDR::SpotModeResolver {

namespace {

const QSet<QString>& knownSpotModes()
{
    static const QSet<QString> kSet = {
        "CW", "SSB", "USB", "LSB", "AM", "FM", "FT8", "FT4",
        "JS8", "RTTY", "PSK31", "PSK63", "PSK", "OLIVIA",
        "JT65", "JT9", "SAM", "NFM", "DIGU", "DIGL"
    };
    return kSet;
}

const QMap<QString, QString>& spotToRadioModeMap()
{
    static const QMap<QString, QString> kMap = {
        {"CW", "CW"}, {"CWL", "CW"}, {"CWU", "CW"},
        {"USB", "USB"}, {"LSB", "LSB"},
        {"FT8", "DIGU"}, {"FT4", "DIGU"}, {"JS8", "DIGU"},
        {"PSK31", "DIGU"}, {"PSK63", "DIGU"}, {"PSK", "DIGU"},
        {"OLIVIA", "DIGU"}, {"JT65", "DIGU"}, {"JT9", "DIGU"},
        {"RTTY", "DIGL"},
        {"AM", "AM"}, {"SAM", "SAM"},
        {"FM", "FM"}, {"NFM", "NFM"},
    };
    return kMap;
}

} // namespace

QString extractSpotModeFromComment(const QString& comment)
{
    const auto& known = knownSpotModes();
    QStringList words = comment.split(' ', Qt::SkipEmptyParts);
    if (!words.isEmpty() && known.contains(words.first().toUpper()))
        return words.first().toUpper();
    if (!words.isEmpty() && known.contains(words.last().toUpper()))
        return words.last().toUpper();
    return {};
}

QString inferSpotModeFromBand(double f)
{
    if ((f >= 1.800 && f < 1.850) || (f >= 3.500 && f < 3.600) ||
        (f >= 7.000 && f < 7.050) || (f >= 10.100 && f < 10.140) ||
        (f >= 14.000 && f < 14.070) || (f >= 18.068 && f < 18.095) ||
        (f >= 21.000 && f < 21.070) || (f >= 24.890 && f < 24.920) ||
        (f >= 28.000 && f < 28.070) || (f >= 50.000 && f < 50.100))
        return "CW";
    if ((f >= 1.840 && f < 1.850) || (f >= 3.570 && f < 3.600) ||
        (f >= 7.040 && f < 7.050) || (f >= 10.130 && f < 10.150) ||
        (f >= 14.070 && f < 14.100) || (f >= 18.095 && f < 18.110) ||
        (f >= 21.070 && f < 21.100) || (f >= 24.915 && f < 24.930) ||
        (f >= 28.070 && f < 28.150))
        return "DIGU";
    if (f >= 10.0)
        return "USB";
    if (f >= 1.8)
        return "LSB";
    return {};
}

QString mapSpotModeToRadioMode(const QString& spotMode, double rxFreqMhz)
{
    const auto& map = spotToRadioModeMap();
    if (map.contains(spotMode))
        return map.value(spotMode);
    if (spotMode == "SSB")
        return (rxFreqMhz >= 10.0) ? "USB" : "LSB";
    return {};
}

QString resolveSpotRadioMode(const QString& explicitMode,
                             const QString& comment,
                             double rxFreqMhz)
{
    QString spotMode = explicitMode.toUpper().trimmed();
    if (spotMode.isEmpty())
        spotMode = extractSpotModeFromComment(comment);
    if (spotMode.isEmpty())
        spotMode = inferSpotModeFromBand(rxFreqMhz);
    if (spotMode.isEmpty())
        return {};
    return mapSpotModeToRadioMode(spotMode, rxFreqMhz);
}

} // namespace AetherSDR::SpotModeResolver
