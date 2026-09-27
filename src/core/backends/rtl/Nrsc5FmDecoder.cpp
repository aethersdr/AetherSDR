#include "Nrsc5FmDecoder.h"
#include "HdFmIqAdapter.h"
#include "core/dsp/FftwPlannerLock.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#ifdef AETHER_ENABLE_NRSC5
#include <nrsc5.h>
#endif

namespace AetherSDR::rtl {
namespace {
class NativePipe final : public Nrsc5FmDecoder::Pipe {
public:
    NativePipe()
    {
#ifdef AETHER_ENABLE_NRSC5
        const auto lock = fftwfPlannerLock();
        if (nrsc5_open_pipe(&m_decoder) != 0) { m_decoder = nullptr; }
#endif
    }
    ~NativePipe() override
    {
#ifdef AETHER_ENABLE_NRSC5
        if (m_decoder) { const auto lock = fftwfPlannerLock(); nrsc5_close(m_decoder); }
#endif
    }
    bool valid() const noexcept override
    {
#ifdef AETHER_ENABLE_NRSC5
        return m_decoder != nullptr;
#else
        return false;
#endif
    }
    bool feed(std::span<const float> input, Callback callback, void* context) noexcept override
    {
#ifdef AETHER_ENABLE_NRSC5
        nrsc5_set_callback(m_decoder, callback, context);
        return nrsc5_pipe_samples_cf32(m_decoder, input.data(), static_cast<unsigned>(input.size())) == 0;
#else
        (void)input; (void)callback; (void)context;
        return false;
#endif
    }
private:
#ifdef AETHER_ENABLE_NRSC5
    nrsc5_t* m_decoder = nullptr;
#endif
};
std::uint64_t monotonicMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
template<std::size_t N> void copyText(std::array<char, N>& destination, const char* source)
{
    destination.fill(0);
    if (!source) { return; }
    for (std::size_t i = 0; i + 1 < N && source[i]; ++i) {
        const unsigned char value = static_cast<unsigned char>(source[i]);
        destination[i] = value < 0x20 || value == 0x7f ? ' ' : source[i];
    }
}
void increment(std::uint32_t& value)
{
    if (value < std::numeric_limits<std::uint32_t>::max()) { ++value; }
}
}
struct Nrsc5FmDecoder::State {
    HdFmRawReception value;
    Sink& sink;
    bool open = false;
    bool failed = false;
    bool hadSync = false;
    std::uint64_t decodedFrames = 0;
    std::uint64_t iqFrames = 0;
    std::uint64_t syncFirst = 0;
    std::array<float, 2048> left{}, right{};
    std::chrono::steady_clock::time_point lastAudio{};
    std::unique_ptr<Pipe> pipe;
    State(HdFmRawReception initial, Sink& output) : value(initial), sink(output) {}
    void retire()
    {
        if (value.audioValid || decodedFrames != 0) {
            value.audioValid = false;
            decodedFrames = 0;
            ++value.audioEpoch;
            sink.audioRetired(value.audioEpoch);
        }
    }
    static void callback(const nrsc5_event_t* event, void* context)
    {
#ifdef AETHER_ENABLE_NRSC5
        if (event) { static_cast<State*>(context)->event(*event); }
#else
        (void)event; (void)context;
#endif
    }
#ifdef AETHER_ENABLE_NRSC5
    void event(const nrsc5_event_t& event)
    {
        if (failed) { return; }
        switch (event.event) {
        case NRSC5_EVENT_SYNC:
            if (!value.synced) {
                if (hadSync) { increment(value.reacquisitionCount); }
                hadSync = true; syncFirst = iqFrames;
            }
            value.synced = true; value.valid = true;
            value.frequencyOffsetHz = std::isfinite(event.sync.freq_offset)
                ? std::optional<double>(event.sync.freq_offset) : std::nullopt;
            break;
        case NRSC5_EVENT_LOST_SYNC:
            if (value.synced) { increment(value.syncLossCount); }
            value.synced = false; value.valid = false; value.syncDurationMs = 0;
            value.stationName.fill(0); value.title.fill(0); value.artist.fill(0);
            value.services = {}; value.merLowerDb.reset(); value.merUpperDb.reset();
            value.cber.reset(); value.frequencyOffsetHz.reset();
            retire();
            break;
        case NRSC5_EVENT_MER:
            value.merLowerDb = std::isfinite(event.mer.lower)
                ? std::optional<double>(event.mer.lower) : std::nullopt;
            value.merUpperDb = std::isfinite(event.mer.upper)
                ? std::optional<double>(event.mer.upper) : std::nullopt;
            break;
        case NRSC5_EVENT_BER:
            value.cber = std::isfinite(event.ber.cber) && event.ber.cber >= 0 && event.ber.cber <= 1
                ? std::optional<double>(event.ber.cber) : std::nullopt;
            break;
        case NRSC5_EVENT_STATION_NAME:
            copyText(value.stationName, event.station_name.name);
            break;
        case NRSC5_EVENT_ID3:
            if (event.id3.program == static_cast<unsigned>(value.selectedProgram)) {
                copyText(value.title, event.id3.title); copyText(value.artist, event.id3.artist);
            }
            break;
        case NRSC5_EVENT_AUDIO_SERVICE:
            if (event.audio_service.program < value.services.size()) {
                value.services[event.audio_service.program].program = static_cast<int>(event.audio_service.program);
                value.services[event.audio_service.program].audioAvailable = true;
            }
            break;
        case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR:
            if (event.asd.program < value.services.size()) {
                value.services[event.asd.program].program = static_cast<int>(event.asd.program);
                value.services[event.asd.program].audioAvailable = true;
            }
            break;
        // SIG channel/port values are not assumed to be program numbers.
        // Names remain empty (UI HD1..HD8 fallback) until that mapping is
        // verified; bounded station and selected-program ID3 text is exposed.
        case NRSC5_EVENT_AUDIO: {
            if (event.audio.program >= value.services.size()) { break; }
            auto& service = value.services[event.audio.program];
            service.program = static_cast<int>(event.audio.program);
            const bool accepted = value.synced && event.audio.flags == NRSC5_AUDIO_FLAGS_NONE
                && event.audio.data && event.audio.count > 0 && event.audio.count % 2 == 0
                && event.audio.count <= 2 * left.size();
            service.audioAvailable = true; // Discovered service; validity belongs to selected PCM.
            if (event.audio.program != static_cast<unsigned>(value.selectedProgram)) { break; }
            if (!accepted) { retire(); break; }
            const std::size_t frames = event.audio.count / 2;
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = event.audio.data[2 * i] / 32768.0f;
                right[i] = event.audio.data[2 * i + 1] / 32768.0f;
            }
            // Publish the state before its PCM through the sink. The consumer
            // drains that status barrier before exposing a decoded block.
            ++value.audioSequence; ++value.observationSequence;
            value.audioValid = true; value.valid = true;
            value.audioMonotonicMs = monotonicMs();
            value.observationMonotonicMs = value.audioMonotonicMs;
            const HdFmAudioIdentity identity{value.sessionId, value.revision, value.audioEpoch, value.audioMonotonicMs};
            if (!sink.decoded(identity, decodedFrames, std::span(left).first(frames), std::span(right).first(frames))) {
                failed = true; value.valid = false; value.audioValid = false; decodedFrames = 0;
                sink.audioRetired(++value.audioEpoch); break;
            }
            decodedFrames += frames;
            value.audioValid = true; value.valid = true;
            lastAudio = std::chrono::steady_clock::now();
            break;
        }
        default: return;
        }
        ++value.observationSequence; value.observationMonotonicMs = monotonicMs();
    }
#endif
};
Nrsc5FmDecoder::Nrsc5FmDecoder(HdFmRawReception initial, Sink& sink, std::unique_ptr<Pipe> pipe)
    : m_state(std::make_unique<State>(initial, sink))
{
    if (initial.selectedProgram < 0 || initial.selectedProgram >= 8) { return; }
    // NativePipe serializes all open/close planner work with every other
    // single-precision FFTW user. Pipe mode has no upstream worker.
    m_state->pipe = pipe ? std::move(pipe) : std::make_unique<NativePipe>();
    m_state->open = m_state->pipe->valid();
}
Nrsc5FmDecoder::~Nrsc5FmDecoder() = default;
bool Nrsc5FmDecoder::valid() const noexcept { return m_state->open && !m_state->failed; }
const HdFmRawReception& Nrsc5FmDecoder::reception() const noexcept { return m_state->value; }
bool Nrsc5FmDecoder::feed(std::span<const float> input, std::uint64_t session,
    std::uint64_t revision) noexcept
{
    if (!valid() || !session || !revision || input.empty() || input.size() % 2 != 0
        || input.size() > 32768 || !std::ranges::all_of(input, [](float value) { return std::isfinite(value); })) {
        withdraw(); return false;
    }
    auto& state = *m_state;
    state.value.sessionId = session; state.value.revision = revision;
    if (!state.pipe->feed(input, State::callback, &state)) {
        withdraw(); return false;
    }
    state.iqFrames += input.size() / 2;
    if (state.value.synced) {
        state.value.syncDurationMs = static_cast<std::uint32_t>(std::min(
            1000.0L * (state.iqFrames - state.syncFirst) / HdFmIqAdapter::kOutputRate,
            static_cast<long double>(std::numeric_limits<std::uint32_t>::max())));
    }
    return valid();
}
void Nrsc5FmDecoder::withdraw() noexcept
{
    m_state->failed = true; m_state->retire();
    m_state->value.valid = false; m_state->value.synced = false;
    ++m_state->value.observationSequence; m_state->value.observationMonotonicMs = monotonicMs();
}
void Nrsc5FmDecoder::resetAudio() noexcept
{
    m_state->retire(); ++m_state->value.observationSequence; m_state->value.observationMonotonicMs = monotonicMs();
}
void Nrsc5FmDecoder::expireAudio() noexcept
{
    if (m_state->value.audioValid && std::chrono::steady_clock::now() - m_state->lastAudio > std::chrono::milliseconds(500)) {
        m_state->retire(); ++m_state->value.observationSequence; m_state->value.observationMonotonicMs = monotonicMs();
    }
}
} // namespace AetherSDR::rtl
