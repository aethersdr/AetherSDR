#include "NnrFilter.h"

#include "NnrControls.h"
#include "Resampler.h"

#include "aether_wdsp.h"

#include <QDebug>

#include <algorithm>

namespace AetherSDR {

namespace {

// WDSP's block size is fixed at construction, so pick one and accumulate to
// it. 256 complex samples at 48 kHz is 5.33 ms — small enough that the
// accumulator never adds meaningful latency on top of NNR's own 51 ms, and a
// whole number of the 256-sample hops the network runs on.
constexpr int kBlockFrames = 256;

// The internal rate, transform and overlap the models were trained for, and
// what RXA.c constructs. Not ours to vary.
constexpr int kNetworkRate = 16000;
constexpr int kFftSize = 512;
constexpr int kOverlap = 2;
constexpr int kLookahead = 1;

// The rate NNR actually runs at here. 24 kHz is not a multiple of the network
// rate, so a 24 kHz source is resampled around the block; a 48 kHz one is not.
constexpr int kProcessingRate = 48000;

}  // namespace

NnrFilter::NnrFilter(int sampleRate)
    : m_sampleRate(sampleRate)
    , m_stereoAdapter(0, sampleRate)
{
    if (!m_stereoAdapter.isValid()) {
        qWarning() << "NnrFilter: unsupported sample rate" << sampleRate;
        return;
    }
    if (sampleRate == 24000) {
        m_up = std::make_unique<Resampler>(24000, kProcessingRate);
        m_down = std::make_unique<Resampler>(kProcessingRate, 24000);
    }

    m_blockFrames = kBlockFrames;
    m_blockIn.assign(static_cast<std::size_t>(m_blockFrames) * 2, 0.0);
    m_blockOut.assign(static_cast<std::size_t>(m_blockFrames) * 2, 0.0);

    // run=1: this object's existence IS the enable, so WDSP's own run flag
    // stays set and AudioEngine simply stops calling process(). position=0
    // to match the xnnr() call below; there is only one call site here.
    // cmode=1 zeroes Q, which we discard anyway.
    m_nnr = create_nnr(1, 0, m_blockFrames, m_blockIn.data(), m_blockOut.data(),
                       kProcessingRate, kNetworkRate, kFftSize, kOverlap,
                       kLookahead, Nnr::maskFloorForStrength(m_strength.load()), 1);
    if (!m_nnr) {
        qWarning() << "NnrFilter: create_nnr() failed";
        return;
    }

    // The adapter pairs each processed block with the dry stereo from the same
    // moment, so it needs the TOTAL latency of this filter -- not NNR's alone.
    //
    // On the 24 kHz path the resamplers dominate: each contributes ~70 ms of
    // linear-phase group delay against NNR's own 51 ms, so declaring only
    // NNR's left the balance reading dry audio ~131 ms stale. At a 3 Hz
    // syllable rate that is a third of a cycle, which applies the gain
    // computed for a gap to a syllable and vice versa -- measured as voice
    // 12 dB down and the gaps 10 dB UP, the exact inverse of what the stage
    // is for. The 48 kHz path has no resamplers and was always correct, which
    // is what made this look like a model problem rather than a wiring one.
    m_stereoAdapter.setProcessingLatencyFrames(totalLatencyFrames());

    m_appliedModel.store(getModel_nnr(static_cast<NNR>(m_nnr)));
    qDebug() << "NnrFilter: initialized at" << sampleRate << "Hz, model slot"
             << m_appliedModel.load() << ", total delay"
             << totalLatencyFrames() * 1000.0 / m_sampleRate << "ms"
             << "(NNR" << getDelay_nnr(static_cast<NNR>(m_nnr)) * 1000.0 / kProcessingRate
             << "ms + resampling)";
}

NnrFilter::~NnrFilter()
{
    if (m_nnr) {
        destroy_nnr(static_cast<NNR>(m_nnr));
    }
}

void NnrFilter::reset()
{
    if (m_nnr) {
        flush_nnr(static_cast<NNR>(m_nnr));
    }
    m_inAccum.clear();
    m_stereoAdapter.reset();
}

int NnrFilter::totalLatencyFrames() const
{
    if (!m_nnr) {
        return 0;
    }
    // NNR's own, converted from the processing rate to the configured one.
    int frames = getDelay_nnr(static_cast<NNR>(m_nnr)) * m_sampleRate / kProcessingRate;
    // Each resampler reports its group delay in ITS OWN source-rate samples:
    // the upsampler's are already the configured rate, the downsampler's are
    // at the processing rate and need converting.
    if (m_up) {
        frames += m_up->groupDelayInputFrames();
    }
    if (m_down) {
        frames += m_down->groupDelayInputFrames() * m_sampleRate / kProcessingRate;
    }
    return frames;
}

int NnrFilter::delaySamples() const
{
    return totalLatencyFrames();
}

void NnrFilter::setStrength(int strength)
{
    m_strength.store(std::clamp(strength, 0, 100));
    m_paramsDirty.store(true);
}

void NnrFilter::setModel(int slot)
{
    m_requestedModel.store(slot);
    m_paramsDirty.store(true);
}

void NnrFilter::setAlpha(double alpha)
{
    m_alpha.store(alpha);
    m_paramsDirty.store(true);
}

void NnrFilter::setAlphaKnee(double kneeDb)
{
    m_alphaKnee.store(kneeDb);
    m_paramsDirty.store(true);
}

void NnrFilter::setTau(double tau)
{
    m_tau.store(tau);
    m_paramsDirty.store(true);
}

void NnrFilter::setMaxGain(double gainDb)
{
    m_maxGain.store(gainDb);
    m_paramsDirty.store(true);
}

void NnrFilter::setSmoothing(double attackMs, double releaseMs)
{
    m_smoothAttackMs.store(attackMs);
    m_smoothReleaseMs.store(releaseMs);
    m_paramsDirty.store(true);
}

// WDSP's standalone setters take no lock of their own, so they are applied
// here on the audio thread rather than from the setters above.
void NnrFilter::applyPendingParameters()
{
    if (!m_paramsDirty.exchange(false)) {
        return;
    }
    auto nnr = static_cast<NNR>(m_nnr);
    setMaskFloor_nnr(nnr, Nnr::maskFloorForStrength(m_strength.load()));
    setAlpha_nnr(nnr, m_alpha.load());
    setAlphaKnee_nnr(nnr, m_alphaKnee.load());
    setTau_nnr(nnr, m_tau.load());
    setMaxGain_nnr(nnr, m_maxGain.load());
    setSmooth_nnr(nnr, m_smoothAttackMs.load(), m_smoothReleaseMs.load());

    const int requested = m_requestedModel.load();
    if (requested != m_appliedModel.load()) {
        // Reports the slot actually in use, which differs from the request
        // when this build has no model there.
        m_appliedModel.store(setModel_nnr(nnr, requested));
    }
}

QByteArray NnrFilter::process(const QByteArray& pcmStereo)
{
    if (!m_nnr || m_blockFrames <= 0 || pcmStereo.isEmpty()) {
        return pcmStereo;
    }

    applyPendingParameters();

    const auto* src = reinterpret_cast<const float*>(pcmStereo.constData());
    const int stereoFrames = pcmStereo.size() / (2 * static_cast<int>(sizeof(float)));
    m_stereoAdapter.pushDryStereo(pcmStereo);

    // 1. Downmix, then resample up for the 24 kHz path only.
    m_monoInput.resize(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i) {
        m_monoInput[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
    }
    QByteArray mono48k = m_up
        ? m_up->process(m_monoInput.data(), stereoFrames)
        : QByteArray(reinterpret_cast<const char*>(m_monoInput.data()),
                     stereoFrames * static_cast<int>(sizeof(float)));

    const auto* mono48kSamples = reinterpret_cast<const float*>(mono48k.constData());
    const int monoSamples48k = mono48k.size() / static_cast<int>(sizeof(float));

    // 2. Accumulate to whole blocks — WDSP's block size cannot change without
    //    rebuilding the block, its FFTW plans and both models.
    const int prevAccumSamples = m_inAccum.size() / static_cast<int>(sizeof(float));
    m_inAccum.resize((prevAccumSamples + monoSamples48k) * sizeof(float));
    {
        auto* floatBuf = reinterpret_cast<float*>(m_inAccum.data());
        for (int i = 0; i < monoSamples48k; ++i) {
            floatBuf[prevAccumSamples + i] = mono48kSamples[i];
        }
    }

    const int totalAccumSamples = prevAccumSamples + monoSamples48k;
    const int completeBlocks = totalAccumSamples / m_blockFrames;
    if (completeBlocks <= 0) {
        return m_stereoAdapter.takeProcessedMono(nullptr, 0);
    }

    auto* accumData = reinterpret_cast<float*>(m_inAccum.data());
    m_processed48k.resize(static_cast<std::size_t>(completeBlocks)
                          * static_cast<std::size_t>(m_blockFrames));

    for (int b = 0; b < completeBlocks; ++b) {
        const float* blockStart = &accumData[b * m_blockFrames];
        // Interleave into I with a silent Q — NNR reads only I.
        for (int i = 0; i < m_blockFrames; ++i) {
            m_blockIn[i * 2] = static_cast<double>(blockStart[i]);
            m_blockIn[i * 2 + 1] = 0.0;
        }
        xnnr(static_cast<NNR>(m_nnr), 0);
        float* out = &m_processed48k[static_cast<std::size_t>(b) * m_blockFrames];
        for (int i = 0; i < m_blockFrames; ++i) {
            out[i] = static_cast<float>(m_blockOut[i * 2]);
        }
    }

    const int consumedSamples = completeBlocks * m_blockFrames;
    const int leftoverSamples = totalAccumSamples - consumedSamples;
    if (leftoverSamples > 0) {
        m_inAccum = QByteArray(reinterpret_cast<const char*>(&accumData[consumedSamples]),
                               leftoverSamples * sizeof(float));
    } else {
        m_inAccum.clear();
    }

    // 3. Back down for the 24 kHz path, then restore the dry stereo balance.
    const int outputMonoSamples = consumedSamples;
    QByteArray downsampled = m_down
        ? m_down->process(m_processed48k.data(), outputMonoSamples)
        : QByteArray(reinterpret_cast<const char*>(m_processed48k.data()),
                     outputMonoSamples * static_cast<int>(sizeof(float)));

    const auto* monoOut = reinterpret_cast<const float*>(downsampled.constData());
    const int monoOutFrames = downsampled.size() / static_cast<int>(sizeof(float));
    return m_stereoAdapter.takeProcessedMono(monoOut, monoOutFrames);
}

}  // namespace AetherSDR
