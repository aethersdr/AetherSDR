#pragma once
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QObject>
#include <QVector>
#include <QByteArray>
#include <algorithm>
#include <memory>
#include <vector>

namespace AetherSDR {

class DaxIqModel;
class WaveOutWriter;
class WfmDsp;

// ---------------------------------------------------------------------------
// DaxIqCaptureDevice — QIODevice sink that forwards DAX PCM blocks
// ---------------------------------------------------------------------------
class DaxIqCaptureDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit DaxIqCaptureDevice(QObject* parent = nullptr) : QIODevice(parent) {}
    bool   isSequential() const override { return true; }
    qint64 readData(char*, qint64)       override { return 0; }
    qint64 writeData(const char* data, qint64 len) override {
        emit pcmReady(QByteArray(data, static_cast<int>(len)));
        return len;
    }
signals:
    void pcmReady(const QByteArray& pcm);
};

// Device plumbing around WfmDsp (see WfmDsp.h for the DSP chain):
//   IQ source (DAX IQ endpoint at its NATIVE rate, or VITA-49 DaxIqModel)
//     → WfmDsp  (NCO mix-down → exactly 48 kHz → discriminator → FIR)
//     → clamp + volume → Float32 stereo → WaveOutWriter (HiFi Cable / VAC)
// Doppler: MainWindow forwards sliceFreq − panCentre via setFreqOffsetHz(); the
// pan stays fixed during a pass and recentres only when the slice would leave
// maxFreqOffsetHz() (SkyRoof's fixed-centre + NCO design).
class WfmDemodulator : public QObject
{
    Q_OBJECT
public:
    static constexpr int DAX_CHANNEL = 1;
    static constexpr int AUDIO_RATE  = 48000;   // VAC output rate, always exact
    static constexpr int FILTER_HZ   = 20000;   // slice filter half-width (MainWindow)

    explicit WfmDemodulator(QObject* parent = nullptr);
    ~WfmDemodulator() override;

    void start(DaxIqModel* daxIq, const QString& deviceId,
               const QString& panId = QString());
    void stop();

    bool isActive() const { return m_active; }
    void setVolume(int pct) { m_volume = std::clamp(pct / 100.0f, 0.0f, 1.0f); }

    // Doppler / offset control. offsetHz = sliceHz − panCentreHz.
    // Phase-continuous; safe no-op while inactive (applied when DSP exists).
    void  setFreqOffsetHz(float offsetHz);
    float maxFreqOffsetHz() const;

signals:
    void commandReady(const QString& cmd);

public slots:
    void onIqSamples(int channel, QVector<float> iqInterleaved, int sampleRate);
    void onStreamChanged(int channel);

private slots:
    void onDaxIqPcm(const QByteArray& pcm);

private:
    void ensureDsp(int iqRateHz);
    void processIqFloat(const float* iq, int frames);

    DaxIqModel*         m_daxIq{nullptr};
    WaveOutWriter*      m_waveOut{nullptr};
    QAudioSource*       m_iqSource{nullptr};
    DaxIqCaptureDevice* m_iqDevice{nullptr};
    bool                m_usingDaxCapture{false};
    bool                m_active{false};
    bool                m_idle{false};   // IQ input gate state (see processIqFloat)
    float               m_volume{1.0f};

    std::unique_ptr<WfmDsp> m_dsp;
    std::vector<float>      m_audioBuf;
    std::vector<float>      m_iqConvBuf;     // Int16/raw → float conversion
    QAudioFormat            m_captureFormat; // actual DAX capture format
    float                   m_freqOffsetHz{0.0f};

    QString m_panId;
    bool    m_panSent{false};
};

} // namespace AetherSDR
