#pragma once

#include <QHostAddress>
#include <QString>

namespace AetherSDR::aetherpd {
inline bool validAdminToken(const QString& token)
{
    // Provisioned shim identities use base64url; independent first installs
    // use lowercase hex. Both encode a 32-byte secret without header syntax.
    if (token.size() == 64) {
        for (const QChar c : token) {
            if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return false; }
        }
        return true;
    }
    if (token.size() != 43) { return false; }
    for (const QChar c : token) {
        if (!((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')
              || (c >= u'0' && c <= u'9') || c == u'-' || c == u'_')) { return false; }
    }
    return true;
}

inline bool lanAddress(const QHostAddress& address)
{
    return !address.isNull() && (address.isLoopback()
        || address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
        || address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
        || address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)
        || address.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16)
        || address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7)
        || address.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10));
}
} // namespace AetherSDR::aetherpd
