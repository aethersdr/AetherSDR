#pragma once

#include <QHostAddress>
#include <QString>

namespace AetherSDR {

// The address as a unicast endpoint, IPv4 or IPv6, or a null address when it
// cannot be one (null, unspecified, broadcast, multicast, or IPv6 link-local
// with no scope, which names no interface to reach it on). An IPv4-mapped IPv6
// address folds to plain IPv4, so one radio compares equal however a socket
// reports it.
inline QHostAddress usableUnicastAddress(const QHostAddress& address)
{
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    if (isV4) {
        if (v4 == 0 || v4 == 0xFFFFFFFFu) {
            return {};
        }
        const QHostAddress plain(v4);
        return plain.isMulticast() ? QHostAddress() : plain;
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol
        || address == QHostAddress::AnyIPv6 || address.isMulticast()
        || (address.isLinkLocal() && address.scopeId().isEmpty())) {
        return {};
    }
    return address;
}

// "host:port", with an IPv6 host in brackets.
inline QString endpointText(const QHostAddress& address, quint16 port)
{
    const QString host = address.toString();
    return address.protocol() == QAbstractSocket::IPv6Protocol
        ? QStringLiteral("[%1]:%2").arg(host).arg(port)
        : QStringLiteral("%1:%2").arg(host).arg(port);
}

}  // namespace AetherSDR
