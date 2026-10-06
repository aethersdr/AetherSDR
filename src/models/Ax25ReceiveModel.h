#pragma once

#include "DecoderAudioModel.h"
#include "core/tnc/AetherAx25LibmodemShim.h"

#include <QObject>
#include <atomic>
#include <memory>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Retained by queued consumers as well as the model. An earlier listener may
// synchronously change the source, so each outward consumer checks this again.
struct Ax25ReceiveContext {
    PcmEpochLease source;
    quint64 generation = 0;
    std::shared_ptr<const std::atomic<quint64>> activeGeneration;
    bool current() const
    {
        return source.current() && activeGeneration
            && activeGeneration->load() == generation;
    }
};

// Selected receive audio and its existing modem worker. No sound device,
// external service or transmit authority is owned by this model.
class Ax25ReceiveModel final : public QObject {
    Q_OBJECT
public:
    explicit Ax25ReceiveModel(RadioModel& radio, QObject* parent = nullptr);
    ~Ax25ReceiveModel() override;

    void setSlice(SliceModel* slice);
    void setEnabled(bool enabled);
    void configure(const Ax25DemodConfig& config);
    void reset();
    void setDiagnosticsLoggingEnabled(bool enabled);
    bool isEnabled() const;
    DecoderAudioModel::RouteStatus routeStatus() const;

signals:
    void frameDecoded(const AetherSDR::Ax25DecodedFrame& frame,
                      const AetherSDR::Ax25ReceiveContext& context);
    void diagnosticsUpdated(const AetherSDR::Ax25DecoderDiagnostics& diagnostics,
                            const AetherSDR::Ax25ReceiveContext& context);
    void statusChanged();
    void routeStatusChanged();
    void sourceReset();
    // Capture observes the same admitted continuous mono24 source as the modem.
    void pcmReady(const AetherSDR::DecoderPcmBlock& block,
                  const AetherSDR::Ax25ReceiveContext& context);

private:
    friend struct Ax25ReceiveModelTestAccess;
    struct QueueState {
        qsizetype inputFrames;
        std::size_t inputBlocks;
        std::size_t outputBlocks;
        bool workerScheduled;
        bool outputScheduled;
    };
    // Read-only private seam for deterministic bounded-queue tests. Barriers
    // live entirely in the test; production scheduling never waits on them.
    QObject* workerForTest() const;
    QueueState queuesForTest() const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Ax25ReceiveContext)
