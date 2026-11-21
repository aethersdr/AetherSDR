// Socket-free sequence/state-machine coverage; no device peer or transport.
#include "core/backends/flex/VitaRxReorder.h"

#include <QByteArray>
#include <cstdio>
#include <vector>
#include <utility>

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
void checkOverlappingGaps(bool timestamps)
{
    for (bool genuineLoss : {false, true}) {
        VitaRxReorder buffer;
        int lost = 0;
        int breaks = 0;
        std::vector<int> delivered;
        const auto collect = [&](const VitaRxReorder::Result& result) {
            lost += result.lost;
            breaks += result.breaks;
            for (int i = 0; i < result.count; ++i) {
                delivered.push_back(result.ready[i].packet.toInt());
            }
        };
        const auto feed = [&](int index, qint64 nowMs) {
            collect(buffer.add(index, key(index, timestamps), nowMs, QByteArray::number(index)));
        };
        feed(0, 0);
        feed(2, 20);
        feed(4, genuineLoss ? 45 : 40);
        if (!genuineLoss) {
            feed(1, 49);
        }
        collect(buffer.expire(50));
        check(buffer.deadlineMs() == (genuineLoss ? 75 : 70),
              "a newer gap keeps the deadline of its own earliest successor");
        feed(3, 59);
        collect(buffer.expire(100));
        const std::vector<int> expected = genuineLoss ? std::vector<int>{0, 2, 3, 4}
                                                    : std::vector<int>{0, 1, 2, 3, 4};
        check(lost == (genuineLoss ? 1 : 0) && breaks == (genuineLoss ? 1 : 0)
              && delivered == expected && buffer.queuedBytes() == 0,
              "overlapping reorders recover without hiding a genuine earlier loss");
    }
    VitaRxReorder buffer;
    buffer.add(0, key(0, timestamps), 0);
    buffer.add(4, key(4, timestamps), 10);
    buffer.add(2, key(2, timestamps), 20);
    buffer.add(1, key(1, timestamps), 25);
    check(buffer.deadlineMs() == 40,
          "resolving a leading gap does not extend an already evidenced later gap");
    const VitaRxReorder::Result expired = buffer.expire(40);
    check(expired.lost == 1 && expired.count == 1,
          "a later gap still expires within 30 ms of its earliest successor");
}
}
int main()
{
    for (int offset = 0; offset < 16; ++offset) {
        // A 1 ms recovery gap reuses counts 7/8/9 after 13/14/15 losses;
        // those were consumed within 30 ms and are indistinguishable from
        // stale copies. Exercise those counts once the ambiguity expires.
        for (int missing = 1; missing <= 15; ++missing) {
            VitaRxReorder buffer;
            int lost = 0;
            int breaks = 0;
            std::vector<int> delivered;
            const auto collect = [&](const VitaRxReorder::Result& result) {
                lost += result.lost;
                breaks += result.breaks;
                for (int j = 0; j < result.count; ++j) {
                    delivered.push_back(result.ready[j].packet.toInt());
                }
            };
            std::vector<int> expected;
            for (int i = 0; i < 30 + missing; ++i) {
                if (i >= 10 && i < 10 + missing) {
                    continue;
                }
                expected.push_back(i);
                const qint64 firstSurvivor = missing >= 13 ? 120 : 91;
                const qint64 now = i < 10 ? i * 10 : firstSurvivor + i - 10 - missing;
                collect(buffer.add((i + offset) & 15, {}, now, QByteArray::number(i)));
            }
            collect(buffer.expire(200));
            check(lost == missing && breaks == 1 && delivered == expected,
                  "compressed survivors after real loss are delivered and counted across every wrap");
        }
    }
    for (int offset = 0; offset < 16; ++offset) {
        for (int last = 9; last <= 16; ++last) {
            VitaRxReorder buffer;
            int lost = 0;
            std::vector<int> delivered;
            const auto feed = [&](int index, qint64 now) {
                const auto result = buffer.add((index + offset) & 15, {}, now,
                                               QByteArray::number(index));
                lost += result.lost;
                for (int i = 0; i < result.count; ++i) {
                    delivered.push_back(result.ready[i].packet.toInt());
                }
            };
            feed(0, 0);
            for (int i = 2; i < last; ++i) {
                feed(i, i - 1);
                check(lost == 0 && delivered.size() == 1,
                      "a keyless burst stays queued before its deadline and full-cycle bound");
            }
            feed(1, last - 1);
            std::vector<int> expected;
            for (int i = 0; i < last; ++i) {
                expected.push_back(i);
            }
            check(lost == 0 && delivered == expected && buffer.deadlineMs() < 0,
                  "delayed keyless PCM recovers the complete burst across every count wrap");
        }
    }
    {
        VitaRxReorder buffer;
        int lost = 0;
        int delivered = 0;
        for (int i = 0; i < 500; ++i) {
            if (i == 1) {
                continue;
            }
            const auto result = buffer.add(i & 15, {}, i / 4);
            lost += result.lost;
            delivered += result.count;
        }
        const auto tail = buffer.expire(1000);
        check(lost + tail.lost == 1 && delivered + tail.count == 499,
              "a full keyless count cycle stays bounded and resolves real loss");
        buffer.reset();
        check(buffer.add(0, {}, 0, QByteArray("a"), 0).count == 1,
              "an immediate packet needs no retained-byte budget");
        const auto refused = buffer.add(2, {}, 1, QByteArray("held"), 0);
        check(refused.rejected && refused.count == 0 && buffer.queuedBytes() == 0,
              "only packets needing retention are refused at the byte budget");
        check(buffer.add(1, {}, 2, QByteArray("b"), 0).count == 1,
              "budget refusal leaves the next in-order packet immediately deliverable");
    }
    {
        VitaRxReorder buffer;
        buffer.add(0, {}, 0);
        buffer.add(2, {}, 10);
        const auto expired = buffer.expire(40);
        check(expired.lost == 1 && expired.count == 1, "keyless deadline resolves the absent count");
        const auto late = buffer.add(1, {}, 41);
        check(late.count == 0 && late.reordered && late.lost == 0,
              "a recently expired count cannot replay late PCM");
        const auto duplicate = buffer.add(2, {}, 42);
        check(duplicate.count == 0 && duplicate.duplicate,
              "a recently delivered keyless packet cannot replay");
        check(buffer.add(3, {}, 43).count == 1, "late PCM leaves the successor live");
    }
    for (quint32 step : {1u, 60u, 61u, 100000u}) {
        for (bool pending : {false, true}) {
            VitaRxReorder buffer;
            const auto timestamp = [](quint32 seconds, quint64 fraction = 0) {
                return VitaRxReorder::Key{seconds, fraction, true, true,
                    VitaRxReorder::Key::Kind::OpusTimestamp};
            };
            buffer.add(0, timestamp(200000), 0, QByteArray("old"));
            if (pending) {
                buffer.add(2, timestamp(200001), 1, QByteArray("queued"));
            }
            const auto result = buffer.add(9, timestamp(200000 - step), 2, QByteArray("new"));
            if (step > 60) {
                check(result.count == 1 && result.ready[0].packet == "new" && result.lost == 0
                      && buffer.queuedBytes() == 0 && buffer.deadlineMs() < 0,
                      "a large Opus clock reset retires queued old audio and releases immediately");
                check(buffer.add(10, timestamp(200001 - step), 3).count == 1,
                      "Opus consumption continues on the reset clock");
            } else {
                check(result.count == 0 && result.reordered,
                      "Opus clock steps up to sixty seconds retain FlexLib stale handling");
            }
        }
    }
    {
        using Kind = VitaRxReorder::Key::Kind;
        VitaRxReorder buffer;
        buffer.add(0, {100, 500, true, true, Kind::OpusTimestamp}, 0);
        check(buffer.add(1, {40, 501, true, true, Kind::OpusTimestamp}, 1).count == 0,
              "fractional Opus time just inside sixty seconds stays stale");
        check(buffer.add(1, {40, 499, true, true, Kind::OpusTimestamp}, 2).count == 1,
              "fractional Opus time just beyond sixty seconds resynchronizes");
        buffer.reset();
        buffer.add(15, {0xffffffffu, 0, true, true, Kind::OpusTimestamp}, 0);
        check(buffer.add(0, {0, 1, true, true, Kind::OpusTimestamp}, 1).count == 1,
              "ordinary seconds wrap remains forward progress");
        buffer.reset();
        buffer.add(0, {100, 12, true, false, Kind::DisplayFrame}, 0);
        check(buffer.add(1, {92, 0, true, false, Kind::DisplayFrame}, 1).count == 0,
              "display frames within the assembler lookback remain stale");
        check(buffer.add(9, {91, 0, true, false, Kind::DisplayFrame}, 2).count == 1,
              "display counter restarts resume accounting without invented loss");
        check(buffer.add(10, {92, 0, true, false, Kind::DisplayFrame}, 3).count == 1,
              "display accounting continues after a counter reset");
    }
    for (bool timestamps : {false, true}) {
        checkOverlappingGaps(timestamps);
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
