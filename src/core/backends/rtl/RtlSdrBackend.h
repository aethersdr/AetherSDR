#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/RestoredRadioState.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVector>

// librtlsdr forward
struct rtlsdr_dev;

namespace AetherSDR::rtl {

class RtlSdrWorker;
class RtlSdrDdc;

// IRadioBackend implementation for RTL-SDR USB dongles.
//
// Receive-only (Principle VI): capabilities().canTransmit = false,
// hostModulates = false. Single DDC, single panadapter.
//
// The worker owns the librtlsdr device handle and runs
// rtlsdr_read_async → IQ conversion → DDC. Backend relays are delivered to
// the main thread through queued connections.
class RtlSdrBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit RtlSdrBackend(QObject* parent = nullptr);
    ~RtlSdrBackend() override;

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;

    // Demodulates in-process (DDC); there is no VITA-49 stream.
    bool ownsRxAudio() const override { return true; }

    // RTL-SDR has no radio-side memory; the client owns frequency, mode,
    // passband, gain, PPM, etc.
    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    // Gain control — rtl-specific.
    void setPanRfGain(const QString& panId, int gainDb) override;

    // ---- Identity ----
    // The backend family string ("rtl"), used by RadioModel::makeBackend().
    static QString familyName();

    // Sample rate validation & clamping against hardware / capabilities constraints
    static uint32_t clampSampleRate(uint32_t requestedHz);

    // Tuner gain an unconfigured dongle comes up at.
    //
    // NOT ZERO, and the distinction is the whole point: rtlsdr_set_tuner_gain_mode(dev, 1)
    // hands the operator manual control of the tuner, and nearestGainTenths() then snaps
    // the request onto the tuner's own discrete table. Both the R820T and the R828D start
    // that table at exactly 0.0 dB, so a 0 default did not mean "unset" -- it programmed
    // the LOWEST gain the hardware offers, and the receiver came up deaf on a strong local
    // signal. Measured on an RTL-SDR Blog V4 (R828D): 24 snaps to the table's 22.9 dB entry
    // and a nearby WFM broadcast is clearly audible; at 0.0 dB the same station is
    // inaudible. Mid-table rather than max: the top of the R828D table (49.6 dB) overloads
    // the front end on the same signal.
    static constexpr int kDefaultRfGainDb = 24;

private:
    // Emit the initial snapshot a freshly-connected device would report.
    void emitInitialState();

    // Parse device index or serial from connect request params.
    int deviceIndexFromParams(const QVariantMap& params) const;
    QString serialFromParams(const QVariantMap& params) const;
    void handleControlApplied(const QString& control, qint64 value);
    void handleControlFailed(const QString& control, const QString& message);
    void queueExtensionRequest(const QString& control, quint64 requestId);

    // ---- State ----
    bool m_connected{false};

    // Device identity (filled on connect)
    QString m_modelName;
    QString m_serial;
    QString m_vendor;
    QString m_product;

    // Slice 0 state — default to 95.2 MHz FM Wide
    double m_sliceFreqHz{95'200'000.0};
    QString m_sliceMode{"WFM"};
    int m_sliceFilterLow{-100000};
    int m_sliceFilterHigh{100000};

    // Pan & Hardware state — default to 95.2 MHz
    double m_panCenterHz{95'200'000.0};
    uint32_t m_sampleRateHz{2'400'000};
    int m_panRfGainDb{kDefaultRfGainDb};
    int m_ppmCorrection{0};
    int m_directSampling{0};
    QVector<int> m_tunerGainsTenths;
    QHash<QString, QVector<quint64>> m_pendingExtensionRequests;
    QString m_pendingPanId{QStringLiteral("0xe1000000")};

    // Non-owning while connected; RtlSdrWorker closes the handle after its
    // async read loop has exited.
    struct rtlsdr_dev* m_device{nullptr};   // rtlsdr_dev_t*

    // Worker thread (owns async USB reader & RtlSdrDdc engine)
    std::unique_ptr<RtlSdrWorker> m_worker;
    RtlSdrDdc* ddc();
};

}  // namespace AetherSDR::rtl
