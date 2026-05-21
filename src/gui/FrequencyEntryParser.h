#pragma once

#include <QString>

namespace AetherSDR::FrequencyEntryParser {

QString normalizedMhzText(const QString& text);
bool isExplicitMhzEntry(const QString& rawText, const QString& normalizedText);

} // namespace AetherSDR::FrequencyEntryParser
