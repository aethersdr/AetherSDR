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
#include "TxTestAuthority.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::icom;

namespace AetherSDR::icom {
struct IcomStreamTestAccess {
    static void writer(IcomStream& stream, std::function<void(std::span<const std::uint8_t>)> write)
    {
        stream.m_testWriter = std::move(write);
    }
    static void retransmit(IcomStream& stream, quint16 sequence)
    {
        stream.handleRetransmitRequest(buildRetransmitRequest(0, 0, sequence));
    }
};
}

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

    // Inject the terminal writer, not a peer: exercise real tracked sends and
    // retransmission dispatch without binding a socket or creating a session.
    {
        TxTestAuthority authority;
        IcomStream stream;
        std::vector<std::vector<std::uint8_t>> writes;
        IcomStreamTestAccess::writer(stream, [&](std::span<const std::uint8_t> packet) {
            writes.emplace_back(packet.begin(), packet.end());
        });
        const auto audio = buildAudio(0, 0, 0, 0, payload);
        stream.sendTrackedTxAudio(audio, authority.context);
        check(writes.size() == 1 && isAudioData(writes.back()), "authorized audio reaches the writer");
        (void)authority.coordinator.finishLocalIntent(authority.operation);
        const auto next = authority.coordinator.acquire(authority.actor, AetherSDR::TxCoordinator::monotonicMs()).operation;
        const auto context = authority.coordinator.mediaContext(authority.producer, next);
        stream.sendTrackedTxAudio(audio, context);
        IcomStreamTestAccess::retransmit(stream, 0);
        check(writes.size() == 3 && !isAudioData(writes.back()) && parseHeader(writes.back()).seq == 0,
              "old operation replay becomes sequence-preserving idle, never new-operation audio");
        IcomStreamTestAccess::retransmit(stream, 1);
        check(writes.size() == 4 && isAudioData(writes.back()), "current audio remains retransmittable");
        authority.producer.invalidate();
        IcomStreamTestAccess::retransmit(stream, 1);
        check(writes.size() == 5 && !isAudioData(writes.back()), "producer teardown also fences retained audio");
        stream.sendTrackedTxAudio(audio, {});
        check(writes.size() == 5, "unowned audio never enters the writer or replay cache");
    }
    {
        TxTestAuthority authority;
        IcomStream stream;
        IcomStreamTestAccess::writer(stream, [&](std::span<const std::uint8_t>) {
            check(authority.coordinator.hasInFlightDispatches(), "terminal audio write remains counted during reentrancy");
            (void)authority.coordinator.cancel(authority.actor, authority.operation);
            check(!authority.coordinator.acknowledgeStopped(authority.operation),
                  "reentrant cancellation cannot acknowledge an entered audio write");
        });
        stream.sendTrackedTxAudio(buildAudio(0, 0, 0, 0, payload), authority.context);
        check(authority.coordinator.acknowledgeStopped(authority.operation),
              "entered audio write releases its guard on return");
    }

    {
        TxTestAuthority authority;
        IcomStream stream;
        std::vector<std::vector<std::uint8_t>> writes;
        int consumed = 0;
        IcomStreamTestAccess::writer(stream, [&](std::span<const std::uint8_t> packet) {
            check(authority.coordinator.hasInFlightDispatches(),
                  "a terminal CI-V command write remains counted through the writer");
            writes.emplace_back(packet.begin(), packet.end());
        });
        const auto packet = buildSerialData(0, 0, 0, 0, payload);
        const AetherSDR::TxCoordinator::Completion completion([&] {
            check(!authority.coordinator.hasInFlightDispatches(),
                  "queue completion follows the command's terminal writer return");
            ++consumed;
        });
        stream.sendTrackedTxCommand(packet, {authority.operation, true, completion});
        check(writes.size() == 1 && writes.back().size() == packet.size() && consumed == 1,
              "an admitted command is written and completed exactly once");
        (void)authority.coordinator.finishLocalIntent(authority.operation);
        // Invalid replay emits protocol Idle without keying authority. Replace
        // the writer's entered-command assertion for these non-TX keepalives.
        IcomStreamTestAccess::writer(stream, [&](std::span<const std::uint8_t> bytes) {
            writes.emplace_back(bytes.begin(), bytes.end());
        });
        IcomStreamTestAccess::retransmit(stream, 0);
        check(writes.back().size() == kHeaderSize && parseHeader(writes.back()).seq == 0,
              "a normally completed operation cannot replay its old key-on");
        stream.sendTrackedTxCommand(packet, {authority.operation, false});
        check(writes.back().size() == packet.size(), "matching cleanup survives normal local completion");
        const auto fresh = authority.coordinator.acquire(
            authority.actor, AetherSDR::TxCoordinator::monotonicMs()).operation;
        IcomStreamTestAccess::retransmit(stream, 1);
        check(writes.back().size() == kHeaderSize && parseHeader(writes.back()).seq == 1,
              "old cleanup replay cannot unkey a new operation");
        stream.sendTrackedTxCommand(packet, {fresh, true});
        IcomStreamTestAccess::retransmit(stream, 2);
        check(writes.back().size() == packet.size() && consumed == 1,
              "current command replay preserves authority and never repeats queue completion");
        const auto before = writes.size();
        stream.sendTrackedTxCommand(packet, {});
        check(writes.size() == before, "a default command has no authority");
    }

    {
        TxTestAuthority authority;
        IcomStream stream;
        std::vector<std::vector<std::uint8_t>> writes;
        IcomStreamTestAccess::writer(stream, [&](std::span<const std::uint8_t> packet) {
            writes.emplace_back(packet.begin(), packet.end());
        });
        using Command = AetherSDR::TxCoordinator::Command;
        using Group = Command::ReplayGroup;
        const auto packet = buildSerialData(0, 0, 0, 0, payload);
        stream.sendTrackedTxCommand(packet, {authority.operation, true, {}, Group::Keying});
        stream.sendTrackedTxCommand(packet, {authority.operation, false, {}, Group::Keying});
        IcomStreamTestAccess::retransmit(stream, 0);
        check(writes.back().size() == kHeaderSize, "key-off supersedes old retained key-on within a shared operation");
        IcomStreamTestAccess::retransmit(stream, 1);
        check(writes.back().size() == packet.size(), "latest key-off remains retryable");
        stream.sendTrackedTxCommand(packet, {authority.operation, true, {}, Group::Keying});
        IcomStreamTestAccess::retransmit(stream, 1);
        check(writes.back().size() == kHeaderSize, "old unkey cannot replay over a newer same-operation contributor");

        const auto batch = std::make_shared<std::atomic<bool>>(true);
        const auto operation = authority.operation.withKeyingPermit([batch] { return batch->load(); });
        stream.sendTrackedTxCommand(packet, {operation, true, {}, Group::CwText});
        stream.sendTrackedTxCommand(packet, {operation, true, {}, Group::CwText});
        IcomStreamTestAccess::retransmit(stream, 3);
        check(writes.back().size() == packet.size(), "new CW chunks do not discard preceding chunks in their batch");
        batch->store(false);
        IcomStreamTestAccess::retransmit(stream, 4);
        check(writes.back().size() == kHeaderSize
                  && authority.operation.permitsDispatch(AetherSDR::TxCoordinator::monotonicMs()),
              "CW batch cancellation fences replay while unrelated MOX keeps its operation alive");
        stream.sendTrackedTxCommand(packet, {authority.operation, false, {}, Group::CwText});
        stream.sendTrackedTxCommand(packet, {authority.operation, true, {}, Group::CwText});
        IcomStreamTestAccess::retransmit(stream, 5);
        check(writes.back().size() == kHeaderSize, "a new CW batch supersedes an old retained abort");
        IcomStreamTestAccess::retransmit(stream, 2);
        check(writes.back().size() == packet.size(), "CW replay changes do not erase a separate current MOX command");
    }

    if (g_failures == 0)
        std::printf("icom_replay_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
