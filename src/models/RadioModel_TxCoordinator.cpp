#include "RadioModel.h"
#include "core/LogManager.h"

#include <chrono>

namespace AetherSDR {

qint64 RadioModel::txMonotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool RadioModel::beginLocalTxActivity(TxActivity activity)
{
    if (m_txSessionClosing) {
        emitInterlockNotification(tr("Transmit is unavailable while the radio disconnects."),
                                  QStringLiteral("tx-session-closing"));
        return false;
    }
    if (!m_backend || !refuseKeyOnTransmitIncapableBackend()
        || !refuseKeyInReceiveOnlyMode()) {
        return false;
    }
    const RadioCapabilities caps = backendCapabilities();
    if ((activity == TxActivity::Cwx && !caps.hasRadioSideCwKeyer)
        || (activity == TxActivity::Atu && !caps.hasTuner)) {
        emitInterlockNotification(tr("This radio does not support the requested transmit operation."),
                                  QStringLiteral("tx-operation-unsupported"));
        return false;
    }
    const QString gate = activity == TxActivity::Tune || activity == TxActivity::Atu
        ? QStringLiteral("tune-start")
        : activity == TxActivity::Mox ? QStringLiteral("xmit") : QStringLiteral("cw-key");
    if (transmitStartBlockedByInhibit(gate)) {
        return false;
    }
    const TxCoordinator::Admission admission = m_txCoordinator.acquire(m_desktopTxActor, txMonotonicMs());
    if (!admission.accepted()) {
        // One message per reason. Only Recovering is reachable while a single
        // desktop actor exists, but the others become reachable as soon as a
        // per-client actor does, and "cleanup is in progress" would then be a
        // wrong explanation rather than a vague one.
        QString message;
        QString key;
        switch (admission.refusal) {
        case TxCoordinator::Refusal::Recovering:
            message = tr("Transmit cleanup is still in progress.");
            key = QStringLiteral("tx-coordinator-recovering");
            break;
        case TxCoordinator::Refusal::Busy:
            message = tr("Another client is transmitting.");
            key = QStringLiteral("tx-coordinator-busy");
            break;
        case TxCoordinator::Refusal::Denied:
            message = tr("This client is not permitted to transmit.");
            key = QStringLiteral("tx-coordinator-denied");
            break;
        case TxCoordinator::Refusal::InvalidActor:
        case TxCoordinator::Refusal::WrongThread:
        case TxCoordinator::Refusal::None:
            message = tr("Transmit is unavailable.");
            key = QStringLiteral("tx-coordinator-unavailable");
            break;
        }
        // m_txSessionClosing short-circuits above, so a Recovering refusal here
        // is a stop that was never acknowledged rather than a normal disconnect
        // gap. That is a permanent admission latch, so say so instead of
        // leaving it to be diagnosed from a silent refusal. See
        // TxCoordinator::acknowledgeStopped().
        if (admission.refusal == TxCoordinator::Refusal::Recovering) {
            qCWarning(lcProtocol)
                << "RadioModel: TX refused — coordinator stop is unacknowledged;"
                << "admission stays closed until the session ends";
        }
        emitInterlockNotification(message, key);
        return false;
    }
    m_txOperation = admission.operation;
    m_txActivities |= static_cast<unsigned>(activity);
    return true;
}

void RadioModel::endLocalTxActivity(TxActivity activity)
{
    m_txActivities &= ~static_cast<unsigned>(activity);
    if (m_txActivities == 0) {
        // Existing desktop sequencers explicitly end their local intent. This
        // fences pending work; it is NOT a claim that the radio is observed RX.
        // Do not use this compatibility completion to authorize another
        // client's TX. Per-client admission/readback is the next Stage 4 step.
        (void)m_txCoordinator.complete(m_txOperation);
    }
}

void RadioModel::stopTxOperation(const TxCoordinator::Operation& operation,
                               TxCoordinator::StopReason reason)
{
    Q_UNUSED(reason);
    // TxCoordinator has already invalidated the keying fence and entered
    // recovery. All cleanup below is key-up/bypass/abort; never re-admit it.
    m_transmitModel.cancelPttRelease();
    const unsigned activities = m_txActivities;
    if (activities & static_cast<unsigned>(TxActivity::Cwx)) {
        m_cwxModel.clearBuffer();
    }
    if (activities & static_cast<unsigned>(TxActivity::CwKey)) {
        sendCwKey(false);
    }
    if (activities & static_cast<unsigned>(TxActivity::CwPtt)) {
        sendCwPtt(false);
    }
    if (activities & static_cast<unsigned>(TxActivity::Tune)) {
        m_transmitModel.stopTune();
    }
    if (activities & static_cast<unsigned>(TxActivity::Atu)) {
        m_transmitModel.atuBypass();
    }
    if (activities != 0) {
        setTransmit(false);
    }
    m_txActivities = 0;
    m_txOperation = operation;
    // No acknowledgment here: queued stop commands are not stopped-radio
    // evidence. The lifecycle caller acknowledges only after transport loss.
}

void RadioModel::resetTxOperations()
{
    // Even an idle coordinator must refuse new intent while the session dies.
    // Close admission BEFORE cancellation/reply/model notifications can reenter
    // us; operation recovery alone only covers a previously active operation.
    m_txSessionClosing = true;
    m_cwInputSession.fetch_add(1, std::memory_order_release);
    m_cwInputNotBefore = std::chrono::steady_clock::now();
    m_transmitModel.cancelPttRelease();
    m_txCoordinator.reset();
    m_cwxModel.resetDrainWatch();
    m_txActivities = 0;
}

void RadioModel::queueCwKeyEdge(bool down, const QString& source, quint64 traceId,
                              quint64 sourceMs, std::chrono::steady_clock::time_point scheduledAt)
{
    const quint64 session = m_cwInputSession.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(this, [this, session, down, source, traceId, sourceMs, scheduledAt] {
        if (session != m_cwInputSession.load(std::memory_order_acquire)
            || m_txSessionClosing || scheduledAt < m_cwInputNotBefore) {
            return;
        }
        sendCwKeyEdge(down, source, traceId, sourceMs, scheduledAt);
    }, Qt::QueuedConnection);
}

} // namespace AetherSDR
