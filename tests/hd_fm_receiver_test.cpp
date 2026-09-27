#include "core/backends/rtl/HdFmReceiver.h"
#include "core/Resampler.h"
// The pinned C API header does not declare C++ linkage guards.
extern "C" {
#include <nrsc5.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using Registry = AetherSDR::rtl::RtlReceiverRegistry;
using Decoder = AetherSDR::rtl::Nrsc5FmDecoder;
using Raw = AetherSDR::rtl::HdFmRawReception;
using Identity = AetherSDR::rtl::HdFmAudioIdentity;
using AetherSDR::rtl::prepareHdFmReceiver;
static int failures = 0;
static void check(bool value, const char* text)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); } }
struct Control {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned requested = 0, done = 0;
    int blocks = 0;
    bool pauseAfter = false, paused = false, release = false, retireOnRelease = false;
    void command(int count, bool pause = false)
    {
        const std::lock_guard lock(mutex);
        blocks = count; pauseAfter = pause; paused = false; release = false; ++requested;
    }
    bool wait(bool forPause)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return forPause ? paused : done == requested; });
    }
    bool releaseWithLoss()
    {
        std::unique_lock lock(mutex);
        retireOnRelease = true; release = true; changed.notify_all();
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return done == requested; });
    }
    void releaseQuietly()
    { const std::lock_guard lock(mutex); retireOnRelease = false; release = true; changed.notify_all(); }
};
// Injects documented callback events into the production wrapper on its real
// private decoder thread. This is queue/lifecycle evidence, not RF decoding.
struct Pipe final : Decoder::Pipe {
    Control& control;
    std::array<std::int16_t, 4096> pcm{};
    explicit Pipe(Control& value) : control(value)
    {
        for (std::size_t i = 0; i < 2048; ++i) {
            pcm[2 * i] = std::int16_t(11000 * std::sin(0.031 * i));
            pcm[2 * i + 1] = std::int16_t(8000 * std::cos(0.057 * i));
        }
    }
    bool valid() const noexcept override { return true; }
    void audio(int blocks, Callback callback, void* context)
    {
        nrsc5_event_t sync{}; sync.event = NRSC5_EVENT_SYNC; callback(&sync, context);
        nrsc5_event_t event{}; event.event = NRSC5_EVENT_AUDIO; event.audio.program = 0;
        event.audio.data = pcm.data(); event.audio.count = pcm.size();
        for (int i = 0; i < blocks; ++i) { callback(&event, context); }
    }
    bool feed(std::span<const float>, Callback callback, void* context) noexcept override
    {
        std::unique_lock lock(control.mutex);
        if (control.done == control.requested) { return true; }
        const unsigned request = control.requested;
        const int count = control.blocks; const bool pause = control.pauseAfter;
        lock.unlock();
        if (count > 0) { audio(count, callback, context); }
        lock.lock();
        if (pause) {
            control.paused = true; control.changed.notify_all();
            control.changed.wait(lock, [&] { return control.release; });
            const bool loss = control.retireOnRelease;
            lock.unlock();
            if (loss) {
                nrsc5_event_t event{}; event.event = NRSC5_EVENT_LOST_SYNC; callback(&event, context);
                audio(1, callback, context);
            }
            lock.lock();
        }
        control.done = request; control.changed.notify_all();
        return true;
    }
};
struct Sink final : Registry::AudioSink {
    Raw last;
    std::uint64_t publication = 0, tapFrames = 0, speakerFrames = 0, nonzeroSpeaker = 0;
    std::uint64_t tapEpoch = 0, tapNext = 0, producedSpeakerFrames = 0;
    bool speakerPaired = false;
    bool ordered = true, paired = false, zeroWhileAcquiring = true, releasePassed = false;
    Identity lastSpeaker;
    Control* retireAtReady = nullptr;
    std::vector<Raw> observations;
    Sink() { observations.reserve(1024); }
    void audioBlock(const Registry::ReceiverSpec&, std::uint64_t, std::span<const float>, std::span<const float>, bool) noexcept override
    { check(false, "HD never impersonates the analog 48 kHz tap"); }
    void hdObservation(const Registry::ReceiverSpec&, const Raw& raw) noexcept override
    {
        ordered &= raw.publicationSequence > publication;
        publication = raw.publicationSequence; last = raw; observations.push_back(raw);
        if (raw.audioValid && retireAtReady) {
            auto* control = retireAtReady; retireAtReady = nullptr;
            releasePassed = control->releaseWithLoss();
        }
    }
    void hdDecodedAudio(const Registry::ReceiverSpec&, const Identity& identity, std::uint64_t first,
        std::span<const float> left, std::span<const float> right, bool discontinuity) noexcept override
    {
        if (identity.audioEpoch != tapEpoch) { tapEpoch = identity.audioEpoch; tapNext = 0; ordered &= discontinuity; }
        ordered &= last.audioValid && identity.audioEpoch == last.audioEpoch && first == tapNext
            && identity.producedMonotonicMs > 0 && left.size() == right.size();
        tapNext += left.size(); tapFrames += left.size();
        for (std::size_t i = 0; i < left.size(); ++i) { paired |= left[i] != right[i]; }
    }
    void hdSpeakerAudio(const Registry::ReceiverSpec&, const Identity& identity, std::uint64_t,
        std::span<const float> left, std::span<const float> right, bool) noexcept override
    {
        lastSpeaker = identity; speakerFrames += left.size();
        if (identity.producedMonotonicMs) { producedSpeakerFrames += left.size(); }
        ordered &= left.size() == right.size() && left.size() % 128 == 0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i] != 0 || right[i] != 0) { ++nonzeroSpeaker; }
            speakerPaired |= left[i] != right[i];
            if (!last.audioValid) { zeroWhileAcquiring &= left[i] == 0 && right[i] == 0; }
            ordered &= std::isfinite(left[i]) && std::isfinite(right[i]);
        }
    }
};
static Registry::ReceiverSpec spec()
{
    Registry::ReceiverSpec value;
    value.handle = {1, 2, 0}; value.passband = {0, 100e6, -90000, 90000, 0, 1000, 1000};
    value.capture = {1, 1, 100e6, 1000000, 450000, 450000}; value.extractRf = true; value.epoch = 7;
    value.hdFm = AetherSDR::rtl::HdFmRecipe{0};
    return value;
}
int main()
{
    // Query the actual converter before starting the worker or its freshness clock.
    const int speakerDelayInput = AetherSDR::Resampler(44100, 48000, 2048).groupDelayInputFrames();
    const auto recipe = spec(); Control control; Sink sink; std::string error;
    auto receiver = prepareHdFmReceiver(recipe, error, std::make_unique<Pipe>(control));
    check(bool(receiver), "injected pipe prepares the production bounded HD receiver");
    if (!receiver) { return 1; }
    std::array<std::complex<float>, 8192> iq{};
    std::uint64_t captureFirst = 0, revision = 9;
    const auto step = [&] {
        const bool result = receiver->processCapture({1, recipe.capture, captureFirst, false, iq, 8, revision}, sink);
        captureFirst += iq.size(); return result;
    };
    check(step() && sink.speakerFrames > 0 && sink.tapFrames == 0 && !sink.last.audioValid,
        "acquiring supplies complete intentional-zero speaker quanta and no fabricated decoded tap");
    // Four native frames fill playout. Hold the worker after its callbacks so
    // loss can occur exactly during the readiness publication, before taps.
    control.command(4, true); check(step() && control.wait(true), "worker reached deterministic post-audio hold");
    sink.retireAtReady = &control;
    check(step() && sink.releasePassed, "loss injected between ready speaker buffer and tap dispatch");
    check(sink.tapFrames == 0 && !sink.last.audioValid && sink.zeroWhileAcquiring,
        "replacement audio epoch cannot inherit old prefill or leak queued native tap");
    // The loss callback also emitted one new native frame. It must remain held
    // until three more complete its own prebuffer, with no lost initial tap.
    control.command(3); check(step() && control.wait(false), "new epoch produced its remaining prebuffer");
    const bool prefillStep = step();
    const bool prefillPassed = prefillStep && sink.last.audioValid && sink.tapFrames == 8192
        && sink.paired && sink.ordered;
    if (!prefillPassed) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr,
            "HD prefill diagnostic: step=%d valid=%d synced=%d audioValid=%d tapFrames=%llu paired=%d "
            "speakerFrames=%llu nonzeroSpeaker=%llu tapEpoch=%llu tapNext=%llu rawEpoch=%llu "
            "speakerEpoch=%llu speakerRevision=%llu speakerProducedMs=%llu nowMs=%lld "
            "publication=%llu observation=%llu audioSequence=%llu iqDrops=%llu pcmDrops=%llu underruns=%llu "
            "ordered=%d zeroWhileAcquiring=%d observations=%zu\n",
            prefillStep, sink.last.valid, sink.last.synced, sink.last.audioValid,
            static_cast<unsigned long long>(sink.tapFrames), sink.paired,
            static_cast<unsigned long long>(sink.speakerFrames), static_cast<unsigned long long>(sink.nonzeroSpeaker),
            static_cast<unsigned long long>(sink.tapEpoch), static_cast<unsigned long long>(sink.tapNext),
            static_cast<unsigned long long>(sink.last.audioEpoch), static_cast<unsigned long long>(sink.lastSpeaker.audioEpoch),
            static_cast<unsigned long long>(sink.lastSpeaker.revision), static_cast<unsigned long long>(sink.lastSpeaker.producedMonotonicMs),
            static_cast<long long>(ms), static_cast<unsigned long long>(sink.last.publicationSequence),
            static_cast<unsigned long long>(sink.last.observationSequence), static_cast<unsigned long long>(sink.last.audioSequence),
            static_cast<unsigned long long>(sink.last.iqDrops), static_cast<unsigned long long>(sink.last.pcmDrops),
            static_cast<unsigned long long>(sink.last.playoutUnderruns), sink.ordered, sink.zeroWhileAcquiring,
            sink.observations.size());
    }
    check(prefillPassed,
        "new-epoch prefill publishes all 8192 ordered original paired 44100 frames");
    // prewarm() consumes r8brain's no-output interval, but its acoustic delay
    // remains. Advance only capture time: no extra decoder AUDIO event can
    // hide a lost prefix, reset, or insufficient replacement-epoch prefill.
    const std::uint64_t audioEpoch = sink.last.audioEpoch;
    const std::uint64_t delayOutput = (static_cast<std::uint64_t>(std::max(0, speakerDelayInput)) * 48000 + 44099) / 44100;
    const std::uint64_t captureRate = static_cast<std::uint64_t>(recipe.capture.achievedSampleRateHz);
    const std::uint64_t maximumStepFrames = ((iq.size() * 48000 + captureRate - 1) / captureRate + 127) / 128 * 128;
    const std::uint64_t minimumStepFrames = iq.size() * 48000 / captureRate / 128 * 128;
    // Readiness can precede the final fixture step. Subtract all audio already
    // consumed, and reserve one callback for the revision-preservation check.
    const std::uint64_t remainingFrames = 8192 - std::min<std::uint64_t>(8192, sink.producedSpeakerFrames);
    const std::uint64_t availableSteps = remainingFrames / maximumStepFrames;
    const std::uint64_t maximumDrainSteps = availableSteps > 0 ? availableSteps - 1 : 0;
    const bool delayFits = speakerDelayInput >= 0
        && delayOutput + 128 <= sink.producedSpeakerFrames + maximumDrainSteps * minimumStepFrames;
    bool drainSucceeded = true;
    std::uint64_t drainSteps = 0;
    if (prefillPassed && delayFits) {
        while (drainSteps < maximumDrainSteps
            && (sink.producedSpeakerFrames < delayOutput + 128 || !sink.nonzeroSpeaker || !sink.speakerPaired)) {
            ++drainSteps;
            if (!step()) { drainSucceeded = false; break; }
        }
    }
    const bool speakerPassed = prefillPassed && delayFits && drainSucceeded
        && sink.producedSpeakerFrames >= delayOutput + 128 && sink.nonzeroSpeaker > 0 && sink.speakerPaired
        && sink.tapFrames == 8192 && sink.tapNext == 8192 && sink.tapEpoch == audioEpoch && sink.ordered
        && sink.last.audioValid && sink.last.audioEpoch == audioEpoch && sink.lastSpeaker.audioEpoch == audioEpoch
        && sink.lastSpeaker.revision == 9 && sink.lastSpeaker.producedMonotonicMs > 0
        && sink.last.iqDrops == 0 && sink.last.pcmDrops == 0 && sink.last.playoutUnderruns == 0;
    if (!speakerPassed) {
        std::fprintf(stderr,
            "HD speaker diagnostic: delayInput=%d delayOutput=%llu steps=%llu/%llu producedFrames=%llu "
            "nonzero=%llu paired=%d tapFrames=%llu epoch=%llu rawEpoch=%llu valid=%d ordered=%d "
            "iqDrops=%llu pcmDrops=%llu underruns=%llu\n",
            speakerDelayInput, static_cast<unsigned long long>(delayOutput),
            static_cast<unsigned long long>(drainSteps), static_cast<unsigned long long>(maximumDrainSteps),
            static_cast<unsigned long long>(sink.producedSpeakerFrames), static_cast<unsigned long long>(sink.nonzeroSpeaker),
            sink.speakerPaired, static_cast<unsigned long long>(sink.tapFrames), static_cast<unsigned long long>(audioEpoch),
            static_cast<unsigned long long>(sink.last.audioEpoch), sink.last.audioValid, sink.ordered,
            static_cast<unsigned long long>(sink.last.iqDrops), static_cast<unsigned long long>(sink.last.pcmDrops),
            static_cast<unsigned long long>(sink.last.playoutUnderruns));
    }
    check(speakerPassed, "bounded capture-only playout produces nonzero distinct 48000 channels after converter delay without losing native taps");
    const std::uint64_t measuredSequence = sink.last.observationSequence;
    check(sink.ordered && sink.zeroWhileAcquiring && sink.last.publicationSequence > 0,
        "measurement and delivery transitions preserve order without false valid audio");
    revision = 10;
    check(step() && sink.lastSpeaker.revision == 9 && sink.lastSpeaker.producedMonotonicMs > 0,
        "buffered speaker prefix retains original token/time across a monitor-only revision");
    check(sink.last.observationSequence >= measuredSequence, "control changes do not invent decoder measurements");
    control.releaseQuietly(); receiver.reset();

    // Global resident reservation covers offered + active decoder workers.
    Control one, two, three;
    auto a = prepareHdFmReceiver(recipe, error, std::make_unique<Pipe>(one));
    auto b = prepareHdFmReceiver(recipe, error, std::make_unique<Pipe>(two));
    auto c = prepareHdFmReceiver(recipe, error, std::make_unique<Pipe>(three));
    check(a && b && !c, "hard process-wide two-worker bound includes old/new resident sessions");
    a.reset(); b.reset();

    Control retained;
    auto reservation = WdspChannel::reserveChannels(1);
    check(reservation.has_value(), "HD conservatively reserves the shared DSP pool");
    std::shared_ptr<Registry::Receiver> original = prepareHdFmReceiver(recipe, error,
        std::make_unique<Pipe>(retained), std::move(reservation));
    auto reused = original;
    reservation.reset(); original.reset();
    check(reused && !WdspChannel::reserveChannels(32),
        "unused reservation follows shared HD receiver after original bank ownership retires");
    reused.reset();
    check(WdspChannel::reserveChannels(32).has_value(), "HD teardown returns the unused pool reservation");

    Control overload; Sink overflowSink;
    auto bounded = prepareHdFmReceiver(recipe, error, std::make_unique<Pipe>(overload));
    std::uint64_t first = 0;
    const auto push = [&] {
        const bool ok = bounded->processCapture({1, recipe.capture, first, false, iq, 8, 9}, overflowSink);
        first += iq.size(); return ok;
    };
    overload.command(0, true); check(push() && overload.wait(true), "worker held before bounded queue overflow");
    bool refused = false;
    for (int i = 0; i < 34 && !refused; ++i) { refused = !push(); }
    check(refused && !overflowSink.last.audioValid && overflowSink.last.iqDrops == 1,
        "acquisition never blocks on a full IQ queue and withdraws truthfully");
    overload.releaseQuietly(); bounded.reset();
    std::printf("%s — %d failure(s)\n", failures ? "FAIL" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
