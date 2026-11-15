#pragma once

#include "core/TailnetAddress.h"

#include <QHostAddress>
#include <QList>
#include <QNetworkInterface>
#include <QString>

namespace AetherSDR {

// How the host names the network interface Tailscale creates.
enum class InterfaceNaming { Linux, Windows, MacOS };

inline constexpr InterfaceNaming hostInterfaceNaming()
{
#if defined(Q_OS_WIN)
    return InterfaceNaming::Windows;
#elif defined(Q_OS_DARWIN)
    return InterfaceNaming::MacOS;
#else
    // Linux, and the BSDs, where tailscaled also creates tailscale0.
    return InterfaceNaming::Linux;
#endif
}

// One host interface, as far as the Tailscale check needs it. Kept apart from
// QNetworkInterface so the check is testable with injected names and addresses.
struct NetworkInterfaceRecord {
    QString name;                 // QNetworkInterface::name(): "tailscale0", "utun4"
    QString humanReadableName;    // Windows' adapter name: "Tailscale"
    QList<QHostAddress> addresses;
};

// Whether an interface is Tailscale's, by the name each OS gives it:
// `tailscale*` on Linux, the adapter "Tailscale" on Windows, and on macOS a
// `utun*` interface holding a tailnet address.
//
// macOS is the weak case: Tailscale's interface is an ordinary utun, and
// WireGuard and other VPNs use utun as well, so a utun is told apart only by
// its address. A WireGuard or OpenVPN tunnel numbered in 100.64.0.0/10 or
// fd7a:115c:a1e0::/48 on a Mac reads as Tailscale. Elsewhere the name alone
// decides, so a LAN numbered in 100.64.0.0/10 is never Tailscale's.
inline bool isTailscaleInterface(InterfaceNaming naming, const QString& name,
                                 const QString& humanReadableName, const QHostAddress& address)
{
    switch (naming) {
    case InterfaceNaming::Linux:
        return name.startsWith(QStringLiteral("tailscale"));
    case InterfaceNaming::Windows:
        return humanReadableName.compare(QStringLiteral("Tailscale"), Qt::CaseInsensitive) == 0;
    case InterfaceNaming::MacOS:
        return name.startsWith(QStringLiteral("utun")) && isTailnetAddress(address);
    }
    return false;
}

// Whether `local`, the address this computer's end of the connection is bound
// to, sits on a Tailscale interface: the connection to the radio actually runs
// through Tailscale, whatever address the radio has (the in-radio shim, or a
// subnet router in front of the radio's LAN). A null address, or one no
// interface holds, is not.
inline bool carriedByTailscaleInterface(InterfaceNaming naming,
                                        const QList<NetworkInterfaceRecord>& interfaces,
                                        const QHostAddress& local)
{
    if (local.isNull()) {
        return false;
    }
    for (const NetworkInterfaceRecord& iface : interfaces) {
        for (const QHostAddress& address : iface.addresses) {
            if (address.isEqual(local, QHostAddress::ConvertV4MappedToIPv4)) {
                return isTailscaleInterface(naming, iface.name, iface.humanReadableName,
                                            address);
            }
        }
    }
    return false;
}

inline QList<NetworkInterfaceRecord> hostNetworkInterfaces()
{
    QList<NetworkInterfaceRecord> records;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        NetworkInterfaceRecord record{iface.name(), iface.humanReadableName(), {}};
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry& entry : entries) {
            record.addresses.append(entry.ip());
        }
        records.append(record);
    }
    return records;
}

// This host's answer for a connection bound to `local`.
inline bool connectionOverTailscale(const QHostAddress& local)
{
    return carriedByTailscaleInterface(hostInterfaceNaming(), hostNetworkInterfaces(), local);
}

}  // namespace AetherSDR
