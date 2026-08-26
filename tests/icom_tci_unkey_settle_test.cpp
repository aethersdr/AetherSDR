// Socket-free policy coverage for TCI's Icom unkey presentation barrier.

#include "core/IcomTciUnkeySettle.h"

#include <cstdio>

using AetherSDR::IcomTciUnkeySettle;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main()
{
    IcomTciUnkeySettle settle;

    const std::uint64_t noReply = settle.begin();
    check(settle.expire(noReply) == IcomTciUnkeySettle::Expiry::TimedOut,
          "an optimistic unkey without CI-V readback times out");
    check(settle.isAwaitingConfirmation(),
          "a no-reply timeout retains the authoritative confirmation barrier");
    check(settle.confirmOff() == IcomTciUnkeySettle::Confirmation::Late,
          "a late authoritative PTT-off readback releases the retained barrier");

    const std::uint64_t confirmed = settle.begin();
    check(settle.confirmOff()
              == IcomTciUnkeySettle::Confirmation::PendingExpiry,
          "an authoritative readback confirms the active settle generation");
    check(settle.expire(confirmed) == IcomTciUnkeySettle::Expiry::Confirmed,
          "the field-tested bounded settle succeeds after CI-V confirmation");

    const std::uint64_t cancelled = settle.begin();
    settle.cancel();
    check(settle.expire(cancelled) == IcomTciUnkeySettle::Expiry::Stale,
          "teardown invalidates an older settle timer");
    check(settle.confirmOff() == IcomTciUnkeySettle::Confirmation::Ignored,
          "teardown also rejects a late confirmation from the cancelled settle");

    return failures == 0 ? 0 : 1;
}
