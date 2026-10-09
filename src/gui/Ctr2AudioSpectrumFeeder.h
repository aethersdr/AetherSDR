#pragma once

#include "ClientEqFftAnalyzer.h"

#include <QObject>

#include <functional>
#include <utility>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class Ctr2ProxyModel;

// Feeds a controller on the CTR2 USB relay the audio spectrum the operator
// is hearing: the post-DSP RX audio tap the Client EQ panels use
// (AudioEngine::copyRecentClientEqRxSamples), as bars about 20 times a
// second, over 0..the active slice's passband width. It runs only while the
// relay is up with a device that
// negotiated the AudioSpectrum extension, so a stock CTR2 costs nothing.
// While no new audio reaches the tap (transmitting, or no RX audio) it sends
// floor-level bars rather than the tap's last, frozen block.
class Ctr2AudioSpectrumFeeder : public QObject {
    Q_OBJECT

public:
    static constexpr int kBars = 32;
    static constexpr int kIntervalMs = 50;

    // The active slice's passband (filter low, high in Hz); {0, 0} for none.
    // The bars span its width (Ctr2ProxyModel::audioSpectrumSpanHz).
    using PassbandSource = std::function<std::pair<int, int>()>;

    Ctr2AudioSpectrumFeeder(Ctr2ProxyModel* model, AudioEngine* audio, PassbandSource passband,
                            QObject* parent = nullptr);

    // Bars in dBFS from FFT magnitudes (dB, bin i at i * sampleRate / 2048):
    // the strongest bin in each of kBars equal bands over 0..spanHz.
    static std::vector<float> barsFromBins(const std::vector<float>& binsDb, double sampleRate,
                                           int spanHz, float correctionDb);

private:
    void follow();
    void tick();

    Ctr2ProxyModel* m_model;
    AudioEngine* m_audio;
    PassbandSource m_passband;
    QTimer* m_timer;
    ClientEqFftAnalyzer m_fft;
    std::vector<float> m_lastBlock;
};

} // namespace AetherSDR
