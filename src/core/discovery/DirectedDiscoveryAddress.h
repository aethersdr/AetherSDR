#pragma once

#include <QAbstractSocket>
#include <QHostAddress>

namespace AetherSDR::discovery {

// A directed probe needs a specific IPv4 host. Subnet-directed broadcasts
// cannot be identified without the destination network's prefix length.
[[nodiscard]] inline bool isDirectedDiscoveryAddress(const QHostAddress& address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol
        && !address.isNull()
        && address != QHostAddress::AnyIPv4
        && address != QHostAddress::Broadcast
        && !address.isMulticast();
}

} // namespace AetherSDR::discovery
