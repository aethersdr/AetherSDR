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
    check(settle.confirm(false) == IcomTciUnkeySettle::Confirmation::Late,
          "a late authoritative PTT-off readback releases the retained barrier");

    const std::uint64_t confirmed = settle.begin();
    check(settle.confirm(false)
              == IcomTciUnkeySettle::Confirmation::PendingExpiry,
          "an authoritative readback confirms the active settle generation");
    check(settle.expire(confirmed) == IcomTciUnkeySettle::Expiry::Confirmed,
          "the field-tested bounded settle succeeds after CI-V confirmation");

    const std::uint64_t cancelled = settle.begin();
    settle.cancel();
    check(settle.expire(cancelled) == IcomTciUnkeySettle::Expiry::Stale,
          "teardown invalidates an older settle timer");
    check(settle.confirm(false) == IcomTciUnkeySettle::Confirmation::Ignored,
          "teardown also rejects a late confirmation from the cancelled settle");

    const std::uint64_t rekeyed = settle.begin();
    check(settle.confirm(false)
              == IcomTciUnkeySettle::Confirmation::PendingExpiry,
          "an early off readback initially confirms the settle generation");
    check(settle.confirm(true)
              == IcomTciUnkeySettle::Confirmation::PendingExpiry,
          "a later keyed readback revokes the earlier off confirmation");
    check(settle.expire(rekeyed) == IcomTciUnkeySettle::Expiry::TimedOut,
          "off then on before expiry fails safe instead of hiding keyed state");
    check(settle.isAwaitingConfirmation(),
          "the re-key timeout retains ownership until a later off readback");
    check(settle.confirm(false) == IcomTciUnkeySettle::Confirmation::Late,
          "a post-timeout off readback releases the conservative barrier");

    const std::uint64_t reconfirmed = settle.begin();
    settle.confirm(false);
    settle.confirm(true);
    settle.confirm(false);
    check(settle.expire(reconfirmed) == IcomTciUnkeySettle::Expiry::Confirmed,
          "off then on then off uses the latest accepted radio state");

    const std::uint64_t keyedAfterTimeout = settle.begin();
    check(settle.expire(keyedAfterTimeout)
              == IcomTciUnkeySettle::Expiry::TimedOut,
          "a second no-reply generation also times out conservatively");
    check(settle.confirm(true)
              == IcomTciUnkeySettle::Confirmation::PendingExpiry,
          "a keyed readback after timeout keeps waiting for authoritative off");
    check(settle.isAwaitingConfirmation(),
          "keyed confirmation after timeout cannot release ownership");
    check(settle.confirm(false) == IcomTciUnkeySettle::Confirmation::Late,
          "the later off readback still releases the timed-out generation");

    return failures == 0 ? 0 : 1;
}
