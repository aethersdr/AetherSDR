#pragma once

#include "core/backends/rtl/RtlSdrDdc.h"

#include <QObject>
#include <QThread>
#include <QVector>
#include <climits>
#include <complex>
#include <atomic>

struct rtlsdr_dev;

namespace AetherSDR::rtl {

// Runs rtlsdr_read_async() and RtlSdrDdc off the main thread (u8 I/Q → float).
//
// Deadlock prevention (§ Hazards #1): read_async blocks until cancel_async, and
// a cancel issued just before the loop starts is lost, so stopReading() repeats
// the cancel every 100 ms for up to 5 s.
//
// Thread safety (§ Hazards #2): set_center_freq / set_tuner_gain /
// set_direct_sampling are USB control transfers (~100-300 ms) that fail or
// deadlock inside the read callback. The setters store the request atomically
// and cancel read_async; run() applies it between async-read sessions.
class RtlSdrWorker : public QThread {
    Q_OBJECT

public:
    explicit RtlSdrWorker(struct rtlsdr_dev* dev, QObject* parent = nullptr);
    ~RtlSdrWorker() override;

    // Start async USB reading loop on this thread
    void startReading();

    // Safely stop reading and wait for worker thread exit. Returns false only
    // when the USB read thread did not acknowledge repeated cancellation.
    bool stopReading();

    bool isReading() const { return m_readerRunning.load(); }

    RtlSdrDdc* ddc() { return &m_ddc; }

    // Dispatch blocking USB control transfers to the worker thread.
    // Setters store requested values in atomic variables and trigger a cancel/restart
    // loop on the worker thread so control transfers are executed safely outside callback.
    void setCenterFrequency(uint32_t hz);
    void setTunerGain(int gainTenths);
    void setDirectSampling(int mode);
    void setPpmCorrection(int ppm);
    void setSampleRate(uint32_t rate);
    void setOffsetTuning(int enable);

signals:
    // Emitted on USB read error or unexpected disconnect
    void readError(const QString& message);

    // Relayed from RtlSdrDdc (cross-thread queued connection to main thread)
    void spectrumFrameReady(int panId, const QByteArray& frame);
    void audioFrameReady(const QByteArray& pcm);

    // USB control completion, emitted only after the transfer has run between
    // async-read sessions. value is read back from librtlsdr where available.
    void controlApplied(const QString& control, qint64 value);
    void controlFailed(const QString& control, const QString& message);

protected:
    void run() override;

private:
    static void rtlsdrCallback(unsigned char* buf, uint32_t len, void* ctx);
    void handleCallback(unsigned char* buf, uint32_t len);

    // Owned by this worker. The run loop closes it only after read_async exits,
    // so the backend can never close a handle still used by libusb.
    struct rtlsdr_dev* m_dev{nullptr};
    std::atomic<bool> m_readerRunning{false};
    std::atomic<bool> m_stopRequested{false};
    RtlSdrDdc m_ddc;

    // Pre-allocated IQ buffer to prevent per-callback heap allocations
    QVector<std::complex<float>> m_iqBuffer;

    // Pending USB control requests (set from any thread, applied between
    // read_async sessions in the run() loop — never inside the callback)
    std::atomic<int64_t> m_pendingCenterHz{-1};
    std::atomic<int> m_pendingGainTenths{INT_MIN};
    std::atomic<int> m_pendingDirectSampling{-1};
    std::atomic<int> m_pendingPpm{INT_MIN};
    std::atomic<int64_t> m_pendingSampleRate{-1};
    std::atomic<int> m_pendingOffsetTuning{-1};

    // Set by setters to signal the callback to cancel read_async so the
    // run() loop can apply USB control transfers outside the callback.
    std::atomic<bool> m_retuneRequested{false};

    // Apply all pending USB control transfers. Called from run() OUTSIDE
    // the rtlsdr_read_async callback context.
    void applyPendingControls();
};

}  // namespace AetherSDR::rtl
