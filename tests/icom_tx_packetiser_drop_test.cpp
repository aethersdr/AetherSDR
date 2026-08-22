// TxPacketizer overflow accounting.
//
// submit() drops the OLDEST bytes past kMaxPendingBytes. For voice that is
// correct — latency must not grow and the freshest audio is what matters. For a
// digital burst it is destructive in a way nothing reported: the oldest bytes of
// an AX.25 transmission are its preamble and opening flag, so what survives
// keys the radio, sounds exactly like packet on a receiver, and syncs on
// nothing. A transmission that lost most of itself looked identical to one that
// did not.
//
// These assertions pin the counter against ARITHMETIC rather than against
// itself: dropped == submitted - retained, computed from the queue's own cap.
// An instrument that under-reports here would turn a lost preamble back into
// silence, which is the exact failure it exists to make visible.

#include "core/backends/icom/IcomAudio.h"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// s16 mono: one sample is two bytes on the wire.
constexpr std::size_t kBytesPerSample = 2;

std::vector<float> tone(std::size_t samples)
{
    std::vector<float> v(samples);
    for (std::size_t i = 0; i < samples; ++i)
        v[i] = (i % 2 == 0) ? 0.5f : -0.5f;
    return v;
}

} // namespace

int main()
{
    using AetherSDR::icom::TxPacketizer;
    const std::size_t cap = TxPacketizer::kMaxPendingBytes;

    // ---- a submit that stays under the cap drops nothing --------------------
    {
        TxPacketizer p;
        const std::size_t samples = (cap / kBytesPerSample) / 4;
        p.submit(tone(samples));
        check(p.droppedBytes() == 0, "an under-cap submit drops nothing");
        check(p.dropEvents() == 0, "and records no overflow event");
        check(p.pendingBytes() == samples * kBytesPerSample,
              "every submitted byte is still queued");
    }

    // ---- an oversized submit drops exactly the excess, from the FRONT -------
    {
        TxPacketizer p;
        // 2.5x the cap in one submit — the shape of a real AX.25 burst against
        // a queue sized for voice latency.
        const std::size_t samples = (cap * 5 / 2) / kBytesPerSample;
        const std::size_t submitted = samples * kBytesPerSample;
        p.submit(tone(samples));

        check(p.pendingBytes() == cap,
              "the queue is left holding exactly its cap");
        // The arithmetic, not the counter's own word for it.
        check(p.droppedBytes() == submitted - cap,
              "droppedBytes equals submitted minus retained");
        check(p.dropEvents() == 1,
              "one oversized submit is ONE overflow event, not thousands");
    }

    // ---- events count submits, bytes accumulate ----------------------------
    {
        TxPacketizer p;
        const std::size_t samples = (cap * 2) / kBytesPerSample;
        p.submit(tone(samples));
        const std::size_t afterFirst = p.droppedBytes();
        p.submit(tone(samples));

        check(p.dropEvents() == 2, "a second overflowing submit is a second event");
        check(p.droppedBytes() > afterFirst,
              "and droppedBytes is cumulative across submits");
        check(p.pendingBytes() == cap, "the cap still bounds the queue");
    }

    // ---- reset clears both counters, not just one --------------------------
    {
        TxPacketizer p;
        p.submit(tone((cap * 2) / kBytesPerSample));
        check(p.droppedBytes() > 0 && p.dropEvents() > 0,
              "counters are non-zero before the reset");
        p.resetDropCounters();
        check(p.droppedBytes() == 0, "resetDropCounters clears the byte total");
        check(p.dropEvents() == 0, "and clears the event count");
    }

    // ---- draining does not disturb the accounting --------------------------
    {
        TxPacketizer p;
        p.submit(tone((cap * 2) / kBytesPerSample));
        const std::size_t droppedBeforeDrain = p.droppedBytes();
        while (!p.takeFrame().empty()) {
        }
        check(p.droppedBytes() == droppedBeforeDrain,
              "draining the queue does not reset the drop total");
    }

    if (g_failures == 0)
        std::printf("icom_tx_packetiser_drop_test: all checks passed\n");
    else
        std::printf("icom_tx_packetiser_drop_test: %d FAILED\n", g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
