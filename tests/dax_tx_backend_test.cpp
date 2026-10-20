// Production decoding and command/reply injection: no transport is connected,
// no listener is bound, and no radio, device, peer or RF is used.
#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"

#include <QCoreApplication>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace AetherSDR;

namespace {
constexpr quint32 kOurs = 0x12345678;
constexpr quint32 kForeign = 0x87654321;
constexpr quint32 kStream = 0x4a000001;
int failures = 0;

void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}

struct Outcome {
    int calls{0};
    bool accepted{false};
    QString reason;
};

DaxTxOwnershipCompletion completion(Outcome& outcome)
{
    return [&outcome](bool accepted, const QString& reason) {
        ++outcome.calls;
        outcome.accepted = accepted;
        outcome.reason = reason;
    };
}

struct Fixture {
    quint32 handle{kOurs};
    QStringList commands;
    std::vector<FlexBackend::CommandReply> replies;
    std::vector<DaxTxRequestGuard> guards;
    std::vector<DaxTxStreamDelta> observations;
    FlexBackend backend;

    Fixture()
    {
        backend.setDaxTxClientHandleProvider([this] { return handle; });
        backend.setDaxTxCommandSink([this](const QString& command, FlexBackend::CommandReply reply,
                                          const DaxTxRequestGuard& guard) {
            commands.append(command);
            replies.push_back(std::move(reply));
            guards.push_back(guard);
            return true;
        });
        QObject::connect(&backend, &IRadioBackend::daxTxStreamChanged, &backend,
            [this](const DaxTxStreamDelta& delta) { observations.push_back(delta); });
    }

    void status(quint32 id = kStream, quint32 owner = kOurs, bool tx = false)
    {
        backend.decodeDaxTxStatus(QStringLiteral("stream 0x%1").arg(id, 0, 16),
            {{QStringLiteral("type"), QStringLiteral("dax_tx")},
             {QStringLiteral("client_handle"), QStringLiteral("0x%1").arg(owner, 0, 16)},
             {QStringLiteral("tx"), tx ? QStringLiteral("1") : QStringLiteral("0")}});
    }

    ReceiveDispatch request(Outcome& outcome, bool take = true, quint32 id = kStream,
                           DaxTxRequestGuard guard = DaxTxRequestGuard::create(5000))
    {
        return backend.requestDaxTxOwnership({id, take, std::move(guard)}, completion(outcome));
    }
};

void exactCommandsAndObservations()
{
    Fixture f;
    f.status();
    Outcome take;
    check(f.request(take) == ReceiveDispatch::Dispatched
        && f.commands == QStringList{QStringLiteral("stream set 0x4a000001 tx=1")},
        "take encodes only the exact client-stream selection command");
    f.replies[0](0, {});
    f.replies[0](0, {});
    check(take.calls == 1 && take.accepted && take.reason.isEmpty(),
        "acceptance is delivered at most once");
    check(f.observations.size() == 1 && f.observations.back().tx == false,
        "ACK before status does not invent selected state");

    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("1")}});
    Outcome release;
    check(f.request(release, false) == ReceiveDispatch::Dispatched
        && f.commands.back() == QStringLiteral("stream set 0x4a000001 tx=0"),
        "release encodes only the exact client-stream yield command");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("0")}});
    f.replies[1](0, {});
    check(release.calls == 1 && release.accepted && f.observations.size() == 3
        && f.observations.back().tx == false,
        "status before ACK stays authoritative without an extra ACK observation");

    Outcome refused;
    check(f.request(refused) == ReceiveDispatch::Dispatched,
        "a valid stream may request another take after release");
    f.replies[2](0x50000001, QStringLiteral("busy"));
    check(refused.calls == 1 && !refused.accepted && refused.reason.contains(QStringLiteral("busy"))
        && f.observations.size() == 3 && f.observations.back().tx == false,
        "radio refusal reports its reason and preserves the latest observation");
}

void strictObjectsAndDeltas()
{
    Fixture f;
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("1")}});
    check(f.observations.empty(), "a typeless unknown stream cannot establish DAX identity");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
        {{QStringLiteral("type"), QStringLiteral("dax_rx")},
         {QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
         {QStringLiteral("tx"), QStringLiteral("1")}});
    check(f.observations.empty(), "another stream type cannot establish DAX TX identity");
    for (const QString& object : {QStringLiteral("stream 0"), QStringLiteral("stream 0x0"),
            QStringLiteral("stream 0x100000000"), QStringLiteral("stream 4294967296"),
            QStringLiteral("stream -1"), QStringLiteral("stream +1"),
            QStringLiteral("stream 0x4a000001 suffix"), QStringLiteral("stream 0x4a000001\n"),
            QStringLiteral("stream 0x4a00000g"), QStringLiteral("tx_audio_stream 0x")}) {
        f.backend.decodeDaxTxStatus(object,
            {{QStringLiteral("type"), QStringLiteral("dax_tx")},
             {QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
             {QStringLiteral("tx"), QStringLiteral("1")}});
    }
    check(f.observations.empty() && f.commands.isEmpty(),
        "malformed, zero and overflowing stream identities emit no observation or command");

    f.backend.decodeDaxTxStatus(QStringLiteral("stream 1241513985"),
        {{QStringLiteral("type"), QStringLiteral("dax_tx")},
         {QStringLiteral("client_handle"), QStringLiteral("305419896")},
         {QStringLiteral("tx"), QStringLiteral("0")}});
    check(f.observations.size() == 1 && f.observations.back().streamId == kStream
        && f.observations.back().clientHandle == kOurs && f.observations.back().tx == false,
        "published decimal stream and client identities are accepted");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0X4A000001"),
                              {{QStringLiteral("tx"), QStringLiteral("1")}});
    check(f.observations.size() == 2 && !f.observations.back().clientHandle
        && f.observations.back().tx == true,
        "known modern stream accepts a strict tx-only delta without inventing an owner field");
    f.backend.decodeDaxTxStatus(QStringLiteral("tx_audio_stream 4a000002"),
        {{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}});
    check(f.observations.size() == 3 && f.observations.back().streamId == kStream + 1
        && !f.observations.back().tx,
        "legacy status and bare hexadecimal IDs preserve an omitted tx field");
    Outcome legacyUnknown;
    check(f.request(legacyUnknown, true, kStream + 1) == ReceiveDispatch::Unsupported
        && legacyUnknown.calls == 1 && !legacyUnknown.accepted,
        "an owned legacy stream with unknown tx cannot be mutated");
}

void identityAndMalformedFields()
{
    Fixture f;
    f.status();
    f.status(kStream + 1, kForeign, true);
    Outcome foreign;
    Outcome unknown;
    Outcome zero;
    check(f.request(foreign, true, kStream + 1) == ReceiveDispatch::Unsupported
        && f.request(unknown, true, kStream + 2) == ReceiveDispatch::Unsupported
        && f.request(zero, true, 0) == ReceiveDispatch::Unsupported && f.commands.isEmpty(),
        "foreign, unknown and zero stream requests never reach the command sink");
    f.handle = 0;
    Outcome unidentified;
    check(f.request(unidentified) == ReceiveDispatch::Unsupported && unidentified.calls == 1,
        "an unknown local client handle cannot select a stream");
    f.handle = kOurs;
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000003"),
        {{QStringLiteral("type"), QStringLiteral("dax_tx")},
         {QStringLiteral("tx"), QStringLiteral("1")}});
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000003"),
        {{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}});
    Outcome missingOwner;
    check(f.request(missingOwner, true, kStream + 2) == ReceiveDispatch::Unsupported,
        "a tx bit observed before a valid owner cannot authorize an owner-only update");

    for (const QString& value : {QString(), QStringLiteral("true"), QStringLiteral("01"),
            QStringLiteral("-1"), QStringLiteral("2"), QStringLiteral("1 ")}) {
        f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                                  {{QStringLiteral("tx"), value}});
        Outcome malformed;
        check(f.observations.back().invalidTx && !f.observations.back().tx
            && f.request(malformed) == ReceiveDispatch::Unsupported,
            "malformed explicit tx revokes the cached observation instead of becoming false");
    }
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("0")}});
    for (const QString& owner : {QString(), QStringLiteral("0"), QStringLiteral("0x0"),
            QStringLiteral("0x100000000"), QStringLiteral("4294967296"),
            QStringLiteral("0x12345678junk"), QStringLiteral("+305419896"),
            QStringLiteral(" 0x12345678")}) {
        f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
            {{QStringLiteral("client_handle"), owner}, {QStringLiteral("tx"), QStringLiteral("1")}});
        Outcome malformed;
        check(f.observations.back().invalidOwner && !f.observations.back().clientHandle
            && f.request(malformed) == ReceiveDispatch::Unsupported,
            "malformed explicit owner never preserves prior local authority");
    }
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}});
    Outcome ownerOnly;
    check(f.request(ownerOnly) == ReceiveDispatch::Unsupported,
        "owner recovery cannot inherit a tx bit received with invalid ownership");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("0")}});
    Outcome recovered;
    check(f.request(recovered) == ReceiveDispatch::Dispatched,
        "fresh owner and valid tx observations allow a new explicit request");
}

void retirementAndStaleReplies()
{
    Fixture f;
    f.status();
    Outcome changedOwner;
    f.request(changedOwner);
    f.status(kStream, kForeign, true);
    f.status();
    f.replies[0](0, {});
    check(changedOwner.calls == 1 && !changedOwner.accepted,
        "owner change and return cannot revive an older command reply");

    Outcome removed;
    f.request(removed);
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001 removed"), {});
    check(f.observations.back().removed, "modern removal is emitted");
    const std::size_t retiredObservations = f.observations.size();
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("1")}});
    check(f.observations.size() == retiredObservations,
        "a partial delta cannot revive a retired stream's type or authority");
    f.status();
    f.replies[1](0, {});
    check(removed.calls == 1 && !removed.accepted,
        "removed and reused numeric stream ID rejects the retired incarnation's reply");

    Outcome invalidTx;
    f.request(invalidTx);
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("tx"), QStringLiteral("invalid")}});
    f.status();
    f.replies[2](0, {});
    check(invalidTx.calls == 1 && !invalidTx.accepted,
        "recovered malformed tx cannot revive a reply from before uncertainty");

    Outcome previousSession;
    f.request(previousSession);
    f.backend.clearExtensionHandles();
    f.status();
    f.replies[3](0, {});
    check(previousSession.calls == 1 && !previousSession.accepted,
        "session reset fences a late reply even when stream and owner IDs recur");

    Outcome changedClient;
    f.request(changedClient);
    f.handle = kForeign;
    f.replies[4](0, {});
    check(changedClient.calls == 1 && !changedClient.accepted,
        "reply cannot be accepted under a different current local client identity");

    f.backend.decodeDaxTxStatus(QStringLiteral("tx_audio_stream 0x4a000002 removed"), {});
    check(f.observations.back().removed && f.observations.back().streamId == kStream + 1,
        "unknown legacy removal is emitted for pending-create tombstone fencing");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000003"),
                              {{QStringLiteral("in_use"), QStringLiteral("0")}});
    check(f.observations.back().removed && f.observations.back().streamId == kStream + 2,
        "unknown in_use=0 removal is emitted");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000004"),
                              {{QStringLiteral("removed"), QString()}});
    check(f.observations.back().removed && f.observations.back().streamId == kStream + 3,
        "unknown bare removed-key status is emitted");
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001"),
                              {{QStringLiteral("type"), QStringLiteral("remote_audio_tx")}});
    check(f.observations.back().removed,
        "a numeric ID reclassified as another stream type retires DAX authority");
}

void sinkAndLifetimeBoundaries()
{
    Fixture f;
    f.status();
    f.backend.setDaxTxCommandSink({});
    Outcome absent;
    check(f.request(absent) == ReceiveDispatch::Unsupported && absent.calls == 1
        && !absent.accepted && !absent.reason.isEmpty(),
        "missing command transport reports immediate refusal");

    FlexBackend::CommandReply late;
    f.backend.setDaxTxCommandSink([&late](const QString&, FlexBackend::CommandReply reply,
                                       const DaxTxRequestGuard&) {
        late = reply;
        reply(0, {});
        return false;
    });
    Outcome declined;
    check(f.request(declined) == ReceiveDispatch::Unsupported && declined.calls == 1
        && !declined.accepted && !declined.reason.isEmpty(),
        "synchronous success cannot override the sink's dispatch refusal");
    late(0, {});
    check(declined.calls == 1, "late reply after dispatch refusal cannot complete twice");

    f.backend.setDaxTxCommandSink([](const QString&, FlexBackend::CommandReply reply,
                                    const DaxTxRequestGuard&) {
        reply(0, {});
        reply(0, {});
        return true;
    });
    Outcome synchronous;
    check(f.request(synchronous) == ReceiveDispatch::Dispatched && synchronous.calls == 1
        && synchronous.accepted, "synchronous duplicate replies complete a dispatched request once");

    f.backend.setDaxTxCommandSink({});
    bool genericSinkCalled = false;
    f.backend.setCommandSink([&genericSinkCalled](const QString&, FlexBackend::CommandReply) {
        genericSinkCalled = true;
        return true;
    });
    Outcome observerOnly;
    check(f.request(observerOnly) == ReceiveDispatch::Unsupported && observerOnly.calls == 1
        && !observerOnly.accepted && !genericSinkCalled,
        "generic unguarded sink cannot dispatch an ownership mutation");

    Outcome destroyed;
    {
        auto backend = std::make_unique<FlexBackend>();
        backend->setDaxTxClientHandleProvider([] { return kOurs; });
        backend->decodeDaxTxStatus(QStringLiteral("tx_audio_stream 0x4a000001"),
            {{QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
             {QStringLiteral("tx"), QStringLiteral("0")}});
        backend->setDaxTxCommandSink([&late](const QString&, FlexBackend::CommandReply reply,
                                           const DaxTxRequestGuard&) {
            late = std::move(reply);
            return true;
        });
        check(backend->requestDaxTxOwnership(
                  {kStream, true, DaxTxRequestGuard::create(5000)}, completion(destroyed))
                  == ReceiveDispatch::Dispatched, "lifetime fixture dispatches through injected transport");
    }
    late(0, {});
    check(destroyed.calls == 0, "destroyed backend's callback never reaches the replacement owner");
}

void terminalWriteGuards()
{
    check(!DaxTxRequestGuard().permits() && !DaxTxRequestGuard::create(0).permits()
        && !DaxTxRequestGuard::create(-1).permits(),
        "default and nonpositive-deadline guards are invalid");
    Fixture f;
    f.status();
    Outcome invalid;
    check(f.request(invalid, true, kStream, {}) == ReceiveDispatch::Unsupported
        && invalid.calls == 1 && !invalid.accepted && f.commands.isEmpty(),
        "a request without a live terminal-write guard is refused before dispatch");

    const DaxTxRequestGuard revoked = DaxTxRequestGuard::create(5000);
    revoked.revoke();
    Outcome revokedOutcome;
    check(f.request(revokedOutcome, true, kStream, revoked) == ReceiveDispatch::Unsupported
        && revokedOutcome.calls == 1 && !revokedOutcome.accepted && f.commands.isEmpty(),
        "a revoked request is refused at admission");
    const DaxTxRequestGuard expired = DaxTxRequestGuard::create(1);
    const DaxTxRequestGuard expiredCopy = expired;
    QThread::msleep(5);
    Outcome expiredOutcome;
    check(!expired.permits() && !expiredCopy.permits()
        && f.request(expiredOutcome, true, kStream, expired) == ReceiveDispatch::Unsupported
        && expiredOutcome.calls == 1 && f.commands.isEmpty(),
        "copies share an immutable expired deadline and cannot admit a mutation");

    const DaxTxRequestGuard original = DaxTxRequestGuard::create(5000);
    Outcome queued;
    check(f.request(queued, true, kStream, original) == ReceiveDispatch::Dispatched
        && f.guards.size() == 1 && f.guards[0].permits(),
        "the guarded sink receives a live copy of the original request guard");
    // Simulate the model's removal handler revoking a request while its command
    // is still queued, then the terminal writer consulting the captured copy.
    f.backend.decodeDaxTxStatus(QStringLiteral("stream 0x4a000001 removed"), {});
    original.revoke();
    int terminalWrites = 0;
    if (f.guards[0].permits()) {
        ++terminalWrites;
    }
    check(terminalWrites == 0 && !f.guards[0].permits(),
        "model revocation reaches the exact queued guard and prevents a stale write");
    f.status();
    f.replies[0](0, {});
    check(queued.calls == 1 && !queued.accepted,
        "a revoked queued request cannot regain acceptance when its numeric ID returns");

    const DaxTxRequestGuard replacement = DaxTxRequestGuard::create(5000);
    Outcome replacementOutcome;
    check(f.request(replacementOutcome, true, kStream, replacement) == ReceiveDispatch::Dispatched
        && f.guards[1].permits() && !f.guards[0].permits(),
        "a new request has independent admission and does not revive the old queued guard");
    f.backend.clearExtensionHandles();
    replacement.revoke();
    if (f.guards[1].permits()) {
        ++terminalWrites;
    }
    check(terminalWrites == 0, "session retirement revocation prevents another queued terminal write");
    f.replies[1](0, {});
    check(replacementOutcome.calls == 1 && !replacementOutcome.accepted,
        "session-retired guard also refuses the eventual command reply");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-dax-tx-backend-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    exactCommandsAndObservations();
    strictObjectsAndDeltas();
    identityAndMalformedFields();
    retirementAndStaleReplies();
    sinkAndLifetimeBoundaries();
    terminalWriteGuards();
    return failures ? 1 : 0;
}
