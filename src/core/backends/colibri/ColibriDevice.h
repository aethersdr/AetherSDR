#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

#include "core/backends/colibri/ColibriLib.h"

namespace AetherSDR::colibri {

// The device half of the Colibri backend: owns the open descriptor and the
// running IQ stream, standing where MetisClient stands for the HL2. Lives on
// the backend's I/O thread (created parentless, moveToThread'd).
//
// The DLL calls the RX callback from ITS OWN internal thread — not ours. The
// callback therefore only copies the block and emits iqBlockReady; the
// connection that feeds the DSP is queued onto this object's (I/O) thread.
// That single queue hop is the whole thread-safety story of the sample path:
// nothing downstream of it ever runs on the library's thread.
class ColibriDevice : public QObject {
    Q_OBJECT

public:
    explicit ColibriDevice(QObject* parent = nullptr);
    ~ColibriDevice() override;

    struct Params {
        QString dllPath;          // optional override; empty = search defaults
        std::uint32_t deviceIndex = 0;
        int sampleRateHz = 48000;
        double frequencyHz = 7'100'000.0;
        double preampDb = 0.0;
    };

    // ---- counters, read by the GUI thread (linkStats/healthSnapshot) ----
    // Atomics rather than mirrors because they are single values with no
    // invariant between them; a torn multi-field snapshot is not possible here.
    [[nodiscard]] std::uint64_t samplesReceived() const { return m_samples.load(); }
    [[nodiscard]] std::uint64_t blocksReceived() const { return m_blocks.load(); }
    [[nodiscard]] bool adcOverload() const { return m_adcOverload.load(); }

public slots:
    // Load the library if needed, open the descriptor, start the stream.
    // Emits opened() or openFailed(reason).
    void openDevice(const AetherSDR::colibri::ColibriDevice::Params& params);
    // Stop + close + release the in-use latch. Idempotent.
    void closeDevice();
    // Stop the stream and restart it at a new rate, keeping the descriptor,
    // frequency and preamp. Returns false (stream stopped, device still open)
    // if the new rate could not be started — the caller decides whether to
    // retry the old rate. Invoked blocking from the backend so the caller
    // knows the stream is already delivering the new rate on return.
    bool restart(int sampleRateHz);

    void setFrequencyHz(double hz);
    void setPreampDb(double db);

signals:
    void opened();
    void openFailed(const QString& reason);
    // One IQ block, normalized complex<float>, still in the DLL's wire order.
    // Emitted from the LIBRARY's thread; every receiver must connect queued
    // (the default for the cross-thread case) — never DirectConnection.
    void iqBlockReady(const std::vector<std::complex<float>>& iq);
    // Change-gated: the flag rides every callback, but only transitions are
    // worth a cross-thread signal.
    void adcOverloadChanged(bool overload);

private:
    static bool COLIBRI_CALL onRx(ColibriComplex* iq, std::uint32_t len,
                                  bool adcOverload, void* user);

    ColibriDescriptor m_dev = nullptr;
    bool m_streaming = false;
    double m_frequencyHz = 7'100'000.0;
    double m_preampDb = 0.0;

    std::atomic<std::uint64_t> m_samples{0};
    std::atomic<std::uint64_t> m_blocks{0};
    std::atomic<bool> m_adcOverload{false};
    // The callback keeps delivering into a stopped/destroyed object unless the
    // stream is stopped first, so this latch is belt-and-braces: a false makes
    // the callback drop the block AND tell the library to stop.
    std::atomic<bool> m_acceptBlocks{false};
};

}  // namespace AetherSDR::colibri

Q_DECLARE_METATYPE(AetherSDR::colibri::ColibriDevice::Params)
