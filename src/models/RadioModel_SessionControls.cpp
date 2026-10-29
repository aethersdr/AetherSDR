#include "RadioModel.h"

#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>
#include <utility>

namespace AetherSDR {

void RadioModel::resetSessionControls()
{
    m_sessionCommands.retireSession();
    const auto cancelled = std::exchange(m_sessionCancellations, {});
    for (const auto& cancel : cancelled) { cancel(); }
}

RadioControlFence RadioModel::sessionControlFence() const
{
    const QPointer<const RadioModel> model(this);
    const RadioControlSession session = controlSession();
    const SessionCommandGuard guard = m_sessionCommands;
    return [model, session, guard] {
        return model && guard.current() && model->controlSessionCurrent(session) && model->sessionTransportCurrent();
    };
}

std::function<void()> RadioModel::sessionContinuation(std::function<void()> action) const
{
    const RadioControlFence current = sessionControlFence();
    return [current, action = std::move(action)] { if (current() && action) { action(); } };
}

ReceiveDispatch RadioModel::requestSessionControl(const SessionControlRequest& request,
                                                 const SessionControlCompletion& completion)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const auto capability = backendCapabilities().sessionControl;
    if (!m_backend || !controlSessionCurrent(session) || !sessionTransportCurrent() || !m_sessionCommands.current()
        || !capability || !capability->supports(request.field)
        || (request.field == SessionControlField::DisconnectClient
            && (!request.value || request.value == clientHandle()))
        || m_sessionCancellations.size() >= 128) {
        if (completion) {
            completion({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Session operation not dispatched")}});
        }
        return ReceiveDispatch::Unsupported;
    }
    const QPointer<RadioModel> model(this);
    const SessionCommandGuard guard = m_sessionCommands.request();
    const quint64 id = ++m_sessionRequestId;
    SessionControlCompletion reply;
    if (completion) {
        const auto finished = std::make_shared<bool>(false);
        const QPointer<QTimer> timer(new QTimer(this));
        timer->setSingleShot(true);
        reply = [model, session, guard, id, finished, timer, completion](const SessionControlReply& response) {
            if (std::exchange(*finished, true)) { return; }
            if (timer) { timer->stop(); timer->deleteLater(); }
            if (!model) { return; }
            model->m_sessionCancellations.remove(id);
            SessionControlReply result = response;
            if (!model->controlSessionCurrent(session) || !model->sessionTransportCurrent() || !model->m_sessionCommands.current()) {
                result = {{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original session ended")}};
            }
            guard.cancel();
            completion(result);
        };
        m_sessionCancellations.insert(id, [reply] {
            reply({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original session ended")}});
        });
        connect(timer, &QTimer::timeout, this, [reply] {
            reply({{RadioControlReply::Kind::Failed, -1, QStringLiteral("No session response")}});
        });
        timer->start(30000);
    }
    const ReceiveDispatch dispatched = m_backend->requestSessionControl(request, guard, reply);
    if (dispatched != ReceiveDispatch::Dispatched && reply) {
        reply({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Session operation not dispatched")}});
    }
    return dispatched;
}

void RadioModel::requestStartupSetup(const RadioSetupRequest& request)
{
    // Reuse the backend encoder without invoking the settings-edit/persistence
    // adapter. The effective identity and path MTU belong to this connection.
    if (request.field != RadioSetupField::ClientStation && request.field != RadioSetupField::NetworkMtu) { return; }
    const RadioControlSession session = controlSession();
    if (!m_backend || !m_sessionCommands.current() || !controlSessionCurrent(session)) { return; }
    const QPointer<RadioModel> model(this);
    const SessionCommandGuard guard = m_sessionCommands;
    m_backend->requestRadioSetup(request, {}, [model, session, guard] {
        return model && guard.current() && model->controlSessionCurrent(session) && model->sessionTransportCurrent();
    });
}

} // namespace AetherSDR
