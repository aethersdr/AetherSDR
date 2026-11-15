#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>

using AetherSDR::GpsDelta;
using AetherSDR::icom::IcomCivBackend;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void startNtpAccess(IcomCivBackend& backend, qint64 now)
    {
        backend.startNtpAccess(now);
    }

    static bool expireNtpAccess(IcomCivBackend& backend, qint64 now)
    {
        return backend.expireNtpAccess(now);
    }

    static void publishNtpAccessResult(IcomCivBackend& backend, std::uint8_t result)
    {
        backend.publishNtpAccessResult(result);
    }

    static bool active(const IcomCivBackend& backend)
    {
        return backend.m_ntpAccess.active();
    }

    static bool timedOut(const IcomCivBackend& backend)
    {
        return backend.m_ntpAccess.timedOut();
    }
};

}  // namespace AetherSDR::icom

using AetherSDR::icom::IcomCivBackendTestAccess;

namespace {
int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    IcomCivBackend backend;
    QStringList statuses;
    QObject::connect(&backend, &IcomCivBackend::gpsChanged,
                     [&statuses](const GpsDelta& delta) {
        if (delta.ntpSyncStatus) {
            statuses.push_back(*delta.ntpSyncStatus);
        }
    });

    constexpr qint64 kStart = 1'000;
    IcomCivBackendTestAccess::startNtpAccess(backend, kStart);
    check(IcomCivBackendTestAccess::active(backend)
              && statuses == QStringList{QStringLiteral("Accessing")},
          "starting NTP access publishes Accessing and arms the deadline");
    check(!IcomCivBackendTestAccess::expireNtpAccess(
              backend, kStart + AetherSDR::icom::IcomNtpAccess::kTimeoutMs - 1),
          "the backend remains pending before the deadline");
    check(IcomCivBackendTestAccess::expireNtpAccess(
              backend, kStart + AetherSDR::icom::IcomNtpAccess::kTimeoutMs)
              && IcomCivBackendTestAccess::timedOut(backend)
              && statuses.back() == QStringLiteral("Timed out"),
          "the exact deadline publishes one terminal timeout");
    const qsizetype terminalCount = statuses.size();
    check(!IcomCivBackendTestAccess::expireNtpAccess(backend, 100'000)
              && statuses.size() == terminalCount,
          "an expired access cannot emit a second timeout");

    IcomCivBackendTestAccess::publishNtpAccessResult(backend, 0x00);
    check(statuses.size() == terminalCount
              && statuses.back() == QStringLiteral("Timed out"),
          "a late pending reply cannot erase the terminal timeout");

    IcomCivBackendTestAccess::startNtpAccess(backend, 200'000);
    IcomCivBackendTestAccess::publishNtpAccessResult(backend, 0x01);
    check(!IcomCivBackendTestAccess::active(backend)
              && !IcomCivBackendTestAccess::timedOut(backend)
              && statuses.back() == QStringLiteral("Succeeded"),
          "a new access clears timeout state and accepts a terminal result");

    return g_failures == 0 ? 0 : 1;
}
