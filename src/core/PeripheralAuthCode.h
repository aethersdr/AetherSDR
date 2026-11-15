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
    // AG's API specifies CR; the issue #2313 capture confirms LF framing and
    // that CRLF reaches the auth parser. Use both, as ordinary AG commands do.
    return "C1|auth code=" + code.toUtf8()
         + (protocol == PeripheralAuthProtocol::AntennaGenius ? QByteArray("\r\n") : QByteArray("\n"));
}

} // namespace AetherSDR
