#pragma once

#include "ClientEqFftAnalyzer.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <utility>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class Ctr2ProxyModel;

// Feeds a controller on the CTR2 USB relay an audio spectrum, about 20 times
// a second, as bars log-spaced in frequency up to the filter width (see
// Source).
//  - Receiving: what the operator hears, from the RX tap the Client EQ panels
//    use (AudioEngine::copyRecentClientEqRxSamples): after the radio's DSP,
//    client NR and the RX chain up to and including the EQ. Stages placed
//    after the EQ, output conversion and volume are not in it.
//  - Transmitting: the transmit audio, from the TX EQ tap
//    (copyRecentClientEqTxSamples).
// It runs only while the relay is up with a device that negotiated the
// AudioSpectrum extension, so a stock CTR2 costs nothing.
// The taps have no freshness signal and copies return their last block
// forever once writes stop (no output device, no mic audio, or another source
// owning the display), so an unchanged block means nothing new is heard: the
// feeder then sends floor-level bars rather than a frozen spectrum. It keeps
// its own analyzer, reset when it switches between RX and TX; the EQ
// editors' smoothing and reset-on-hide stay theirs.
class Ctr2AudioSpectrumFeeder : public QObject {
    Q_OBJECT

public:
    static constexpr int kBars = 32;
    static constexpr int kIntervalMs = 50;

    // What to show: while transmitting, the TX audio over the TX filter;
    // otherwise the RX audio over the active slice's passband (filter low and
    // high in Hz; both 0 for none). The bars span the filter's width
    // (Ctr2ProxyModel::audioSpectrumSpanHz).
    struct Source {
        bool transmitting{false};
        int filterLow{0};
        int filterHigh{0};
    };
    using SourceFn = std::function<Source()>;

    Ctr2AudioSpectrumFeeder(Ctr2ProxyModel* model, AudioEngine* audio, SourceFn source,
                            QObject* parent = nullptr);

    // Bars in dBFS from FFT magnitudes (dB, bin i at i * sampleRate / 2048),
    // one per log-spaced band from lowHz to spanHz: the strongest bin in the
    // band, or, for a band narrower than a bin, the magnitude interpolated at
    // its centre, so the low bars do not repeat one bin as flat steps.
    static std::vector<float> barsFromBins(const std::vector<float>& binsDb, double sampleRate,
                                           int lowHz, int spanHz, float correctionDb);

private:
    void follow();
    void tick();

    Ctr2ProxyModel* m_model;
    QPointer<AudioEngine> m_audio;
    SourceFn m_source;
    QTimer* m_timer;
    ClientEqFftAnalyzer m_fft;
    bool m_transmitting{false};       // which tap the analyzer's state belongs to
    std::vector<float> m_lastRxBlock;
    std::vector<float> m_lastTxBlock;
};

} // namespace AetherSDR
