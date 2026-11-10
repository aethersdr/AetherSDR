#pragma once

#include <QAbstractSocket>
#include <QHostAddress>
#include <QList>
#include <QString>

namespace AetherSDR {

struct NetworkPathCandidate {
    QString      interfaceId;
    QString      interfaceName;
    QHostAddress address;

    bool isValid() const;
    QString label() const;
};

class NetworkPathResolver {
public:
    using Family = QAbstractSocket::NetworkLayerProtocol;

    static QList<NetworkPathCandidate> enumerateIpv4Candidates();
    // Source addresses of one family; IPv6 leaves out link-local. A saved
    // path matches by interface first, so a rotating IPv6 address still resolves.
    static QList<NetworkPathCandidate> enumerateCandidates(Family family);
    // A saved path resolves to its interface's address in `family`, so a path
    // chosen for an IPv4 radio still applies when the radio is reached over IPv6.
    static NetworkPathCandidate resolveExplicitSelection(const QString& interfaceId,
                                                         const QString& interfaceName,
                                                         const QHostAddress& lastSuccessfulAddress,
                                                         Family family = QAbstractSocket::IPv4Protocol,
                                                         const QHostAddress& target = {});
    // The candidate a saved selection names, or -1: the exact saved address
    // on the saved interface, else the interface by id, then by name, else the
    // exact address on any interface. An IPv6 interface usually carries both a
    // GUA and a ULA, so the exact address must win over "first on the interface";
    // without it, the interface's address sharing the longest prefix with
    // `target` wins (RFC 6724 rule 8), so a ULA radio gets the ULA source.
    static qsizetype matchSelection(const QList<NetworkPathCandidate>& candidates,
                                    const QString& interfaceId,
                                    const QString& interfaceName,
                                    const QHostAddress& lastSuccessfulAddress,
                                    const QHostAddress& target = {});
    static NetworkPathCandidate autoCandidate();
    static bool isUsableIpv4(const QHostAddress& address);
    static bool isUsableSourceAddress(const QHostAddress& address);
    // The family a connect target needs: IPv6 for an IPv6 literal, else IPv4
    // (an IPv4 literal, or a name, which has always been probed over IPv4).
    static Family targetFamily(const QString& host);
};

} // namespace AetherSDR
