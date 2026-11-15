#include "FrequencyEntryParser.h"

namespace AetherSDR::FrequencyEntryParser {

QString normalizedMhzText(const QString& text)
{
    QString clean = text.trimmed();
    const int firstDot = clean.indexOf(QLatin1Char('.'));
    if (firstDot >= 0) {
        const QString beforeDot = clean.left(firstDot);
        const QString afterDot = clean.mid(firstDot + 1).remove(QLatin1Char('.'));
        clean = beforeDot + QLatin1Char('.') + afterDot;
    }
    return clean;
}

bool isExplicitMhzEntry(const QString& rawText, const QString& normalizedText)
{
    const int dot = normalizedText.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return false;
    }

    // Display-style entries are already MHz.kHz.Hz, even when the current
    // slice is not yet on an XVTR/high-frequency band.
    if (rawText.count(QLatin1Char('.')) >= 2) {
        return true;
    }

    // Single-dot entries with a normal MHz field are explicit MHz. Preserve
    // the historic HF shortcut where `14225.0` means 14.225 MHz by treating
    // five-plus leading digits as kHz-style input.
    return dot <= 4;
}

} // namespace AetherSDR::FrequencyEntryParser
