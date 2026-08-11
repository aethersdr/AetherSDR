#pragma once

#include "core/Biquad.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QList>
#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QAudioSink;
class QAudioSource;
class QSerialPort;
class QTimer;

namespace AetherSDR::ft991 {

class Ft991Spectrum;

// QIODevice sink for the codec capture stream (the DaxIqCaptureDevice
// pattern from WfmDemodulator): QAudioSource pushes into writeData, which
// forwards the raw bytes as a signal on the owning (I/O) thread.
class Ft991CaptureSink : public QIODevice {
    Q_OBJECT
public:
    explicit Ft991CaptureSink(QObject* parent = nullptr) : QIODevice(parent) {}
    bool isSequential() const override { return true; }
    qint64 readData(char*, qint64) override { return 0; }
    qint64 writeData(const char* data, qint64 len) override
    {
        emit pcmReady(QByteArray(data, static_cast<int>(len)));
        return len;
    }
signals:
    void pcmReady(const QByteArray& pcm);
};

// The device half of the FT-991 backend: owns the CAT serial port, the USB
// codec capture/playback streams, the audio-band FFT and the resamplers.
// Lives on the backend's I/O thread (created parentless, moveToThread'd) —
// the ColibriDevice position, without the foreign-thread callback: Qt
// delivers serial readyRead and codec PCM on this object's own thread.
//
// CAT is strictly one query in flight: a query is sent, its response (or a
// timeout) releases the next. Set frames are written immediately — the radio
// does not answer sets — each followed by an enqueued confirming query, so
// the authoritative state always comes back off the wire.
class Ft991Device : public QObject {
    Q_OBJECT

public:
    explicit Ft991Device(QObject* parent = nullptr);
    ~Ft991Device() override;

    struct Params {
        QString portName;        // "COM5", "/dev/ttyUSB0", …
        int baudRate = 38400;    // menu 031 CAT RATE; framing fixed 8N2
        QString audioInHint;     // substring match on capture description
        QString audioOutHint;    // substring match on playback description
        double spectrumSpanHz = 4000.0;
    };

    // What connect resolved — carried by opened() so the backend can report
    // honest pan geometry and a truthful health panel.
    struct OpenInfo {
        double coveredSpanHz = 0.0;   // what the FFT bins genuinely cover
        QString audioInDesc;
        int audioInRateHz = 0;
        QString audioOutDesc;         // empty: no playback device found (RX-only session)
        int audioOutRateHz = 0;
    };

    // ---- counters, read by the GUI thread (linkStats/healthSnapshot) ----
    [[nodiscard]] std::uint64_t serialRxBytes() const { return m_serialRxBytes.load(); }
    [[nodiscard]] std::uint64_t serialTxBytes() const { return m_serialTxBytes.load(); }
    [[nodiscard]] std::uint64_t audioBlocks() const { return m_audioBlocks.load(); }
    [[nodiscard]] std::uint64_t audioSamples() const { return m_audioSamples.load(); }
    [[nodiscard]] int catTimeouts() const { return m_catTimeouts.load(); }
    [[nodiscard]] int catRttMs() const { return m_rttMs.load(); }

public slots:
    // Open the serial port, verify the radio (ID -> 0670), fetch the initial
    // dial/mode, open the codec. Emits opened() or openFailed(reason); the
    // initial catFrequency/catMode signals precede opened().
    void openDevice(const AetherSDR::ft991::Ft991Device::Params& params);
    // Unkey if we keyed, close everything. Idempotent.
    void closeDevice();

    void setFrequencyHz(double hz);
    void setMode(const QString& neutral);
    void setPtt(bool tx);
    void setPowerWatts(int watts);
    void setAgc(const QString& neutral);
    void setSpectrumRateFps(int fps);

    // ---- radio-side DSP (CAT SH/NA/NB/NL/BC/BP) ----
    // Width: NA must be chosen before SH (the CAT manual's order — the SH
    // index tables differ between narrow and wide banks).
    void setRadioWidth(const QString& neutralMode, int widthHz);
    void setRadioNoiseBlanker(bool on);
    void setRadioNoiseBlankerLevel(int level0to10);
    void setRadioAutoNotch(bool on);
    void setRadioNoiseReduction(bool on);
    void setRadioNoiseReductionLevel(int level1to15);
    // The clarifier: ONE shared offset, two enable flags (see Ft991Cat).
    // Offset first, then the flag — so enabling never bites at a stale one.
    void setRadioClarifier(bool ritOn, bool xitOn, int offsetHz);
    // Position first, then enable — so the notch never lands somewhere the
    // operator did not ask for.
    void setRadioManualNotch(bool on, int hz);

    // The slice passband, in SliceModel's signed convention (LSB negative).
    // Applied HOST-SIDE as a Butterworth biquad cascade on the 24 kHz audio
    // — the radio already demodulated, so this is the only place the app's
    // filter handles can actually narrow anything. Shapes the speaker AND
    // the slice/TCI feed (the Flex semantic: DAX carries the slice's
    // filtered audio); the spectrum stays pre-filter, like a real pan.
    void setAudioPassband(int lowHz, int highHz);

    // Engine TX audio: int16 interleaved stereo at engineRateHz, already
    // shaped by the TX chain; gain is the backend's MIC-slider factor.
    // Dropped when we have not keyed the radio.
    void submitTxAudio(const QByteArray& int16Stereo, int engineRateHz,
                       float gain);
    // TUNE carrier: keys TX and plays a steady tone into the codec until
    // stopTune(). Drive swapping (PC) is the backend's job — it knows the
    // operator's RF/TUNE power split.
    void startTune();
    void stopTune();

signals:
    void opened(const AetherSDR::ft991::Ft991Device::OpenInfo& info);
    void openFailed(const QString& reason);
    // The link died mid-session: serial port gone (USB unplug), CAT gone
    // quiet (radio powered off), or the codec capture failed. Emitted
    // AFTER the device has torn itself down — the backend only reports.
    void linkLost(const QString& reason);

    // Change-gated CAT state (values as the radio reported them).
    void catFrequency(double hz);
    void catMode(const QString& neutral);
    void catTxState(int state);            // 0 RX, 1 CAT TX, 2 radio PTT
    void catSMeter(int raw0to255);         // every reading, not change-gated
    void catPower(int watts);
    void catAgc(const QString& neutral);
    void catWidth(int shIndex);            // SH index; backend owns the table
    void catNarrow(bool narrow);
    void catNoiseBlanker(bool on);
    void catNoiseBlankerLevel(int level0to10);
    void catAutoNotch(bool on);
    void catNoiseReduction(bool on);
    void catNoiseReductionLevel(int level1to15);
    void catManualNotch(bool on);
    void catManualNotchHz(int hz);
    // TX meters, every reading while transmitting (raw 0..255).
    void catTxPowerMeter(int raw);
    void catTxSwrMeter(int raw);
    // Clarifier state from the IF poll (change-gated as a triple).
    void catClarifier(bool ritOn, bool xitOn, int offsetHz);

    // 24 kHz interleaved-stereo float RX audio (AudioEngine's native format).
    void audioBlockReady(const std::vector<float>& stereoPcm);
    // Audio-band spectrum, dBFS, index 0 = 0 Hz ascending (binCount bins
    // covering OpenInfo::coveredSpanHz).
    void spectrumFrame(const std::vector<float>& binsDbfs);

private:
    void onSerialReadyRead();
    void onPollTick();
    void onCapturePcm(const QByteArray& pcm);
    void handleFrame(const QByteArray& frame);
    void enqueueQuery(const QByteArray& frame);
    void trySendNextQuery();
    void writeFrame(const QByteArray& frame);
    bool openAudio(OpenInfo* info, QString* error);
    void startTxSink();
    void stopTxSink();
    void failOpen(const QString& reason);
    void failLink(const QString& reason);
    void appendMonoFloat(const QByteArray& pcm, std::vector<float>& mono) const;
    [[nodiscard]] bool spectrumFrameDue();

    // ---- CAT ----
    QSerialPort* m_serial = nullptr;
    QTimer* m_pollTimer = nullptr;
    QByteArray m_rxBuf;
    QList<QByteArray> m_pendingQueries;
    QByteArray m_inFlight;
    QElapsedTimer m_inFlightClock;
    quint32 m_tick = 0;
    int m_idAttempts = 0;
    // Consecutive expired queries while Running — the radio-went-quiet
    // detector. Reset by every parsed frame.
    int m_consecutiveTimeouts = 0;

    enum class State { Idle, AwaitId, AwaitInitial, Running };
    State m_state = State::Idle;
    bool m_haveFreq = false;
    bool m_haveMode = false;

    Params m_params;

    // Last-reported CAT state, for change gating.
    double m_freqHz = 0.0;
    QString m_mode;
    int m_txState = 0;
    int m_powerWatts = -1;
    QString m_agc;
    bool m_pttRequested = false;
    int m_shIndex = -1;
    int m_narrow = -1;
    int m_nb = -1;
    int m_nbLevel = -1;
    int m_autoNotch = -1;
    int m_nr = -1;
    int m_nrLevel = -1;
    int m_manualNotch = -1;
    int m_manualNotchHz = -1;
    int m_ritOn = -1;
    int m_xitOn = -1;
    bool m_loggedBadInfo = false;
    int m_clarifierHz = 0;
    bool m_haveClarifier = false;

    // ---- RX audio ----
    QAudioSource* m_audioIn = nullptr;
    Ft991CaptureSink* m_capSink = nullptr;
    QAudioFormat m_capFormat;
    std::unique_ptr<Ft991Spectrum> m_spectrum;
    std::vector<float> m_monoScratch;
    std::vector<float> m_binsScratch;
    std::vector<float> m_mono24k;
    std::vector<float> m_stereoOut;

    // Host-side RX passband (see setAudioPassband). Two sections each way
    // make a 4th-order Butterworth; skipped entirely at wide-open edges so
    // the default AM/FM path adds no phase shift it does not need.
    std::array<Biquad, 2> m_rxHighpass;
    std::array<Biquad, 2> m_rxLowpass;
    bool m_rxHighpassOn = false;
    bool m_rxLowpassOn = false;
    double m_rxPhase = 0.0;          // capture-rate -> 24 kHz resampler phase
    float m_rxLastSample = 0.0f;     // previous input sample for interpolation

    // Spectrum pacing (the colibri shape: skip the transform when a frame is
    // not due; a due frame spans several PCM blocks, so the flag holds until
    // one completes).
    int m_spectrumIntervalMs = 0;    // 0 = uncapped
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;

    // ---- TX audio ----
    QAudioDevice m_audioOutDevice;
    QAudioFormat m_outFormat;
    QAudioSink* m_txSink = nullptr;
    QIODevice* m_txIo = nullptr;     // owned by m_txSink
    double m_txPhase = 0.0;          // engine-rate -> codec-rate resampler phase
    float m_txLastL = 0.0f;
    QByteArray m_txScratch;
    bool m_tuneToneActive = false;
    QTimer* m_toneTimer = nullptr;
    double m_tonePhase = 0.0;

    // ---- counters (GUI-thread readable) ----
    std::atomic<std::uint64_t> m_serialRxBytes{0};
    std::atomic<std::uint64_t> m_serialTxBytes{0};
    std::atomic<std::uint64_t> m_audioBlocks{0};
    std::atomic<std::uint64_t> m_audioSamples{0};
    std::atomic<int> m_catTimeouts{0};
    std::atomic<int> m_rttMs{-1};

    static constexpr int kPollIntervalMs = 100;
    static constexpr int kQueryTimeoutMs = 400;
    static constexpr int kIdRetries = 3;
    // ~5 s of unanswered CAT (timeouts accumulate at ~2/s) = link dead.
    static constexpr int kMaxConsecutiveTimeouts = 10;
    static constexpr int kAudioOutRateFallbackHz = 48000;
    static constexpr double kTuneToneHz = 1500.0;
    static constexpr double kTuneToneAmplitude = 0.7;
    static constexpr int kEngineRxRateHz = 24000;   // AudioEngine native RX rate
};

}  // namespace AetherSDR::ft991

Q_DECLARE_METATYPE(AetherSDR::ft991::Ft991Device::Params)
Q_DECLARE_METATYPE(AetherSDR::ft991::Ft991Device::OpenInfo)
