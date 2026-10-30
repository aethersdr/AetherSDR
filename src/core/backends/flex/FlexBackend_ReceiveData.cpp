#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexReceiveData.h"
#include <QPointer>
#include <QThread>
#include <type_traits>

namespace AetherSDR {

void FlexBackend::initializeReceiveData()
{
    m_receiveData = std::make_shared<FlexReceiveData>([this](quint64 generation) {
        // close() synchronizes with this event-posting callback before teardown.
        QMetaObject::invokeMethod(this, [this, generation] { drainReceiveData(generation); },
                                  Qt::QueuedConnection);
    });
}

void FlexBackend::retireReceiveData()
{
    m_receiveData->retire();
    // Traffic counters are cumulative for this transport's lifetime. Only
    // session-local observations disappear at disconnect.
    m_receiveTelemetry.lease.reset();
    m_receiveTelemetry.localEndpoint.clear();
    m_receiveTelemetry.firstPacketSeen = false;
    m_receiveTelemetry.gapMs = 0;
    m_receiveTelemetry.gapMaxMs = 0;
    m_receiveTelemetry.jitterMs = 0;
}

void FlexBackend::drainReceiveData(quint64 generation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const QVector<FlexReceiveData::Event> events = m_receiveData->take(generation);
    const QPointer<FlexBackend> owner(this);
    for (const FlexReceiveData::Event& event : events) {
        if (!owner) { return; }
        std::visit([&owner](const auto& payload) {
            if (!payload.current()) { return; }
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, ReceiveSpectrumFrame>) {
                emit owner->receiveSpectrumReady(payload);
            } else if constexpr (std::is_same_v<Payload, ReceiveWaterfallRow>) {
                emit owner->receiveWaterfallReady(payload);
            } else if constexpr (std::is_same_v<Payload, ReceiveMeterBatch>) {
                emit owner->receiveMetersReady(payload);
            } else {
                owner->m_receiveTelemetry = payload;
                emit owner->receiveTelemetryReady(payload);
            }
        }, event);
    }
}

} // namespace AetherSDR
