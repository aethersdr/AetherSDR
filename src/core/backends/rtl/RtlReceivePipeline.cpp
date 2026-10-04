#include "RtlReceivePipeline.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::rtl {
namespace {
WdspChannel::Mode dspMode(RtlCaptureTransaction::Mode mode)
{
    using M = RtlCaptureTransaction::Mode;
    switch (mode) {
    case M::Am: return WdspChannel::Mode::Am;
    case M::Sam: return WdspChannel::Mode::Sam;
    case M::Fm: case M::Fmn: return WdspChannel::Mode::Fm;
    case M::Lsb: return WdspChannel::Mode::Lsb;
    case M::Cw: return WdspChannel::Mode::Cwu;
    case M::Cwr: return WdspChannel::Mode::Cwl;
    default: return WdspChannel::Mode::Usb;
    }
}
}
RtlReceivePipeline::RtlReceivePipeline(std::size_t capacity)
    : m_registry({8, capacity, std::min<std::size_t>(32, capacity * 2)})
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
        if (id < 0 || id >= 8 || validated[id] || receiver.audioGain < 0
            || receiver.audioGain > 100 || receiver.audioPan < 0 || receiver.audioPan > 100
            || receiver.squelchLevel < 0 || receiver.squelchLevel > 100) { return Submission::Failed; }
        validated[id] = true;
    }
    std::array<bool, 8> receiving{};
    bool legacy = false;
    std::uint8_t receivingMask = 0;
    for (int id : state.receivingIds) {
        if (id < 0 || id >= 8 || receiving[id] || !validated[id]) { return Submission::Failed; }
        receiving[id] = true;
        receivingMask |= static_cast<std::uint8_t>(1u << id);
        const auto receiver = std::ranges::find_if(state.receivers, [id](const auto& value) {
            return value.passband.stableId == id;
        });
        legacy = legacy || (receiver->mode != Transaction::Mode::Fm
            && receiver->mode != Transaction::Mode::Fmn);
        if (legacy && receiver->squelchEnabled) { return Submission::Failed; }
    }
    if (legacy && (state.receivers.size() != 1 || state.receivingIds.size() != 1)) { return Submission::Failed; }
    const bool advanceCaptureEpoch = resetCapture || legacy != m_legacy
        || receivingMask != m_receivingMask;
    if (advanceCaptureEpoch && m_nextEpoch == std::numeric_limits<std::uint64_t>::max()) { return Submission::Failed; }
    auto epochs = m_epochs;
    auto specs = m_specs;
    if (!m_session) {
        m_session = m_registry.beginSession(state.capture);
        m_reader = m_registry.attachReader();
        if (!m_session || !m_reader) { return Submission::Failed; }
    }
    const unsigned faults = m_faults.load(std::memory_order_acquire);
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
            spec.dsp.fmReceive = WdspChannel::FmReceive{};
            spec.dsp.fmDeviationHz = receiver.mode == Transaction::Mode::Fmn ? 2500.0 : 5000.0;
            spec.dsp.filterLowHz = receiver.passband.filterLowHz;
            spec.dsp.filterHighHz = receiver.passband.filterHighHz;
            if (resetCapture || (faults & (1u << id)) || m_specs[id].handle != spec.handle
                || m_specs[id].passband != spec.passband || m_specs[id].dsp != spec.dsp) {
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
    return m_reader.processBlock({m_session, m_capture, firstSample, false, samples}, *this,
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
    std::array<RtlAudioMixer::Input, 8> inputs;
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto& spec = *views[i].spec;
        const unsigned monitor = m_monitor[spec.handle.slot].load(std::memory_order_relaxed);
        inputs[i] = {spec.handle.slot, spec.handle.instance, spec.epoch,
            (monitor & 255) / 100.0f, ((monitor >> 8) & 255) / 100.0f, (monitor & (1 << 16)) != 0};
    }
    const auto clock = [this](std::uint64_t sample) {
        return static_cast<std::uint64_t>(static_cast<long double>(sample) * 48000 / m_capture.achievedSampleRateHz);
    };
    if (!m_mixer.configure(m_token.session, m_captureEpoch,
            std::span(inputs).first(views.size()), clock(block.firstSample))) {
        // No stale map/audio may survive a rejected configuration. This is a
        // defensive invariant failure: normal registry handles and validated
        // transaction tokens cannot reach it. Repair through the existing owner.
        m_mixer.reset();
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
        if (m_spectrumFresh) {
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
            m_faults.fetch_or(1u << view.spec->handle.slot, std::memory_order_release);
        }
    }
    m_spectrumFresh = false;
    m_mixer.drain(clock(block.firstSample + block.samples.size()), *this);
    m_mixerLate.store(m_mixer.lateFrames(), std::memory_order_relaxed);
    m_mixerRejected.store(m_mixer.rejectedBlocks(), std::memory_order_relaxed);
    m_observed.store(true, std::memory_order_release);
}
RtlReceivePipeline::Diagnostics RtlReceivePipeline::diagnostics() const noexcept
{
    return {m_observed.load(std::memory_order_acquire),
        m_drops.load(std::memory_order_relaxed), m_mixerLate.load(std::memory_order_relaxed),
        m_mixerRejected.load(std::memory_order_relaxed),
        m_mixerConfigurationFailures.load(std::memory_order_relaxed)};
}
void RtlReceivePipeline::audioBlock(const RtlReceiverRegistry::ReceiverSpec& spec, std::uint64_t first,
    std::span<const float> left, std::span<const float> right, bool discontinuity) noexcept
{
    Packet packet;
    packet.token = m_token; packet.captureEpoch = m_captureEpoch;
    packet.instance = spec.handle.instance; packet.receiverEpoch = spec.epoch;
    packet.slot = spec.handle.slot; packet.firstSample = first;
    packet.frames = left.size(); packet.discontinuity = discontinuity;
    if (left.size() != right.size() || left.size() > 1024) { return; }
    std::array<float, 1024> gatedLeft{}, gatedRight{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float gain = m_squelch[packet.slot].gain(first + i);
        gatedLeft[i] = left[i] * gain; gatedRight[i] = right[i] * gain;
        packet.samples[2 * i] = gatedLeft[i]; packet.samples[2 * i + 1] = gatedRight[i];
    }
    enqueue(packet); // independent tap, before gain/mute/pan
    m_mixer.push(packet.slot, packet.instance, packet.receiverEpoch, first,
        std::span(gatedLeft).first(left.size()), std::span(gatedRight).first(right.size()));
}
void RtlReceivePipeline::speakerBlock(std::uint64_t first, std::span<const float> samples, bool discontinuity) noexcept
{
    Packet packet;
    packet.token = m_token; packet.captureEpoch = m_captureEpoch;
    packet.firstSample = first; packet.frames = samples.size() / 2; packet.discontinuity = discontinuity;
    std::copy(samples.begin(), samples.end(), packet.samples.begin());
    enqueue(packet);
}
bool RtlReceivePipeline::enqueue(const Packet& packet) noexcept
{
    const unsigned write = m_write.load(std::memory_order_relaxed);
    const unsigned next = (write + 1) % kPackets;
    if (next == m_read.load(std::memory_order_acquire)) { m_drops.fetch_add(1, std::memory_order_relaxed); return false; }
    m_packets[write] = packet;
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
