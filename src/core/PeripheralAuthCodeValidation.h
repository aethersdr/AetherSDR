#pragma once

#include <QString>

namespace AetherSDR {

// An authorization code is interpolated into a line-oriented 4O3A command.
inline bool validPeripheralAuthCode(const QString& code)
{
    if (code.isEmpty() || code.size() > 128) {
        return false;
    }
    for (const QChar character : code) {
        const ushort value = character.unicode();
        if (value <= 0x20 || value > 0x7e || character == u'|') {
            return false;
        }
    }
    return true;
}

} // namespace AetherSDR
