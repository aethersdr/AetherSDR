#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>

namespace MasterSDR {

struct NetworkPathCandidate {
    QString      interfaceId;
    QString      interfaceName;
    QHostAddress address;

    bool isValid() const;
    QString label() const;
};

class NetworkPathResolver {
public:
    static QList<NetworkPathCandidate> enumerateIpv4Candidates();
    static NetworkPathCandidate resolveExplicitSelection(const QString& interfaceId,
                                                         const QString& interfaceName,
                                                         const QHostAddress& lastSuccessfulAddress);
    static NetworkPathCandidate autoCandidate();
    static bool isUsableIpv4(const QHostAddress& address);
};

} // namespace MasterSDR
