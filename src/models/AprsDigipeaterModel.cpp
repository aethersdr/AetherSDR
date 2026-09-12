#include "models/AprsDigipeaterModel.h"

namespace AetherSDR {
AprsDigipeaterModel::AprsDigipeaterModel(QObject* parent) : QObject(parent)
{
    connect(&m_beacon, &AprsBeacon::transmitFrame, this,
            [this](const QByteArray& raw) { enqueue(raw, true); });
    connect(&m_beacon, &AprsBeacon::activity, this, &AprsDigipeaterModel::activity);
}

void AprsDigipeaterModel::setEnabled(bool on)
{
    on = on && m_baud == 1200 && m_engine.myAddress().isValid();
    const bool wasEnabled = m_enabled;
    m_enabled = on;
    if (!on) {
        m_beacon.setEnabled(false);
        m_queue.removeIf([](const PendingFrame& frame) { return frame.digi; });
        if (wasEnabled) {
            emit disarmed();
        }
    }
}

void AprsDigipeaterModel::setBaud(int baud)
{
    m_baud = baud;
    if (baud != 1200) {
        setEnabled(false);
    }
}

void AprsDigipeaterModel::setMyAddress(const ax25::Address& address)
{
    if (!(address == m_engine.myAddress())) {
        setEnabled(false);
    }
    m_engine.setMyAddress(address);
    m_beacon.setMyAddress(address);
}

void AprsDigipeaterModel::enqueue(const QByteArray& raw, bool digi)
{
    if (raw.isEmpty() || (digi && !m_enabled)) {
        return;
    }
    if (m_queue.size() >= kMaxQueueDepth) {
        m_queue.dequeue();
        emit activity(QStringLiteral("Modem TX queue full; dropping oldest pending frame."));
    }
    m_queue.enqueue({raw, digi});
    emit queued();
}

void AprsDigipeaterModel::receiveFrame(const QByteArray& raw)
{
    const auto frame = ax25::Frame::decode(raw);
    if (!frame || frame->type != ax25::FrameType::UI || frame->info.isEmpty()) {
        return;
    }
    emit heard(frame->src.toString(), AprsFillInDigipeater::tnc2(*frame));
    if (!m_enabled) {
        return;
    }
    const auto decision = m_engine.consider(*frame);
    if (decision.outgoing) {
        enqueue(decision.outgoing->encode(), true);
        emit repeated(AprsFillInDigipeater::tnc2(*decision.outgoing));
    } else if (decision.drop == AprsFillInDigipeater::Drop::Duplicate
               || decision.drop == AprsFillInDigipeater::Drop::NoAliasMatch) {
        emit dropped();
    }
}
} // namespace AetherSDR
