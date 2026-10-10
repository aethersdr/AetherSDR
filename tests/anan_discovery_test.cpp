// aetherd ANAN P2 -- discovery broadcast targeting.
//
// WHAT THIS IS FOR. A discovery datagram sent to 255.255.255.255 leaves by one
// interface: whichever the host's default route picks. On a machine with the
// radio on a second NIC, a VPN up, or a virtual bridge ahead of the default
// route, the radio never sees the sweep and never appears in the picker -- which
// is the symptom this addresses. AnanDiscovery::broadcastTargets() turns the
// host's own address entries into one directed broadcast per subnet.
//
// It is tested here rather than against a live host because the interesting
// network shapes (multi-homed, a point-to-point tunnel with no broadcast
// address, IPv6 alongside IPv4) are exactly the ones a developer machine and a
// CI runner do not have. QNetworkAddressEntry can be built by hand; a
// QNetworkInterface cannot, which is why the flag filtering sits in
// AnanDiscovery.cpp and only this half is covered.
//
// No sockets, no radio, nothing sent.

#include "core/backends/anan/AnanDiscovery.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QList>
#include <QNetworkAddressEntry>

#include <cstdio>

using AetherSDR::anan::AnanDiscovery;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// One ordinary IPv4 entry: an address, its netmask, and the broadcast address
// the platform derives from them.
static QNetworkAddressEntry ipv4Entry(const char* ip, const char* mask, const char* broadcast)
{
    QNetworkAddressEntry e;
    e.setIp(QHostAddress(QLatin1String(ip)));
    e.setNetmask(QHostAddress(QLatin1String(mask)));
    e.setBroadcast(QHostAddress(QLatin1String(broadcast)));
    return e;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QHostAddress global(QHostAddress::Broadcast);   // 255.255.255.255

    // THE PROPERTY THAT MATTERS MOST: the global broadcast is always present and
    // always first. Everything else is added to what this already did, so a host
    // whose enumeration is empty, wrong, or unlike anything anticipated here is
    // no worse off than before the change. A regression that made the directed
    // addresses REPLACE it would break discovery on every single-NIC machine.
    {
        const auto none = AnanDiscovery::broadcastTargets({});
        check(none.size() == 1 && none.first() == global,
              "no interfaces at all still sweeps 255.255.255.255");
    }

    // The ordinary multi-homed case this exists for: two subnets, the radio on
    // whichever one is not the default route.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
            ipv4Entry("10.0.5.2", "255.255.255.0", "10.0.5.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 3, "two subnets -> the global broadcast plus one each");
        check(targets.first() == global, "the global broadcast stays first");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "the first subnet's directed broadcast is swept");
        check(targets.contains(QHostAddress(QLatin1String("10.0.5.255"))),
              "and so is the second's -- the whole point of the change");
    }

    // A point-to-point link (a VPN tun, usually) has an address but NO broadcast
    // address. Sending to a null address is an error, not a no-op, so it has to
    // be dropped rather than passed through.
    {
        QNetworkAddressEntry tunnel;
        tunnel.setIp(QHostAddress(QLatin1String("10.8.0.6")));
        tunnel.setNetmask(QHostAddress(QLatin1String("255.255.255.255")));
        // No setBroadcast() -- which is the real shape of a tun device.
        QList<QNetworkAddressEntry> entries{
            tunnel,
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2,
              "an interface with no broadcast address contributes no target");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "and does not stop the interface beside it being swept");
        for (const QHostAddress& t : targets)
            check(!t.isNull(), "no null address is ever returned as a target");
    }

    // Loopback. Its subnet broadcast reaches this host only; the radio is not
    // there, and sweeping it would be noise on every machine.
    {
        QNetworkAddressEntry lo = ipv4Entry("127.0.0.1", "255.0.0.0", "127.255.255.255");
        const auto targets = AnanDiscovery::broadcastTargets({lo});
        check(targets.size() == 1 && targets.first() == global,
              "loopback contributes no directed broadcast");
    }

    // IPv6 has no broadcast concept at all, and protocol 2 discovery is IPv4
    // only. An IPv6 entry must be skipped rather than producing something that
    // cannot be sent to.
    {
        QNetworkAddressEntry v6;
        v6.setIp(QHostAddress(QLatin1String("fe80::1")));
        v6.setPrefixLength(64);
        QList<QNetworkAddressEntry> entries{
            v6,
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2, "an IPv6 entry contributes no target");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "and the IPv4 entry beside it is still swept");
    }

    // Two addresses on the SAME subnet (a secondary IP, or the same subnet on
    // two NICs) derive the same broadcast address. Sending twice would duplicate
    // every reply and double the sweep's traffic for nothing.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
            ipv4Entry("192.168.1.77", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2, "a repeated broadcast address is sent to once");
    }

    // And the global broadcast appearing as an interface's own broadcast address
    // must not add a second copy of it either.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("10.1.2.3", "0.0.0.0", "255.255.255.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 1 && targets.first() == global,
              "an entry whose broadcast IS 255.255.255.255 is not duplicated");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_discovery_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
