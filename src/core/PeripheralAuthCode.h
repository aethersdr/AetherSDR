#pragma once

#include "PeripheralAuthCodeValidation.h"
#include <QByteArray>

namespace AetherSDR {

inline constexpr int kMaxPeripheralLineLength = 64 * 1024;

inline int recordPeripheralAuthFailure(int& consecutiveFailures)
{
    if (consecutiveFailures < 3) {
        ++consecutiveFailures;
    }
    return consecutiveFailures;
}

enum class PeripheralAuthProtocol { Tgxl, Pgxl, AntennaGenius };

inline QByteArray peripheralAuthCommand(PeripheralAuthProtocol protocol, const QString& code)
{
    if (!validPeripheralAuthCode(code)) {
        return {};
    }
    if (protocol == PeripheralAuthProtocol::Tgxl) {
        return "C1|auth " + code.toUtf8() + '\n';
    }
    return "C1|auth code=" + code.toUtf8()
         + (protocol == PeripheralAuthProtocol::AntennaGenius ? '\r' : '\n');
}

} // namespace AetherSDR
