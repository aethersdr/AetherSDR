#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

template<typename Test>
bool runLeakChecked(const char* name, Test&& test)
{
    const uint64_t baseline = WdspChannel::outstandingAllocationsForTest();
    if (!test()) {
        return false;
    }
    const uint64_t outstanding = WdspChannel::outstandingAllocationsForTest();
    if (outstanding != baseline) {
        std::cerr << "FAIL: " << name << " leaked "
                  << (outstanding - baseline) << " WDSP allocations\n";
        return false;
    }
    return true;
}

double rms(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

double maximumDifference(std::span<const float> left, std::span<const float> right)
{
    double difference = 0.0;
    for (std::size_t sample = 0; sample < left.size(); ++sample) {
        difference = std::max(difference,
                              std::abs(static_cast<double>(left[sample]) -
                                       static_cast<double>(right[sample])));
    }
    return difference;
}

void fillComplexTone(std::span<float> i, std::span<float> q,
                     int sampleRate, double frequencyHz, std::size_t offset)
{
    for (std::size_t sample = 0; sample < i.size(); ++sample) {
        const double phase = 2.0 * std::numbers::pi * frequencyHz *
                             static_cast<double>(offset + sample) /
                             static_cast<double>(sampleRate);
        i[sample] = static_cast<float>(0.1 * std::cos(phase));
        q[sample] = static_cast<float>(0.1 * std::sin(phase));
    }
}

void fillAudioTone(std::span<float> left, std::span<float> right,
                   int sampleRate, double frequencyHz, std::size_t offset)
{
    for (std::size_t sample = 0; sample < left.size(); ++sample) {
        const double phase = 2.0 * std::numbers::pi * frequencyHz *
                             static_cast<double>(offset + sample) /
                             static_cast<double>(sampleRate);
        const float value = static_cast<float>(0.1 * std::cos(phase));
        left[sample] = value;
        right[sample] = value;
    }
}

bool runVector(WdspChannel::Direction direction)
{
    WdspChannel::Config config;
    config.direction = direction;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.mode = WdspChannel::Mode::Usb;
    config.blockForOutput = true;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str())) {
        return false;
    }
    std::unique_ptr<WdspChannel> reference = WdspChannel::create(config, &error);
    if (!require(reference != nullptr, error.c_str())) {
        return false;
    }

    std::vector<float> inputI(config.inputBlockSize);
    std::vector<float> inputQ(config.inputBlockSize);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());
    std::vector<float> referenceLeft(reference->outputBlockSize());
    std::vector<float> referenceRight(reference->outputBlockSize());
    double accumulatedEnergy = 0.0;

    for (std::size_t block = 0; block < 24; ++block) {
        if (direction == WdspChannel::Direction::Receive) {
            fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                            block * config.inputBlockSize);
        } else {
            fillAudioTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                          block * config.inputBlockSize);
        }

        const uint64_t allocationsBefore = WdspChannel::allocationSequenceForTest();
        const WdspChannel::ProcessResult result =
            channel->processIq(inputI, inputQ, outputLeft, outputRight);
        const WdspChannel::ProcessResult referenceResult =
            reference->processIq(inputI, inputQ, referenceLeft, referenceRight);
        const uint64_t allocationsAfter = WdspChannel::allocationSequenceForTest();
        if (!require(result == WdspChannel::ProcessResult::Ok,
                     "blocking vector processing failed") ||
            !require(referenceResult == WdspChannel::ProcessResult::Ok,
                     "reference vector processing failed") ||
            !require(allocationsAfter == allocationsBefore,
                     "WDSP allocated inside processIq") ||
            !require(maximumDifference(outputLeft, referenceLeft) < 1.0e-6 &&
                         maximumDifference(outputRight, referenceRight) < 1.0e-6,
                     "identical WDSP channels produced different vectors")) {
            return false;
        }
        if (block >= 8) {
            accumulatedEnergy += rms(outputLeft) + rms(outputRight);
        }
        if (!require(std::ranges::all_of(outputLeft, [](float value) {
                         return std::isfinite(value);
                     }), "WDSP produced non-finite output")) {
            return false;
        }
    }

    return require(accumulatedEnergy > 0.01,
                   direction == WdspChannel::Direction::Receive
                       ? "RX vector produced no demodulated audio"
                       : "TX vector produced no IQ output");
}

bool runUnderrunTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 64;
    config.dspBlockSize = 2048;
    config.blockForOutput = false;

    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config);
    if (!require(channel != nullptr, "could not create underrun channel")) {
        return false;
    }

    std::vector<float> inputI(config.inputBlockSize, 0.0f);
    std::vector<float> inputQ(config.inputBlockSize, 0.0f);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());
    int underruns = 0;
    for (int block = 0; block < 512; ++block) {
        const WdspChannel::ProcessResult result =
            channel->processIq(inputI, inputQ, outputLeft, outputRight);
        if (result == WdspChannel::ProcessResult::Underrun) {
            ++underruns;
        } else if (!require(result == WdspChannel::ProcessResult::Ok,
                            "unexpected result in underrun test")) {
            return false;
        }
    }
    return require(underruns > 0, "nonblocking test did not report an underrun");
}

bool runReconfigurationTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.blockForOutput = true;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str())) {
        return false;
    }

    const uint64_t allocationsBefore = WdspChannel::allocationSequenceForTest();
    config.inputBlockSize = 512;
    config.inputSampleRate = 96000;
    config.dspSampleRate = 48000;
    config.outputSampleRate = 48000;
    if (!require(channel->reconfigure(config, &error), error.c_str()) ||
        !require(channel->outputBlockSize() == 256,
                 "reconfiguration calculated the wrong output block size") ||
        !require(WdspChannel::allocationSequenceForTest() > allocationsBefore,
                 "reconfiguration did not rebuild WDSP resources")) {
        return false;
    }

    std::vector<float> inputI(config.inputBlockSize);
    std::vector<float> inputQ(config.inputBlockSize);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());
    fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0, 0);
    const uint64_t processAllocations = WdspChannel::allocationSequenceForTest();
    const WdspChannel::ProcessResult result =
        channel->processIq(inputI, inputQ, outputLeft, outputRight);
    return require(result == WdspChannel::ProcessResult::Ok,
                   "processing after reconfiguration failed") &&
           require(WdspChannel::allocationSequenceForTest() == processAllocations,
                   "processing after reconfiguration allocated memory");
}

// WDSP notch handles are POSITIONAL, and the whole stable-id mapping in
// Hl2Backend is built on exactly how they shift. Pin that behaviour here rather
// than inferring it from nbp.c, so a WDSP refresh that changes it fails loudly.
bool runNotchIndexTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.blockForOutput = true;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str())) {
        return false;
    }

    const double tuneHz = 7'000'000.0;
    if (!require(channel->setNotchTuneFrequency(tuneHz),
                 "could not set the notch tune frequency") ||
        !require(channel->notchCount() == 0,
                 "a fresh channel reported existing notches")) {
        return false;
    }

    // Append three, then confirm they read back in the order they went in.
    for (int index = 0; index < 3; ++index) {
        if (!require(channel->addNotch(index, tuneHz + 1000.0 * (index + 1),
                                       400.0, true),
                     "could not add a notch")) {
            return false;
        }
    }
    if (!require(channel->notchCount() == 3, "notch count did not reach 3")) {
        return false;
    }

    // Deleting the middle notch must close the gap: what was index 2 becomes
    // index 1. A caller holding stable ids has to remap here or it edits the
    // wrong notch — this is the exact behaviour Hl2Backend::notchIndexFor()
    // and the ordered m_notches vector exist to absorb.
    if (!require(channel->removeNotch(1), "could not remove the middle notch") ||
        !require(channel->notchCount() == 2, "notch count did not fall to 2")) {
        return false;
    }
    double center = 0.0;
    double width = 0.0;
    bool active = false;
    if (!require(channel->notchAt(0, &center, &width, &active),
                 "could not read notch 0 back") ||
        !require(std::abs(center - (tuneHz + 1000.0)) < 1.0,
                 "notch 0 moved when a later notch was deleted") ||
        !require(channel->notchAt(1, &center, &width, &active),
                 "could not read notch 1 back") ||
        !require(std::abs(center - (tuneHz + 3000.0)) < 1.0,
                 "deleting a notch did not shift the ones above it down")) {
        return false;
    }

    // Centres round-trip as ABSOLUTE Hz, unmodified. If a sign correction ever
    // creeps back into WdspChannel, this is what catches it.
    if (!require(channel->editNotch(0, tuneHz - 1500.0, 250.0, false),
                 "could not edit a notch") ||
        !require(channel->notchAt(0, &center, &width, &active),
                 "could not read an edited notch back") ||
        !require(std::abs(center - (tuneHz - 1500.0)) < 1.0,
                 "an edited notch centre did not round-trip") ||
        !require(std::abs(width - 250.0) < 1.0,
                 "an edited notch width did not round-trip") ||
        !require(!active, "an edited notch kept the wrong active flag")) {
        return false;
    }

    // Out-of-range handles are refused rather than silently clamped.
    if (!require(!channel->removeNotch(9), "removing a nonexistent notch succeeded") ||
        !require(!channel->editNotch(9, tuneHz, 200.0, true),
                 "editing a nonexistent notch succeeded") ||
        !require(!channel->notchAt(9, &center, &width, &active),
                 "reading a nonexistent notch succeeded")) {
        return false;
    }

    // The documented width floor, which the UI's width presets are built on.
    if (!require(std::abs(channel->minimumNotchWidthHz() - 200.0) < 1.0,
                 "the 2048-tap minimum notch width is no longer 200 Hz")) {
        return false;
    }
    return require(channel->setNotchesEnabled(false) &&
                       channel->setNotchesEnabled(true),
                   "could not toggle the global notch run flag");
}

// The functional half: a notch parked on a tone must actually remove it, and a
// notch parked on that tone's MIRROR must not. Index bookkeeping can be right
// while the audio is untouched, and an inverted axis passes every API check.
bool runNotchAttenuationTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.inputSampleRate = 48000;
    config.dspSampleRate = 48000;
    config.outputSampleRate = 48000;
    config.mode = WdspChannel::Mode::Usb;
    config.filterLowHz = 150.0;
    config.filterHighHz = 3000.0;
    // AGC off with a fixed gain: an AGC would claw the level back after the
    // notch and mask exactly the attenuation being measured.
    config.agcMode = 0;
    config.agcFixedGainDb = 0.0;
    config.blockForOutput = true;
    // The notch-capable tap count HL2 runs while a notch is placed
    // (Hl2RxDsp::kRxFilterTapsLong), so the null is measured in the
    // configuration the operator hears rather than in WDSP's 2048 default —
    // which is also the one whose 200 Hz notch floor this measurement exists
    // to get away from.
    config.filterTaps = 8192;

    const double tuneHz = 7'000'000.0;
    // The tone's audio pitch, and therefore the RF frequency it corresponds to.
    // Negative baseband because RXA as configured passes the opposite sign to
    // its passband bounds (see Hl2RxDsp::onIqBlock) — this is the geometry the
    // HL2 actually runs in, so the notch has to work in it.
    const double toneBasebandHz = -1500.0;
    const double toneRfHz = tuneHz + 1500.0;

    const auto measure = [&](double notchRfHz) -> double {
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!channel) {
            return -1.0;
        }
        if (!channel->setNotchTuneFrequency(tuneHz)) {
            return -1.0;
        }
        if (notchRfHz != 0.0) {
            if (!channel->addNotch(0, notchRfHz, 400.0, true) ||
                !channel->setNotchesEnabled(true)) {
                return -1.0;
            }
        }
        std::vector<float> inputI(config.inputBlockSize);
        std::vector<float> inputQ(config.inputBlockSize);
        std::vector<float> outputLeft(channel->outputBlockSize());
        std::vector<float> outputRight(channel->outputBlockSize());
        double energy = 0.0;
        // Discard the first blocks: the channel's mute ramp and the filter's
        // own group delay both suppress output that has nothing to do with the
        // notch, and 8192-tap masks take a while to fill.
        constexpr std::size_t kSettleBlocks = 40;
        constexpr std::size_t kTotalBlocks = 80;
        for (std::size_t block = 0; block < kTotalBlocks; ++block) {
            fillComplexTone(inputI, inputQ, config.inputSampleRate,
                            toneBasebandHz, block * config.inputBlockSize);
            if (channel->processIq(inputI, inputQ, outputLeft, outputRight) !=
                WdspChannel::ProcessResult::Ok) {
                return -1.0;
            }
            if (block >= kSettleBlocks) {
                energy += rms(outputLeft);
            }
        }
        return energy;
    };

    const double unnotched = measure(0.0);
    const double notched = measure(toneRfHz);
    // The mirror image: as far below the tuned frequency as the tone is above.
    // A notch here must leave the tone alone. If this attenuates instead of the
    // one above, the notch axis is inverted — the failure an API-only test
    // cannot see, because both notches are equally well-formed.
    const double mirrorNotched = measure(tuneHz - 1500.0);

    if (!require(unnotched > 0.0 && notched >= 0.0 && mirrorNotched >= 0.0,
                 "notch attenuation measurement failed to run")) {
        return false;
    }
    if (!require(unnotched > 1.0e-4, "the unnotched tone produced no audio")) {
        return false;
    }
    return require(notched < unnotched * 0.25,
                   "a notch on the tone did not attenuate it") &&
           require(mirrorNotched > unnotched * 0.75,
                   "a notch on the tone's mirror image attenuated the tone — "
                   "the notch frequency axis is inverted");
}

bool runLifecycleTest()
{
    const uint64_t baseline = WdspChannel::outstandingAllocationsForTest();
    for (int iteration = 0; iteration < 3; ++iteration) {
        std::unique_ptr<WdspChannel> first = WdspChannel::create({});
        std::unique_ptr<WdspChannel> second = WdspChannel::create({});
        if (!require(first != nullptr && second != nullptr,
                     "could not create concurrent WDSP channels") ||
            !require(first->channelIdForTest() != second->channelIdForTest(),
                     "RAII owners received the same WDSP channel ID")) {
            return false;
        }
        second.reset();
        first.reset();
        const uint64_t outstanding = WdspChannel::outstandingAllocationsForTest();
        if (outstanding != baseline) {
            std::cerr << "FAIL: WDSP teardown allocation baseline=" << baseline
                      << " outstanding=" << outstanding
                      << " iteration=" << iteration << '\n';
            return false;
        }
    }
    return true;
}

// The wisdom cache must be on disk when open() RETURNS — not at process exit.
//
// What this pins: export used to be a std::atexit handler alone, and the app's
// own signal path (Hl2EmergencyStop restores SIG_DFL and re-raises) turns every
// SIGTERM, crash and Force Quit into an exit that never runs one. The plans were
// measured and then thrown away, so the next launch paid full first-run cost
// again — for anyone who had ever force-quit, on every run.
//
} // namespace

int main()
{
    const uint64_t allocationBaseline = WdspChannel::outstandingAllocationsForTest();
    WdspChannel::Config invalid;
    invalid.inputSampleRate = 44100;
    invalid.dspSampleRate = 48000;
    std::string validationError;
    if (!require(WdspChannel::create(invalid, &validationError) == nullptr,
                 "invalid non-integral rate configuration was accepted") ||
        !runLeakChecked("lifecycle test", runLifecycleTest) ||
        !runLeakChecked("RX vector", [] {
            return runVector(WdspChannel::Direction::Receive);
        }) ||
        !runLeakChecked("TX vector", [] {
            return runVector(WdspChannel::Direction::Transmit);
        }) ||
        !runLeakChecked("underrun test", runUnderrunTest) ||
        !runLeakChecked("reconfiguration test", runReconfigurationTest) ||
        !runLeakChecked("notch index test", runNotchIndexTest) ||
        !runLeakChecked("notch attenuation test", runNotchAttenuationTest) ||
        !require(WdspChannel::outstandingAllocationsForTest() == allocationBaseline,
                 "WDSP test suite left allocations outstanding")) {
        return 1;
    }

    std::cout << "WDSP channel tests passed\n";
    return 0;
}
