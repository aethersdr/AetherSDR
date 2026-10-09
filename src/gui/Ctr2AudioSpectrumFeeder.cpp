#include "Ctr2AudioSpectrumFeeder.h"

#include "core/AudioEngine.h"
#include "core/ClientEq.h"
#include "models/Ctr2ProxyModel.h"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR {

Ctr2AudioSpectrumFeeder::Ctr2AudioSpectrumFeeder(Ctr2ProxyModel* model, AudioEngine* audio,
                                                 SourceFn source, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_audio(audio)
    , m_source(std::move(source))
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(kIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &Ctr2AudioSpectrumFeeder::tick);
    connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2AudioSpectrumFeeder::follow);
    follow();
}

void Ctr2AudioSpectrumFeeder::follow()
{
    if (m_model->audioSpectrumWanted()) {
        if (!m_timer->isActive()) {
            m_fft.reset();
            m_lastRxBlock.clear();
            m_lastTxBlock.clear();
            m_timer->start();
        }
    } else {
        m_timer->stop();
    }
}

std::vector<float> Ctr2AudioSpectrumFeeder::barsFromBins(const std::vector<float>& binsDb,
                                                         double sampleRate, int lowHz, int spanHz,
                                                         float correctionDb)
{
    std::vector<float> bars(kBars, -INFINITY);   // an empty band reads as silence
    const int last = static_cast<int>(binsDb.size()) - 1;
    if (last < 1 || sampleRate <= 0) {
        return bars;
    }
    const double binHz = ClientEqFftAnalyzer::binFreq(1, sampleRate);
    for (int b = 0; b < kBars; ++b) {
        const double lo = Ctr2ProxyModel::audioSpectrumBandEdgeHz(lowHz, spanHz, kBars, b);
        const double hi = Ctr2ProxyModel::audioSpectrumBandEdgeHz(lowHz, spanHz, kBars, b + 1);
        const int first = std::max(1, static_cast<int>(std::ceil(lo / binHz)));
        const int stop = std::min(last, static_cast<int>(std::ceil(hi / binHz)) - 1);
        float db = -INFINITY;
        for (int i = first; i <= stop; ++i) {
            db = std::max(db, binsDb[i]);
        }
        if (first > stop) {
            // Narrower than a bin: interpolate at the band's centre.
            const double pos = std::min<double>(last, 0.5 * (lo + hi) / binHz);
            const int j = std::clamp(static_cast<int>(pos), 1, last - 1);
            const double t = std::clamp(pos - j, 0.0, 1.0);
            db = static_cast<float>(binsDb[j] * (1.0 - t) + binsDb[j + 1] * t);
        }
        bars[b] = db + correctionDb;
    }
    return bars;
}

void Ctr2AudioSpectrumFeeder::tick()
{
    if (!m_model->audioSpectrumWanted()) {
        m_timer->stop();
        return;
    }
    if (!m_audio) {
        return;
    }
    const Source src = m_source ? m_source() : Source{};
    if (src.transmitting != m_transmitting) {
        m_transmitting = src.transmitting;   // RX smoothing must not bleed into TX
        m_fft.reset();
    }
    std::vector<float> samples(ClientEqFftAnalyzer::kFftSize, 0.0f);
    const bool ok = src.transmitting
        ? m_audio->copyRecentClientEqTxSamples(samples.data(), ClientEqFftAnalyzer::kFftSize)
        : m_audio->copyRecentClientEqRxSamples(samples.data(), ClientEqFftAnalyzer::kFftSize);
    if (!ok) {
        return;
    }
    // Each EQ runs at its producer's rate, which is not always 24 kHz; read
    // it every tick. Too low a rate (or none yet) has no useful spectrum.
    const ClientEq* eq = src.transmitting ? m_audio->clientEqTx() : m_audio->clientEqRx();
    const double fs = eq ? eq->sampleRate() : 24000.0;
    if (!(fs >= 8000.0)) {
        return;
    }
    const int span = Ctr2ProxyModel::audioSpectrumSpanHz(src.filterLow, src.filterHigh, fs);
    const int low = Ctr2ProxyModel::audioSpectrumLowHz(span);
    // An unchanged block means the tap is not being written: nothing new.
    std::vector<float>& last = src.transmitting ? m_lastTxBlock : m_lastRxBlock;
    if (samples == last) {
        m_fft.reset();
        m_model->sendAudioSpectrum(low, span, std::vector<float>(kBars, -INFINITY));
        return;
    }
    last = samples;
    m_fft.update(samples.data(), ClientEqFftAnalyzer::kFftSize);
    const std::vector<float> bars =
        barsFromBins(m_fft.magnitudesDb(), fs, low, span, m_fft.coherentGainCorrectionDb());
    m_model->sendAudioSpectrum(low, span, bars);
}

} // namespace AetherSDR
