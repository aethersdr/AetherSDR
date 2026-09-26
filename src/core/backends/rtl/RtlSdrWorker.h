#pragma once

#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlDcBlocker.h"

#include <QThread>
#include <QVector>
#include <atomic>
#include <complex>
#include <memory>
#include <optional>

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
    RtlReceivePipeline::Diagnostics diagnostics() const { return m_pipeline->diagnostics(); }
    bool needsRepair() const { return m_pipeline->needsRepair(); }
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
    RtlSdrDdc m_ddc;
    RtlDcBlocker m_dcBlocker;
    bool m_dcSuppression = false;
    QVector<std::complex<float>> m_iqBuffer;
};
} // namespace AetherSDR::rtl
