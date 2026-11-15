#pragma once

#include <QString>

namespace AetherSDR {

inline constexpr int kFlexUdpPortInUseCode = 0x500000A9;

inline bool isUdpPortInUseError(int code, const QString& body)
{
    return code == kFlexUdpPortInUseCode
        || body.contains(QStringLiteral("Port/IP pair already in use"), Qt::CaseInsensitive);
}

inline bool shouldRetryLanUdpPortRegistration(bool isWan, int code, const QString& body)
{
    return !isWan && isUdpPortInUseError(code, body);
}

} // namespace AetherSDR
