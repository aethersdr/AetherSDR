#include "models/RadioModel.h"
#include <QPointer>

namespace AetherSDR {

std::optional<ReceiveTelemetry> RadioModel::backendReceiveTelemetry() const
{
    return m_backend ? m_backend->receiveTelemetry() : std::nullopt;
}

void RadioModel::wireBackendReceiveData()
{
    const quint64 generation = m_backendReceiverGeneration;
    const QPointer<RadioModel> model(this);
    connect(m_backend.get(), &IRadioBackend::receiveSpectrumReady, this,
            [model, generation](const ReceiveSpectrumFrame& frame) {
        if (!model || generation != model->m_backendReceiverGeneration || !frame.current()) { return; }
        emit model->panFeedSpectrumReady(frame.streamId, frame.binsDbm, frame.emittedNs, frame.decodeScale);
    });
    connect(m_backend.get(), &IRadioBackend::receiveWaterfallReady, this,
            [model, generation](const ReceiveWaterfallRow& row) {
        if (!model || generation != model->m_backendReceiverGeneration || !row.current()) { return; }
        emit model->panFeedWaterfallAutoBlackLevel(row.streamId, row.autoBlack);
        if (!model || generation != model->m_backendReceiverGeneration || !row.current()) { return; }
        emit model->panFeedWaterfallRowReady(row.streamId, row.intensity, row.lowMhz,
                                           row.highMhz, row.timecode, row.emittedNs);
    });
    connect(m_backend.get(), &IRadioBackend::receiveMetersReady, this,
            [model, generation](const ReceiveMeterBatch& batch) {
        if (!model || generation != model->m_backendReceiverGeneration || !batch.current()) { return; }
        model->m_meterModel.updateReceivedValues(batch);
    });
}

} // namespace AetherSDR
