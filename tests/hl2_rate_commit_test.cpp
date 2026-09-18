// Hl2RateCommit — the ordering rule a pan-bandwidth change depends on.
//
// WHY THIS TEST EXISTS AND WHAT IT CANNOT COVER. The end-to-end seam here is
// Hl2Backend::applyPanBandwidth() driving three threads, and exercising that
// needs a MetisClient and a localhost peer — the fixture class tests/tests.cmake
// deliberately retired. An assertion written against hl2_backend_test would sit
// inside that retired block and be green forever.
//
// So what is pinned here is the rule that was actually broken: a crossing that
// has not written the register has not changed the rate, no matter what the
// backend's optimistic m_sampleRateHz says in the meantime. Hl2Backend holds one
// RateCommitLedger and reads it for both decisions that got this wrong, so
// pinning the ledger pins the rule at its single source.

#include "core/backends/hl2/Hl2RateCommit.h"

#include <cstdio>

using AetherSDR::hl2::RateCommitLedger;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("[%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond)
        ++g_failures;
}

int main()
{
    // ── A fresh ledger reports the rate it was seeded with ───────────────
    {
        RateCommitLedger led(48000);
        check(led.committed() == 48000, "a new ledger reports its seeded rate");
        check(led.generation() == 0, "and no crossing has begun");
    }

    // ── THE BLOCKER. Two crossings overlap; the second one fails. ────────
    //
    // This is one drag of a zoom slider: 48k -> 192k starts, and 192k -> 384k
    // starts before the first build finishes.
    {
        RateCommitLedger led(48000);

        // Crossing A: 48k -> 192k. The backend's m_sampleRateHz moves to 192000
        // here, optimistically. The register is NOT written yet.
        const int previousRateA = led.committed();
        const quint64 genA = led.beginCrossing();
        check(previousRateA == 48000, "crossing A would restore to 48 kHz");
        check(led.isCurrent(genA), "crossing A is current while it is alone");

        // Crossing B: 192k -> 384k, before A's build finished. THIS is the line
        // the bug was on: the old code captured the backend's m_sampleRateHz,
        // which A already moved to 192000 without committing it.
        const int previousRateB = led.committed();
        const quint64 genB = led.beginCrossing();

        check(previousRateB == 48000,
              "crossing B restores to 48 kHz — the rate actually on the wire — "
              "not to A's uncommitted 192 kHz");
        check(!led.isCurrent(genA), "crossing A is superseded by B");
        check(led.isCurrent(genB), "and B is now the current crossing");

        // A finishes first and finds itself superseded: it installs nothing and
        // writes nothing, so the committed rate has still never moved.
        check(led.committed() == 48000,
              "a superseded crossing commits nothing");

        // B's build fails. The rate it puts back is the one the radio is
        // genuinely running at.
        check(previousRateB == led.committed(),
              "a failed crossing restores exactly what is committed");
    }

    // ── A crossing that succeeds commits, and the next one sees it ───────
    {
        RateCommitLedger led(48000);
        const quint64 gen = led.beginCrossing();
        check(led.isCurrent(gen), "the only crossing is current");
        led.commit(192000);   // stands for the register write on the I/O thread
        check(led.committed() == 192000, "a committed crossing moves the rate");

        const int previousRate = led.committed();
        led.beginCrossing();
        check(previousRate == 192000,
              "the next crossing restores to the rate the last one committed");
    }

    // ── A receiver opened mid-build is built for the committed rate ──────
    //
    // The same read, at the other call site. While a crossing to 384 kHz is in
    // flight and uncommitted, a receiver opened now must be built for 48 kHz —
    // the IQ the radio is actually sending — and rebuilt afterwards if the
    // crossing lands.
    {
        RateCommitLedger led(48000);
        led.beginCrossing();   // 48k -> 384k in flight, register unwritten
        check(led.committed() == 48000,
              "a receiver opened mid-crossing is built for the rate on the wire");
        led.commit(384000);
        check(led.committed() == 384000,
              "and once the crossing commits, that receiver is the one left "
              "needing a rebuild");
    }

    // ── Generations are monotonic, so no crossing can revive ─────────────
    {
        RateCommitLedger led(48000);
        const quint64 g1 = led.beginCrossing();
        const quint64 g2 = led.beginCrossing();
        const quint64 g3 = led.beginCrossing();
        check(g1 < g2 && g2 < g3, "generations increase");
        check(!led.isCurrent(g1) && !led.isCurrent(g2) && led.isCurrent(g3),
              "only the newest crossing is current");
    }

    if (g_failures == 0)
        std::printf("hl2_rate_commit_test: all checks passed\n");
    else
        std::printf("hl2_rate_commit_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
