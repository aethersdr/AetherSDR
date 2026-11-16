#include "core/LogManager.h"
#include "FlexBackend.h"
#include "FlexLanSession.h"
#include "WanConnection.h"
#include "FlexProfileWritePolicy.h"
#include "core/PerfTelemetry.h"

#include <QDateTime>
#include <QThread>
#include <memory>
#include <utility>

namespace AetherSDR {
namespace {
constexpr int kNotDispatchedCode = 0x50000063;
FlexCommandSession::Completion commandReply(FlexBackend::CommandReply reply)
{
    if (!reply) { return {}; }
    return [reply = std::move(reply)](const FlexCommandSession::Reply& result) {
        reply(result.kind == FlexCommandSession::ReplyKind::NotDispatched && result.code == -1
                  ? kNotDispatchedCode : result.code, result.body);
    };
}
FlexCommandSession::Options guardedOptions(const RadioControlFence& current)
{
    FlexCommandSession::Options options;
    if (current.writeGuard()) {
        options.guard = *current.writeGuard();
        options.guarded = true;
    }
    return options;
}
}

FlexCommandSession& FlexBackend::commandSession()
{
    if (m_commandsForTest) { return *m_commandsForTest; }
    return m_commandWan ? m_commandWan->commands() : m_lanSession->commands();
}

quint64 FlexBackend::bindWanCommandSession(WanConnection* connection)
{
    m_commandWan = connection;
    m_profileWriteHoldUtcMs = 0;
    return m_statusIngress.bindLive(connection);
}

bool FlexBackend::dispatchWire(const QString& command, CommandReply reply,
                              const FlexCommandSession::Options& options)
{
    return dispatchCommand(command, options, commandReply(std::move(reply)));
}

bool FlexBackend::dispatchControl(const QString& command, const RadioControlCompletion& completion,
                                 const FlexCommandSession::Options& options, RadioControlReply::Kind refusal)
{
    FlexCommandSession::Completion reply;
    if (completion) {
        reply = [completion, refusal](const FlexCommandSession::Reply& result) {
            using Kind = RadioControlReply::Kind;
            const Kind kind = result.kind == FlexCommandSession::ReplyKind::NotDispatched ? Kind::Cancelled
                : result.kind == FlexCommandSession::ReplyKind::Unknown ? Kind::Failed
                : result.code == 0 ? Kind::Accepted : refusal;
            completion({kind, result.code, result.body});
        };
    }
    return dispatchCommand(command, options, std::move(reply));
}

bool FlexBackend::dispatchCommand(const QString& command, const FlexCommandSession::Options& options,
                                  FlexCommandSession::Completion reply)
{
    if (QThread::currentThread() != thread()
        || !(m_commandsForTest ? m_commandsForTest->active() : isConnected())) { return false; }
    if (QDateTime::currentMSecsSinceEpoch() < m_profileWriteHoldUtcMs
        && isProfileOwnedRadioStateWrite(command)) {
        if (command.startsWith(QStringLiteral("display pan set "))
            && (command.contains(QStringLiteral("center=")) || command.contains(QStringLiteral("bandwidth="))
                || command.contains(QStringLiteral(" band=")))) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: DROPPED a routed pan field write during profile load —"
                << "this should have been deferred via requestPan*()" << command;
        } else {
            qCDebug(lcProtocol).noquote() << "RadioModel: suppressing profile-load radio-state write" << command;
        }
        if (reply) {
            reply({FlexCommandSession::ReplyKind::NotDispatched, kProfileLoadSuppressedCommandCode,
                   QStringLiteral("Suppressed during profile load.")});
        }
        return false;
    }
    if (PerfTelemetry::instance().enabled() && command.startsWith(QStringLiteral("display pan set "))
        && command.contains(QStringLiteral(" center="))) {
        PerfTelemetry::instance().recordPanCenterCommand();
    }
    return commandSession().submit(command, options, std::move(reply)) != 0;
}

void FlexBackend::initializeCommands()
{
    m_sink = [this](const QString& command, CommandReply reply) {
        return dispatchWire(command, std::move(reply));
    };
    m_answeredSink = [this](const QString& command, const std::function<void(bool)>& answer) {
        const auto answered = std::make_shared<bool>(false);
        const auto reply = [answered, answer](int code, const QString&) {
            if (!std::exchange(*answered, true) && answer) { answer(code == 0); }
        };
        if (!dispatchWire(command, reply)) { reply(kNotDispatchedCode, {}); }
    };
    m_setupSink = [this](const QString& command, const RadioSetupCompletion& completion) {
        return dispatchControl(command, completion);
    };
    m_setupEntitySink = [this](const QString& command, const RadioSetupCompletion& completion,
                              const std::shared_ptr<SetupStatusLifetime>& identity) {
        FlexCommandSession::Options options;
        options.permitsWrite = [identity] { return !identity || identity->current(); };
        return dispatchControl(command, completion, options);
    };
    m_audioSink = [this](const QString& command, CommandReply reply, const AudioControlGuard& guard) {
        FlexCommandSession::Options options;
        options.permitsWrite = [guard] { return guard.permits(); };
        return dispatchWire(command, std::move(reply), options);
    };
    m_daxTxSink = [this](const QString& command, CommandReply reply, const DaxTxRequestGuard& guard) {
        FlexCommandSession::Options options;
        options.permitsWrite = [guard] { return guard.permits(); };
        return dispatchWire(command, std::move(reply), options);
    };
    m_daxTxClientHandleProvider = [this] {
        const auto session = connectionSession();
        return m_commandWan ? m_commandWan->clientHandle() : session ? session->clientHandle : 0;
    };
    m_sessionSink = [this](const QString& command, const SessionCommandGuard& guard,
                          const RadioControlCompletion& completion) {
        FlexCommandSession::Options options;
        options.guard = guard;
        options.guarded = true;
        return dispatchControl(command, completion, options, RadioControlReply::Kind::Refused);
    };
    m_cwControls->setControlSink(m_sessionSink);
    m_transferSink = [this](const QString& command, const FileTransferGuard& guard,
                           const RadioControlCompletion& completion) {
        FlexCommandSession::Options options;
        options.permitsWrite = [guard] { return guard.current(); };
        return dispatchControl(command, completion, options);
    };
    m_serviceCommandSink = [this](const QString& command, const RadioControlFence& current, CommandReply reply) {
        if (!current || !current()) { return false; }
        FlexCommandSession::Options options = guardedOptions(current);
        // Service fences are immutable atomic lifetimes, including the owned
        // TUNE restore whose normal session-command guard is already retired.
        if (current) { options.permitsWrite = [current] { return current(); }; }
        options.cleanup = current.cleanup();
        return dispatchWire(command, std::move(reply), options);
    };
    m_administrationSink = [this](const QString& command, const RadioControlCompletion& completion,
        const RadioControlFence& current, const std::function<void()>& written) {
        if (!current || !current()) { return false; }
        FlexCommandSession::Options options = guardedOptions(current);
        options.written = [current, written](FlexCommandSession::WriteResult result) {
            if (result == FlexCommandSession::WriteResult::Written && current() && written) { written(); }
        };
        return dispatchControl(command, completion, options);
    };
    m_cwxSink = [this](const QString& command, const TxCoordinator::Command& fence,
                      const RadioControlFence& current, CommandReply reply) {
        if (current && !current()) { return false; }
        FlexCommandSession::Options options;
        options.operation = fence.operation;
        options.tx = true;
        options.keying = fence.keying;
        options.cleanup = !fence.keying;
        if (current) { options.permitsWrite = [current] { return current(); }; }
        options.consumed = [completion = fence.completion] { completion.finish(); };
        const bool accepted = dispatchWire(command, std::move(reply), options);
        if (!accepted) { fence.completion.finish(); }
        return accepted;
    };
    m_txSink = [this](const QString& command, const TxCoordinator::Command& fence) {
        return m_cwxSink(command, fence, {}, {});
    };
    m_cwControls->setTxSink(m_txSink);
    m_repliedTxSink = [this](const QString& command, const TxCoordinator::Command& fence, CommandReply reply) {
        return m_cwxSink(command, fence, {}, std::move(reply));
    };
    m_cwxConfigurationSink = [this](const QString& command, const RadioControlFence& current) {
        if (current && !current()) { return false; }
        FlexCommandSession::Options options;
        if (current) { options.permitsWrite = [current] { return current(); }; }
        return dispatchWire(command, {}, options);
    };
    m_sliceSink = {}; // Slice verbs share the default command route.
    m_sequenceProvider = [this] { return commandSession().allocateSequence(); };
}

} // namespace AetherSDR
