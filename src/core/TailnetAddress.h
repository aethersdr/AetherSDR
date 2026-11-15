#pragma once

#include <QHostAddress>

namespace AetherSDR {

// Whether an address is a tailnet address: Tailscale assigns IPv4 from the
// 100.64.0.0/10 shared address space (RFC 6598) and IPv6 from
// fd7a:115c:a1e0::/48. A radio reached at such an address is remote, usually
// over a relayed or rate-limited path, so it gets the bandwidth defaults a
// SmartLink connection does (RFC #6271 ruling D4). 100.64.0.0/10 is also
// carrier-grade NAT space; a radio reached there is remote as well, so the
// same default fits.
inline bool isTailnetAddress(const QHostAddress& address)
{
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        return address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
    }
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const quint32 v4 = address.toIPv4Address(&isV4);
        if (isV4) {
            return isTailnetAddress(QHostAddress(v4));
        }
        return address.isInSubnet(QHostAddress(QStringLiteral("fd7a:115c:a1e0::")), 48);
    }
    return false;
}

}  // namespace AetherSDR
