#pragma once

#include "ClientEqFftAnalyzer.h"

#include <QObject>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class Ctr2ProxyModel;

// Feeds a controller on the CTR2 USB relay the audio spectrum the operator
// is hearing: the post-DSP RX audio tap the Client EQ panels use
// (AudioEngine::copyRecentClientEqRxSamples), as bars over 0..4 kHz about 20
// times a second. It runs only while the relay is up with a device that
// negotiated the AudioSpectrum extension, so a stock CTR2 costs nothing.
class Ctr2AudioSpectrumFeeder : public QObject {
    Q_OBJECT

public:
    static constexpr int kBars = 32;
    static constexpr int kSpanHz = 4000;
    static constexpr int kIntervalMs = 50;

    Ctr2AudioSpectrumFeeder(Ctr2ProxyModel* model, AudioEngine* audio, QObject* parent = nullptr);

    // Bars in dBFS from FFT magnitudes (dB, bin i at i * sampleRate / 2048):
    // the strongest bin in each band of kSpanHz / kBars Hz.
    static std::vector<float> barsFromBins(const std::vector<float>& binsDb, double sampleRate,
                                           float correctionDb);

private:
    void follow();
    void tick();

    Ctr2ProxyModel* m_model;
    AudioEngine* m_audio;
    QTimer* m_timer;
    ClientEqFftAnalyzer m_fft;
};

} // namespace AetherSDR
