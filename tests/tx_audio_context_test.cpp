#include "core/AudioEngine.h"
#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QVector>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* claim)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", claim);
    failures += !condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // No capture device, socket binding, radio peer or terminal writer. Exercise
    // the real AudioEngine packetizer through injected PCM and collect signals.
    AudioEngine audio;
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto producer = coordinator.registerProducer();
    const auto first = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const auto oldContext = coordinator.mediaContext(producer, first);
    QVector<QByteArray> packets;
    QVector<TxCoordinator::Context> contexts;
    QObject::connect(&audio, &AudioEngine::txPacketReady,
                     [&](const QByteArray& packet, const TxCoordinator::Context& context) {
        packets.append(packet);
        contexts.append(context);
    });
    audio.setTxStreamId(0x1234);
    audio.setTransmitting(true);
    constexpr int kHalfPacketBytes = 64 * 2 * sizeof(float);
    const QByteArray oldHalf(kHalfPacketBytes, '\x3f');
    const QByteArray newHalf(kHalfPacketBytes, '\0');
    audio.sendModemTxAudio(oldHalf, oldContext);
    check(packets.isEmpty(), "partial old modem frame remains buffered");
    (void)coordinator.finishLocalIntent(first);
    const auto second = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const auto current = coordinator.mediaContext(producer, second);
    audio.sendModemTxAudio(newHalf, current);
    audio.sendModemTxAudio(oldHalf, oldContext);
    audio.discardTxMedia(oldContext);
    audio.sendModemTxAudio(newHalf, current);
    check(packets.size() == 1 && packets.first().right(2 * kHalfPacketBytes) == newHalf + newHalf,
          "new audio neither inherits old residue nor loses its partial frame to stale cleanup");
    check(contexts.size() == 1 && contexts.first().sameContext(current),
          "packet retains the original producer and operation across the transport signal");
    audio.sendModemTxAudio(newHalf + newHalf, {});
    check(packets.size() == 1, "unowned modem audio never reaches transport");
    (void)coordinator.cancel(actor, second);
    check(!contexts.first().beginDispatch(TxCoordinator::monotonicMs()),
          "cancellation after packetization still fences the terminal writer");

    int monitorFrames = 0;
    int transportFrames = 0;
    QObject::connect(&audio, &AudioEngine::txFinalMonitorPcmReady,
                     [&](const QByteArray&, bool) { ++monitorFrames; });
    QObject::connect(&audio, &AudioEngine::txTransportPcmReady,
                     [&](const QByteArray&, bool, const TxCoordinator::Context&) { ++transportFrames; });
    audio.setHostModulation(true);
    audio.feedDaxTxAudio(newHalf, current);
    check(monitorFrames == 1 && transportFrames == 0,
          "a local monitor tap is independent of cancelled transport authority");
    (void)coordinator.acknowledgeStopped(second);
    const auto third = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const auto live = coordinator.mediaContext(producer, third);
    audio.feedDaxTxAudio(newHalf, live);
    check(monitorFrames == 2 && transportFrames == 1,
          "valid producer audio reaches both local monitoring and the host-modulation seam");
    producer.invalidate();
    audio.sendModemTxAudio(newHalf, live);
    check(monitorFrames == 3 && transportFrames == 1,
          "producer destruction fences modem transport without removing its local tap");
    return failures ? 1 : 0;
}
