#include "TxVoiceProcessor.h"

#include "ClientComp.h"
#include "ClientDeEss.h"
#include "ClientEq.h"
#include "ClientFinalLimiter.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientQuindarTone.h"
#include "ClientReverb.h"
#include "ClientTube.h"
#include "ClientTxTestTone.h"
#include "RNNoiseFilter.h"
#include "Resampler.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
Q_LOGGING_CATEGORY(lcTxVoiceProcessor, "aether.audio.tx.voice")
}

TxVoiceProcessor::TxVoiceProcessor() = default;
TxVoiceProcessor::~TxVoiceProcessor() = default;

bool TxVoiceProcessor::prepare(int inputRate, int maxInputFrames)
{
    if (inputRate <= 0 || maxInputFrames <= 0) {
        return false;
    }

    m_inputRate = inputRate;
    m_maxInputFrames = maxInputFrames;
    if (inputRate != kDspRate) {
        m_inputResampler = std::make_unique<Resampler>(
            inputRate,
            kDspRate,
            maxInputFrames,
            kVoiceSrcTransitionBandPercent);
    } else {
        m_inputResampler.reset();
    }

    m_maxDspFrames = static_cast<int>(
        std::ceil(static_cast<double>(maxInputFrames) * kDspRate / inputRate)) + 32;
    m_outputLeftResampler = std::make_unique<Resampler>(
        kDspRate,
        kTransportRate,
        m_maxDspFrames,
        kVoiceSrcTransitionBandPercent);
    m_outputRightResampler = std::make_unique<Resampler>(
        kDspRate,
        kTransportRate,
        m_maxDspFrames,
        kVoiceSrcTransitionBandPercent);

    m_inputMono.reserve(static_cast<size_t>(maxInputFrames));
    m_outputLeft.reserve(static_cast<size_t>(m_maxDspFrames));
    m_outputRight.reserve(static_cast<size_t>(m_maxDspFrames));
    m_resampledMono48.reserve(m_maxDspFrames * static_cast<int>(sizeof(float)));
    m_resampledLeft24.reserve(
        (m_maxDspFrames / 2 + 32) * static_cast<int>(sizeof(float)));
    m_resampledRight24.reserve(
        (m_maxDspFrames / 2 + 32) * static_cast<int>(sizeof(float)));
    m_rnnoiseOutput48.reserve(
        m_maxDspFrames * kChannels * static_cast<int>(sizeof(float)));
    m_work48.reserve(m_maxDspFrames * kChannels * static_cast<int>(sizeof(float)));
    m_normalized48.reserve(m_work48.capacity());
    m_postStrip48.reserve(m_work48.capacity());
    m_transportFloat.reserve(
        (m_maxDspFrames / 2 + 32) * kChannels * static_cast<int>(sizeof(float)));
    prepareProcessors();
    m_prepared = true;
    reset();
    return true;
}

void TxVoiceProcessor::prepareProcessors()
{
    if (m_processors.eq) {
        m_processors.eq->prepare(kDspRate);
    }
    if (m_processors.comp) {
        m_processors.comp->prepare(kDspRate);
    }
    if (m_processors.gate) {
        m_processors.gate->prepare(kDspRate);
    }
    if (m_processors.deEss) {
        m_processors.deEss->prepare(kDspRate);
    }
    if (m_processors.tube) {
        m_processors.tube->prepare(kDspRate);
    }
    if (m_processors.pudu) {
        m_processors.pudu->prepare(kDspRate);
    }
    if (m_processors.reverb) {
        m_processors.reverb->prepare(kDspRate);
    }
    if (m_processors.finalLimiter) {
        m_processors.finalLimiter->prepare(kDspRate);
    }
    if (m_processors.testTone) {
        m_processors.testTone->prepare(kDspRate);
    }
    if (m_processors.quindar) {
        m_processors.quindar->prepare(kDspRate);
    }
}

void TxVoiceProcessor::reset()
{
    m_ditherState = kDitherSeed;
    m_warnedEgressFrameMismatch = false;
    m_egressRecoveryPending = false;
    if (m_inputResampler) {
        m_inputResampler->reset();
    }
    if (m_outputLeftResampler) {
        m_outputLeftResampler->reset();
    }
    if (m_outputRightResampler) {
        m_outputRightResampler->reset();
    }
    if (m_processors.eq) {
        m_processors.eq->reset();
    }
    if (m_processors.comp) {
        m_processors.comp->reset();
    }
    if (m_processors.gate) {
        m_processors.gate->reset();
    }
    if (m_processors.deEss) {
        m_processors.deEss->reset();
    }
    if (m_processors.tube) {
        m_processors.tube->reset();
    }
    if (m_processors.pudu) {
        m_processors.pudu->reset();
    }
    if (m_processors.reverb) {
        m_processors.reverb->reset();
    }
    if (m_processors.finalLimiter) {
        m_processors.finalLimiter->reset();
    }
    if (m_processors.testTone) {
        m_processors.testTone->reset();
    }
    if (m_processors.quindar) {
        m_processors.quindar->reset();
    }
    if (auto* rnnoise = m_rnnoise.load(std::memory_order_acquire);
        m_rnnoiseEnabled.load(std::memory_order_relaxed) && rnnoise) {
        rnnoise->reset();
    }
    m_work48.resize(0);
    m_resampledMono48.resize(0);
    m_resampledLeft24.resize(0);
    m_resampledRight24.resize(0);
    m_rnnoiseOutput48.resize(0);
    m_normalized48.resize(0);
    m_postStrip48.resize(0);
    m_transportFloat.resize(0);
}

void TxVoiceProcessor::setProcessors(const Processors& processors) noexcept
{
    m_processors = processors;
    if (m_prepared) {
        prepareProcessors();
    }
}

void TxVoiceProcessor::setStageOrder(uint64_t packedStages) noexcept
{
    m_packedStages = packedStages;
}

void TxVoiceProcessor::setRnnoiseEnabled(bool enabled) noexcept
{
    m_rnnoiseEnabled.store(enabled, std::memory_order_relaxed);
}

void TxVoiceProcessor::setRnnoise(RNNoiseFilter* rnnoise) noexcept
{
    m_rnnoise.store(rnnoise, std::memory_order_release);
}

void TxVoiceProcessor::setMicGain(float gain) noexcept
{
    m_micGain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.0f) : 1.0f;
}

void TxVoiceProcessor::setMeasurementCaptureEnabled(bool enabled) noexcept
{
    m_captureMeasurements = enabled;
    if (!enabled) {
        m_normalized48.resize(0);
        m_postStrip48.resize(0);
    }
}

bool TxVoiceProcessor::processCapturedInt16(QByteArray& canonicalInputOutput)
{
    if (!m_prepared || canonicalInputOutput.isEmpty()) {
        return false;
    }
    const int inputFrames = canonicalInputOutput.size()
        / (kChannels * static_cast<int>(sizeof(int16_t)));
    if (inputFrames <= 0) {
        return false;
    }

    // m_maxInputFrames is the expected callback size used for preparation and
    // steady-state allocation. Device callbacks can exceed it after a
    // scheduling stall; the stateful resamplers already process such input in
    // bounded chunks, and the scratch buffers may grow for this exceptional
    // case. Process the delayed audio now instead of creating a silent hole.

    const auto* input = reinterpret_cast<const int16_t*>(
        canonicalInputOutput.constData());
    m_inputMono.resize(static_cast<size_t>(inputFrames));
    for (int frame = 0; frame < inputFrames; ++frame) {
        m_inputMono[static_cast<size_t>(frame)] = input[frame * 2] / 32768.0f;
    }

    return processCapturedMono(inputFrames, canonicalInputOutput);
}

bool TxVoiceProcessor::processCapturedFloat32(QByteArray& canonicalInputOutput)
{
    if (!m_prepared || canonicalInputOutput.isEmpty()) {
        return false;
    }
    const int inputFrames = canonicalInputOutput.size()
        / (kChannels * static_cast<int>(sizeof(float)));
    if (inputFrames <= 0) {
        return false;
    }

    // No scaling and no rounding: the device already handed us the engine's
    // float mix. This is the single line the whole change exists for — the
    // Int16 route reaches the same place via /32768.0f, having first been
    // quantized on the way out of the host audio engine.
    //
    // Scrub non-finite samples HERE rather than relying on processWorkBuffer()'s
    // scrub at the egress SRC. This is the one place the float entry point is
    // structurally weaker than the Int16 one: processCapturedInt16() cannot
    // produce NaN/Inf, this can, and processCapturedMono() runs the stateful
    // INGRESS resampler before that scrub is reached — so a single NaN from a
    // misbehaving driver would poison the ingress SRC permanently. Same failure
    // mode testNonFiniteSamplesCannotPoisonEgressSrc guards on the egress side.
    // The engine path is already safe because canonicalizeFloat32ToMonoStereo()
    // applies finiteOrZero(), but this is a public method and must not depend on
    // an invariant asserted in another file.
    const auto* input = reinterpret_cast<const float*>(
        canonicalInputOutput.constData());
    m_inputMono.resize(static_cast<size_t>(inputFrames));
    for (int frame = 0; frame < inputFrames; ++frame) {
        const float sample = input[frame * 2];
        m_inputMono[static_cast<size_t>(frame)] =
            std::isfinite(sample) ? sample : 0.0f;
    }

    return processCapturedMono(inputFrames, canonicalInputOutput);
}

bool TxVoiceProcessor::processCapturedMono(int inputFrames,
                                           QByteArray& transportInt16Output)
{
    const float* mono48Samples = m_inputMono.data();
    int frames48 = inputFrames;
    if (m_inputResampler) {
        frames48 = m_inputResampler->process(
            m_inputMono.data(), inputFrames, m_resampledMono48);
        mono48Samples = reinterpret_cast<const float*>(
            m_resampledMono48.constData());
    }
    if (frames48 <= 0) {
        return false;
    }

    m_work48.resize(frames48 * kChannels * static_cast<int>(sizeof(float)));
    auto* work = reinterpret_cast<float*>(m_work48.data());
    for (int frame = 0; frame < frames48; ++frame) {
        work[frame * 2] = mono48Samples[frame];
        work[frame * 2 + 1] = mono48Samples[frame];
    }
    return processWorkBuffer(frames48, transportInt16Output);
}

bool TxVoiceProcessor::processFloat48(const float* interleavedStereo,
                                      int frames,
                                      QByteArray& transportInt16Output)
{
    if (!m_prepared || !interleavedStereo || frames <= 0
        || frames > m_maxDspFrames) {
        return false;
    }
    m_work48.resize(frames * kChannels * static_cast<int>(sizeof(float)));
    std::copy_n(interleavedStereo, frames * kChannels,
                reinterpret_cast<float*>(m_work48.data()));
    return processWorkBuffer(frames, transportInt16Output);
}

bool TxVoiceProcessor::processWorkBuffer(int frames48,
                                         QByteArray& transportInt16Output)
{
    if (m_captureMeasurements) {
        m_normalized48 = m_work48;
    }

    // One acquire load for the whole block: re-reading the member between the
    // guard and the call would reintroduce the window this is here to close.
    auto* const rnnoise = m_rnnoise.load(std::memory_order_acquire);
    if (m_rnnoiseEnabled.load(std::memory_order_relaxed) && rnnoise
        && rnnoise->isValid()) {
        rnnoise->process48kStereo(m_work48, m_rnnoiseOutput48);
        m_work48.swap(m_rnnoiseOutput48);
    }

    auto* work = reinterpret_cast<float*>(m_work48.data());
    if (m_processors.testTone && m_processors.testTone->isEnabled()) {
        m_processors.testTone->process(work, frames48, kChannels);
    }
    processChannelStrip(m_work48);
    if (m_captureMeasurements) {
        m_postStrip48 = m_work48;
    }

    work = reinterpret_cast<float*>(m_work48.data());
    if (m_micGain < 0.999f) {
        for (int sample = 0; sample < frames48 * kChannels; ++sample) {
            work[sample] *= m_micGain;
        }
    }
    if (m_processors.quindar) {
        m_processors.quindar->process(work, frames48, kChannels);
    }
    if (m_processors.finalLimiter) {
        m_processors.finalLimiter->process(work, frames48, kChannels);
    }

    // A malformed parameter transition or third-party DSP failure must not
    // poison the stateful egress SRC with NaN/Inf values. Replace only
    // non-finite samples; finite over-range samples are still measured and
    // clipped once at the integer transport boundary below.
    for (int sample = 0; sample < frames48 * kChannels; ++sample) {
        if (!std::isfinite(work[sample])) {
            work[sample] = 0.0f;
        }
    }

    m_outputLeft.resize(static_cast<size_t>(frames48));
    m_outputRight.resize(static_cast<size_t>(frames48));
    for (int frame = 0; frame < frames48; ++frame) {
        m_outputLeft[static_cast<size_t>(frame)] = work[frame * 2];
        m_outputRight[static_cast<size_t>(frame)] = work[frame * 2 + 1];
    }
    const int leftOutputFrames = m_outputLeftResampler->process(
        m_outputLeft.data(), frames48, m_resampledLeft24);
    const int rightOutputFrames = m_outputRightResampler->process(
        m_outputRight.data(), frames48, m_resampledRight24);
    const int outputFrames = reconcileEgressFrameCounts(
        leftOutputFrames, rightOutputFrames);
    if (outputFrames <= 0) {
        m_transportFloat.resize(0);
        transportInt16Output.resize(0);
        return false;
    }

    m_transportFloat.resize(
        outputFrames * kChannels * static_cast<int>(sizeof(float)));
    transportInt16Output.resize(
        outputFrames * kChannels * static_cast<int>(sizeof(int16_t)));
    auto* outputFloat = reinterpret_cast<float*>(m_transportFloat.data());
    auto* outputInt16 = reinterpret_cast<int16_t*>(
        transportInt16Output.data());
    const auto* left = reinterpret_cast<const float*>(m_resampledLeft24.constData());
    const auto* right = reinterpret_cast<const float*>(m_resampledRight24.constData());
    for (int frame = 0; frame < outputFrames; ++frame) {
        outputFloat[frame * 2] = left[frame];
        outputFloat[frame * 2 + 1] = right[frame];
        const bool exactDigitalSilence = left[frame] == 0.0f
            && right[frame] == 0.0f;
        // Advance the streaming PRNG for every frame so dither remains
        // block-partition invariant. Exact zero has no quantization error to
        // decorrelate, so preserve the transport's digital-silence code.
        const float ditherLsb = nextTpdfDitherLsb();
        if (exactDigitalSilence) {
            outputInt16[frame * 2] = 0;
            outputInt16[frame * 2 + 1] = 0;
            continue;
        }
        for (int channel = 0; channel < kChannels; ++channel) {
            outputInt16[frame * 2 + channel] = quantizeTransportSample(
                outputFloat[frame * 2 + channel], ditherLsb);
        }
    }
    return true;
}

int TxVoiceProcessor::reconcileEgressFrameCounts(
    int leftFrames, int rightFrames)
{
    if (leftFrames == rightFrames) {
        m_warnedEgressFrameMismatch = false;
        return leftFrames;
    }

    if (!m_warnedEgressFrameMismatch) {
        qCWarning(lcTxVoiceProcessor)
            << "TX egress SRC channel frame mismatch: left=" << leftFrames
            << "right=" << rightFrames
            << "-- transmitting the common prefix and requesting recovery"
               " between callbacks";
        m_warnedEgressFrameMismatch = true;
    }

    // Both resamplers consumed the same input interval, so their common
    // prefix belongs to the current aligned timeline. Preserve that prefix,
    // discard only the unmatched tail, and ask the owner to reset both
    // histories between callbacks so the dropped tail cannot become a
    // permanent L/R offset. Resampler::reset() performs prewarming and must not
    // run from this realtime processing call.
    m_egressRecoveryPending = true;
    return std::min(leftFrames, rightFrames);
}

void TxVoiceProcessor::recoverEgressAfterMismatch()
{
    if (!m_egressRecoveryPending) {
        return;
    }
    m_outputLeftResampler->reset();
    m_outputRightResampler->reset();
    m_egressRecoveryPending = false;
    m_warnedEgressFrameMismatch = false;
}

uint32_t TxVoiceProcessor::nextDitherRandom24() noexcept
{
    // SplitMix64 is deterministic, allocation-free, and has no zero-state
    // trap. The upper 24 bits provide more than enough resolution for dither
    // applied at an int16 boundary.
    m_ditherState += 0x9E3779B97F4A7C15ULL;
    uint64_t mixed = m_ditherState;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    mixed ^= mixed >> 31U;
    return static_cast<uint32_t>(mixed >> 40U);
}

float TxVoiceProcessor::nextTpdfDitherLsb() noexcept
{
    constexpr float kRandom24Scale = 1.0f / 16777216.0f;
    const int32_t first = static_cast<int32_t>(nextDitherRandom24());
    const int32_t second = static_cast<int32_t>(nextDitherRandom24());
    return static_cast<float>(first - second) * kRandom24Scale;
}

int16_t TxVoiceProcessor::quantizeTransportSample(
    float sample, float ditherLsb) noexcept
{
    const double dithered = static_cast<double>(sample) * 32768.0
        + static_cast<double>(ditherLsb);
    const double saturated = std::clamp(dithered, -32768.0, 32767.0);
    return static_cast<int16_t>(std::lround(saturated));
}

void TxVoiceProcessor::processChannelStrip(QByteArray& float48Stereo) noexcept
{
    auto* samples = reinterpret_cast<float*>(float48Stereo.data());
    const int frames = float48Stereo.size()
        / (kChannels * static_cast<int>(sizeof(float)));
    for (int index = 0; index < kMaxStages; ++index) {
        const Stage stage = static_cast<Stage>(
            (m_packedStages >> (index * 8)) & 0xFF);
        switch (stage) {
            case Stage::None:
                return;
            case Stage::Eq:
                if (m_processors.eq && m_processors.eq->isEnabled()) {
                    m_processors.eq->process(samples, frames, kChannels);
                }
                if (m_processors.eqTap) {
                    m_processors.eqTap(m_processors.eqTapContext, samples, frames);
                }
                break;
            case Stage::Comp:
                if (m_processors.comp
                    && (m_processors.comp->isEnabled()
                        || m_processors.comp->driveDb() > 0.0f
                        || m_processors.comp->phaseRotatorStages() > 0
                        || m_processors.comp->limiterEnabled())) {
                    m_processors.comp->process(samples, frames, kChannels);
                }
                break;
            case Stage::Gate:
                if (m_processors.gate && m_processors.gate->isEnabled()) {
                    m_processors.gate->process(samples, frames, kChannels);
                }
                break;
            case Stage::DeEss:
                if (m_processors.deEss && m_processors.deEss->isEnabled()) {
                    m_processors.deEss->process(samples, frames, kChannels);
                }
                break;
            case Stage::Tube:
                if (m_processors.tube && m_processors.tube->isEnabled()) {
                    m_processors.tube->process(samples, frames, kChannels);
                }
                break;
            case Stage::Enh:
                if (m_processors.pudu && m_processors.pudu->isEnabled()) {
                    m_processors.pudu->process(samples, frames, kChannels);
                }
                break;
            case Stage::Reverb:
                if (m_processors.reverb && m_processors.reverb->isEnabled()) {
                    m_processors.reverb->process(samples, frames, kChannels);
                }
                break;
        }
    }
}

const QByteArray& TxVoiceProcessor::transportFloat32Stereo() const noexcept
{
    return m_transportFloat;
}

const QByteArray& TxVoiceProcessor::normalizedFloat48Stereo() const noexcept
{
    return m_normalized48;
}

const QByteArray& TxVoiceProcessor::postChannelStripFloat48Stereo() const noexcept
{
    return m_postStrip48;
}

int TxVoiceProcessor::latencyFrames() const noexcept
{
    int frames = 0;
    if (m_inputResampler) {
        frames += static_cast<int>(std::lround(
            static_cast<double>(m_inputResampler->groupDelayInputFrames())
            * kDspRate / m_inputRate));
    }
    if (m_outputLeftResampler) {
        frames += static_cast<int>(std::lround(
            static_cast<double>(m_outputLeftResampler->groupDelayInputFrames())
            * kDspRate / m_outputLeftResampler->srcRate()));
    }

    auto* const rnnoise = m_rnnoise.load(std::memory_order_acquire);
    frames += m_rnnoiseEnabled.load(std::memory_order_relaxed) && rnnoise
            && rnnoise->isValid()
        ? 480
        : 0;
    for (int index = 0; index < kMaxStages; ++index) {
        const Stage stage = static_cast<Stage>(
            (m_packedStages >> (index * 8)) & 0xFF);
        if (stage == Stage::None) {
            break;
        }
        if (stage == Stage::Gate && m_processors.gate
            && m_processors.gate->isEnabled()) {
            frames += static_cast<int>(std::lround(
                m_processors.gate->lookaheadMs() * kDspRate / 1000.0f));
        }
    }
    return frames;
}

} // namespace AetherSDR
