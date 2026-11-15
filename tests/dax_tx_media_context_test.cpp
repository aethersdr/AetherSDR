// Production media contexts and packetization only: no audio device, socket,
// radio peer, terminal writer or RF is used.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/DaxTxAudioPermit.h"
#include "core/TxCoordinator.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QThread>

#include <cstdio>
#include <memory>
#include <vector>

using namespace AetherSDR;

namespace {
constexpr qint64 kNow = 100;
constexpr quint32 kStream = 0x4a000001;
int failures = 0;

void check(bool passed, const char* claim)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", claim);
    failures += !passed;
}

struct Fixture {
    TxCoordinator coordinator{[](const TxCoordinator::Operation&, TxCoordinator::StopReason) {},
                              [] { return kNow; }};
    TxCoordinator::Actor actor{coordinator.registerActor({true, 0})};
    TxCoordinator::Producer producer{coordinator.registerProducer()};
    TxCoordinator::Request request{producer.request()};
    TxCoordinator::Operation operation{coordinator.acquire(actor, kNow).operation};
    TxCoordinator::Intent intent{
        coordinator.beginRequest(request, operation, TxCoordinator::Activity::Mox)};

    TxCoordinator::Context context() const { return coordinator.mediaContext(request); }
};

void queuedPcmRetainsOriginalRestriction()
{
    Fixture f;
    AudioEngine audio;
    std::vector<QByteArray> packets;
    std::vector<TxCoordinator::Context> contexts;
    QObject::connect(&audio, &AudioEngine::txPacketReady,
        [&](const QByteArray& packet, const TxCoordinator::Context& context,
            const DaxTxAudioPermit&) {
            packets.push_back(packet);
            contexts.push_back(context);
        });
    const DaxTxAudioPermit oldPermit = DaxTxAudioPermit::confirmed(kStream);
    const TxCoordinator::Context oldContext = f.context().withMediaPermit(
        [oldPermit] { return oldPermit.permits(); });
    const TxCoordinator::Context copiedOldContext = oldContext;
    audio.setTransmitting(true);
    audio.setDaxTxAudioRoute(kStream, oldPermit);
    constexpr int kHalfPacketBytes = 64 * 2 * sizeof(float);
    const QByteArray oldHalf(kHalfPacketBytes, '\x3f');
    const QByteArray freshHalf(kHalfPacketBytes, '\0');
    audio.sendModemTxAudio(oldHalf, oldContext);
    check(packets.empty(), "old DAX PCM may have an incomplete packet before revocation");
    check(QMetaObject::invokeMethod(&audio, [&audio, oldHalf, copiedOldContext] {
        audio.sendModemTxAudio(oldHalf + oldHalf, copiedOldContext);
    }, Qt::QueuedConnection), "old PCM is queued with its original copied context");

    oldPermit.revoke();
    const DaxTxAudioPermit freshPermit = DaxTxAudioPermit::confirmed(kStream);
    const TxCoordinator::Context freshContext = f.context().withMediaPermit(
        [freshPermit] { return freshPermit.permits(); });
    audio.setDaxTxAudioRoute(kStream, freshPermit);
    audio.sendModemTxAudio(freshHalf, freshContext);
    QCoreApplication::processEvents();
    check(packets.empty() && !oldContext.permitsDispatch(kNow)
        && !copiedOldContext.beginDispatch(kNow) && freshContext.permitsDispatch(kNow),
        "same-ID reacquisition cannot admit old PCM queued before packetization");
    audio.sendModemTxAudio(freshHalf, freshContext);
    check(packets.size() == 1 && packets.front().right(2 * kHalfPacketBytes) == freshHalf + freshHalf
        && contexts.front().sameContext(freshContext),
        "fresh DAX packet contains no old accumulator residue and retains its restriction");
    check(!oldContext.sameContext(freshContext) && oldContext.sameContext(copiedOldContext),
        "restriction identity separates generations while copied queued contexts stay compatible");

    bool workerDenied = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        workerDenied = !copiedOldContext.permitsDispatch(kNow)
            && !copiedOldContext.beginDispatch(kNow) && freshContext.beginDispatch(kNow);
    }));
    worker->start();
    worker->wait();
    check(workerDenied, "transport workers read the copied atomic restriction without model access");
}

void restrictionsOnlyDeny()
{
    Fixture f;
    const TxCoordinator::Context base = f.context();
    const TxCoordinator::Context allowed = base.withMediaPermit([] { return true; });
    const TxCoordinator::Context denied = base.withMediaPermit([] { return false; });
    const TxCoordinator::Context cannotWiden = denied.withMediaPermit([] { return true; });
    const TxCoordinator::Context laterDenied = allowed.withMediaPermit([] { return false; });
    const TxCoordinator::Context missingPermit = base.withMediaPermit({});
    check(base.permitsDispatch(kNow) && allowed.permitsDispatch(kNow)
        && !denied.permitsDispatch(kNow) && !cannotWiden.permitsDispatch(kNow)
        && !laterDenied.beginDispatch(kNow) && !missingPermit.beginDispatch(kNow),
        "conjoined restrictions and an empty predicate can only deny an existing context");
    check(!TxCoordinator::Context{}.withMediaPermit([] { return true; }).beginDispatch(kNow)
        && !f.coordinator.mediaContext(f.producer).withMediaPermit([] { return true; }).permitsDispatch(kNow),
        "a permissive restriction cannot admit default or unowned producer media");
    TxCoordinator foreign([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    check(!foreign.mediaContext(f.producer, f.operation).withMediaPermit([] { return true; })
        .beginDispatch(kNow), "a media restriction cannot cross coordinator provenance");
    const TxCoordinator::Context operationDenied = f.coordinator.mediaContext(
        f.producer, f.operation.withKeyingPermit([] { return false; }))
        .withMediaPermit([] { return true; });
    check(!operationDenied.beginDispatch(kNow), "a media predicate cannot widen an operation restriction");
    check(base.sameContext(f.context()) && !base.sameContext(allowed)
        && !allowed.sameContext(base.withMediaPermit([] { return true; })),
        "each new restriction has a distinct accumulator identity even when both currently permit");
    check(f.coordinator.endIntent(f.intent) && !allowed.permitsDispatch(kNow)
        && !allowed.withMediaPermit([] { return true; }).beginDispatch(kNow),
        "ending the original producer intent cannot be undone by adding a media predicate");

    const TxCoordinator::Producer microphone = foreign.registerProducer(true);
    const TxCoordinator::Context micContext = foreign.mediaContext(microphone);
    check(micContext.permitsDispatch(kNow) && !foreign.hasOwnership()
        && !micContext.withMediaPermit([] { return false; }).beginDispatch(kNow)
        && micContext.permitsDispatch(kNow),
        "continuous microphone provenance remains independent and can also be restricted without keying");
}

void weakRequestsAndExactChildren()
{
    Fixture f;
    TxCoordinator::Request::Weak abandoned;
    {
        const TxCoordinator::Request input = f.producer.request();
        abandoned = input.weak();
        check(abandoned.lock().sameRequest(input), "weak input evidence retains exact live identity");
    }
    check(!abandoned.lock().valid(), "weak evidence never retains an abandoned preparation");
    const TxCoordinator::Request root = f.producer.request();
    const TxCoordinator::Request child = root.derive();
    const TxCoordinator::Intent childIntent = f.coordinator.beginRequest(
        child, f.operation, TxCoordinator::Activity::Mox);
    const auto children = f.coordinator.requestsFrom(root);
    check(childIntent.pending() && children.size() == 1 && children.front().sameRequest(child),
        "exact original input enumerates only its admitted child");
    (void)f.coordinator.closeRequest(root);
    check(!child.valid() && f.coordinator.requestsFrom(root).size() == 1,
        "retired original denies its child while preserving exact cleanup evidence");
    check(f.coordinator.requestsFrom(f.request).size() == 1,
        "unrelated admitted request remains distinct from another original");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-dax-tx-media-context-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    queuedPcmRetainsOriginalRestriction();
    restrictionsOnlyDeny();
    weakRequestsAndExactChildren();
    return failures ? 1 : 0;
}
