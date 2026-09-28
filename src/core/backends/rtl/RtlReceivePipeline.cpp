#include "RtlReceivePipeline.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace AetherSDR::rtl {
namespace {
std::optional<std::uint64_t> mixerOrigin(std::uint64_t firstSample, double rateHz) noexcept
{
    // The extractor accepts only integral, bounded achieved rates. Validate
    // before casting and share its alignment so these clocks cannot diverge.
    if (!std::isfinite(rateHz) || rateHz != std::floor(rateHz)
        || rateHz < 225001 || rateHz > 3000000) { return std::nullopt; }
    const auto rate = static_cast<std::uint64_t>(rateHz);
    const auto captureFirst = RtlRfExtractor::alignedCaptureFirst(firstSample, rate, 48000);
    if (!captureFirst) { return std::nullopt; }
    const std::uint64_t common = std::gcd(rate, std::uint64_t{48000});
    const std::uint64_t periods = *captureFirst / (rate / common);
    const std::uint64_t framesPerPeriod = 48000 / common;
    constexpr std::uint64_t margin = RtlAudioMixer::kQuantum + RtlAudioMixer::kDeadlineFrames;
    if (periods > (std::numeric_limits<std::uint64_t>::max() - margin) / framesPerPeriod) {
        return std::nullopt;
    }
    return periods * framesPerPeriod;
}
WdspChannel::Mode dspMode(RtlCaptureTransaction::Mode mode)
{
    using M = RtlCaptureTransaction::Mode;
    switch (mode) {
    case M::Am: return WdspChannel::Mode::Am;
    case M::Sam: return WdspChannel::Mode::Sam;
    case M::Fm: case M::Fmn: return WdspChannel::Mode::Fm;
    case M::Wfm: return WdspChannel::Mode::Wbfm;
    case M::Lsb: return WdspChannel::Mode::Lsb;
    case M::Cw: return WdspChannel::Mode::Cwu;
    case M::Cwr: return WdspChannel::Mode::Cwl;
    default: return WdspChannel::Mode::Usb;
    }
}
}
RtlReceivePipeline::RtlReceivePipeline(std::size_t capacity, bool enableWfm)
    : m_enableWfm(enableWfm), m_registry({8, capacity, std::min<std::size_t>(32, capacity * 2)})
{
    for (auto& monitor : m_monitor) { monitor.store(100 | (50 << 8)); }
}
RtlReceivePipeline::Submission RtlReceivePipeline::prepareDetailed(
    const Transaction::State& state, bool resetCapture, bool verifiedRollback)
{
    if (!m_registry || state.receivers.empty() || state.receivers.size() > 8) { return Submission::Failed; }
    // Refusal must not replace an already prepared bank or consume its epoch.
    // Validate the complete input before registry/session mutations, including
    // the legacy path (which submits an empty receiver bank).
    std::array<bool, 8> validated{};
    for (const auto& receiver : state.receivers) {
        const int id = receiver.passband.stableId;
        if (id < 0 || id >= 8 || validated[id]
            || receiver.mode < Transaction::Mode::Am || receiver.mode > Transaction::Mode::Cwr
            || receiver.audioGain < 0
            || receiver.audioGain > 100 || receiver.audioPan < 0 || receiver.audioPan > 100
            || receiver.squelchLevel < 0 || receiver.squelchLevel > 100
            || (receiver.wfmDeemphasisUs != 50 && receiver.wfmDeemphasisUs != 75)
            || (receiver.mode == Transaction::Mode::Wfm && receiver.squelchEnabled)) { return Submission::Failed; }
        // Parking changes reception membership, not the configured graph's
        // admission contract. A legacy recipe cannot hide outside the capture
        // and then resume alongside a native 48 kHz receiver.
        const bool native = receiver.mode == Transaction::Mode::Fm
            || receiver.mode == Transaction::Mode::Fmn
            || (m_enableWfm && receiver.mode == Transaction::Mode::Wfm);
        if (!native && state.receivers.size() != 1) { return Submission::Failed; }
        if (receiver.mode == Transaction::Mode::Wfm && receiver.wfmHdStereo) {
            if (!m_enableWfm) { return Submission::Failed; }
#ifndef AETHER_ENABLE_NRSC5
            return Submission::Failed;
#else
            if (receiver.hdProgram < 0 || receiver.hdProgram >= 8 || state.receivers.size() != 1) {
                return Submission::Failed;
            }
#endif
        }
        validated[id] = true;
    }
    const unsigned faults = m_faults.load(std::memory_order_acquire);
    std::array<bool, 8> receiving{};
    bool legacy = false;
    bool wfmTransition = false;
    std::uint8_t receivingMask = 0;
    for (int id : state.receivingIds) {
        if (id < 0 || id >= 8 || receiving[id] || !validated[id]) { return Submission::Failed; }
        receiving[id] = true;
        receivingMask |= static_cast<std::uint8_t>(1u << id);
        const auto receiver = std::ranges::find_if(state.receivers, [id](const auto& value) {
            return value.passband.stableId == id;
        });
        legacy = legacy || (receiver->mode != Transaction::Mode::Fm
            && receiver->mode != Transaction::Mode::Fmn
            && !(m_enableWfm && receiver->mode == Transaction::Mode::Wfm));
        if (legacy && receiver->squelchEnabled) { return Submission::Failed; }
        const auto& prior = m_specs[id];
        const bool wide = m_enableWfm && receiver->mode == Transaction::Mode::Wfm;
        if (wide || prior.dsp.wbfmReceive) {
            const auto deemphasis = receiver->wfmDeemphasisUs == 50
                ? WdspChannel::WbfmReceive::Deemphasis::Us50
                : WdspChannel::WbfmReceive::Deemphasis::Us75;
            wfmTransition |= (wide && (faults & (1u << id)))
                || !wide || !prior.dsp.wbfmReceive
                || prior.passband != receiver->passband
                || prior.dsp.wbfmReceive->deemphasis != deemphasis
                || prior.dsp.wbfmReceive->forceMono != receiver->wfmForceMono
                || prior.hdFm.has_value() != receiver->wfmHdStereo
                || (prior.hdFm && prior.hdFm->program != receiver->hdProgram);
        }
    }
    if (legacy && (state.receivers.size() != 1 || state.receivingIds.size() != 1)) { return Submission::Failed; }
    // WFM changes the causal decoder graph even when final PCM stays 48 kHz.
    // Give the speaker a discontinuity too, retiring the previous graph's
    // queued waveform and client-effect history instead of appending its tail.
    // A withdrawn WFM receiver has lost that graph even if its recipe is unchanged.
    const bool advanceCaptureEpoch = resetCapture || legacy != m_legacy
        || receivingMask != m_receivingMask || wfmTransition;
    if (advanceCaptureEpoch && m_nextEpoch == std::numeric_limits<std::uint64_t>::max()) { return Submission::Failed; }
    auto epochs = m_epochs;
    auto specs = m_specs;
    if (!m_session) {
        m_session = m_registry.beginSession(state.capture);
        m_reader = m_registry.attachReader();
        if (!m_session || !m_reader) { return Submission::Failed; }
    }
    std::array<RtlReceiverRegistry::ReceiverSpec, 8> desired;
    std::array<bool, 8> used{};
    std::size_t count = 0;
    if (!legacy) {
        for (const auto& receiver : state.receivers) {
            const int id = receiver.passband.stableId;
            if (!receiving[id]) { continue; }
            if (id < 0 || id >= 8 || used[id]) { return Submission::Failed; }
            used[id] = true;
            if (verifiedRollback && !m_handles[id]) { m_handles[id] = m_registry.currentHandle(id); }
            if (m_handles[id] != m_registry.currentHandle(id)) { m_handles[id].reset(); }
            if (!m_handles[id]) { m_handles[id] = m_registry.reserveSlot(id); }
            if (!m_handles[id]) {
                if (m_registry.slotAwaitingRetirement(id)) { return Submission::RetryRetiringSlot; }
                // A destructor may have completed between the failed reserve
                // and the status query. Retry that narrow race once.
                m_handles[id] = m_registry.reserveSlot(id);
                if (!m_handles[id]) { return Submission::Failed; }
            }
            auto spec = RtlReceiverRegistry::ReceiverSpec{};
            spec.handle = *m_handles[id]; spec.passband = receiver.passband;
            spec.capture = state.capture; spec.extractRf = true;
            spec.dsp.mode = dspMode(receiver.mode);
            if (receiver.mode == Transaction::Mode::Wfm) {
                spec.dsp.inputSampleRate = 384000;
                spec.dsp.inputBlockSize = 2048;
                spec.dsp.dspSampleRate = 192000;
                spec.dsp.dspBlockSize = 1024;
                spec.dsp.outputSampleRate = 48000;
                spec.dsp.wbfmReceive = WdspChannel::WbfmReceive{};
                spec.dsp.wbfmReceive->forceMono = receiver.wfmForceMono;
                spec.dsp.wbfmReceive->deemphasis = receiver.wfmDeemphasisUs == 50
                    ? WdspChannel::WbfmReceive::Deemphasis::Us50
                    : WdspChannel::WbfmReceive::Deemphasis::Us75;
            } else {
                spec.dsp.fmReceive = WdspChannel::FmReceive{};
                spec.dsp.fmDeviationHz = receiver.mode == Transaction::Mode::Fmn ? 2500.0 : 5000.0;
            }
            if (receiver.mode == Transaction::Mode::Wfm && receiver.wfmHdStereo) {
                spec.hdFm = HdFmRecipe{receiver.hdProgram};
            }
            spec.dsp.filterLowHz = receiver.passband.filterLowHz;
            spec.dsp.filterHighHz = receiver.passband.filterHighHz;
            if (resetCapture || (faults & (1u << id)) || m_specs[id].handle != spec.handle
                || m_specs[id].passband != spec.passband || m_specs[id].dsp != spec.dsp
                || m_specs[id].hdFm != spec.hdFm) {
                if (epochs[id] == std::numeric_limits<std::uint64_t>::max()) { return Submission::Failed; }
                ++epochs[id];
            }
            spec.epoch = epochs[id];
            specs[id] = spec;
            desired[count++] = spec;
        }
    }
    const auto result = verifiedRollback
        ? m_registry.submitVerifiedRollback(state.capture, std::span(desired).first(count))
        : m_registry.submit(state.capture, std::span(desired).first(count));
    if (result == RtlReceiverRegistry::Result::Busy) { return Submission::RetryPlannerBusy; }
    if (result != RtlReceiverRegistry::Result::Accepted) { return Submission::Failed; }
    for (std::size_t i = 0; i < used.size(); ++i) { if (!used[i]) { m_handles[i].reset(); } }
    m_epochs = epochs;
    m_specs = specs;
    m_prepared = m_registry.service().requested;
    for (const auto& receiver : state.receivers) {
        m_nextSquelch[receiver.passband.stableId] = {receiver.squelchEnabled, receiver.squelchLevel};
        m_nextMonitor[receiver.passband.stableId] = static_cast<unsigned>(receiver.audioGain
            | (receiver.audioPan << 8) | (receiver.audioMute ? 1 << 16 : 0));
    }
    m_nextToken = state.token; m_nextCapture = state.capture; m_nextLegacy = legacy;
    m_nextReceivingMask = receivingMask;
    if (advanceCaptureEpoch) { ++m_nextEpoch; }
    return Submission::Accepted;
}
RtlReceivePipeline::Preparation RtlReceivePipeline::service()
{
    const auto status = m_registry.service();
    if (status.result != RtlReceiverRegistry::Result::Accepted) { return Preparation::Failed; }
    return status.prepared == m_prepared ? Preparation::Ready : Preparation::Pending;
}
bool RtlReceivePipeline::adopt() noexcept
{
    if (!m_reader.adoptPrepared(m_session, m_nextCapture, m_prepared)) { return false; }
    if (m_token.session != m_nextToken.session || m_captureEpoch != m_nextEpoch) {
        m_mixerOrigin.reset();
        m_hdSpeakerEpoch = 0; m_hdAudioValid = false; m_hdSpeakerIdentities.fill({});
    }
    const bool resetSquelch = m_captureEpoch != m_nextEpoch;
    for (std::size_t slot = 0; slot < m_monitor.size(); ++slot) {
        m_monitor[slot].store(m_nextMonitor[slot], std::memory_order_relaxed);
        m_squelchConfig[slot] = m_nextSquelch[slot];
        m_squelch[slot].configure(m_nextSquelch[slot].enabled, m_nextSquelch[slot].level, resetSquelch);
    }
    if (resetSquelch) { m_spectrumFresh = false; m_squelchEpoch.fill(0); }
    m_token = m_nextToken; m_capture = m_nextCapture;
    m_captureEpoch = m_nextEpoch; m_legacy = m_nextLegacy;
    m_receivingMask = m_nextReceivingMask;
    m_faults.store(0, std::memory_order_release); // old bank faults cannot request a second reset
    return true;
}
bool RtlReceivePipeline::process(std::uint64_t firstSample,
    std::span<const std::complex<float>> samples) noexcept
{
    return m_reader.processBlock({m_session, m_capture, firstSample, false, samples,
        m_token.session, m_token.revision}, *this,
        m_reader.activeRevision());
}
void RtlReceivePipeline::stop() { m_reader.stop(); }
void RtlReceivePipeline::observeSpectrum(std::span<const float> bins, std::uint64_t firstSample) noexcept
{
    if (bins.size() != m_spectrum.size()) { return; }
    std::copy(bins.begin(), bins.end(), m_spectrum.begin());
    m_spectrumFirstSample = firstSample; m_spectrumFresh = true;
}
void RtlReceivePipeline::setMonitor(int slot, int gain, int pan, bool mute) noexcept
{
    if (slot < 0 || slot >= 8) { return; }
    m_monitor[slot].store(static_cast<unsigned>(std::clamp(gain, 0, 100)
        | (std::clamp(pan, 0, 100) << 8) | (mute ? 1 << 16 : 0)), std::memory_order_relaxed);
}
void RtlReceivePipeline::process(const RtlReceiverRegistry::SampleBlock& block,
    std::span<const RtlReceiverRegistry::ReceiverView> views) noexcept
{
    if (m_legacy) { return; }
    if (m_startupTrace && !m_startupTrace->nativeFirstNs) {
        m_startupTrace->markProducer(RtlStartupTrace::FirstNativeBlock, m_token.session,
            m_token.revision, block.firstSample, -1, 0, block.samples.size());
    }
    std::array<RtlAudioMixer::Input, 8> inputs;
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto& spec = *views[i].spec;
        const unsigned monitor = m_monitor[spec.handle.slot].load(std::memory_order_relaxed);
        m_traceStableIds[spec.handle.slot] = spec.passband.stableId;
        inputs[i] = {spec.handle.slot, spec.handle.instance, spec.hdFm ? m_hdSpeakerEpoch : spec.epoch,
            (monitor & 255) / 100.0f, ((monitor >> 8) & 255) / 100.0f, (monitor & (1 << 16)) != 0};
    }
    const auto clock = [this](std::uint64_t sample) {
        return static_cast<std::uint64_t>(static_cast<long double>(sample) * 48000 / m_capture.achievedSampleRateHz);
    };
    m_traceCaptureFirst = block.firstSample;
    m_traceCaptureFrames = block.samples.size();
    m_traceCaptureClock = clock(block.firstSample + block.samples.size());
    if (!m_mixerOrigin) { m_mixerOrigin = mixerOrigin(block.firstSample, m_capture.achievedSampleRateHz); }
    if (!m_mixerOrigin || !m_mixer.configure(m_token.session, m_captureEpoch,
            std::span(inputs).first(views.size()), *m_mixerOrigin)) {
        // No stale map/audio may survive a rejected configuration. This is a
        // defensive invariant failure: normal registry handles and validated
        // transaction tokens cannot reach it. Repair through the existing owner.
        m_mixer.reset();
        m_mixerOrigin.reset();
        m_mixerConfigurationFailures.fetch_add(1, std::memory_order_relaxed);
        m_observed.store(true, std::memory_order_release);
        m_faults.fetch_or(0xff, std::memory_order_release);
        return;
    }
    for (const auto& view : views) {
        const int slot = view.spec->handle.slot;
        auto& gate = m_squelch[slot];
        if (m_squelchEpoch[slot] != view.spec->epoch) {
            gate.configure(m_squelchConfig[slot].enabled, m_squelchConfig[slot].level, true);
            m_squelchEpoch[slot] = view.spec->epoch;
        }
        if (m_spectrumFresh && !view.spec->dsp.wbfmReceive) {
            const auto& passband = view.spec->passband;
            const double binHz = m_capture.achievedSampleRateHz / m_spectrum.size();
            const double offset = passband.carrierHz - m_capture.centerHz;
            // Include the nearest bin for sub-bin passbands. This is a coarse
            // signal-level gate, not a claim of calibrated in-channel power.
            const int low = std::clamp(static_cast<int>(std::floor(
                (offset + passband.filterLowHz) / binHz + 1024)), 0, 2047);
            const int high = std::clamp(static_cast<int>(std::ceil(
                (offset + passband.filterHighHz) / binHz + 1024)), low, 2047);
            float peak = -120.0f;
            for (int bin = low; bin <= high; ++bin) {
                if (!std::isfinite(m_spectrum[bin])) { peak = std::numeric_limits<float>::quiet_NaN(); break; }
                peak = std::max(peak, m_spectrum[bin]);
            }
            gate.observe(peak, clock(m_spectrumFirstSample));
        }
        if (!view.receiver->processCapture(block, *this)) {
            const unsigned bit = 1u << slot;
            if (!(m_faults.fetch_or(bit, std::memory_order_release) & bit)) {
                m_receiverWithdrawals[slot].fetch_add(1, std::memory_order_relaxed);
                TraceEvent event = traceContext();
                event.slot = slot; event.stableId = view.spec->passband.stableId;
                event.instance = view.spec->handle.instance;
                event.receiverEpoch = view.spec->epoch;
                event.failure = view.receiver->processingFailure();
                enqueueTrace(event);
            }
        }
    }
    m_spectrumFresh = false;
    m_mixer.drain(m_traceCaptureClock, *this);
    m_mixerLate.store(m_mixer.lateFrames(), std::memory_order_relaxed);
    m_mixerRejected.store(m_mixer.rejectedBlocks(), std::memory_order_relaxed);
    m_observed.store(true, std::memory_order_release);
}
RtlReceivePipeline::Diagnostics RtlReceivePipeline::diagnostics() const noexcept
{
    Diagnostics result{m_observed.load(std::memory_order_acquire),
        m_drops.load(std::memory_order_relaxed), m_mixerLate.load(std::memory_order_relaxed),
        m_mixerRejected.load(std::memory_order_relaxed),
        m_mixerConfigurationFailures.load(std::memory_order_relaxed),
        m_traceDrops.load(std::memory_order_relaxed)};
    result.queuedPackets = (m_write.load(std::memory_order_acquire) + kPackets
        - m_read.load(std::memory_order_acquire)) % kPackets;
    result.packetQueueHighWater = m_packetQueueHighWater.load(std::memory_order_relaxed);
    for (std::size_t slot = 0; slot < m_receiverWithdrawals.size(); ++slot) {
        result.receiverWithdrawals[slot] = m_receiverWithdrawals[slot].load(std::memory_order_relaxed);
    }
    return result;
}
void RtlReceivePipeline::audioBlock(const RtlReceiverRegistry::ReceiverSpec& spec, std::uint64_t first,
    std::span<const float> left, std::span<const float> right, bool discontinuity) noexcept
{
    audioBlockWithStatus(spec, first, left, right, discontinuity, std::nullopt);
}
void RtlReceivePipeline::audioBlockWithStatus(const RtlReceiverRegistry::ReceiverSpec& spec,
    std::uint64_t first, std::span<const float> left, std::span<const float> right,
    bool discontinuity, std::optional<AetherSDR::WfmReceptionDiagnostics> reception) noexcept
{
    Packet packet;
    packet.token = m_token; packet.captureEpoch = m_captureEpoch;
    packet.instance = spec.handle.instance; packet.receiverEpoch = spec.epoch;
    packet.slot = spec.handle.slot; packet.firstSample = first;
    packet.frames = left.size(); packet.discontinuity = discontinuity;
    packet.wfmReception = reception;
    if (reception) {
        packet.wfmStereoDetected = reception->pilotLocked
            && !(spec.dsp.wbfmReceive && spec.dsp.wbfmReceive->forceMono);
    }
    if (left.size() != right.size() || left.size() > 1024) { return; }
    std::array<float, 1024> gatedLeft{}, gatedRight{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        // WBFM already applies WDSP's internal automatic squelch and the
        // qualified paired-channel gain before this independent tap.
        const float gain = spec.dsp.wbfmReceive ? 1.0f : m_squelch[packet.slot].gain(first + i);
        gatedLeft[i] = left[i] * gain; gatedRight[i] = right[i] * gain;
        packet.samples[2 * i] = gatedLeft[i]; packet.samples[2 * i + 1] = gatedRight[i];
    }
    enqueue(packet); // independent tap, before gain/mute/pan
    // A retained receiver can finish buffered PCM from before a new speaker
    // epoch. Keep its tap continuous, but do not offer that retired prefix to
    // the new mix. Use the fixed origin, never the advancing playback cursor:
    // genuinely late current-epoch blocks must still fail mixer validation.
    if (m_mixerOrigin && first < *m_mixerOrigin && !left.empty()
        && left.size() <= *m_mixerOrigin - first
        && std::ranges::all_of(std::span(gatedLeft).first(left.size()),
            [](float value) { return std::isfinite(value); })
        && std::ranges::all_of(std::span(gatedRight).first(right.size()),
            [](float value) { return std::isfinite(value); })) {
        return;
    }
    m_mixer.push(packet.slot, packet.instance, packet.receiverEpoch, first,
        std::span(gatedLeft).first(left.size()), std::span(gatedRight).first(right.size()));
}
void RtlReceivePipeline::speakerBlock(std::uint64_t first, std::span<const float> samples, bool discontinuity) noexcept
{
    Packet packet;
    packet.token = m_token; packet.captureEpoch = m_captureEpoch;
    packet.firstSample = first; packet.frames = samples.size() / 2; packet.discontinuity = discontinuity;
    packet.audioEpoch = m_hdSpeakerEpoch;
    if (m_hdSpeakerEpoch) {
        for (std::size_t i = 0; i < packet.frames; ++i) {
            const auto& source = m_hdSpeakerIdentities[(first + i) % RtlAudioMixer::kCapacity];
            if (source.producedMonotonicMs && (packet.producedMonotonicMs == 0
                || source.revision < packet.token.revision
                || (source.revision == packet.token.revision
                    && source.producedMonotonicMs < packet.producedMonotonicMs))) {
                packet.token = {source.sessionId, source.revision};
                packet.producedMonotonicMs = source.producedMonotonicMs;
            }
        }
    }
    std::copy(samples.begin(), samples.end(), packet.samples.begin());
    enqueue(packet);
}
void RtlReceivePipeline::hdObservation(const RtlReceiverRegistry::ReceiverSpec& spec,
    const HdFmRawReception& reception) noexcept
{
    if (!spec.hdFm || reception.sessionId == 0 || reception.revision == 0) { return; }
    // There is initially one HD wide receiver. Temporarily withdrawing its
    // mixer input flushes queued contributions before the next zero/PCM push.
    // HD supplies complete 128-frame quanta, so no old partial tail survives.
    if ((m_hdAudioValid && !reception.audioValid) || m_hdSpeakerEpoch != reception.audioEpoch) {
        m_mixer.configure(m_token.session, m_captureEpoch, {}, m_mixer.nextSample());
        m_hdSpeakerIdentities.fill({});
    }
    m_hdAudioValid = reception.audioValid;
    m_hdSpeakerEpoch = reception.audioEpoch;
    const unsigned write = m_hdWrite.load(std::memory_order_relaxed);
    const unsigned next = (write + 1) % kHdObservations;
    if (next == m_hdRead.load(std::memory_order_acquire)) {
        m_drops.fetch_add(1, std::memory_order_relaxed);
        m_faults.fetch_or(1u << spec.handle.slot, std::memory_order_release);
        return;
    }
    m_hdObservations[write] = {{reception.sessionId, reception.revision}, m_captureEpoch,
        spec.handle.instance, spec.epoch, reception.audioEpoch, spec.handle.slot, reception};
    m_hdWrite.store(next, std::memory_order_release);
}
bool RtlReceivePipeline::takeHdObservation(HdFmObservation& output) noexcept
{
    const unsigned read = m_hdRead.load(std::memory_order_relaxed);
    if (read == m_hdWrite.load(std::memory_order_acquire)) { return false; }
    output = m_hdObservations[read];
    m_hdRead.store((read + 1) % kHdObservations, std::memory_order_release);
    return true;
}
void RtlReceivePipeline::hdDecodedAudio(const RtlReceiverRegistry::ReceiverSpec& spec,
    const HdFmAudioIdentity& identity, std::uint64_t first, std::span<const float> left,
    std::span<const float> right, bool discontinuity) noexcept
{
    if (!spec.hdFm || left.empty() || left.size() != right.size() || left.size() > 1024
        || (m_faults.load(std::memory_order_acquire) & (1u << spec.handle.slot))) { return; }
    Packet packet;
    packet.token = {identity.sessionId, identity.revision}; packet.captureEpoch = m_captureEpoch;
    packet.instance = spec.handle.instance; packet.receiverEpoch = spec.epoch;
    packet.audioEpoch = identity.audioEpoch; packet.producedMonotonicMs = identity.producedMonotonicMs;
    packet.slot = spec.handle.slot; packet.firstSample = first; packet.frames = left.size();
    packet.sampleRateHz = 44100; packet.discontinuity = discontinuity;
    for (std::size_t i = 0; i < left.size(); ++i) {
        packet.samples[2 * i] = left[i]; packet.samples[2 * i + 1] = right[i];
    }
    enqueue(packet); // Native decoded tap; no monitor gain/pan/mute and no resampling.
}
void RtlReceivePipeline::hdSpeakerAudio(const RtlReceiverRegistry::ReceiverSpec& spec,
    const HdFmAudioIdentity& identity, std::uint64_t first,
    std::span<const float> left, std::span<const float> right, bool discontinuity) noexcept
{
    (void)discontinuity; // Receiver/audio epoch retirement owns PCM lifetime changes.
    if (!spec.hdFm || left.empty() || left.size() != right.size() || left.size() > 1024
        || (m_faults.load(std::memory_order_acquire) & (1u << spec.handle.slot))) { return; }
    const unsigned monitor = m_monitor[spec.handle.slot].load(std::memory_order_relaxed);
    const RtlAudioMixer::Input input{spec.handle.slot, spec.handle.instance, identity.audioEpoch,
        (monitor & 255) / 100.0f, ((monitor >> 8) & 255) / 100.0f, (monitor & (1 << 16)) != 0};
    if (!m_mixer.configure(m_token.session, m_captureEpoch, std::span(&input, 1), first)) {
        m_mixerConfigurationFailures.fetch_add(1); m_faults.fetch_or(1u << spec.handle.slot); return;
    }
    m_hdSpeakerEpoch = identity.audioEpoch;
    for (std::size_t i = 0; i < left.size(); ++i) {
        m_hdSpeakerIdentities[(first + i) % RtlAudioMixer::kCapacity] = identity;
    }
    m_mixer.push(spec.handle.slot, spec.handle.instance, identity.audioEpoch, first, left, right);
}
RtlReceivePipeline::TraceEvent RtlReceivePipeline::traceContext() const noexcept
{
    TraceEvent event;
    event.token = m_token; event.hardwareGeneration = m_capture.generation;
    event.captureEpoch = m_captureEpoch;
    event.captureFirst = m_traceCaptureFirst; event.captureFrames = m_traceCaptureFrames;
    event.captureClock = m_traceCaptureClock;
    return event;
}
void RtlReceivePipeline::missingFrames(const RtlAudioMixer::Input& input, std::uint64_t first,
    std::uint64_t captureClock, const RtlAudioMixer::MissingMask& missing) noexcept
{
    TraceEvent event = traceContext();
    event.kind = TraceEvent::Kind::MixerMissing;
    event.slot = input.slot; event.stableId = m_traceStableIds[input.slot];
    event.instance = input.instance; event.receiverEpoch = input.epoch;
    event.captureClock = captureClock; event.quantumFirst = first; event.missingMask = missing;
    enqueueTrace(event);
}
void RtlReceivePipeline::enqueueTrace(const TraceEvent& event) noexcept
{
    const unsigned write = m_traceWrite.load(std::memory_order_relaxed);
    const unsigned next = (write + 1) % kTraceEvents;
    if (next == m_traceRead.load(std::memory_order_acquire)) {
        m_traceDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_traceEvents[write] = event;
    m_traceWrite.store(next, std::memory_order_release);
}
bool RtlReceivePipeline::takeTraceEvent(TraceEvent& output) noexcept
{
    const unsigned read = m_traceRead.load(std::memory_order_relaxed);
    if (read == m_traceWrite.load(std::memory_order_acquire)) { return false; }
    output = m_traceEvents[read];
    m_traceRead.store((read + 1) % kTraceEvents, std::memory_order_release);
    return true;
}
bool RtlReceivePipeline::enqueue(const Packet& packet) noexcept
{
    const unsigned write = m_write.load(std::memory_order_relaxed);
    const unsigned next = (write + 1) % kPackets;
    const unsigned read = m_read.load(std::memory_order_acquire);
    if (m_startupTrace) { ++m_startupTrace->packetAttempts; }
    if (next == read) {
        m_drops.fetch_add(1, std::memory_order_relaxed);
        if (m_startupTrace) {
            ++m_startupTrace->packetDrops;
            if (!m_startupTrace->firstDrop) {
                m_startupTrace->firstDrop = true;
                m_startupTrace->markProducer(RtlStartupTrace::FirstDrop, packet.token.session,
                    packet.token.revision, m_traceCaptureFirst, packet.slot, packet.firstSample,
                    packet.frames, kPackets - 1);
            }
        }
        return false;
    }
    if (m_startupTrace && m_startupTrace->firstDrop && !m_startupTrace->firstRecovery) {
        m_startupTrace->firstRecovery = true;
        m_startupTrace->markProducer(RtlStartupTrace::FirstRecovery, packet.token.session,
            packet.token.revision, m_traceCaptureFirst, packet.slot, packet.firstSample,
            packet.frames, (write + kPackets - read) % kPackets);
    }
    m_packets[write] = packet;
    m_packets[write].enqueuedMonotonicNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const std::uint64_t queued = (next + kPackets - read) % kPackets;
    if (m_startupTrace && queued == kPackets - 1 && !m_startupTrace->firstFull) {
        m_startupTrace->firstFull = true;
        m_startupTrace->markProducer(RtlStartupTrace::FirstQueueFull, packet.token.session,
            packet.token.revision, m_traceCaptureFirst, packet.slot, packet.firstSample,
            packet.frames, queued);
    }
    // Sole producer: no compare/exchange loop or callback contention needed.
    if (queued > m_packetQueueHighWater.load(std::memory_order_relaxed)) {
        m_packetQueueHighWater.store(queued, std::memory_order_relaxed);
    }
    m_write.store(next, std::memory_order_release);
    return true;
}
bool RtlReceivePipeline::takePacket(Packet& output) noexcept
{
    const unsigned read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire)) { return false; }
    output = m_packets[read];
    m_read.store((read + 1) % kPackets, std::memory_order_release);
    return true;
}
} // namespace AetherSDR::rtl
