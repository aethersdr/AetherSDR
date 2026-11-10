#pragma once

#include <QAbstractSocket>
#include <QHostAddress>
#include <QString>

namespace AetherSDR {

// Match Qt's socket family rather than address equivalence: mapped IPv6
// literals remain IPv6 endpoints, and scoped addresses keep their interface.
inline bool matchesLanAddressFamily(const QHostAddress& radioAddress,
                                    const QHostAddress& localAddress)
{
    const QAbstractSocket::NetworkLayerProtocol protocol = radioAddress.isNull()
        ? QAbstractSocket::IPv4Protocol : radioAddress.protocol();
    return !localAddress.isNull() && localAddress.protocol() == protocol;
}

struct LanBindAddress {
    QHostAddress address;
    QString reason;
};

inline LanBindAddress selectLanBindAddress(const QHostAddress& radioAddress,
                                          const QHostAddress& explicitAddress,
                                          const QHostAddress& sessionAddress,
                                          const QHostAddress& tcpAddress)
{
    if (matchesLanAddressFamily(radioAddress, explicitAddress)) {
        return {explicitAddress, QStringLiteral("explicit")};
    }
    if (matchesLanAddressFamily(radioAddress, sessionAddress)) {
        return {sessionAddress, QStringLiteral("probe-session")};
    }
    if (matchesLanAddressFamily(radioAddress, tcpAddress)) {
        return {tcpAddress, QStringLiteral("tcp-local")};
    }
    return {QHostAddress(radioAddress.protocol() == QAbstractSocket::IPv6Protocol
                            ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4), {}};
}

} // namespace AetherSDR
