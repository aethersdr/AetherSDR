#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>
#include <memory>

namespace AetherSDR {

// Information about a radio discovered via SmartLink (WAN).
struct WanRadioInfo {
    QString serial;
    QString nickname;
    QString callsign;
    QString model;
    QString status;         // "Available", "In Use", etc.
    QString publicIp;
    int     publicTlsPort{-1};
    int     publicUdpPort{-1};
    bool    upnpSupported{false};
    bool    requiresHolePunch{false};
    int     licensedClients{1};
    QString maxLicensedVersion;
    QString guiClientPrograms;
    QString guiClientStations;
    QString guiClientHandles;
    QString guiClientIps;
    QString guiClientHosts;
};

struct PinnedCertInfo {
    QString host;
    QString fingerprintHex;
    QString pinnedAtIso;
};

// Engine-only account lease. No credentials or account protocol escape through
// this contract; a lease remains held until its radio session has closed.
class RemoteRadioAccount {
public:
    using Ready = std::function<void(quint64, const QString&)>;
    using Failed = std::function<void(quint64, const QString&)>;
    virtual ~RemoteRadioAccount() = default;
    virtual quint64 beginConnection(const QString& serial, quint16 localPort,
        const QList<quint32>& disconnectHandles, Ready ready, Failed failed) = 0;
    virtual void cancelConnection(quint64 lease) = 0;
};

struct RemoteRadioRequest {
    std::shared_ptr<RemoteRadioAccount> account;
    WanRadioInfo radio;
    QList<quint32> disconnectHandles;
};

} // namespace AetherSDR
