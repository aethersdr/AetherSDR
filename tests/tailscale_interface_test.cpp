// Tailscale-interface check for Radio Setup's IP Configuration gate, socket-free:
// every interface name and address is injected.
#include "core/TailscaleInterface.h"

#include <iostream>

using namespace AetherSDR;

int main()
{
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok = false;
        }
    };
    const auto addr = [](const char* text) { return QHostAddress(QString::fromLatin1(text)); };
    const auto iface = [&addr](const char* name, const char* human, const char* address) {
        return NetworkInterfaceRecord{QString::fromLatin1(name), QString::fromLatin1(human),
                                      {addr(address)}};
    };

    // Linux: the name decides.
    check(isTailscaleInterface(InterfaceNaming::Linux, QStringLiteral("tailscale0"), {},
                               addr("100.101.102.103")),
          "Linux tailscale0 is Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::Linux, QStringLiteral("eth0"), {},
                                addr("100.64.1.2")),
          "a Linux LAN numbered in 100.64/10 is not Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::Linux, QStringLiteral("wg0"), {},
                                addr("100.64.1.2")),
          "a Linux WireGuard tunnel is not Tailscale's, whatever its address");
    check(!isTailscaleInterface(InterfaceNaming::Linux, QStringLiteral("tun0"), {},
                                addr("10.8.0.2")),
          "a Linux OpenVPN tunnel is not Tailscale's");

    // Windows: the adapter's name decides.
    check(isTailscaleInterface(InterfaceNaming::Windows, QStringLiteral("ethernet_32777"),
                               QStringLiteral("Tailscale"), addr("100.101.102.103")),
          "the Windows Tailscale adapter is Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::Windows, QStringLiteral("ethernet_7"),
                                QStringLiteral("Ethernet"), addr("100.64.1.2")),
          "a Windows Ethernet adapter in 100.64/10 is not Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::Windows, QStringLiteral("ethernet_9"),
                                QStringLiteral("WireGuard Tunnel"), addr("10.8.0.2")),
          "a Windows WireGuard adapter is not Tailscale's");

    // macOS: a utun holding a tailnet address.
    check(isTailscaleInterface(InterfaceNaming::MacOS, QStringLiteral("utun4"), {},
                               addr("100.101.102.103")),
          "a macOS utun with a tailnet IPv4 address is Tailscale's");
    check(isTailscaleInterface(InterfaceNaming::MacOS, QStringLiteral("utun4"), {},
                               addr("fd7a:115c:a1e0::1234")),
          "a macOS utun with a tailnet IPv6 address is Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::MacOS, QStringLiteral("utun3"), {},
                                addr("10.8.0.2")),
          "a macOS WireGuard utun outside the tailnet ranges is not Tailscale's");
    check(!isTailscaleInterface(InterfaceNaming::MacOS, QStringLiteral("en0"), {},
                                addr("100.64.1.2")),
          "a macOS LAN numbered in 100.64/10 is not Tailscale's");

    // The lookup: the interface that holds the connection's local address.
    const QList<NetworkInterfaceRecord> linuxHost{
        iface("lo", "lo", "127.0.0.1"),
        iface("eth0", "eth0", "192.168.1.10"),
        iface("tailscale0", "tailscale0", "100.101.102.103"),
    };
    check(carriedByTailscaleInterface(InterfaceNaming::Linux, linuxHost,
                                      addr("100.101.102.103")),
          "a connection bound to tailscale0's address runs over Tailscale");
    check(carriedByTailscaleInterface(InterfaceNaming::Linux, linuxHost,
                                      addr("::ffff:100.101.102.103")),
          "a v4-mapped local address matches tailscale0's IPv4 address");
    check(!carriedByTailscaleInterface(InterfaceNaming::Linux, linuxHost, addr("192.168.1.10")),
          "a connection bound to the LAN interface is local");
    check(!carriedByTailscaleInterface(InterfaceNaming::Linux, linuxHost, QHostAddress()),
          "no local address is not Tailscale");
    check(!carriedByTailscaleInterface(InterfaceNaming::Linux, linuxHost, addr("100.64.9.9")),
          "a tailnet-range address no interface holds is not Tailscale");

    const QList<NetworkInterfaceRecord> cgnatLan{iface("eth0", "eth0", "100.64.0.20")};
    check(!carriedByTailscaleInterface(InterfaceNaming::Linux, cgnatLan, addr("100.64.0.20")),
          "a LAN numbered in 100.64/10 is local");

    return ok ? 0 : 1;
}
