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
    for (const auto family : {QAbstractSocket::IPv4Protocol, QAbstractSocket::IPv6Protocol}) {
        qsizetype live = 0;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            const auto flags = iface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
                || (flags & QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                const QHostAddress ip = entry.ip();
                live += ip.protocol() == family && !ip.isLoopback() && !ip.isLinkLocal();
            }
        }
        check(R::enumerateCandidates(family).size() == live,
              "every live address of the family is a candidate");
    }
    check(R::enumerateIpv4Candidates().size()
              == R::enumerateCandidates(QAbstractSocket::IPv4Protocol).size(),
          "the IPv4 list is unchanged for existing callers");

    std::printf("%s network_path_resolver_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
