#include "NnrFilter.h"

#include "NnrControls.h"
#include "Resampler.h"

#include "aether_wdsp.h"
#include "core/dsp/FftwPlannerLock.h"

#include <QDebug>

#include <algorithm>
#include <limits>

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
{
    if (sampleRate != 24000 && sampleRate != 48000) {
        qWarning() << "NnrFilter: unsupported sample rate" << sampleRate;
        return;
    }

    m_blockFrames = kBlockFrames;
    for (int channel = 0; channel < 2; ++channel) {
        if (sampleRate == 24000) {
            m_up[channel] = std::make_unique<Resampler>(24000, kProcessingRate);
            m_down[channel] = std::make_unique<Resampler>(kProcessingRate, 24000);
        }
        m_blockIn[channel].assign(static_cast<std::size_t>(m_blockFrames) * 2, 0.0);
        m_blockOut[channel].assign(static_cast<std::size_t>(m_blockFrames) * 2, 0.0);

        // run=1: this object's existence IS the enable, so WDSP's own run flag
        // stays set and AudioEngine simply stops calling process(). position=0
        // to match the xnnr() call below; there is only one call site here.
        // cmode=1 zeroes Q, which we discard anyway.
        //
        // create_nnr() plans two FFTW_PATIENT double-precision transforms
        // (nnr.c calc_nnr), so it takes the process-global planner lock like
        // every other fftw_* user (#6287). Without it this raced NR2 wisdom
        // generation and segfaulted inside libfftw3. Guarded here rather than
        // in vendored WDSP. The wait is as long as the other holder's plan.
        {
            auto lock = fftwPlannerLock();
            m_nnr[channel] = create_nnr(1, 0, m_blockFrames, m_blockIn[channel].data(),
                                        m_blockOut[channel].data(), kProcessingRate,
                                        kNetworkRate, kFftSize, kOverlap, kLookahead,
                                        Nnr::maskFloorForStrength(m_strength.load()), 1);
        }
        if (!m_nnr[channel]) {
            qWarning() << "NnrFilter: create_nnr() failed";
            return;
        }
    }

    m_appliedModel.store(getModel_nnr(static_cast<NNR>(m_nnr[0])));
    qDebug() << "NnrFilter: initialized at" << sampleRate << "Hz, model slot"
             << m_appliedModel.load() << ", total delay"
             << totalLatencyFrames() * 1000.0 / m_sampleRate << "ms"
             << "(NNR" << getDelay_nnr(static_cast<NNR>(m_nnr[0])) * 1000.0 / kProcessingRate
             << "ms + resampling)";
}

NnrFilter::~NnrFilter()
{
    // destroy_nnr() destroys both plans (nnr.c decalc_nnr): same lock (#6287).
    auto lock = fftwPlannerLock();
    for (void* nnr : m_nnr) {
        if (nnr) {
            destroy_nnr(static_cast<NNR>(nnr));
        }
    }
}

void NnrFilter::reset()
{
    for (int channel = 0; channel < 2; ++channel) {
        if (m_nnr[channel]) {
            flush_nnr(static_cast<NNR>(m_nnr[channel]));
        }
        m_inAccum[channel].clear();
    }
}

int NnrFilter::totalLatencyFrames() const
{
    if (!m_nnr[0]) {
        return 0;
    }
    // NNR's own, converted from the processing rate to the configured one.
    int frames = getDelay_nnr(static_cast<NNR>(m_nnr[0])) * m_sampleRate / kProcessingRate;
    // Each resampler reports its group delay in ITS OWN source-rate samples:
    // the upsampler's are already the configured rate, the downsampler's are
    // at the processing rate and need converting.
    if (m_up[0]) {
        frames += m_up[0]->groupDelayInputFrames();
    }
    if (m_down[0]) {
        frames += m_down[0]->groupDelayInputFrames() * m_sampleRate / kProcessingRate;
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
    const int requested = m_requestedModel.load();
    const bool switchModel = requested != m_appliedModel.load();
    for (void* instance : m_nnr) {
        auto nnr = static_cast<NNR>(instance);
        setMaskFloor_nnr(nnr, Nnr::maskFloorForStrength(m_strength.load()));
        setAlpha_nnr(nnr, m_alpha.load());
        setAlphaKnee_nnr(nnr, m_alphaKnee.load());
        setTau_nnr(nnr, m_tau.load());
        setMaxGain_nnr(nnr, m_maxGain.load());
        setSmooth_nnr(nnr, m_smoothAttackMs.load(), m_smoothReleaseMs.load());
        if (switchModel) {
            // Reports the slot actually in use, which differs from the
            // request when this build has no model there. Both channels load
            // from the same build, so they land on the same slot.
            m_appliedModel.store(setModel_nnr(nnr, requested));
        }
    }
}

QByteArray NnrFilter::process(const QByteArray& pcmStereo)
{
    if (!isValid() || m_blockFrames <= 0 || pcmStereo.isEmpty()) {
        return pcmStereo;
    }

    applyPendingParameters();

    const auto* src = reinterpret_cast<const float*>(pcmStereo.constData());
    const int stereoFrames = pcmStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // Both channels see the same sample counts through identically configured
    // resamplers, so they reach the same whole-block count and the two NNR
    // instances advance in lockstep.
    int completeBlocks = std::numeric_limits<int>::max();
    std::array<int, 2> totalAccumSamples{0, 0};
    for (int channel = 0; channel < 2; ++channel) {
        // 1. Split out this channel, then resample up for the 24 kHz path only.
        auto& channelInput = m_channelInput[channel];
        channelInput.resize(stereoFrames);
        for (int i = 0; i < stereoFrames; ++i) {
            channelInput[i] = src[i * 2 + channel];
        }
        QByteArray input48k = m_up[channel]
            ? m_up[channel]->process(channelInput.data(), stereoFrames)
            : QByteArray(reinterpret_cast<const char*>(channelInput.data()),
                         stereoFrames * static_cast<int>(sizeof(float)));

        // 2. Accumulate to whole blocks — WDSP's block size cannot change
        //    without rebuilding the block, its FFTW plans and both models.
        const int samples48k = input48k.size() / static_cast<int>(sizeof(float));
        const int prevAccumSamples =
            m_inAccum[channel].size() / static_cast<int>(sizeof(float));
        m_inAccum[channel].append(input48k);
        totalAccumSamples[channel] = prevAccumSamples + samples48k;
        completeBlocks = std::min(completeBlocks,
                                  totalAccumSamples[channel] / m_blockFrames);
    }
    if (completeBlocks <= 0) {
        return {};
    }

    const int consumedSamples = completeBlocks * m_blockFrames;
    int outputFrames = std::numeric_limits<int>::max();
    for (int channel = 0; channel < 2; ++channel) {
        const auto* accumData = reinterpret_cast<const float*>(m_inAccum[channel].constData());
        auto& processed = m_processed48k[channel];
        processed.resize(static_cast<std::size_t>(consumedSamples));
        auto& blockIn = m_blockIn[channel];
        const auto& blockOut = m_blockOut[channel];

        for (int b = 0; b < completeBlocks; ++b) {
            const float* blockStart = &accumData[b * m_blockFrames];
            // Interleave into I with a silent Q — NNR reads only I.
            for (int i = 0; i < m_blockFrames; ++i) {
                blockIn[i * 2] = static_cast<double>(blockStart[i]);
                blockIn[i * 2 + 1] = 0.0;
            }
            xnnr(static_cast<NNR>(m_nnr[channel]), 0);
            float* out = &processed[static_cast<std::size_t>(b) * m_blockFrames];
            for (int i = 0; i < m_blockFrames; ++i) {
                out[i] = static_cast<float>(blockOut[i * 2]);
            }
        }

        m_inAccum[channel].remove(0, consumedSamples * static_cast<int>(sizeof(float)));

        // 3. Back down for the 24 kHz path.
        m_channelOutput[channel] = m_down[channel]
            ? m_down[channel]->process(processed.data(), consumedSamples)
            : QByteArray(reinterpret_cast<const char*>(processed.data()),
                         consumedSamples * static_cast<int>(sizeof(float)));
        outputFrames = std::min(
            outputFrames,
            static_cast<int>(m_channelOutput[channel].size() / sizeof(float)));
    }
    // Identical resamplers fed identical counts stay in lockstep, so the min
    // above never drops a sample. A mismatch would be a silent, cumulative L/R
    // skew: log it once in release, abort in debug.
    if (m_channelOutput[0].size() != m_channelOutput[1].size() && !m_lockstepWarned) {
        m_lockstepWarned = true;
        qWarning() << "NnrFilter: L/R output lengths diverged"
               << m_channelOutput[0].size() << m_channelOutput[1].size();
    }
    Q_ASSERT(m_channelOutput[0].size() == m_channelOutput[1].size());

    QByteArray output(outputFrames * 2 * static_cast<int>(sizeof(float)),
                      Qt::Uninitialized);
    auto* stereo = reinterpret_cast<float*>(output.data());
    const auto* left = reinterpret_cast<const float*>(m_channelOutput[0].constData());
    const auto* right = reinterpret_cast<const float*>(m_channelOutput[1].constData());
    for (int i = 0; i < outputFrames; ++i) {
        stereo[i * 2] = std::clamp(left[i], -1.0f, 1.0f);
        stereo[i * 2 + 1] = std::clamp(right[i], -1.0f, 1.0f);
    }
    return output;
}

}  // namespace AetherSDR
