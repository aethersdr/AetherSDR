#include "NetworkPathResolver.h"

#include <QNetworkInterface>

#include <algorithm>

namespace AetherSDR {

namespace {

QString interfaceLabel(const QNetworkInterface& iface)
{
    const QString human = iface.humanReadableName().trimmed();
    if (!human.isEmpty())
        return human;
    return iface.name().trimmed();
}

}

bool NetworkPathCandidate::isValid() const
{
    return NetworkPathResolver::isUsableSourceAddress(address);
}

QString NetworkPathCandidate::label() const
{
    QString iface = interfaceName.trimmed();
    if (iface.isEmpty())
        iface = interfaceId.trimmed();
    if (iface.isEmpty())
        return address.toString();
    return QStringLiteral("%1 (%2)").arg(iface, address.toString());
}

QList<NetworkPathCandidate> NetworkPathResolver::enumerateIpv4Candidates()
{
    return enumerateCandidates(QAbstractSocket::IPv4Protocol);
}

QList<NetworkPathCandidate> NetworkPathResolver::enumerateCandidates(Family family)
{
    QList<NetworkPathCandidate> out;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) ||
            !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != family || !isUsableSourceAddress(addr))
                continue;

            NetworkPathCandidate candidate;
            candidate.interfaceId = iface.name();
            candidate.interfaceName = interfaceLabel(iface);
            candidate.address = addr;
            out.append(candidate);
        }
    }

    std::sort(out.begin(), out.end(), [](const NetworkPathCandidate& lhs,
                                         const NetworkPathCandidate& rhs) {
        if (lhs.interfaceName == rhs.interfaceName)
            return lhs.address.toString() < rhs.address.toString();
        return lhs.interfaceName < rhs.interfaceName;
    });
    return out;
}

NetworkPathCandidate NetworkPathResolver::resolveExplicitSelection(const QString& interfaceId,
                                                                   const QString& interfaceName,
                                                                   const QHostAddress& lastSuccessfulAddress,
                                                                   Family family)
{
    const auto candidates = enumerateCandidates(family);

    auto findMatch = [&](auto&& predicate) -> NetworkPathCandidate {
        for (const auto& candidate : candidates) {
            if (predicate(candidate))
                return candidate;
        }
        return {};
    };

    if (!interfaceId.trimmed().isEmpty()) {
        auto match = findMatch([&](const NetworkPathCandidate& candidate) {
            return candidate.interfaceId == interfaceId;
        });
        if (match.isValid())
            return match;
    }

    if (!interfaceName.trimmed().isEmpty()) {
        auto match = findMatch([&](const NetworkPathCandidate& candidate) {
            return candidate.interfaceName == interfaceName;
        });
        if (match.isValid())
            return match;
    }

    if (isUsableSourceAddress(lastSuccessfulAddress)) {
        auto match = findMatch([&](const NetworkPathCandidate& candidate) {
            return candidate.address == lastSuccessfulAddress;
        });
        if (match.isValid())
            return match;
    }

    return {};
}

NetworkPathCandidate NetworkPathResolver::autoCandidate()
{
    const auto candidates = enumerateIpv4Candidates();
    if (candidates.size() == 1)
        return candidates.first();
    return {};
}

bool NetworkPathResolver::isUsableIpv4(const QHostAddress& address)
{
    return !address.isNull()
        && address.protocol() == QAbstractSocket::IPv4Protocol
        && !address.isLoopback();
}

bool NetworkPathResolver::isUsableSourceAddress(const QHostAddress& address)
{
    if (isUsableIpv4(address))
        return true;
    return address.protocol() == QAbstractSocket::IPv6Protocol
        && !address.isLoopback() && !address.isLinkLocal() && !address.isMulticast()
        && address != QHostAddress::AnyIPv6;
}

NetworkPathResolver::Family NetworkPathResolver::targetFamily(const QString& host)
{
    const QHostAddress literal(host.trimmed());
    return literal.protocol() == QAbstractSocket::IPv6Protocol
        ? QAbstractSocket::IPv6Protocol : QAbstractSocket::IPv4Protocol;
}

} // namespace AetherSDR
