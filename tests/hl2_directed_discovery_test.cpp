// The directed (unicast) discovery target list keeps only usable targets.
//
// Pure policy, no socket and no event loop: setDirectedTargets drops null and
// non-IPv4 addresses (writeDatagram() to either sends nothing from an IPv4
// socket) and de-duplicates, so a repeated target costs one probe per sweep.
// That a unicast probe reaches a routed radio is a property of real firmware
// and is covered on hardware, not by a fake peer here.

#include "core/backends/hl2/Hl2Discovery.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

using AetherSDR::hl2::Hl2Discovery;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++failures;
}

QList<QHostAddress> accepted(const QList<QHostAddress>& targets)
{
    Hl2Discovery discovery;
    discovery.setDirectedTargets(targets);
    return discovery.directedTargets();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QHostAddress a(QStringLiteral("192.168.50.7"));
    const QHostAddress b(QStringLiteral("10.1.2.3"));

    check(accepted({}).isEmpty(), "no target configured: nothing is probed");
    check(accepted({QHostAddress(QStringLiteral("not an address")), QHostAddress::AnyIPv6}).isEmpty(),
          "null and non-IPv4 targets are dropped");
    check(accepted({a}) == QList<QHostAddress>{a}, "an IPv4 target is kept");
    check(accepted({a, a, b, a}) == (QList<QHostAddress>{a, b}), "a repeated target is kept once, in order");
    check(accepted({QHostAddress(QStringLiteral("not an address")), QHostAddress::AnyIPv6, a, a}) == QList<QHostAddress>{a},
          "invalid entries do not displace valid ones");

    Hl2Discovery discovery;
    discovery.setDirectedTargets({a});
    discovery.setDirectedTargets({b});
    check(discovery.directedTargets() == QList<QHostAddress>{b}, "a new list replaces the old one");
    return failures == 0 ? 0 : 1;
}
