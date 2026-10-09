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
// is hearing: the RX tap the Client EQ panels use
// (AudioEngine::copyRecentClientEqRxSamples), which is after the radio's DSP,
// client NR and the RX chain up to and including the EQ; stages the operator
// placed after the EQ, output conversion and volume are not in it. Bars
// about 20 times a
// second, over 0..the active slice's passband width. It runs only while the
// relay is up with a device that
// negotiated the AudioSpectrum extension, so a stock CTR2 costs nothing.
// The tap has no freshness signal and copies return its last block forever
// once writes stop (transmitting, no output device, or another source owning
// the display), so an unchanged block means nothing new is heard: the feeder
// then sends floor-level bars rather than a frozen spectrum. It keeps its own
// analyzer; the EQ editor's smoothing and reset-on-hide stay the editor's.
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
