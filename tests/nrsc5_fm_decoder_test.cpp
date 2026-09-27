#include "core/backends/rtl/Nrsc5FmDecoder.h"
#include <nrsc5.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using Decoder = AetherSDR::rtl::Nrsc5FmDecoder;
using Raw = AetherSDR::rtl::HdFmRawReception;
using Identity = AetherSDR::rtl::HdFmAudioIdentity;
static int failures = 0;
static void check(bool value, const char* text)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); } }
struct Events final : Decoder::Pipe {
    std::vector<nrsc5_event_t> events;
    bool good = true;
    bool valid() const noexcept override { return true; }
    bool feed(std::span<const float>, Callback callback, void* context) noexcept override
    { for (const auto& event : events) { callback(&event, context); } events.clear(); return good; }
};
struct Sink final : Decoder::Sink {
    Decoder* decoder = nullptr;
    std::vector<Identity> identities;
    std::vector<std::uint64_t> firsts, retired;
    std::vector<float> left, right;
    bool accept = true, statusFirst = true;
    bool decoded(const Identity& identity, std::uint64_t first, std::span<const float> l,
                 std::span<const float> r) noexcept override
    {
        const auto& state = decoder->reception();
        statusFirst &= state.audioValid && state.audioEpoch == identity.audioEpoch
            && state.audioMonotonicMs == identity.producedMonotonicMs && state.audioSequence > 0;
        identities.push_back(identity); firsts.push_back(first);
        left.insert(left.end(), l.begin(), l.end()); right.insert(right.end(), r.begin(), r.end());
        return accept;
    }
    void audioRetired(std::uint64_t epoch) noexcept override { retired.push_back(epoch); }
};
int main()
{
    Sink sink;
    auto pipe = std::make_unique<Events>(); auto* events = pipe.get();
    Raw initial; initial.receiverEpoch = 9; initial.frequencyHz = 100100000; initial.selectedProgram = 2;
    Decoder decoder(initial, sink, std::move(pipe)); sink.decoder = &decoder;
    std::array<float, 512> iq{};
    const auto submit = [&](nrsc5_event_t event, std::uint64_t revision = 3) {
        events->events.push_back(event); return decoder.feed(iq, 7, revision);
    };
    nrsc5_event_t event{}; event.event = NRSC5_EVENT_STATION_NAME; event.station_name.name = "KTEST\nFM";
    check(submit(event) && !decoder.reception().audioValid && sink.left.empty(), "station identity alone cannot establish valid audio");
    check(std::strcmp(decoder.reception().stationName.data(), "KTEST FM") == 0, "borrowed text is copied and controls bounded");
    event = {}; event.event = NRSC5_EVENT_SYNC; event.sync.freq_offset = 125.5f;
    check(submit(event) && decoder.reception().synced && !decoder.reception().audioValid, "RF sync and selected audio validity are independent");
    event = {}; event.event = NRSC5_EVENT_AUDIO_SERVICE; event.audio_service.program = 2;
    check(submit(event) && decoder.reception().services[2].audioAvailable, "audio service declaration enables discovery before audio decodes");
    std::array<std::int16_t, 4096> pcm{};
    for (std::size_t i = 0; i < 2048; ++i) { pcm[2 * i] = std::int16_t(i - 1024); pcm[2 * i + 1] = std::int16_t(1024 - i); }
    event = {}; event.event = NRSC5_EVENT_AUDIO; event.audio.program = 1; event.audio.data = pcm.data(); event.audio.count = pcm.size();
    check(submit(event) && sink.left.empty() && !decoder.reception().audioValid, "another program never drives selected PCM or green state");
    event.audio.program = 2; event.audio.flags = NRSC5_AUDIO_FLAGS_DECODING_ERROR;
    check(submit(event) && sink.left.empty() && decoder.reception().services[2].audioAvailable,
        "concealed audio does not decode or remove the discovered service");
    event.audio.flags = NRSC5_AUDIO_FLAGS_NONE;
    check(submit(event) && submit(event) && sink.left.size() == 4096 && sink.firsts == std::vector<std::uint64_t>{0, 2048},
        "selected 44100 stereo preserves ordered native sample counts");
    check(sink.statusFirst && sink.left[0] == -1024 / 32768.0f && sink.right[0] == 1024 / 32768.0f
        && sink.identities.back().sessionId == 7 && sink.identities.back().revision == 3
        && sink.identities.back().producedMonotonicMs > 0, "status precedes distinct paired PCM with original publication identity/time");
    const auto epoch = decoder.reception().audioEpoch;
    event.audio.flags = NRSC5_AUDIO_FLAGS_UNAVAILABLE;
    check(submit(event) && !decoder.reception().audioValid && decoder.reception().audioEpoch == epoch + 1,
        "missing selected audio retires the complete PCM epoch");
    event.audio.flags = NRSC5_AUDIO_FLAGS_NONE;
    check(submit(event, 4) && sink.firsts.back() == 0 && sink.identities.back().revision == 4,
        "reacquired audio starts its own sequence with the actual new token");
    event = {}; event.event = NRSC5_EVENT_MER; event.mer.lower = std::numeric_limits<float>::infinity(); event.mer.upper = 12.0f;
    check(submit(event) && !decoder.reception().merLowerDb && decoder.reception().merUpperDb == 12.0,
        "nonfinite diagnostics do not enter observed models");
    event = {}; event.event = NRSC5_EVENT_LOST_SYNC;
    check(submit(event) && !decoder.reception().synced && !decoder.reception().audioValid
        && decoder.reception().stationName[0] == 0 && !decoder.reception().services[2].audioAvailable,
        "loss clears station/program observations and retires queued audio");
    event = {}; event.event = NRSC5_EVENT_SYNC;
    check(submit(event) && decoder.reception().syncLossCount == 1 && decoder.reception().reacquisitionCount == 1,
        "loss/reacquisition counts report actual decoder events");
    event = {}; event.event = NRSC5_EVENT_AUDIO; event.audio.program = 2; event.audio.data = pcm.data(); event.audio.count = 3;
    check(submit(event) && !decoder.reception().audioValid, "malformed odd stereo audio is refused");
    event.audio.count = pcm.size(); sink.accept = false;
    check(!submit(event) && !decoder.valid() && !decoder.reception().audioValid,
        "downstream bounded-queue refusal fails closed and retires partially published audio");
    std::printf("ALL PASS — %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
