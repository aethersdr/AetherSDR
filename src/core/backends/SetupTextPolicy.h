#pragma once

#include <QString>

namespace AetherSDR {

// Human-readable intent text; wire escaping belongs to the family encoder.
inline bool setupTextValid(const QString& value)
{
    if (value.size() > 256) { return false; }
    for (const QChar ch : value) {
        if (!ch.isPrint()) { return false; }
    }
    return true;
}

} // namespace AetherSDR
