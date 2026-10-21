#include "FlexCwxControls.h"
#include "FlexBackend.h"

#include <QPointer>
#include <QThread>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR {

ReceiveDispatch FlexBackend::requestCwTextControl(const CwTextRequest& request,
    const TxCoordinator::Command& command, const RadioControlFence& current,
    const CwTextCompletion& completion)
{
    if (QThread::currentThread() != thread() || !request.valid() || !m_cwxSink || command.keying != request.keys()
        || (request.keys() && command.operation.independent())
        || !command.permitsDispatch(TxCoordinator::monotonicMs())
        || (current && !current())
        || (request.origin == CwTextRequest::Origin::Transient
            && !command.operation.permitsDispatch(TxCoordinator::monotonicMs()))) {
        return ReceiveDispatch::Unsupported;
    }
    const QString wire = FlexCwxControls::encode(request);
    struct ReplyState {
        enum class Dispatch { Pending, Accepted, Refused };
        Dispatch dispatch{Dispatch::Pending};
        std::optional<CwTextReply> early;
        bool answered{false};
    };
    const std::shared_ptr<ReplyState> state = std::make_shared<ReplyState>();
    const QPointer<FlexBackend> alive(this);
    const auto deliver = [state, alive, completion, current](const CwTextReply& reply) {
        if (state->answered || state->dispatch == ReplyState::Dispatch::Refused) { return; }
        if (state->dispatch == ReplyState::Dispatch::Pending) {
            if (!state->early) { state->early = reply; }
            return;
        }
        state->answered = true;
        if (alive && completion && (!current || current())) { completion(reply); }
    };
    CommandReply reply;
    if (completion) {
        reply = [deliver, block = request.block](int code, const QString& body) {
            deliver(FlexCwxControls::decodeReply(code, body, block));
        };
    }
    const auto sink = m_cwxSink;
    const bool accepted = sink(wire, command, current, std::move(reply));
    state->dispatch = accepted ? ReplyState::Dispatch::Accepted : ReplyState::Dispatch::Refused;
    if (!accepted) { return ReceiveDispatch::Unsupported; }
    if (state->early) { deliver(*state->early); }
    return ReceiveDispatch::Dispatched;
}

} // namespace AetherSDR
