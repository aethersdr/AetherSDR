#pragma once

#include "core/DecoderPcmAdapter.h"

#include <QObject>
#include <QPointer>

#include <memory>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// The selected receiver's pre-monitor PCM, independent of the speaker stream.
// This model and its consumers live on RadioModel's thread. Source bindings
// carry a generation so a queued delivery cannot survive selection/teardown.
class DecoderAudioModel final : public QObject {
    Q_OBJECT
public:
    enum class Consumer { Cw, Rtty };
    // Bound means an input subscription exists, not that PCM has arrived.
    // SharedRxAudio is bound too: a DAX-capable radio with no channel assigned
    // to the selected slice decodes the radio's shared receive stream instead,
    // which carries every audible slice and follows speaker gain and mute.
    enum class RouteStatus { Inactive, SharedRxAudio, DaxTransportUnavailable, Bound };
    DecoderAudioModel(RadioModel& radio, Consumer consumer, QObject* parent = nullptr);
    ~DecoderAudioModel() override;

    void setSlice(SliceModel* slice);
    void setEnabled(bool enabled);
    RouteStatus routeStatus() const;

signals:
    // The same admitted source, before fixed-rate conversion. Native-rate
    // consumers retain the producer frame/lease and perform their own SRC.
    void nativePcmReady(const AetherSDR::PcmFrame& frame);
    void pcmReady(const AetherSDR::DecoderPcmBlock& block);
    void sourceReset();
    void routeStatusChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
