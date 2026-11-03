// Socket-free sequence/state-machine coverage; no device peer or transport.
#include "core/backends/flex/VitaRxReorder.h"

#include <QByteArray>
#include <cstdio>
#include <vector>

using AetherSDR::VitaRxReorder;
namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}
VitaRxReorder::Key key(int index, bool valid = true)
{
    return {static_cast<quint32>(1000 + index / 100),
            static_cast<quint64>(index % 100) * 96, valid};
}
void checkLoss(int missing, bool timestamps)
{
    VitaRxReorder buffer;
    int lost = 0;
    int breaks = 0;
    int delivered = 0;
    for (int i = 0; i < 120; ++i) {
        if (i >= 32 && i < 32 + missing) {
            continue;
        }
        const VitaRxReorder::Result result = buffer.add(i & 15, key(i, timestamps), i * 10);
        lost += result.lost;
        breaks += result.breaks;
        delivered += result.count;
    }
    const VitaRxReorder::Result tail = buffer.expire(1500);
    lost += tail.lost;
    breaks += tail.breaks;
    delivered += tail.count;
    if (lost != missing || breaks != 1 || delivered != 120 - missing) {
        std::fprintf(stderr, "gap=%d timestamps=%d lost=%d breaks=%d delivered=%d\n",
                     missing, timestamps, lost, breaks, delivered);
    }
    check(lost == missing && breaks == 1 && delivered == 120 - missing,
          "real burst loss is counted exactly once, with every surviving packet delivered");
}
}
int main()
{
    for (bool timestamps : {false, true}) {
        for (int missing = 1; missing <= 15; ++missing) {
            checkLoss(missing, timestamps);
        }
        VitaRxReorder buffer;
        int lost = 0;
        int reordered = 0;
        int delivered = 0;
        for (int i = 0; i < 200; ++i) {
            const int source = i == 15 ? 16 : i == 16 ? 15 : i;
            const VitaRxReorder::Result result = buffer.add(source & 15,
                key(source, timestamps), i * 10, QByteArray::number(source));
            lost += result.lost;
            reordered += result.reordered ? 1 : 0;
            for (int j = 0; j < result.count; ++j) {
                check(result.ready[j].packet.toInt() == delivered++, "release order crosses 15-to-0 wrap");
            }
        }
        check(lost == 0 && reordered == 1 && delivered == 200, "a reordered pair is not loss");
        buffer.reset();
        lost = 0;
        int breaks = 0;
        for (int i = 0; i < 100; ++i) {
            if (i == 32 || i == 49) {
                continue;
            }
            const VitaRxReorder::Result result = buffer.add(i & 15, key(i, timestamps), i * 10);
            lost += result.lost;
            breaks += result.breaks;
        }
        check(lost == 2 && breaks == 2, "two isolated losses with adjacent modulo counts remain two gaps");
    }
    {
        VitaRxReorder buffer;
        buffer.add(0, key(0), 0);
        const auto held = buffer.add(2, key(2), 1);
        check(held.count == 0 && held.lost == 0, "a successor waits before the deadline");
        check(buffer.expire(30).count == 0, "deadline is relative to the gap's first arrival");
        const auto expired = buffer.expire(31);
        check(expired.count == 1 && expired.lost == 1 && expired.breaks == 1,
              "timer expiry releases a surviving tail even if no further packet arrives");
        const auto late = buffer.add(1, key(1), 32);
        check(late.count == 0 && late.lost == 0 && late.reordered,
              "a packet arriving after the playout deadline is not decoded again");
        const auto duplicate = buffer.add(2, key(2), 33);
        check(duplicate.count == 0 && duplicate.duplicate && duplicate.lost == 0,
              "duplicate last delivered timestamp is ignored");
    }
    {
        VitaRxReorder buffer;
        int delivered = 0;
        int lost = 0;
        for (int i = 0; i < 500; ++i) {
            if (i == 32) {
                continue;
            }
            const auto result = buffer.add(i & 15, key(i), i / 4);
            delivered += result.count;
            lost += result.lost;
        }
        const auto tail = buffer.expire(1000);
        check(delivered + tail.count == 499 && lost + tail.lost == 1,
              "a fast display stream can wrap before the wall-clock deadline");
    }
    {
        VitaRxReorder buffer;
        check(buffer.add(-1, {}, 0).rejected, "negative sequence is refused");
        check(buffer.add(16, {}, 0).rejected, "sequence outside the wire field is refused");
        check(buffer.add(0, {}, 0, QByteArray(VitaRxReorder::kMaxPacketBytes + 1, 'x')).rejected,
              "oversized retained packet is refused");
        buffer.add(0, key(0), 0, QByteArray("first"));
        buffer.add(2, key(2), 1, QByteArray("held"));
        check(buffer.queuedBytes() == 4, "only queued payload bytes are retained");
        buffer.reset();
        check(buffer.queuedBytes() == 0 && buffer.deadlineMs() < 0
              && buffer.expire(1000).count == 0, "reset retires the queue and its deadline");
        check(buffer.add(9, key(9), 1001).count == 1, "a reused stream begins without a false gap");
    }
    {
        VitaRxReorder buffer;
        buffer.add(0, {1, 1, true, true}, 0);
        check(buffer.add(1, {1, 1, true, true}, 10).count == 1,
              "non-unique Opus timestamps fall back to sequence ordering");
        buffer.reset();
        buffer.add(0, {1, 1, true}, 0);
        const auto repeatedFragment = buffer.add(1, {1, 1, true}, 10);
        check(repeatedFragment.duplicate && repeatedFragment.count == 0,
              "a repeated display fragment does not disable its unique frame key");
    }
    std::printf("vita_rx_reorder: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
