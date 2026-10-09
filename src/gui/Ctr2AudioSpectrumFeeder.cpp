#include "Ctr2AudioSpectrumFeeder.h"

#include "core/AudioEngine.h"
#include "core/ClientEq.h"
#include "core/Ctr2HidFraming.h"
#include "models/Ctr2ProxyModel.h"

#include <QTimer>

#include <algorithm>
#include <vector>

namespace AetherSDR {

Ctr2AudioSpectrumFeeder::Ctr2AudioSpectrumFeeder(Ctr2ProxyModel* model, AudioEngine* audio,
                                                 QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_audio(audio)
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
            m_timer->start();
        }
    } else {
        m_timer->stop();
    }
}

std::vector<float> Ctr2AudioSpectrumFeeder::barsFromBins(const std::vector<float>& binsDb,
                                                         double sampleRate, float correctionDb)
{
    std::vector<float> bars(kBars, float(ctr2hid::spectrum::kFloorDb));
    const double barHz = double(kSpanHz) / kBars;
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
    m_fft.update(samples.data(), ClientEqFftAnalyzer::kFftSize);
    const ClientEq* eq = m_audio->clientEqRx();
    const double fs = eq ? eq->sampleRate() : 24000.0;
    const std::vector<float> bars =
        barsFromBins(m_fft.magnitudesDb(), fs, m_fft.coherentGainCorrectionDb());
    m_model->sendAudioSpectrum(ctr2hid::spectrum::encode(kSpanHz, bars));
}

} // namespace AetherSDR
