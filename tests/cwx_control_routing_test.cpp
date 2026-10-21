// Socket-free typed keyer, real Flex encoder and callback-lifetime regressions.
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "models/CwxModel.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexCwxControls.h"

#include <QCoreApplication>
#include <QPointer>
#include <QStringList>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) { ++failures; }
}

void encodingAndValidation()
{
    using Field = CwTextRequest::Field;
    const std::vector<std::pair<CwTextRequest, QByteArray>> cases{
        {{Field::Send, 0, QStringLiteral("CQ \"DX\" = + ( & $"), 17},
         QByteArray("cwx send \"") + QByteArray::fromHex("43517f1e44581e7f3d7f2b7f287f267f24") + "\" 17"},
        {{Field::SendMacro, 12}, "cwx macro send 12"},
        {{Field::SaveMacro, 0, QStringLiteral("CQ TEST")}, QByteArray("cwx macro save 1 \"CQ\x7f" "TEST\"")},
        {{Field::SaveMacro, 11, {}}, "cwx macro save 12 \"\""},
        {{Field::Speed, 5}, "cwx wpm 5"}, {{Field::Speed, 100}, "cwx wpm 100"},
        {{Field::Delay, 0}, "cwx delay 0"}, {{Field::Delay, 2000}, "cwx delay 2000"},
        {{Field::Qsk, 1}, "cwx qsk_enabled 1"}, {{Field::Qsk, 0}, "cwx qsk_enabled 0"},
        {{Field::Erase, 1}, "cwx erase 1"}, {{Field::Clear}, "cwx clear"}};
    for (const auto& [request, wire] : cases) {
        check(request.valid() && FlexCwxControls::encode(request).toUtf8() == wire,
              "published Flex command bytes and macro-slot boundaries are preserved");
    }
    const std::vector<CwTextRequest> invalid{
        {Field::Send, 0, "CQ", 0}, {Field::Send, 0, "CQ\nCW", 1},
        {Field::Send, 0, QString(QChar(0x1e)), 1}, {Field::Send, 0, QString(QChar(0x7f)), 1},
        {Field::SendMacro, 0}, {Field::SendMacro, 13}, {Field::SaveMacro, -1, "CQ"},
        {Field::SaveMacro, 12, "CQ"}, {Field::Speed, 4}, {Field::Speed, 101},
        {Field::Delay, -1}, {Field::Delay, 2001}, {Field::Qsk, 2}, {Field::Erase, -1},
        {Field::Clear, 1}, {static_cast<Field>(999)}};
    for (const CwTextRequest& request : invalid) {
        check(!request.valid() && FlexCwxControls::encode(request).isEmpty(),
              "invalid register, slot, framing character or logical block is refused");
    }
    check(FlexCwxControls::decodeReply(0, "100,17", 17).startIndex == 100,
          "reply index is normalized independently of submitted character count");
    for (const QString& reply : {QString(), QStringLiteral("1"), QStringLiteral("-1,17"),
             QStringLiteral("1,18"), QStringLiteral("1,17,4"), QStringLiteral("2147483648,17"),
             QString(1000, QLatin1Char('1'))}) {
        check(FlexCwxControls::decodeReply(0, reply, 17).kind == CwTextReply::Kind::Invalid,
              "malformed, mismatched and oversized replies never report a queued block");
    }
}

void backendAdmissionAndReplies()
{
    TxTestAuthority authority;
    FlexBackend backend;
    check(backend.capabilities().cwTextControl
              && backend.capabilities().cwTextControl->supports(CwTextRequest::Field::Clear)
              && !RadioCapabilities{}.cwTextControl,
          "buffered keyer support is explicit and omission defaults to absent");
    const CwTextRequest send{CwTextRequest::Field::Send, 0, "CQ", 1};
    const TxCoordinator::Command key{authority.operation, true};
    check(backend.requestCwTextControl(send, key, [] { return true; }) == ReceiveDispatch::Unsupported,
          "a missing production sink cannot report successful dispatch");
    int writes = 0;
    std::vector<FlexBackend::CommandReply> replies;
    backend.setCwxCommandSink([&](const QString&, const TxCoordinator::Command& command,
                                  const RadioControlFence&, FlexBackend::CommandReply reply) {
        ++writes;
        command.completion.finish();
        replies.push_back(std::move(reply));
        return true;
    });
    check(backend.requestCwTextControl(send, {}, [] { return true; }) == ReceiveDispatch::Unsupported
              && backend.requestCwTextControl(send, {authority.operation, false}, [] { return true; })
                  == ReceiveDispatch::Unsupported && writes == 0,
          "default authority or a cleanup label cannot authorize a text send");
    check(backend.requestCwTextControl(send, key, [] { return false; }) == ReceiveDispatch::Unsupported
              && writes == 0, "a cancelled batch never enters the backend writer");
    int queued = 0;
    bool current = true;
    check(backend.requestCwTextControl(send, key, [&] { return current; }, [&](const CwTextReply& result) {
        if (result.kind == CwTextReply::Kind::Queued) { ++queued; }
    }) == ReceiveDispatch::Dispatched && writes == 1 && queued == 0,
          "positive control reaches one writer but does not invent a radio reply");
    replies.back()(0, "40,1");
    replies.back()(0, "40,1");
    check(queued == 1, "duplicate radio callbacks settle a submitted block at most once");
    backend.requestCwTextControl(send, key, [&] { return current; }, [&](const CwTextReply&) { ++queued; });
    current = false;
    replies.back()(0, "50,1");
    check(queued == 1, "a late reply cannot observe a retired original session or batch");

    for (const bool accept : {false, true}) {
        int delivered = 0;
        backend.setCwxCommandSink([&](const QString&, const TxCoordinator::Command&,
                                      const RadioControlFence&, FlexBackend::CommandReply reply) {
            reply(0, "4,1");
            reply(0, "5,1");
            return accept;
        });
        const ReceiveDispatch result = backend.requestCwTextControl(send, key, [] { return true; },
            [&](const CwTextReply&) { ++delivered; });
        check(delivered == (accept ? 1 : 0)
                  && result == (accept ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported),
              "synchronous refusal wins over early callbacks; accepted early replies settle once");
    }
    TxCoordinator idle([](const auto&, auto) {});
    writes = 0;
    backend.setCwxCommandSink([&](const QString&, const TxCoordinator::Command&,
                                  const RadioControlFence&, FlexBackend::CommandReply) { ++writes; return true; });
    const TxCoordinator::Command setting{idle.cleanupFence(), false};
    check(backend.requestCwTextControl({CwTextRequest::Field::Speed, 30}, setting, [] { return true; })
              == ReceiveDispatch::Dispatched && writes == 1 && !idle.hasOwnership(),
          "ordinary speed configuration never acquires transmit ownership");
    check(backend.requestCwTextControl({CwTextRequest::Field::Clear}, setting, [] { return true; })
              == ReceiveDispatch::Dispatched && !idle.hasOwnership(),
          "cleanup remains available without permission to start a transmission");
    const auto owner = idle.registerActor({true, 0});
    check(idle.acquire(owner, TxCoordinator::monotonicMs()).accepted(),
          "cleanup-generation test admits its replacement owner");
    const int before = writes;
    check(backend.requestCwTextControl({CwTextRequest::Field::Clear}, setting, [] { return true; })
              == ReceiveDispatch::Unsupported && writes == before,
          "old idle cleanup cannot clear a newly admitted operation");
}

void modelIntentAndEchoFences()
{
    CwxModel model;
    int textRequests = 0;
    int notifications = 0;
    model.setRequestSender([&](const CwTextRequest& request, int, int, CwxModel::TransmissionPermit) {
        if (request.keys()) { ++textRequests; }
        return ReceiveDispatch::Dispatched;
    });
    QObject::connect(&model, &CwxModel::transmissionRequested, &model,
        [&](const QString& text, int) { ++notifications; check(text == "CQ \"DX\"", "sidetone receives plain text, not encoded wire bytes"); });
    model.send(QStringLiteral("CQ \"DX\""));
    check(textRequests == 1 && notifications == 1, "one logical text block is dispatched and announced once");
    model.setRequestSender([](const CwTextRequest&, int, int, CwxModel::TransmissionPermit) {
        return ReceiveDispatch::Unsupported;
    });
    model.send(QStringLiteral("REJECTED"));
    check(notifications == 1, "a refused send cannot announce a successful sidetone dispatch");

    CwxModel echo;
    int refusedBatches = 0;
    int falseDispatches = 0;
    QObject::connect(&echo, &CwxModel::transmissionCancelled, &echo, [&] { ++refusedBatches; });
    QObject::connect(&echo, &CwxModel::transmissionDispatched, &echo,
        [&](int, bool) { ++falseDispatches; });
    echo.setRequestSender([](const CwTextRequest&, int, int, CwxModel::TransmissionPermit) {
        return ReceiveDispatch::Unsupported;
    });
    echo.send("+CQ");
    check(refusedBatches == 1 && falseDispatches == 0,
          "a refused transient cancels the admitted batch without claiming text handoff");
    echo.applyStatus({{"wpm", "23"}});
    check(echo.speed() == 23, "a refused transient speed request cannot swallow later radio state");

    CwxModel edits;
    QStringList changes;
    CwxModel::TransmissionPermit oldSpeed;
    edits.setRequestSender([&](const CwTextRequest& request, int, int, CwxModel::TransmissionPermit current) {
        if (request.origin == CwTextRequest::Origin::Transient) { oldSpeed = std::move(current); }
        return ReceiveDispatch::Dispatched;
    });
    QObject::connect(&edits, &CwxModel::controlRequested, &edits, [&](const CwTextRequest& request) {
        if (request.field == CwTextRequest::Field::Speed) {
            changes << QString::number(request.value);
            if (request.origin == CwTextRequest::Origin::Transient) { edits.setSpeed(31); }
        }
    });
    edits.send("+CQ");
    check(edits.speed() == 31 && changes == QStringList({"23", "31", "31"}),
          "reentrant edit cancels the superseded batch and cleanup restores only the newest speed");
    check(oldSpeed && !oldSpeed(), "a superseded transient cannot survive its queued delivery fence");

    CwxModel delayed;
    std::vector<CwxModel::TransmissionPermit> speedWrites;
    delayed.setRequestSender([&](const CwTextRequest& request, int, int, CwxModel::TransmissionPermit current) {
        if (request.field == CwTextRequest::Field::Speed) { speedWrites.push_back(std::move(current)); }
        return ReceiveDispatch::Dispatched;
    });
    delayed.send("+CQ");
    check(speedWrites.size() == 2 && speedWrites[0]() && speedWrites[1](),
          "positive control retains valid queued transient and restoration fences");
    delayed.setSpeed(37);
    check(!speedWrites[0]() && !speedWrites[1](), "a newer speed edit revokes both old queued speed writes");

    CwxModel session;
    session.send("+CQ");
    session.resetDrainWatch();
    session.applyStatus({{"wpm", "23"}});
    check(session.speed() == 23, "old-session speed echoes cannot suppress replacement-session readback");

    CwxModel refused;
    refused.setRequestSender([](const CwTextRequest&, int, int, CwxModel::TransmissionPermit) {
        return ReceiveDispatch::Unsupported;
    });
    const int previousSpeed = refused.speed();
    const int previousDelay = refused.delay();
    refused.setSpeed(35);
    refused.setDelay(300);
    refused.setQsk(true);
    check(refused.speed() == previousSpeed && refused.delay() == previousDelay && !refused.qskOn(),
          "refused configuration requests cannot display unsupported values as accepted");

    CwxModel duplicate;
    CwxModel::TransmissionPermit configured;
    int configurationWrites = 0;
    duplicate.setRequestSender([&](const CwTextRequest&, int, int, CwxModel::TransmissionPermit current) {
        ++configurationWrites;
        configured = std::move(current);
        return ReceiveDispatch::Dispatched;
    });
    duplicate.setSpeed(35);
    duplicate.setSpeed(35);
    check(configurationWrites == 1 && configured(),
          "repeating the current speed does not cancel its still-queued original write");

    for (const CwTextRequest::Field field : {CwTextRequest::Field::Clear, CwTextRequest::Field::Erase}) {
        CwxModel cleanup;
        CwxModel::TransmissionPermit queuedCleanup;
        cleanup.setRequestSender([&](const CwTextRequest& request, int, int, CwxModel::TransmissionPermit current) {
            if (request.field == field) { queuedCleanup = std::move(current); }
            return ReceiveDispatch::Dispatched;
        });
        if (field == CwTextRequest::Field::Clear) { cleanup.clearBuffer(); }
        else { cleanup.erase(1); }
        check(queuedCleanup && queuedCleanup(), "original-batch cleanup has a positive delivery fence");
        cleanup.resetDrainWatch();
        check(queuedCleanup && !queuedCleanup(), "queued cleanup retires with its original batch");
    }

    int idle = 0;
    QObject::connect(&delayed, &CwxModel::queueEmpty, &delayed, [&] { ++idle; });
    const int epoch = delayed.drainEpoch();
    delayed.handleSendReply({CwTextReply::Kind::Queued, std::numeric_limits<int>::max(), 1, 0}, epoch, 2);
    check(delayed.cwxEndIndex() == -1 && delayed.drainEpoch() != epoch && idle == 0,
          "index overflow cancels only the original batch and never invents radio-idle evidence");
}

void receiverDestruction()
{
    TxTestAuthority authority;
    auto backend = std::make_unique<FlexBackend>();
    int replies = 0;
    backend->setCwxCommandSink([&](const QString&, const TxCoordinator::Command&,
                                   const RadioControlFence&, FlexBackend::CommandReply reply) {
        backend.reset();
        reply(0, "1,1");
        return true;
    });
    const ReceiveDispatch result = backend->requestCwTextControl(
        {CwTextRequest::Field::Send, 0, "CQ", 1}, {authority.operation, true}, [] { return true; },
        [&](const CwTextReply&) { ++replies; });
    check(result == ReceiveDispatch::Dispatched && !backend && replies == 0,
          "synchronous backend destruction cannot deliver into a retired receiver");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile("cwx-control-routing");
    QCoreApplication app(argc, argv);
    encodingAndValidation();
    backendAdmissionAndReplies();
    modelIntentAndEchoFences();
    receiverDestruction();
    return failures == 0 ? 0 : 1;
}
