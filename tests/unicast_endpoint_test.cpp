// Socket-free: the unicast-endpoint helpers the CTR2 relay and TCP proxy use
// to accept a radio AetherSDR reached over IPv4 or IPv6.
#include "core/UnicastEndpoint.h"

#include <QHostAddress>
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
    check(usableUnicastAddress(a("192.168.50.254")) == a("192.168.50.254"), "IPv4 unicast is kept");
    check(usableUnicastAddress(a("fd58:eee:46e8::10")) == a("fd58:eee:46e8::10"), "IPv6 ULA is kept");
    check(usableUnicastAddress(a("2605:59ca:5044:7d00::10")) == a("2605:59ca:5044:7d00::10"),
          "IPv6 global unicast is kept");
    const QHostAddress mapped = usableUnicastAddress(a("::ffff:192.168.50.254"));
    check(mapped == a("192.168.50.254") && mapped.protocol() == QAbstractSocket::IPv4Protocol,
          "IPv4-mapped IPv6 folds to plain IPv4");

    check(usableUnicastAddress(QHostAddress()).isNull(), "null is rejected");
    check(usableUnicastAddress(a("0.0.0.0")).isNull(), "IPv4 unspecified is rejected");
    check(usableUnicastAddress(a("::")).isNull(), "IPv6 unspecified is rejected");
    check(usableUnicastAddress(a("255.255.255.255")).isNull(), "IPv4 broadcast is rejected");
    check(usableUnicastAddress(a("224.0.0.1")).isNull(), "IPv4 multicast is rejected");
    check(usableUnicastAddress(a("::ffff:224.0.0.1")).isNull(), "mapped IPv4 multicast is rejected");
    check(usableUnicastAddress(a("ff02::1")).isNull(), "IPv6 multicast is rejected");
    check(usableUnicastAddress(a("fe80::1")).isNull(),
          "IPv6 link-local with no scope names no interface and is rejected");
    check(!usableUnicastAddress(a("fe80::1%1")).isNull(), "IPv6 link-local with a scope is kept");

    check(endpointText(a("192.168.50.254"), 4992) == QStringLiteral("192.168.50.254:4992"),
          "IPv4 endpoint is host:port");
    check(endpointText(a("fd58:eee:46e8::10"), 4992) == QStringLiteral("[fd58:eee:46e8::10]:4992"),
          "IPv6 endpoint brackets the host");

    std::printf("%s unicast_endpoint_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
