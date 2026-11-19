// A session to a peer on this host (hpsdrsim at 127.0.0.1) binds loopback, so
// its replies come from the address the sender filter accepts. Socket-free:
// the route choice is a pure function of the target, tested directly; no bind,
// no peer.

#include "core/backends/hl2/MetisClient.h"

#include <cstdio>

using AetherSDR::hl2::MetisClient;

namespace {
int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}
}  // namespace

int main()
{
    const QHostAddress sim(QStringLiteral("127.0.0.1"));
    const QHostAddress lan(QStringLiteral("192.168.36.55"));    // the host's own en0
    const QHostAddress radio(QStringLiteral("192.168.8.2"));
    const QHostAddress nic(QStringLiteral("192.168.8.10"));
    const QHostAddress broadcast(QHostAddress::Broadcast);

    // The d281 case: discovery reported en0 as the route to 127.0.0.1.
    check(MetisClient::sessionBindAddress(sim, lan) == QHostAddress(QHostAddress::LocalHost),
          "a session to 127.0.0.1 binds loopback, whatever route discovery reported");
    check(MetisClient::sessionBindAddress(sim, {}) == QHostAddress(QHostAddress::LocalHost),
          "and with no route reported");
    check(MetisClient::sessionBindAddress(QHostAddress(QStringLiteral("127.0.0.2")), lan)
              == QHostAddress(QHostAddress::LocalHost),
          "a 127.0.0.2 target binds 127.0.0.1 too (the bind only: its replies come from "
          "127.0.0.1, so that session still fails the sender filter)");
    // IPv6 loopback is not mapped to an IPv4 bind that cannot reach it.
    check(MetisClient::sessionBindAddress(QHostAddress(QHostAddress::LocalHostIPv6), {})
              == QHostAddress(QHostAddress::AnyIPv4),
          "::1 keeps the wildcard bind, never an IPv4 127.0.0.1 one");
    // A radio on the network: exactly as before.
    check(MetisClient::sessionBindAddress(radio, nic) == nic,
          "a radio on the LAN binds the interface discovery found");
    check(MetisClient::sessionBindAddress(radio, {}) == QHostAddress(QHostAddress::AnyIPv4),
          "and the wildcard when none was found");

    check(MetisClient::discoveryWalksInterface(true, true, sim),
          "a probe to 127.0.0.1 goes out on loopback");
    check(!MetisClient::discoveryWalksInterface(true, false, sim),
          "and not from a LAN interface, whose reply would come from the LAN address");
    check(MetisClient::discoveryWalksInterface(true, false, radio)
              && !MetisClient::discoveryWalksInterface(true, true, radio),
          "a unicast probe to a radio walks the LAN interfaces only, as before");
    check(MetisClient::discoveryWalksInterface(true, false, broadcast)
              && !MetisClient::discoveryWalksInterface(true, true, broadcast),
          "the broadcast sweep walks the LAN interfaces only, as before");
    check(!MetisClient::discoveryWalksInterface(false, true, sim)
              && !MetisClient::discoveryWalksInterface(false, false, radio),
          "an interface that is down or not running is never walked");

    std::printf("%s\n", g_failures == 0 ? "hl2_sim_loopback_bind_test: all checks passed"
                                        : "hl2_sim_loopback_bind_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
