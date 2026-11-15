// Socket-free: the VPN source-path resolver's family rules. Reads this host's
// interface list (QNetworkInterface) but opens no socket.
#include "core/NetworkPathResolver.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}
QHostAddress a(const char* text) { return QHostAddress(QString::fromLatin1(text)); }
}

int main()
{
    using R = NetworkPathResolver;
    check(R::targetFamily(QStringLiteral("fd58:eee:46e8::10")) == QAbstractSocket::IPv6Protocol,
          "an IPv6 literal targets IPv6");
    check(R::targetFamily(QStringLiteral(" 2605:59ca:5044:7d00::10 ")) == QAbstractSocket::IPv6Protocol,
          "surrounding whitespace does not hide an IPv6 literal");
    check(R::targetFamily(QStringLiteral("192.168.50.254")) == QAbstractSocket::IPv4Protocol,
          "an IPv4 literal targets IPv4");
    check(R::targetFamily(QStringLiteral("radio.example")) == QAbstractSocket::IPv4Protocol,
          "a host name keeps the IPv4 default");
    check(R::targetFamily(QString()) == QAbstractSocket::IPv4Protocol, "empty text keeps the IPv4 default");
    check(R::targetFamily(QStringLiteral("::ffff:192.168.50.254")) == QAbstractSocket::IPv4Protocol,
          "an IPv4-mapped literal is an IPv4 radio");

    check(R::isUsableSourceAddress(a("192.168.50.236")), "IPv4 unicast is a source");
    check(R::isUsableSourceAddress(a("fd58:eee:46e8:0:c382:93d2:e9ef:daf7")), "IPv6 ULA is a source");
    check(R::isUsableSourceAddress(a("2605:59ca:5044:7d00:1e54:eb06:745d:bb92")), "IPv6 global is a source");
    check(!R::isUsableSourceAddress(a("127.0.0.1")) && !R::isUsableSourceAddress(a("::1")),
          "loopback is never a source path");
    check(!R::isUsableSourceAddress(a("fe80::1")), "IPv6 link-local is not a saved source path");
    check(!R::isUsableSourceAddress(a("::")) && !R::isUsableSourceAddress(a("ff02::1")),
          "unspecified and multicast IPv6 are not sources");
    check(!R::isUsableSourceAddress(QHostAddress()), "null is not a source");

    for (const auto family : {QAbstractSocket::IPv4Protocol, QAbstractSocket::IPv6Protocol}) {
        bool allMatch = true;
        for (const NetworkPathCandidate& c : R::enumerateCandidates(family))
            allMatch = allMatch && c.address.protocol() == family && c.isValid();
        check(allMatch, "every enumerated candidate is a valid source of the requested family");
    }
    // Every live address of the family is offered, including ones with a
    // finite lifetime (DHCP leases, SLAAC), which most LAN addresses are.
    // Interfaces can change between two snapshots, so retry before failing.
    const auto liveCount = [](QAbstractSocket::NetworkLayerProtocol family) {
        qsizetype live = 0;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            const auto flags = iface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
                || (flags & QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                const QHostAddress ip = entry.ip();
                live += ip.protocol() == family && R::isUsableSourceAddress(ip);
            }
        }
        return live;
    };
    for (const auto family : {QAbstractSocket::IPv4Protocol, QAbstractSocket::IPv6Protocol}) {
        bool same = false;
        for (int attempt = 0; attempt < 3 && !same; ++attempt)
            same = R::enumerateCandidates(family).size() == liveCount(family);
        check(same, "every live address of the family is a candidate");
    }
    // One interface with a GUA and a ULA (sorted GUA first), as IPv6 LANs
    // usually have, plus a second interface.
    const auto cand = [](const char* id, const char* name, const char* addr) {
        return NetworkPathCandidate{QString::fromLatin1(id), QString::fromLatin1(name), a(addr)};
    };
    const QList<NetworkPathCandidate> list{
        cand("wlan0", "Wi-Fi", "2001:db8:1::20"),
        cand("wlan0", "Wi-Fi", "fd00:1::20"),
        cand("tun0", "VPN", "fd7a:115c:a1e0::5"),
    };
    const QString wlan = QStringLiteral("wlan0"), wifi = QStringLiteral("Wi-Fi");
    check(R::matchSelection(list, wlan, wifi, a("fd00:1::20")) == 1,
          "the exact saved ULA wins over the interface's first (GUA) address");
    check(R::matchSelection(list, wlan, wifi, a("2001:db8:1::20")) == 0,
          "the exact saved GUA is kept too");
    check(R::matchSelection(list, wlan, wifi, a("192.168.50.236")) == 0,
          "a saved address of the other family falls back to the interface");
    check(R::matchSelection(list, wlan, wifi, a("fd00:1::99")) == 0,
          "a vanished address falls back to the interface");
    check(R::matchSelection(list, wlan, wifi, a("192.168.50.236"), a("fd00:1::10")) == 1,
          "falling back, the interface's ULA serves a ULA radio (longest prefix)");
    check(R::matchSelection(list, wlan, wifi, a("192.168.50.236"), a("2001:db8:1::10")) == 0,
          "falling back, the interface's GUA serves a GUA radio");
    check(R::matchSelection(list, wlan, wifi, a("2001:db8:1::20"), a("fd00:1::10")) == 0,
          "an exact saved address still wins over a closer prefix");
    check(R::matchSelection(list, QStringLiteral("wlan1"), wifi, a("fd00:1::99")) == 0,
          "a renamed interface id still matches by name");
    check(R::matchSelection(list, QStringLiteral("eth9"), QStringLiteral("Gone"),
                            a("fd7a:115c:a1e0::5")) == 2,
          "with no interface match, the exact address on any interface is used");
    check(R::matchSelection(list, QStringLiteral("eth9"), QStringLiteral("Gone"), a("fd00:9::1")) == -1,
          "nothing matching resolves to no candidate");
    check(R::matchSelection(list, QStringLiteral("tun0"), QString(), a("fd00:1::20")) == 2,
          "an exact address on another interface does not override the saved interface");

    check(R::enumerateIpv4Candidates().size()
              == R::enumerateCandidates(QAbstractSocket::IPv4Protocol).size(),
          "the IPv4 list is unchanged for existing callers");

    std::printf("%s network_path_resolver_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
