// The retransmit buffer's eviction order.
//
// RS-BA1 lets the radio ask us to re-send a packet by sequence number, so each
// stream keeps the last kReplayDepth packets. The buffer is a QMap keyed by
// sequence — and the sequence space WRAPS at 0xFFFF, which makes "the lowest
// key" and "the oldest packet" the same thing only until it does.
//
// After a wrap the map holds keys near 0xFFFF and near 0x0000 together, so
// evicting the lowest key throws away the FRESHEST packets: exactly the ones a
// retransmit request is most likely to name. The caller then gets an Idle
// carrying that sequence instead of the payload — a silently dropped CI-V
// command or audio frame, once every ~11 minutes of transmit at 100 packets/s.
//
// This is the same hazard onReorderTick() documents and avoids; it bit the
// other direction here.

#include "core/backends/icom/IcomStream.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<std::uint8_t> payload{0xDE, 0xAD};

    // Fill past the retain depth ACROSS A WRAP: 0xFFC0.. rolls through 0x0000.
    {
        IcomStream s;
        for (int i = 0; i < 400; ++i)
            s.retainForTest(static_cast<quint16>(0xFFC0 + i), payload);

        const auto held = s.retainedSequences();
        check(!held.isEmpty(), "the buffer retains something at all");

        // The LAST sequences written must still be there. Under the old
        // lowest-key eviction the post-wrap sequences (0x0000 upward) were the
        // first thrown away, so the newest packets were exactly the missing
        // ones.
        const quint16 newest = static_cast<quint16>(0xFFC0 + 399);
        check(held.contains(newest), "the most recently sent sequence is still retained");
        check(held.contains(static_cast<quint16>(newest - 1)),
              "and so is the one before it");

        // And the genuinely oldest — pre-wrap — must have gone.
        check(!held.contains(static_cast<quint16>(0xFFC0)),
              "while the oldest sequence has been evicted");

        // Oldest-first ordering, so the front is what leaves next.
        check(held.size() <= 256, "the buffer respects its depth");
    }

    // No wrap: behaviour is unchanged for the ordinary case.
    {
        IcomStream s;
        for (int i = 0; i < 300; ++i)
            s.retainForTest(static_cast<quint16>(1000 + i), payload);
        const auto held = s.retainedSequences();
        check(held.contains(1299), "newest retained without a wrap");
        check(!held.contains(1000), "oldest evicted without a wrap");
    }

    // Re-retaining the same sequence must not double-count it into the order
    // deque, or the buffer evicts early and under-retains.
    {
        IcomStream s;
        for (int i = 0; i < 10; ++i)
            s.retainForTest(42, payload);
        check(s.retainedSequences().size() == 1,
              "re-retaining one sequence keeps one entry, not ten");
    }

    if (g_failures == 0)
        std::printf("icom_replay_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
