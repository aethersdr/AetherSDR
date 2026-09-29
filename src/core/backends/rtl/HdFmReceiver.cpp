#include "HdFmReceiver.h"
#include "HdFmIqAdapter.h"
#include "Nrsc5FmDecoder.h"
#include "RtlCaptureTransaction.h"
#include "core/Resampler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>

namespace AetherSDR::rtl {
namespace {
using Registry = RtlReceiverRegistry;
template<class T, unsigned N> class Queue {
public:
    bool push(const T& value) noexcept
    {
        const unsigned write = m_write.load(std::memory_order_relaxed);
        if (write - m_read.load(std::memory_order_acquire) == N) { return false; }
        m_values[write % N] = value;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }
    bool peek(T& value) const noexcept
    {
        const unsigned read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) { return false; }
        value = m_values[read % N];
        return true;
    }
    bool pop(T& value) noexcept
    {
        const unsigned read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) { return false; }
        value = m_values[read % N];
        m_read.store(read + 1, std::memory_order_release);
        return true;
    }
private:
    std::array<T, N> m_values{};
    alignas(64) std::atomic<unsigned> m_write{0};
    alignas(64) std::atomic<unsigned> m_read{0};
};
std::atomic<unsigned> residentWorkers{0};
class ResidentLease {
public:
    ResidentLease()
    {
        unsigned count = residentWorkers.load();
        while (count < 2) {
            if (residentWorkers.compare_exchange_weak(count, count + 1)) { m_held = true; break; }
        }
    }
    ~ResidentLease() { if (m_held) { residentWorkers.fetch_sub(1); } }
    explicit operator bool() const { return m_held; }
private:
    bool m_held = false;
};
SharedCapturePolicy::SliceDescriptor footprint(const Registry::ReceiverSpec& spec)
{
    RtlCaptureTransaction::Receiver receiver{spec.passband, RtlCaptureTransaction::Mode::Wfm};
    receiver.wfmHdStereo = true;
    return RtlCaptureTransaction::effectivePassband(receiver);
}
struct IqJob {
    std::uint64_t first = 0, session = 0, revision = 0;
    std::size_t frames = 0;
    std::array<std::complex<float>, 8192> samples{};
};
struct AudioBlock {
    HdFmAudioIdentity identity;
    std::uint64_t first = 0;
    std::size_t frames = 0;
    std::array<float, 1024> left{}, right{};
};
class HdFmReceiver final : public Registry::Receiver, private HdFmIqAdapter::Sink,
    private Nrsc5FmDecoder::Sink {
public:
    HdFmReceiver(const Registry::ReceiverSpec& spec, std::unique_ptr<Nrsc5FmDecoder::Pipe> pipe,
        std::optional<WdspChannel::Reservation> reservation)
        : m_reservation(std::move(reservation)), m_spec(spec), m_iq(spec.capture, footprint(spec)),
          m_leftRate(44100, 48000, 2048), m_rightRate(44100, 48000, 2048)
    {
        if (!m_lease || !spec.hdFm || !m_iq.valid()) { return; }
        HdFmRawReception initial;
        initial.receiverEpoch = spec.epoch;
        initial.frequencyHz = static_cast<std::int64_t>(std::llround(spec.passband.carrierHz));
        initial.selectedProgram = spec.hdFm->program;
        m_last = initial;
        m_decoder = std::make_unique<Nrsc5FmDecoder>(initial, static_cast<Nrsc5FmDecoder::Sink&>(*this), std::move(pipe));
        if (!m_decoder->valid()) { return; }
        m_convertedLeft.reserve(16384 * sizeof(float)); m_convertedRight.reserve(16384 * sizeof(float));
        m_thread = std::thread([this] { run(); });
        m_valid = true;
    }
    ~HdFmReceiver() override
    {
        // Registry retirement executor only, after the acquisition reader's
        // acknowledgement. Stop ignores the remaining queue, finishes only
        // the current bounded IQ job, then closes the pipe on this executor.
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable()) { m_thread.join(); }
    }
    bool valid() const noexcept { return m_valid; }
    WdspChannel::ProcessResult processIq(std::span<const float>, std::span<const float>) noexcept override
    { return WdspChannel::ProcessResult::InvalidBuffer; }
    std::span<const float> left() const noexcept override { return {}; }
    std::span<const float> right() const noexcept override { return {}; }
    bool processCapture(const Registry::SampleBlock& block, Registry::AudioSink& sink) noexcept override
    {
        // The registry marks the first adopted delivery discontinuous. A new
        // prepared decoder has no prior stream to preserve; later boundaries
        // must still withdraw instead of splicing independent RF histories.
        if (!m_valid || block.session != m_spec.handle.session || block.capture != m_spec.capture
            || (block.discontinuity && m_haveInput) || !block.publicationSession || !block.publicationRevision
            || block.samples.empty() || block.samples.data() == nullptr || block.samples.size() > 8192
            || block.firstSample > std::numeric_limits<std::uint64_t>::max() - block.samples.size()
            || (m_haveInput && block.firstSample != m_nextInput)) { return fail(sink); }
        if (m_last.sessionId == 0) {
            m_last.sessionId = block.publicationSession; m_last.revision = block.publicationRevision;
        }
        m_haveInput = true; m_nextInput = block.firstSample + block.samples.size();
        m_latestInput.store(m_nextInput, std::memory_order_release);
        m_job.first = block.firstSample; m_job.session = block.publicationSession;
        m_job.revision = block.publicationRevision; m_job.frames = block.samples.size();
        std::copy(block.samples.begin(), block.samples.end(), m_job.samples.begin());
        if (!m_failed.load(std::memory_order_acquire) && !m_input.push(m_job)) {
            m_iqDrops.fetch_add(1); m_failed.store(true, std::memory_order_release);
        }
        HdFmRawReception observation;
        for (unsigned i = 0; i < 32 && m_observations.pop(observation); ++i) { m_last = observation; }
        reconcileEpoch();
        if (m_failed.load(std::memory_order_acquire)) { return fail(sink); }
        m_last.iqDrops = m_iqDrops.load(); m_last.pcmDrops = m_pcmDrops.load();
        m_last.playoutUnderruns = m_underruns;
        publish(sink);
        AudioBlock audio;
        for (unsigned i = 0; i < 32 && m_speaker.peek(audio); ++i) {
            reconcileEpoch();
            // Retirement is released before any new-epoch PCM is published.
            // Re-read after peek; never discard new PCM against a cached epoch.
            if (!m_speaker.pop(audio)) { break; }
            reconcileEpoch();
            if (audio.identity.audioEpoch != m_playoutEpoch || m_waitingReset) { continue; }
            if (m_buffered + audio.frames > kPlayoutFrames) {
                m_pcmDrops.fetch_add(1); return fail(sink);
            }
            for (std::size_t n = 0; n < audio.frames; ++n) {
                const std::size_t at = (m_readPosition + m_buffered++) % kPlayoutFrames;
                m_playoutLeft[at] = audio.left[n]; m_playoutRight[at] = audio.right[n];
                m_playoutIdentities[at] = audio.identity;
            }
        }
        reconcileEpoch();
        if (!m_ready && !m_waitingReset && m_last.audioValid && m_buffered >= kPrefillFrames) {
            m_ready = true;
        }
        publish(sink);
        for (unsigned i = 0; i < 32 && m_tap.peek(audio); ++i) {
            // Producer publishes successful-audio status before PCM. Draining
            // can reveal a new epoch, which must acquire its own prebuffer.
            for (unsigned j = 0; j < 32 && m_observations.pop(observation); ++j) { m_last = observation; }
            reconcileEpoch();
            publish(sink);
            if (!m_ready && audio.identity.audioEpoch == m_playoutEpoch) { break; }
            if (!m_tap.pop(audio)) { break; }
            reconcileEpoch();
            if (!m_ready || !m_last.audioValid || audio.identity.audioEpoch != m_playoutEpoch
                || m_waitingReset || audio.identity.sessionId != block.publicationSession
                || audio.identity.revision != block.publicationRevision) { continue; }
            sink.hdDecodedAudio(m_spec, audio.identity, audio.first,
                std::span(audio.left).first(audio.frames), std::span(audio.right).first(audio.frames), m_firstTap);
            m_firstTap = false;
        }
        const auto rate = static_cast<std::uint64_t>(block.capture.achievedSampleRateHz);
        if (!m_speakerNext) {
            const auto aligned = RtlRfExtractor::alignedCaptureFirst(block.firstSample, rate, 48000);
            if (!aligned) { return fail(sink); }
            const std::uint64_t divisor = std::gcd(rate, std::uint64_t{48000});
            const std::uint64_t sourceQuantum = rate / divisor, audioQuantum = 48000 / divisor;
            if (*aligned / sourceQuantum > std::numeric_limits<std::uint64_t>::max() / audioQuantum) {
                return fail(sink);
            }
            m_speakerNext = (*aligned / sourceQuantum) * audioQuantum;
        }
        const std::uint64_t end = static_cast<std::uint64_t>(static_cast<long double>(m_nextInput) * 48000 / rate);
        // Emit complete mixer quanta only. On source retirement no residual
        // partial quantum can retain a former program's audio in the mixer.
        while (end >= *m_speakerNext && end - *m_speakerNext >= 128) {
            const std::size_t frames = std::min<std::uint64_t>(1024, ((end - *m_speakerNext) / 128) * 128);
            reconcileEpoch();
            publish(sink);
            m_audioLeft.fill(0); m_audioRight.fill(0);
            HdFmAudioIdentity identity{block.publicationSession, block.publicationRevision, m_playoutEpoch, 0};
            if (m_ready && m_buffered < frames) {
                ++m_underruns; m_ready = false; m_waitingReset = true; m_buffered = 0;
                m_resetAudio.store(true, std::memory_order_release);
                m_last.audioValid = false; m_last.playoutUnderruns = m_underruns;
                publish(sink, true);
            }
            if (m_ready) {
                for (std::size_t n = 0; n < frames; ++n) {
                    m_audioLeft[n] = m_playoutLeft[m_readPosition];
                    m_audioRight[n] = m_playoutRight[m_readPosition];
                    const auto& source = m_playoutIdentities[m_readPosition];
                    // A control-only revision can leave an older prefix. Keep
                    // its original token so the owner can reject that quantum.
                    if (identity.producedMonotonicMs == 0 || source.revision < identity.revision
                        || (source.revision == identity.revision
                            && source.producedMonotonicMs < identity.producedMonotonicMs)) {
                        identity = source;
                    }
                    m_readPosition = (m_readPosition + 1) % kPlayoutFrames; --m_buffered;
                }
            }
            sink.hdSpeakerAudio(m_spec, identity, *m_speakerNext,
                std::span(m_audioLeft).first(frames), std::span(m_audioRight).first(frames), m_firstSpeaker);
            m_firstSpeaker = false; *m_speakerNext += frames;
        }
        return true;
    }
private:
    static constexpr std::size_t kPlayoutFrames = 32768;
    static constexpr std::size_t kPrefillFrames = 8192;
    ResidentLease m_lease;
    // Remains charged across shared receiver reuse by a control-only bank.
    // Destroy after the decoder, rather than releasing with the original bank.
    std::optional<WdspChannel::Reservation> m_reservation;
    const Registry::ReceiverSpec m_spec;
    HdFmIqAdapter m_iq;
    Resampler m_leftRate, m_rightRate;
    QByteArray m_convertedLeft, m_convertedRight;
    std::unique_ptr<Nrsc5FmDecoder> m_decoder;
    Queue<IqJob, 32> m_input;
    Queue<AudioBlock, 32> m_tap, m_speaker;
    Queue<HdFmRawReception, 32> m_observations;
    std::thread m_thread;
    std::atomic<bool> m_stop{false}, m_failed{false}, m_resetAudio{false};
    std::atomic<std::uint64_t> m_audioEpoch{1}, m_latestInput{0}, m_iqDrops{0}, m_pcmDrops{0};
    bool m_valid = false;
    // Acquisition-only state.
    IqJob m_job;
    HdFmRawReception m_last, m_published;
    bool m_haveInput = false, m_firstTap = true, m_firstSpeaker = true;
    std::uint64_t m_nextInput = 0, m_playoutEpoch = 1, m_underruns = 0, m_publicationSequence = 0;
    std::optional<std::uint64_t> m_speakerNext;
    std::array<float, kPlayoutFrames> m_playoutLeft{}, m_playoutRight{};
    std::array<HdFmAudioIdentity, kPlayoutFrames> m_playoutIdentities{};
    std::array<float, 1024> m_audioLeft{}, m_audioRight{};
    std::size_t m_readPosition = 0, m_buffered = 0;
    bool m_ready = false, m_waitingReset = false;
    // Worker-only feed context and converter sequence.
    std::uint64_t m_feedSession = 0, m_feedRevision = 0, m_convertedFrames = 0;
    bool fail(Registry::AudioSink& sink) noexcept
    {
        m_failed.store(true, std::memory_order_release);
        m_last.valid = false; m_last.audioValid = false; m_last.synced = false;
        m_last.iqDrops = m_iqDrops.load(); m_last.pcmDrops = m_pcmDrops.load();
        publish(sink, true);
        return false;
    }
    void reconcileEpoch() noexcept
    {
        const std::uint64_t epoch = m_audioEpoch.load(std::memory_order_acquire);
        if (epoch != m_playoutEpoch) {
            m_playoutEpoch = epoch; m_buffered = 0; m_readPosition = 0;
            m_ready = false; m_waitingReset = false; m_firstTap = true;
        }
        if (m_last.audioEpoch != epoch || m_waitingReset) {
            m_last.audioEpoch = epoch; m_last.audioValid = false;
        }
    }
    void publish(Registry::AudioSink& sink, bool force = false) noexcept
    {
        reconcileEpoch();
        HdFmRawReception value = m_last;
        // Worker snapshots do not own acquisition/queue counters; stamp every
        // publication after all possible observation drains and epoch changes.
        value.iqDrops = m_iqDrops.load(std::memory_order_relaxed);
        value.pcmDrops = m_pcmDrops.load(std::memory_order_relaxed);
        value.playoutUnderruns = m_underruns;
        value.audioValid = value.audioValid && m_ready && !m_waitingReset;
        if (force || value.observationSequence != m_published.observationSequence
            || value.audioEpoch != m_published.audioEpoch || value.revision != m_published.revision
            || value.audioValid != m_published.audioValid || value.iqDrops != m_published.iqDrops
            || value.pcmDrops != m_published.pcmDrops || value.playoutUnderruns != m_published.playoutUnderruns) {
            value.publicationSequence = ++m_publicationSequence;
            sink.hdObservation(m_spec, value); m_published = value;
        }
    }
    bool hdIq(std::span<const float> samples) noexcept override
    { return !m_stop.load() && m_decoder->feed(samples, m_feedSession, m_feedRevision); }
    void audioRetired(std::uint64_t epoch) noexcept override
    {
        m_leftRate.reset(); m_rightRate.reset(); m_convertedFrames = 0;
        m_audioEpoch.store(epoch, std::memory_order_release);
    }
    bool queueAudio(Queue<AudioBlock, 32>& queue, const HdFmAudioIdentity& identity,
        std::uint64_t first, std::span<const float> left, std::span<const float> right) noexcept
    {
        for (std::size_t offset = 0; offset < left.size(); offset += 1024) {
            AudioBlock block;
            block.identity = identity; block.first = first + offset;
            block.frames = std::min<std::size_t>(1024, left.size() - offset);
            std::copy_n(left.data() + offset, block.frames, block.left.data());
            std::copy_n(right.data() + offset, block.frames, block.right.data());
            if (!queue.push(block)) { m_pcmDrops.fetch_add(1); return false; }
        }
        return true;
    }
    bool decoded(const HdFmAudioIdentity& identity, std::uint64_t first,
        std::span<const float> left, std::span<const float> right) noexcept override
    {
        if (!m_observations.push(m_decoder->reception())) { return false; }
        if (!queueAudio(m_tap, identity, first, left, right)) { return false; }
        const int count = m_leftRate.process(left.data(), static_cast<int>(left.size()), m_convertedLeft);
        const int other = m_rightRate.process(right.data(), static_cast<int>(right.size()), m_convertedRight);
        if (count < 0 || count != other || count > 16384) { return false; }
        const std::span convertedLeft(reinterpret_cast<const float*>(m_convertedLeft.constData()), count);
        const std::span convertedRight(reinterpret_cast<const float*>(m_convertedRight.constData()), count);
        if (!std::ranges::all_of(convertedLeft, [](float v) { return std::isfinite(v); })
            || !std::ranges::all_of(convertedRight, [](float v) { return std::isfinite(v); })) { return false; }
        if (!queueAudio(m_speaker, identity, m_convertedFrames, convertedLeft, convertedRight)) { return false; }
        m_convertedFrames += static_cast<std::uint64_t>(count);
        return true;
    }
    void run()
    {
        IqJob job;
        while (!m_stop.load(std::memory_order_acquire)) {
            if (m_failed.load(std::memory_order_acquire)) { m_decoder->withdraw(); break; }
            if (m_resetAudio.exchange(false, std::memory_order_acq_rel)) { m_decoder->resetAudio(); }
            // Expiry is independent of whether RF callbacks keep arriving.
            m_decoder->expireAudio();
            if (!m_input.pop(job)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const std::uint64_t latest = m_latestInput.load(std::memory_order_acquire);
            if (latest < job.first || latest - job.first > m_spec.capture.achievedSampleRateHz / 4) {
                m_iqDrops.fetch_add(1); m_failed.store(true, std::memory_order_release); continue;
            }
            m_feedSession = job.session; m_feedRevision = job.revision;
            if (!m_iq.process(job.first, std::span(job.samples).first(job.frames), *this)) {
                m_failed.store(true, std::memory_order_release); continue;
            }
            if (!m_observations.push(m_decoder->reception())) {
                // Losing a status barrier is a stream failure, never permission
                // to keep a green claim based on subsequently queued PCM.
                m_failed.store(true, std::memory_order_release);
            }
        }
        if (m_failed.load()) { m_decoder->withdraw(); m_observations.push(m_decoder->reception()); }
    }
};
}
std::unique_ptr<Registry::Receiver> prepareHdFmReceiver(const Registry::ReceiverSpec& spec, std::string& error,
    std::unique_ptr<Nrsc5FmDecoder::Pipe> pipe, std::optional<WdspChannel::Reservation> reservation)
{
    if (!spec.hdFm || spec.hdFm->program < 0 || spec.hdFm->program >= 8) {
        error = "Invalid digital program"; return nullptr;
    }
    auto receiver = std::make_unique<HdFmReceiver>(spec, std::move(pipe), std::move(reservation));
    if (!receiver->valid()) {
        error = "Digital decoder unavailable, RF footprint invalid, or two resident workers already reserved";
        return nullptr;
    }
    return receiver;
}
} // namespace AetherSDR::rtl
