#include "RadioModel.h"

#include <QPointer>
#include <QThread>
#include <QTimer>

namespace AetherSDR {

TransmitServiceCompletion RadioModel::transmitServiceCompletion(
    const TransmitServiceCompletion& completion, const std::shared_ptr<std::atomic_bool>& current)
{
    const QPointer<RadioModel> model(this);
    const RadioControlSession session = controlSession();
    const auto connection = std::make_shared<QMetaObject::Connection>();
    const TransmitServiceCompletion reply = [model, session, current, connection, completion]
        (const TransmitServiceReply& result) {
        if (!current->exchange(false, std::memory_order_acq_rel)) { return; }
        QObject::disconnect(*connection);
        if (!model) { return; }
        if (!model->controlSessionCurrent(session)) {
            completion({TransmitServiceReply::Outcome::Unknown, 0,
                        QStringLiteral("The original radio session ended; execution is unknown.")});
        } else {
            completion(result);
        }
    };
    *connection = connect(this, &RadioModel::connectionStateChanged, this, [model, session, reply] {
        if (!model || !model->controlSessionCurrent(session)) {
            reply({TransmitServiceReply::Outcome::Unknown, 0, {}});
        }
    });
    QTimer::singleShot(30000, this, [reply] {
        reply({TransmitServiceReply::Outcome::Unknown, 0,
               QStringLiteral("No radio response; execution is unknown.")});
    });
    return reply;
}

void RadioModel::dispatchDvk(const DvkRequest& request, quint64 requestId)
{
    if (QThread::currentThread() != thread() || !m_dvkModel.requestCurrent(requestId)) { return; }
    const QPointer<RadioModel> model(this);
    const RadioControlSession session = controlSession();
    const auto pending = std::make_shared<std::atomic_bool>(true);
    const auto sessionAlive = m_transmitServiceSession;
    const RadioControlFence current = [pending, sessionAlive] {
        return pending->load(std::memory_order_acquire) && sessionAlive->load(std::memory_order_acquire);
    };
    const TransmitServiceCompletion reply = transmitServiceCompletion(
        [model, requestId](const TransmitServiceReply& result) {
            if (model) { model->m_dvkModel.handleControlReply(requestId, result); }
        }, pending);
    const auto refused = [reply](const QString& reason) {
        reply({TransmitServiceReply::Outcome::NotDispatched, 0, reason});
    };
    if (!m_backend || !controlSessionCurrent(session) || !request.valid()
        || !backendCapabilities().hasVoiceKeyer) {
        refused(tr("Radio voice keyer is unavailable."));
        return;
    }
    // Recording must not turn microphone input into unintended VOX transmit.
    if (request.operation == DvkRequest::Operation::RecordStart
        && (!m_transmitModel.voxStateConfirmed() || m_transmitModel.voxEnable())) {
        refused(tr("Confirm VOX is off before recording on the radio."));
        return;
    }
    TxCoordinator::Command command;
    if (request.operation == DvkRequest::Operation::PlaybackStart) {
        if (!beginLocalTxActivity(TxActivity::VoiceKeyer)) {
            if (model) { refused(tr("Voice playback was not admitted for transmission.")); }
            return;
        }
        if (!model || !controlSessionCurrent(session) || !m_dvkModel.requestCurrent(requestId)) { return; }
        m_dvkTxRequest = requestId;
        m_dvkTxIntent = m_localTxIntents.value(TxActivity::VoiceKeyer);
        const TxCoordinator::Intent intent = m_dvkTxIntent;
        command.operation = m_txOperation.withKeyingPermit([intent, current] {
            return current() && intent.permitsDispatch(txMonotonicMs());
        });
    }
    if (m_backend->requestDvk(request, reply, command, current) == ReceiveDispatch::Unsupported) {
        refused(tr("Voice keyer request was not dispatched."));
    }
}

void RadioModel::dispatchNavtex(const NavtexRequest& request, quint64 requestId)
{
    if (QThread::currentThread() != thread() || !m_navtexModel.requestCurrent(requestId)) { return; }
    const QPointer<RadioModel> model(this);
    const RadioControlSession session = controlSession();
    const auto pending = std::make_shared<std::atomic_bool>(true);
    const auto sessionAlive = m_transmitServiceSession;
    const RadioControlFence current = [pending, sessionAlive] {
        return pending->load(std::memory_order_acquire) && sessionAlive->load(std::memory_order_acquire);
    };
    const TransmitServiceCompletion reply = transmitServiceCompletion(
        [model, requestId](const TransmitServiceReply& result) {
            if (model) { model->m_navtexModel.handleSendResponse(requestId, result); }
        }, pending);
    if (!m_backend || !controlSessionCurrent(session) || !request.valid()
        || m_navtexModel.status() != NavtexStatus::Active || m_navtexTxRequest
        || !beginLocalTxActivity(TxActivity::Navtex)) {
        if (model) { reply({TransmitServiceReply::Outcome::NotDispatched, 0,
                            tr("NAVTEX submission was not admitted for transmission.")}); }
        return;
    }
    if (!model || !controlSessionCurrent(session) || !m_navtexModel.requestCurrent(requestId)) { return; }
    m_navtexTxRequest = requestId;
    m_navtexTxIntent = m_localTxIntents.value(TxActivity::Navtex);
    const TxCoordinator::Intent intent = m_navtexTxIntent;
    TxCoordinator::Command command;
    command.operation = m_txOperation.withKeyingPermit([intent, current] {
        return current() && intent.permitsDispatch(txMonotonicMs());
    });
    if (m_backend->requestNavtex(request, reply, command) == ReceiveDispatch::Unsupported) {
        reply({TransmitServiceReply::Outcome::NotDispatched, 0, tr("NAVTEX submission was not dispatched.")});
    }
}

} // namespace AetherSDR
