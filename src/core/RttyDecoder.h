#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QThread>

#include <atomic>
#include <cmath>

namespace AetherSDR {

// Client-side RTTY (Baudot/ITA2) decoder using mark/space bandpass filters.
// Runs decoding on a worker thread. Feed it 24 kHz stereo float32 PCM
// (the same format audioDataReady emits) and it emits decoded text.
//
// Usage:
//   decoder.start();
//   connect(panStream, &PanadapterStream::audioDataReady, &decoder, &RttyDecoder::feedAudio);
//   connect(&decoder, &RttyDecoder::textDecoded, panel, &PanadapterApplet::appendRttyText);

class RttyDecoder : public QObject {
    Q_OBJECT

public:
    explicit RttyDecoder(QObject* parent = nullptr);
    ~RttyDecoder() override;

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    int   markFreqHz()     const { return m_markFreqHz; }
    int   shiftHz()        const { return m_shiftHz; }
    float baudRate()       const { return m_baudRate; }
    bool  reversePolarity() const { return m_reverse; }

public slots:
    void feedAudio(const QByteArray& pcm24kStereoFloat);

    void setMarkFreqHz(int hz);
    void setShiftHz(int hz);
    void setBaudRate(float baud);
    void setReversePolarity(bool rev);

signals:
    // confidence: 0.5 = weak, 1.0 = perfect (mark/space ratio during character)
    void textDecoded(const QString& text, float confidence);

    // markLevel/spaceLevel are fractional envelope strengths (0–1, sum ≈ 1).
    // snrDb > 3 typically means locked.
    void statsUpdated(float markLevel, float spaceLevel, float snrDb, bool locked);

private:
    void decodeLoop();
    void recalcFilterCoeffs();

    struct BiquadState  { double x1{}, x2{}, y1{}, y2{}; };
    struct BiquadCoeffs { double b0{}, b1{}, b2{}, a1{}, a2{}; };

    static BiquadCoeffs designBandpass(double centerHz, double bwHz, double sampleRate);
    static double processBiquad(BiquadCoeffs& c, BiquadState& s, double x);

    static char baudotToAscii(int code, bool figs);

    static constexpr double kSampleRate  = 24000.0;
    static constexpr int    kBaudotLtrs  = 0x1F;
    static constexpr int    kBaudotFigs  = 0x1B;
    static constexpr int    kBaudotNull  = 0x00;
    static constexpr int    kRingCapacity = static_cast<int>(kSampleRate) * sizeof(float) * 4; // 4 s mono

    QThread*   m_workerThread{nullptr};
    QMutex     m_bufMutex;
    QByteArray m_ringBuf;

    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_paramsChanged{false};

    std::atomic<int>   m_markFreqHz{2125};
    std::atomic<int>   m_shiftHz{170};
    std::atomic<float> m_baudRate{45.45f};
    std::atomic<bool>  m_reverse{false};

    // Filter state (worker thread only — no atomic needed)
    BiquadCoeffs m_markCoeffs;
    BiquadCoeffs m_spaceCoeffs;
    BiquadState  m_markState;
    BiquadState  m_spaceState;
    double       m_markEnv{};
    double       m_spaceEnv{};
};

} // namespace AetherSDR
