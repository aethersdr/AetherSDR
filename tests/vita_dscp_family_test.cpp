// Socket-free: unbound native UDP descriptors only (no bind, send or connect).
// The marker must set the option that marks the socket's own family: IP_TOS on
// IPv4, IPV6_TCLASS on IPv6, where IP_TOS reads back as set but leaves native
// IPv6 packets unmarked.
#include "core/backends/flex/VitaDscpMarker.h"
#include "core/backends/flex/VitaDscpFlow.h"

#include <QHostAddress>
#include <cstdio>

#if defined(Q_OS_UNIX)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}
#if defined(Q_OS_UNIX)
int option(int fd, int level, int name)
{
    int value = -1;
    socklen_t length = sizeof(value);
    return ::getsockopt(fd, level, name, &value, &length) == 0 ? value : -1;
}
#endif
}

int main()
{
#if defined(Q_OS_UNIX)
    const int v6 = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (v6 < 0) {
        std::fprintf(stderr, "SKIP: no IPv6 UDP socket on this host\n");
        return 77;
    }
    VitaDscpMarker marker;
    VitaDscpResult result = marker.apply(v6, QHostAddress(QStringLiteral("fd58:eee:46e8::10")), {4991});
    check(result.applied, "IPv6 socket accepts EF marking");
    check(option(v6, IPPROTO_IPV6, IPV6_TCLASS) == kVitaIpTosEf,
          "IPv6 socket carries EF in IPV6_TCLASS, the class its own packets use");
    check(result.detail.contains(QStringLiteral("IPV6_TCLASS")), "IPv6 result names IPV6_TCLASS");
    marker.reset();
    ::close(v6);

#if defined(Q_OS_LINUX)
    // A dual-stack IPv6 socket sending to an IPv4 radio emits IPv4-mapped
    // packets, which take IP_TOS. (macOS refuses IP_TOS on an IPv6 socket.)
    const int dual = ::socket(AF_INET6, SOCK_DGRAM, 0);
    result = marker.apply(dual, QHostAddress(QStringLiteral("192.168.50.254")), {4991});
    check(result.applied && option(dual, IPPROTO_IP, IP_TOS) == kVitaIpTosEf
              && option(dual, IPPROTO_IPV6, IPV6_TCLASS) != kVitaIpTosEf,
          "an IPv6 socket with an IPv4 destination is marked with IP_TOS, not IPV6_TCLASS");
    marker.reset();
    ::close(dual);
#endif

    const int v4 = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (v4 < 0) {
        std::fprintf(stderr, "SKIP: no IPv4 UDP socket on this host\n");
        return 77;
    }
    result = marker.apply(v4, QHostAddress(QStringLiteral("192.168.50.254")), {4991});
    check(result.applied && option(v4, IPPROTO_IP, IP_TOS) == kVitaIpTosEf,
          "IPv4 socket still carries EF in IP_TOS");
    check(result.detail.contains(QStringLiteral("IP_TOS")), "IPv4 result names IP_TOS");
    marker.reset();
    ::close(v4);

    std::printf("%s vita_dscp_family_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
#else
    std::fprintf(stderr, "SKIP: POSIX socket options only; Windows marks through qWAVE\n");
    return 77;
#endif
}
