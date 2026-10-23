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
    const qsizetype index = matchSelection(candidates, interfaceId, interfaceName,
                                           lastSuccessfulAddress);
    return index < 0 ? NetworkPathCandidate{} : candidates.at(index);
}

qsizetype NetworkPathResolver::matchSelection(const QList<NetworkPathCandidate>& candidates,
                                              const QString& interfaceId,
                                              const QString& interfaceName,
                                              const QHostAddress& lastSuccessfulAddress)
{
    auto find = [&](auto&& predicate) -> qsizetype {
        for (qsizetype i = 0; i < candidates.size(); ++i) {
            if (candidates.at(i).isValid() && predicate(candidates.at(i)))
                return i;
        }
        return -1;
    };
    const bool hasId = !interfaceId.trimmed().isEmpty();
    const bool hasName = !interfaceName.trimmed().isEmpty();
    const bool hasAddress = isUsableSourceAddress(lastSuccessfulAddress);

    if (hasAddress && (hasId || hasName)) {
        const qsizetype exact = find([&](const NetworkPathCandidate& c) {
            return c.address == lastSuccessfulAddress
                && (hasId ? c.interfaceId == interfaceId : c.interfaceName == interfaceName);
        });
        if (exact >= 0)
            return exact;
    }
    if (hasId) {
        const qsizetype byId = find([&](const NetworkPathCandidate& c) {
            return c.interfaceId == interfaceId;
        });
        if (byId >= 0)
            return byId;
    }
    if (hasName) {
        const qsizetype byName = find([&](const NetworkPathCandidate& c) {
            return c.interfaceName == interfaceName;
        });
        if (byName >= 0)
            return byName;
    }
    if (hasAddress) {
        return find([&](const NetworkPathCandidate& c) {
            return c.address == lastSuccessfulAddress;
        });
    }
    return -1;
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
