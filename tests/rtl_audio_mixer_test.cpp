#include "core/backends/rtl/RtlAudioMixer.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using Mixer = AetherSDR::rtl::RtlAudioMixer;
static int failures = 0;
static void check(bool value, const char* message)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
struct Sink final : Mixer::Sink {
    std::vector<float> samples;
    std::uint64_t next = 0;
    bool valid = true;
    struct Missing {
        Mixer::Input input;
        std::uint64_t first;
        std::uint64_t clock;
        Mixer::MissingMask mask;
    };
    std::vector<Missing> missing;
    void missingFrames(const Mixer::Input& input, std::uint64_t first,
        std::uint64_t clock, const Mixer::MissingMask& mask) noexcept override
    { missing.push_back({input, first, clock, mask}); }
    void speakerBlock(std::uint64_t first, std::span<const float> input, bool discontinuity) noexcept override
    {
        valid &= first == next && discontinuity == samples.empty() && input.size() == Mixer::kQuantum * 2;
        samples.insert(samples.end(), input.begin(), input.end()); next += input.size() / 2;
    }
};
int main()
{
    auto mixer = std::make_unique<Mixer>();
    std::array<Mixer::Input, 2> inputs{{{0, 1, 1}, {3, 2, 1}}};
    std::array<float, Mixer::kQuantum> left, right;
    left.fill(0.8f); right.fill(-0.8f);
    Sink sink; sink.samples.reserve(40000); sink.missing.reserve(64);
    check(mixer->configure(1, 1, inputs, 0), "sparse independent inputs configured");
    check(mixer->push(0, 1, 1, 0, left, right), "first slice queues capture positions");
    mixer->drain(128, sink);
    check(sink.samples.empty(), "mix waits briefly for matching second slice");
    check(mixer->push(3, 2, 1, 0, left, right), "second slice queues same capture positions");
    mixer->drain(128, sink);
    check(sink.next == 128 && sink.samples[0] == 0.8f && sink.samples[1] == -0.8f,
        "single stereo mix preserves anti-phase channels and summation headroom");
    check(!mixer->push(0, 1, 1, 0, left, right), "already emitted audio cannot double feed");
    check(mixer->push(0, 1, 1, 128, left, right), "healthy sibling progresses");
    mixer->drain(256 + Mixer::kDeadlineFrames - 1, sink);
    check(sink.next == 128, "late receiver gets bounded deadline");
    mixer->drain(256 + Mixer::kDeadlineFrames, sink);
    check(sink.next == 256 && sink.samples[256] == 0.4f && sink.samples[257] == -0.4f,
        "deadline substitutes silence without stalling or shifting healthy audio");
    check(sink.missing.size() == 1 && sink.missing.front().input.slot == 3
        && sink.missing.front().input.instance == 2 && sink.missing.front().input.epoch == 1
        && sink.missing.front().first == 128
        && sink.missing.front().clock == 256 + Mixer::kDeadlineFrames
        && sink.missing.front().mask[0] == std::numeric_limits<std::uint64_t>::max()
        && sink.missing.front().mask[1] == std::numeric_limits<std::uint64_t>::max(),
        "diagnostics distinguish the expired sparse receiver and exact 128-frame hole");
    check(mixer->lateFrames() == 128 && !mixer->push(3, 2, 1, 128, left, right),
        "expired receiver audio cannot reappear at current time");
    inputs[0].gain = 0.5f; inputs[0].pan = 0; inputs[1].mute = true;
    check(mixer->configure(1, 1, inputs, 0), "monitor settings update without resetting capture clock");
    mixer->push(0, 1, 1, 256, left, right); mixer->push(3, 2, 1, 256, left, right);
    mixer->drain(384, sink);
    check(sink.samples[512] == 0.2f && sink.samples[513] == 0, "gain mute and balance apply to monitor only");
    inputs[0].gain = 1; inputs[0].pan = 0.5f;
    mixer->push(0, 1, 1, 384, left, right);
    check(mixer->configure(1, 1, std::span(inputs).first(1), 0), "middle removal retains survivor queues");
    mixer->drain(512, sink);
    check(sink.next == 512 && sink.samples[768] == 0.8f, "removal cannot reset survivor audio history");
    inputs[1] = {3, 3, 2};
    mixer->configure(1, 1, inputs, 0);
    check(!mixer->push(3, 2, 2, 512, left, right), "reused slot rejects previous instance even with current epoch");
    check(!mixer->push(3, 3, 1, 512, left, right), "new receiver rejects old format epoch");
    left[4] = std::numeric_limits<float>::quiet_NaN();
    check(!mixer->push(3, 3, 2, 512, left, right), "nonfinite block refused atomically");
    left.fill(10); right.fill(-10);
    mixer->push(0, 1, 1, 512, left, right); mixer->push(3, 3, 2, 512, left, right);
    mixer->drain(640, sink);
    check(sink.samples[1024] == 1 && sink.samples[1025] == -1, "final output clips boundedly");
    check(!mixer->push(0, 1, 1, 640 + Mixer::kCapacity, left, right), "future queue cannot grow without bound");
    check(!mixer->push(0, 1, 1, std::numeric_limits<std::uint64_t>::max(), left, right), "sample position overflow refused");
    check(sink.valid, "speaker sample count monotonic with exactly one start discontinuity");
    check(mixer->configure(2, 1, inputs, 0) && mixer->nextSample() == 0, "reconnect starts new source clock");
    const auto rejected = mixer->rejectedBlocks();
    check(rejected >= 7, "queue refusals are observable");
    // Two isolated absent contributions are not a contiguous two-frame gap.
    auto sparseMixer = std::make_unique<Mixer>();
    const std::array<Mixer::Input, 1> single{{{4, 99, 7}}};
    Sink sparse; sparse.samples.reserve(256); sparse.missing.reserve(1);
    check(sparseMixer->configure(7, 9, single, 0), "sparse diagnostic input configured");
    left.fill(.2f); right.fill(-.2f);
    check(sparseMixer->push(4, 99, 7, 1, std::span(left).first(63), std::span(right).first(63)),
        "queue audio after first missing frame");
    check(sparseMixer->push(4, 99, 7, 65, std::span(left).first(63), std::span(right).first(63)),
        "queue audio after separated missing frame");
    sparseMixer->drain(128 + Mixer::kDeadlineFrames, sparse);
    check(sparseMixer->lateFrames() == 2 && sparse.missing.size() == 1
        && sparse.missing.front().mask[0] == 1 && sparse.missing.front().mask[1] == 1,
        "missing masks retain exact separated sample positions across a 64-bit boundary");
    std::fprintf(stderr, "rtl_audio_mixer_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
