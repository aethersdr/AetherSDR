#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/RestoredRadioState.h"
#include "core/RtlSliceSettings.h"
#include "core/RtlDeviceSettings.h"
#include <QSet>
#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlViewport.h"

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
// hostModulates = false. One capture/panadapter, prepared FM receiver bank;
// the other existing modes retain the exclusive legacy DDC.
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
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }

    // RTL-SDR has no radio-side memory; the client owns frequency, mode,
    // passband, gain, PPM, etc.
    void configureSettingsScope(const RadioSettingsScope&, const RadioSerialIdentity&) override;
    std::optional<bool> storeOperatingState(const RadioSettingsScope&, const RestoredRadioState&) override;
    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;
    HealthSnapshot healthSnapshot() const override;

    bool createSlice(const QString& panId, double frequencyHz) override;
    bool removeSlice(int sliceId) override;
    void setSliceFrequency(int sliceId, double hz) override;
    bool requestReceiveTune(int sliceId, double hz, ReceiveTuneView view) override;
    bool recenterReceiveCapture(const QString& panId) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    void setPanAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setSliceSquelch(int sliceId, bool enabled, int level) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
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
    // Increase only with the integrated architecture evidence described in
    // docs/rtl-m1-runtime.md. This is independent of the eight stable slots.
    static constexpr int kQualifiedReceiverCapacity = 1;

private:
    bool hasAcceptedSlice(int sliceId) const;
    // Emit the initial snapshot a freshly-connected device would report.
    void emitInitialState();

    // Parse device index or serial from connect request params.
    int deviceIndexFromParams(const QVariantMap& params) const;
    QString serialFromParams(const QVariantMap& params) const;
    friend struct RtlCaptureBackendTestAccess;
    void startCapture(std::unique_ptr<RtlSdrWorker> worker);
    void serviceCapture();
    bool requestCapture(const RtlCaptureTransaction::Desired& desired,
                        const QString& extension = {}, quint64 requestId = 0);
    void publishCapture();
    void drainAudio();
    void publishLegacyPcm(const QByteArray& pcm, const QByteArray& preMonitor);
    void retireNativeAudio();
    void emitSliceState(const RtlCaptureTransaction::Receiver& receiver);
    void updateMonitor(int sliceId);
    bool activateSettings();
    void restoreAcceptedSlices();
    QVector<RtlSliceSettings::Slice> acceptedSettings() const;
    void finishExtensions(bool success, RtlCaptureTransaction::Token token = {});
    QVariantMap deviceSettingsStatus() const;
    void saveAcceptedDeviceSettings();
    void verifyDeviceSettingsIdentity(const QString& serial);
    bool acceptsFrame(quint64 session, quint64 revision) const;
    void requestViewport(bool followDrag = false);
    void publishViewport();
    QByteArray viewportFrame(const QByteArray& frame) const;


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
    int m_receiveGain{100};
    bool m_receiveMuted{false};
    int m_sliceFilterLow{-100000};
    int m_sliceFilterHigh{100000};

    // Pan & Hardware state — default to 95.2 MHz
    double m_panCenterHz{95'200'000.0};
    uint32_t m_sampleRateHz{2'400'000};
    std::optional<RtlViewport> m_viewport;
    double m_viewCenterRequestHz = 0;
    double m_viewSpanRequestHz = 0;
    RtlCaptureTransaction::Token m_pendingViewport;
    QString m_captureStatus;
    int m_panRfGainDb{kDefaultRfGainDb};
    int m_ppmCorrection{0};
    bool m_dcSuppression = false;
    RtlDeviceSettings::ReadResult m_savedDeviceSettings;
    bool m_deviceSettingsAllowed = false;
    bool m_deviceSettingsSaved = false;
    QString m_deviceSettingsReason;
    int m_directSampling{0};
    QVector<int> m_tunerGainsTenths;
    struct PendingExtension {
        quint64 requestId;
        RtlCaptureTransaction::Token token;
    };
    QHash<QString, PendingExtension> m_pendingExtensionRequests; // one promise per control
    // Conservative profile until integrated architecture measurements qualify
    // higher admission. Eight registry slots are representation, not capacity.
    int m_receiverCapacity = kQualifiedReceiverCapacity;
    RtlCaptureTransaction m_capture{{8, kQualifiedReceiverCapacity}};
    std::optional<RtlCaptureTransaction::State> m_lastPublished;
    struct Monitor { int gain = 100; int pan = 50; bool mute = false; };
    std::array<Monitor, 8> m_monitors;
    struct NativeAudio {
        std::unique_ptr<PcmProducer> producer;
        quint64 captureEpoch = 0;
        quint64 instance = 0;
        quint64 receiverEpoch = 0;
        quint64 nextSample = 0;
    };
    NativeAudio m_speakerAudio;
    std::array<NativeAudio, 8> m_sliceAudio;
    RtlCaptureTransaction::Desired m_requested;
    QTimer m_captureTimer;
    RtlCaptureTransaction::Token m_published;
    bool m_connecting{false};
    RadioSettingsScope m_settingsScope;
    RadioSerialIdentity m_settingsIdentity;
    RtlSliceSettings::ReadResult m_savedSettings;
    bool m_settingsActive = false;
    bool m_restoreAttempted = false;
    RtlCaptureTransaction::Token m_restoreToken;
    QVector<int> m_removedSettings;
    QSet<int> m_omittedSettings;
    QString m_pendingPanId{QStringLiteral("0xe1000000")};

    // Non-owning while connected; RtlSdrWorker closes the handle after its
    // async read loop has exited.
    struct rtlsdr_dev* m_device{nullptr};   // rtlsdr_dev_t*

    // Worker thread (owns async USB reader & RtlSdrDdc engine)
    std::unique_ptr<RtlSdrWorker> m_worker;
    RtlReceivePipeline::Diagnostics m_diagnostics; // owner-thread health cache
    RtlSdrDdc* ddc();
};

}  // namespace AetherSDR::rtl
