#pragma once

#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlDcBlocker.h"

#include <QThread>
#include <QVector>
#include <array>
#include <atomic>
#include <complex>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

struct rtlsdr_dev;

namespace AetherSDR::rtl {

// Owns the USB handle until readAsync has exited. Exactly one immutable command
// crosses to the acquisition context. The backend polls the acknowledgment
// before reusing its storage; no mutex, allocation or destruction for receiver
// reconfiguration occurs in the sample callback.
class RtlSdrWorker : public QThread {
    Q_OBJECT
public:
    using Transaction = RtlCaptureTransaction;
    class Device : public Transaction::DeviceOperations {
    public:
        using Callback = void (*)(unsigned char*, std::uint32_t, void*);
        virtual bool resetBuffer() = 0;
        virtual int readAsync(Callback callback, void* context) = 0;
        virtual void cancelAsync() = 0; // only cross-thread device operation
    };

    explicit RtlSdrWorker(struct rtlsdr_dev* dev, QObject* parent = nullptr, std::size_t capacity = 1);
    explicit RtlSdrWorker(std::unique_ptr<Device> device, QObject* parent = nullptr, std::size_t capacity = 1);
    ~RtlSdrWorker() override;
    void startReading();
    bool stopReading();
    bool isReading() const { return m_readerRunning.load(); }
    RtlSdrDdc* ddc() { return &m_ddc; }

    // Backend-thread calls. One in-flight work item, enforced by the mailbox.
    bool submit(const Transaction::Work& work);
    std::optional<Transaction::Result> takeResult();
    void serviceCancellation();
    bool takeAudio(RtlReceivePipeline::Packet& packet) { return m_pipeline->takePacket(packet); }
    bool takeHdObservation(RtlReceivePipeline::HdFmObservation& value) { return m_pipeline->takeHdObservation(value); }
    // Independently sampled counters since construction. Timing covers each
    // valid callback's adoption and processing, not USB transport latency.
    // The final bucket is unbounded; finite percentile bounds are conservative.
    static constexpr std::array<std::uint64_t, 16> kCallbackDurationUpperBoundsNs{
        50'000, 100'000, 200'000, 400'000, 800'000, 1'200'000, 1'600'000, 2'000'000,
        2'400'000, 2'800'000, 3'400'000, 5'000'000, 10'000'000, 20'000'000,
        50'000'000, std::numeric_limits<std::uint64_t>::max()};
    RtlReceivePipeline::Diagnostics diagnostics() const;
    bool takeTraceEvent(RtlReceivePipeline::TraceEvent& event) { return m_pipeline->takeTraceEvent(event); }
    bool needsRepair() const { return m_pipeline->needsRepair(); }
    void setStartupTrace(std::shared_ptr<RtlStartupTrace> trace)
    { m_startupTrace = std::move(trace); m_pipeline->setStartupTrace(m_startupTrace.get()); }
    std::uint64_t startupDroppedPackets() const { return m_pipeline->droppedPackets(); }
    std::uint64_t startupQueuedPackets() const { return m_pipeline->startupQueuedPackets(); }
    void setMonitor(int slot, int gain, int pan, bool mute) { m_pipeline->setMonitor(slot, gain, pan, mute); }

signals:
    void readError(const QString& message);
    // Stamp at production, never infer identity on delivery to the backend.
    void spectrumFrameReady(quint64 session, quint64 revision, int panId, const QByteArray& frame);
    void waterfallRowReady(quint64 session, quint64 revision, int panId, const QByteArray& row);
    void audioFrameReady(quint64 session, quint64 revision, const QByteArray& pcm, const QByteArray& preMonitor);

protected:
    void run() override;

private:
    enum class Command { Idle, Preparing, Receiver, Hardware, Applying, Complete };
    static void rtlsdrCallback(unsigned char* buf, std::uint32_t len, void* ctx);
    void handleCallback(unsigned char* buf, std::uint32_t len);
    void applyDdc(const Transaction::State& state);
    void applyHardware();
    bool prepareHardwareResult();
    void cancelReading();
    void recordCallback(std::uint64_t durationNs, std::uint32_t iqSamples) noexcept;

    std::shared_ptr<RtlStartupTrace> m_startupTrace;
    std::unique_ptr<Device> m_device;
    std::atomic<bool> m_readerRunning{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<Command> m_command{Command::Idle};
    // Written before publishing Receiver/Hardware, read until Complete. Not
    // touched by the backend again until it acquires Complete in takeResult().
    std::optional<Transaction::Work> m_work;
    std::optional<Transaction::Result> m_result;
    bool m_preparationSubmitted = false; // backend thread only
    unsigned m_prepareAttempts = 0;
    Transaction::Token m_applied; // acquisition-context only
    bool m_legacyReceiving = false; // acquisition-context only
    int m_legacyFilterLowHz = 0;
    int m_legacyFilterHighHz = 0;
    std::unique_ptr<RtlReceivePipeline> m_pipeline;
    std::uint64_t m_firstSample = 0;
    std::uint32_t m_captureRateHz = 0; // acquisition-context only
    std::atomic<std::uint64_t> m_callbackCount{0};
    std::atomic<std::uint64_t> m_callbackIqSamples{0};
    std::atomic<std::uint64_t> m_malformedCallbacks{0};
    std::atomic<std::uint64_t> m_callbackTotalNs{0};
    std::atomic<std::uint64_t> m_callbackMaxNs{0};
    std::atomic<std::uint64_t> m_callbackDeadlineMisses{0};
    std::array<std::atomic<std::uint64_t>, kCallbackDurationUpperBoundsNs.size()> m_callbackDurationBuckets{};
    std::atomic<std::uint64_t> m_usbReadStarts{0};
    std::atomic<std::uint64_t> m_usbCancelRequests{0};
    RtlSdrDdc m_ddc;
    RtlDcBlocker m_dcBlocker;
    bool m_dcSuppression = false;
    QVector<std::complex<float>> m_iqBuffer;
};
} // namespace AetherSDR::rtl
