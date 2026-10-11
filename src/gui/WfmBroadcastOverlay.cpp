#include "WfmBroadcastOverlay.h"
#include "WfmPresentationSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <algorithm>
#include <cmath>

namespace AetherSDR {
WfmBroadcastOverlay::WfmBroadcastOverlay(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<QVector<WfmBroadcastOverlayRecord>>();
    connect(&WfmPresentationSettings::instance(), &WfmPresentationSettings::overlayEnabledChanged,
            this, &WfmBroadcastOverlay::refresh);
}
void WfmBroadcastOverlay::bind(RadioModel* model, const QString& panId)
{
    for (const auto& connection : m_modelConnections) { disconnect(connection); }
    m_modelConnections.clear();
    m_model = model;
    m_panId = panId;
    if (model) {
        m_modelConnections.append(connect(model, &RadioModel::sliceAdded,
            this, &WfmBroadcastOverlay::rebindSlices));
        m_modelConnections.append(connect(model, &RadioModel::sliceRemoved,
            this, &WfmBroadcastOverlay::rebindSlices));
        // Reconnect can re-adopt the same SliceModel without sliceAdded.
        m_modelConnections.append(connect(model, &RadioModel::slotOccupancyChanged,
            this, &WfmBroadcastOverlay::rebindSlices));
        m_modelConnections.append(connect(model, &RadioModel::connectionStateChanged,
            this, &WfmBroadcastOverlay::refresh));
        m_modelConnections.append(connect(model, &RadioModel::capabilitiesChanged,
            this, &WfmBroadcastOverlay::refresh));
        m_modelConnections.append(connect(model, &QObject::destroyed, this, [this] {
            m_model = nullptr;
            rebindSlices();
        }));
    }
    rebindSlices();
}
void WfmBroadcastOverlay::rebindSlices()
{
    for (const auto& connection : m_sliceConnections) { disconnect(connection); }
    m_sliceConnections.clear();
    if (m_model) {
        for (SliceModel* slice : m_model->slices()) {
            m_sliceConnections.append(connect(slice, &SliceModel::hdFmReceptionChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::wfmAudioModeChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::hdProgramChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::frequencyChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::modeChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::panIdChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::inCaptureChanged,
                this, &WfmBroadcastOverlay::refresh));
            m_sliceConnections.append(connect(slice, &SliceModel::externalReceiveReplacementChanged,
                this, &WfmBroadcastOverlay::refresh));
        }
    }
    refresh();
}
void WfmBroadcastOverlay::refresh()
{
    QVector<WfmBroadcastOverlayRecord> next;
    const RadioCapabilities caps = m_model ? m_model->backendCapabilities() : RadioCapabilities{};
    if (m_model && m_model->isConnected() && caps.broadcastFmReceive
        && caps.broadcastFmReceive->hdStereo
        && WfmPresentationSettings::instance().broadcastOverlayEnabled()) {
        for (const SliceModel* slice : m_model->slices()) {
            if (next.size() >= 8) { break; }
            if (m_model->slice(slice->sliceId()) != slice || slice->panId() != m_panId
                || slice->mode() != QLatin1String("WFM") || !slice->inCapture()
                || slice->externalReceiveReplacementActive()
                || slice->wfmAudioMode() != WfmAudioMode::HdStereo
                || !slice->frequencyReportedKnown()) { continue; }
            const HdFmReception& value = slice->hdFmReception();
            if (!value.valid || !value.synced || value.selectedProgram != slice->hdProgram()
                || value.selectedProgram < 0 || value.selectedProgram > 7
                || value.frequencyHz != std::llround(slice->reportedFrequency() * 1.0e6)
                || (value.stationName.isEmpty() && value.title.isEmpty() && value.artist.isEmpty())) { continue; }
            next.append({slice->sliceId(), value.sessionId, value.receiverEpoch, value.revision,
                value.frequencyHz, value.selectedProgram, value.stationName, value.title, value.artist});
        }
    }
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.sliceId < b.sliceId; });
    if (next == m_records) { return; }
    m_records = next;
    emit overlaysChanged(m_records);
}
}
