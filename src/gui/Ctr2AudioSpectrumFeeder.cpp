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
                                                 PassbandSource passband, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_audio(audio)
    , m_passband(std::move(passband))
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
            m_lastBlock.clear();
            m_timer->start();
        }
    } else {
        m_timer->stop();
    }
}

std::vector<float> Ctr2AudioSpectrumFeeder::barsFromBins(const std::vector<float>& binsDb,
                                                         double sampleRate, int spanHz,
                                                         float correctionDb)
{
    std::vector<float> bars(kBars, -INFINITY);   // an empty band reads as silence
    const double barHz = double(spanHz) / kBars;
    for (int i = 1; i < static_cast<int>(binsDb.size()); ++i) {
        const double hz = ClientEqFftAnalyzer::binFreq(i, sampleRate);
        const int bar = static_cast<int>(hz / barHz);
        if (bar >= kBars) {
            break;
        }
        bars[bar] = std::max(bars[bar], binsDb[i] + correctionDb);
    }
    return bars;
}

void Ctr2AudioSpectrumFeeder::tick()
{
    if (!m_model->audioSpectrumWanted()) {
        m_timer->stop();
        return;
    }
    std::vector<float> samples(ClientEqFftAnalyzer::kFftSize, 0.0f);
    if (!m_audio || !m_audio->copyRecentClientEqRxSamples(samples.data(),
                                                         ClientEqFftAnalyzer::kFftSize)) {
        return;
    }
    // The tap is not written while transmitting, and copy returns its last
    // block regardless; an unchanged block means nothing new is heard.
    // The EQ runs at the producer rate, which is not always 24 kHz; read it
    // every tick. Too low a rate (or none yet) has no useful spectrum.
    const ClientEq* eq = m_audio->clientEqRx();
    const double fs = eq ? eq->sampleRate() : 24000.0;
    if (!(fs >= 8000.0)) {
        return;
    }
    const std::pair<int, int> pb = m_passband ? m_passband() : std::pair<int, int>{0, 0};
    const int span = Ctr2ProxyModel::audioSpectrumSpanHz(pb.first, pb.second, fs);
    if (samples == m_lastBlock) {
        m_fft.reset();
        m_model->sendAudioSpectrum(span, std::vector<float>(kBars, -INFINITY));
        return;
    }
    m_lastBlock = samples;
    m_fft.update(samples.data(), ClientEqFftAnalyzer::kFftSize);
    const std::vector<float> bars =
        barsFromBins(m_fft.magnitudesDb(), fs, span, m_fft.coherentGainCorrectionDb());
    m_model->sendAudioSpectrum(span, bars);
}

} // namespace AetherSDR
