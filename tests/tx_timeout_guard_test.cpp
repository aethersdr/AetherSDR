// TxTimeoutGuard (RFC #7021): the host transmit time-out's timer, warning and
// release latch, on an injected clock. Pure policy: no Qt event loop, no
// socket, no radio.

#include "models/TxTimeoutGuard.h"

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

struct FakeClock {
    qint64 now{1'000'000};
    TxTimeoutGuard::Clock clock() { return [this] { return now; }; }
};

using Event = TxTimeoutGuard::Event;

} // namespace

int main()
{
    // Defaults: enabled, 180 s, warning 10 s; range 15-1800.
    {
        const TxTimeoutSettings d = TxTimeoutSettings::defaultsFor(0);
        report("defaults_enabled_180s_warning_10s",
               d.enabled && d.timeoutS == 180 && d.warningS == 10);
        TxTimeoutSettings wide;
        wide.timeoutS = 5;
        wide.warningS = 500;
        const TxTimeoutSettings low = wide.clamped();
        wide.timeoutS = 99999;
        const TxTimeoutSettings high = wide.clamped();
        report("range_clamps_to_15_1800_and_warning_0_60",
               low.timeoutS == 15 && high.timeoutS == 1800 && low.warningS == 60);
    }

    // Second layer: never shorter than the radio's own interlock timeout.
    {
        report("radio_timeout_longer_than_default_wins",
               TxTimeoutSettings::defaultsFor(300'000).timeoutS == 300);
        report("radio_timeout_shorter_keeps_default",
               TxTimeoutSettings::defaultsFor(60'000).timeoutS == 180);
        report("radio_timeout_rounds_up_to_whole_seconds",
               TxTimeoutSettings::defaultsFor(200'500).timeoutS == 201);
    }

    // One feature document round-trips; junk fields fall back.
    {
        TxTimeoutSettings s;
        s.enabled = false;
        s.timeoutS = 600;
        s.warningS = 0;
        const TxTimeoutSettings back = TxTimeoutSettings::fromJson(s.toJson(), {});
        report("document_round_trips", back == s);
        QJsonObject junk{{QStringLiteral("enabled"), QStringLiteral("yes")},
                         {QStringLiteral("timeoutS"), 4}};
        const TxTimeoutSettings repaired = TxTimeoutSettings::fromJson(junk, {});
        report("document_junk_is_repaired",
               repaired.enabled && repaired.timeoutS == 15 && repaired.warningS == 10);
    }

    // Expiry exactly at the limit, not a millisecond before.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        g.setTransmitting(true, QStringLiteral("MOX/PTT"));
        c.now += 179'999;
        const Event before = g.poll();
        c.now += 1;
        const Event at = g.poll();
        report("expires_at_180000_ms_not_before",
               before != Event::Expired && at == Event::Expired && g.lastExpiredAfterS() == 180
                   && g.source() == QStringLiteral("MOX/PTT"));
        report("expiry_latches_and_disarms", g.latched() && !g.armed());
        report("expiry_fires_once", g.poll() == Event::None);
    }

    // The timer bounds one key-down: an unkey before the limit disarms it, and
    // the next over starts from zero.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        g.setTransmitting(true, {});
        c.now += 170'000;
        g.setTransmitting(false, {});
        g.setTransmitting(true, {});
        c.now += 170'000;
        report("each_over_is_timed_from_its_own_key_down",
               g.poll() != Event::Expired && g.armed() && !g.latched());
    }

    // Warning once, warningS before the limit; 0 turns it off.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        g.setTransmitting(true, {});
        c.now += 169'999;
        const Event early = g.poll();
        c.now += 1;
        const Event warn = g.poll();
        const qint64 left = g.remainingMs();
        c.now += 1000;
        const Event again = g.poll();
        report("warning_at_10s_before_once",
               early == Event::None && warn == Event::Warning && left == 10'000
                   && again == Event::None);

        TxTimeoutGuard quiet(c.clock());
        TxTimeoutSettings off;
        off.warningS = 0;
        quiet.setSettings(off);
        quiet.setTransmitting(true, {});
        bool warned = false;
        for (int i = 0; i < 179; ++i) {
            c.now += 1000;
            warned = warned || quiet.poll() == Event::Warning;
        }
        report("warning_zero_is_off", !warned);
    }

    // Disabled: nothing is armed and nothing expires.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        TxTimeoutSettings off;
        off.enabled = false;
        g.setSettings(off);
        g.setTransmitting(true, {});
        c.now += 3'600'000;
        report("disabled_never_expires", g.poll() == Event::None && !g.armed() && !g.latched());
    }

    // Latch: an input still held keeps it after the radio is idle; its release
    // clears it. A dead input counts as released.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        bool tciAlive = true;
        (void)g.noteHold(0x10, [] { return true; });
        (void)g.noteHold(0x20, [&tciAlive] { return tciAlive; });
        g.setTransmitting(true, {});
        c.now += 180'000;
        (void)g.poll();
        g.setTransmitting(false, {});
        report("held_inputs_keep_latch_after_unkey", g.latched() && g.holderCount() == 2);
        g.noteRelease(0x10);
        report("one_release_of_two_keeps_latch", g.latched());
        tciAlive = false;
        g.refreshLatch();
        report("dead_holder_counts_as_released", !g.latched() && g.holderCount() == 0);
    }

    // The radio still transmitting keeps the latch even with nothing held.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        g.setTransmitting(true, {});
        c.now += 180'000;
        (void)g.poll();
        g.refreshLatch();
        const bool whileKeyed = g.latched();
        g.setTransmitting(false, {});
        report("latch_holds_while_radio_keyed_then_clears", whileKeyed && !g.latched());
    }

    // forgetHolders() never erases a latched input.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        (void)g.noteHold(0x30, [] { return true; });
        g.setTransmitting(true, {});
        c.now += 180'000;
        (void)g.poll();
        g.setTransmitting(false, {});
        g.forgetHolders();
        report("forget_holders_is_refused_while_latched", g.latched() && g.holderCount() == 1);
    }

    // Idle bookkeeping is pruned before an over, so a stale hold cannot latch.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        (void)g.noteHold(0x40, [] { return true; });
        g.forgetHolders();
        g.setTransmitting(true, {});
        c.now += 180'000;
        (void)g.poll();
        g.setTransmitting(false, {});
        report("forgotten_idle_holder_does_not_latch", !g.latched());
    }

    // A full holder table evicts only dead entries; a live one never slips.
    {
        FakeClock c;
        TxTimeoutGuard g(c.clock());
        bool dead = false;
        bool allRecorded = true;
        for (quintptr k = 1; k <= TxTimeoutGuard::kMaximumHolders; ++k) {
            allRecorded &= k == 1 ? g.noteHold(k, [&dead] { return !dead; })
                                  : g.noteHold(k, [] { return true; });
        }
        const bool thirtyThirdRefused = !g.canHold(999) && !g.noteHold(999, [] { return true; });
        report("holder_table_bounded", g.holderCount() == TxTimeoutGuard::kMaximumHolders);
        report("thirty_third_live_holder_is_refused_not_dropped",
               allRecorded && thirtyThirdRefused && g.holderCount() == TxTimeoutGuard::kMaximumHolders);
        report("full_table_still_accepts_a_holder_it_has",
               g.canHold(2) && g.noteHold(2, [] { return true; }));
        dead = true;
        report("a_dead_holder_makes_room", g.canHold(999) && g.noteHold(999, [] { return true; }));
    }

    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
