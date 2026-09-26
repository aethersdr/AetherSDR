#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
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

// A CONSTANT-ENVELOPE FM SIGNAL at baseband: a carrier on the tuned frequency
// whose phase carries a single audio tone. exp(j * beta * sin(2*pi*fa*t)) with
// beta = peakDeviation / audioHz, which is the textbook modulation index — so
// the instantaneous frequency swings +/- peakDeviationHz about zero at the
// audio rate, and the amplitude never moves. Phase is a function of the
// absolute sample index, so successive blocks join without a discontinuity;
// a per-block phase reset would put a click at every block boundary and the
// detector would demodulate the clicks.
void fillFmTone(std::span<float> i, std::span<float> q, int sampleRate,
                double audioHz, double peakDeviationHz, std::size_t offset)
{
    const double index = peakDeviationHz / audioHz;
    for (std::size_t sample = 0; sample < i.size(); ++sample) {
        const double t = static_cast<double>(offset + sample) /
                         static_cast<double>(sampleRate);
        const double phase = index * std::sin(2.0 * std::numbers::pi * audioHz * t);
        i[sample] = static_cast<float>(0.5 * std::cos(phase));
        q[sample] = static_cast<float>(0.5 * std::sin(phase));
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

// #5734, AetherSDR WDSP patch 13: A BLOCKED processIq() MUST NOT BE RELEASED
// BEFORE THE WORKER HAS TAKEN ITS INPUT.
//
// With blockForOutput and inputBlockSize == dspBlockSize, WDSP's input ring
// holds two blocks. Worker iteration w reads the slot host call w wrote, and
// host call w + 2 writes that same slot again. The only thing keeping them
// apart is ordering inside dexchange(): upstream released the host (Sem_OutReady)
// FIRST and copied its input slot out SECOND. A worker preempted between the two
// processed block w + 2, or a torn mix of w and w + 2, in place of block w --
// and nothing reported it. runVector() above caught it as two identical channels
// diverging under CPU load (0.058 on #5734's box); on an idle one the window is
// a few instructions wide and the host never wins.
//
// This case makes the host win on purpose, so it does not need a loaded
// machine: the port's test-only pause puts the worker to sleep right after the
// release, which is exactly the preemption #5734's load produced. Before patch
// 13 the input copy was still pending at that point. Measured with the reorder
// reverted: RX differs by 0.00205633 at block 3 -- the same figure #5734
// recorded on Arch under load, to every printed digit -- and TX by 1.0e-5 at
// block 6, on every run. At RX block 3 the correct stream is exact zeros (the
// mute delay-up) and the overwritten one is a later, ramped block, which is
// the "rmsB=0" #5734 read as a channel that wrote nothing. With patch 13 the
// copy is already done, the pause is only a slower worker, and the output is
// the same stream an unpaused channel produces.
//
// The comparison is against an unpaused channel rather than a stored vector
// for the reason runVector() gives: it pins "the scheduler does not change the
// numbers", which is the actual contract, and not whatever the chain
// computed on the day the vector was recorded.
bool runWorkerHandoffTest(WdspChannel::Direction direction)
{
    struct PauseReset {
        ~PauseReset() { WdspChannel::setWorkerHandoffPauseForTest(0); }
    } pauseReset;

    WdspChannel::Config config;
    config.direction = direction;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.mode = WdspChannel::Mode::Usb;
    config.blockForOutput = true;
    constexpr std::size_t kBlocks = 24;

    const auto clock = [&](unsigned pauseMicroseconds,
                           std::vector<std::vector<float>>& left,
                           std::vector<std::vector<float>>& right) {
        WdspChannel::setWorkerHandoffPauseForTest(pauseMicroseconds);
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!require(channel != nullptr, error.c_str())) {
            return false;
        }
        std::vector<float> inputI(config.inputBlockSize);
        std::vector<float> inputQ(config.inputBlockSize);
        left.assign(kBlocks, std::vector<float>(channel->outputBlockSize()));
        right.assign(kBlocks, std::vector<float>(channel->outputBlockSize()));
        for (std::size_t block = 0; block < kBlocks; ++block) {
            if (direction == WdspChannel::Direction::Receive) {
                fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                                block * config.inputBlockSize);
            } else {
                fillAudioTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                              block * config.inputBlockSize);
            }
            if (!require(channel->processIq(inputI, inputQ, left[block], right[block]) ==
                             WdspChannel::ProcessResult::Ok,
                         "handoff test: processIq failed")) {
                return false;
            }
        }
        return true;
    };

    std::vector<std::vector<float>> referenceLeft, referenceRight;
    std::vector<std::vector<float>> pausedLeft, pausedRight;
    // 5 ms: three orders of magnitude longer than the host needs to return,
    // fill the next block and write it, so an unfixed tree loses every time
    // rather than when the scheduler happens to cooperate.
    if (!clock(0, referenceLeft, referenceRight) ||
        !clock(5000, pausedLeft, pausedRight)) {
        return false;
    }
    double energy = 0.0;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        const double difference =
            std::max(maximumDifference(pausedLeft[block], referenceLeft[block]),
                     maximumDifference(pausedRight[block], referenceRight[block]));
        if (difference >= 1.0e-6) {
            std::cerr << "FAIL: " << (direction == WdspChannel::Direction::Receive ? "RX" : "TX")
                      << " block " << block << " differs by " << difference
                      << " when the worker is slow to return after releasing the host\n";
            return require(false,
                           "a slow DSP worker changed the output: the host overwrote "
                           "an input block the worker had not yet read (#5734)");
        }
        energy += rms(referenceLeft[block]);
    }
    // A positive control on the comparison: two silent streams agree trivially.
    return require(energy > 0.01, "handoff test: reference stream carried no signal");
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
    // The tap count HL2 actually runs (Hl2RxDsp::kRxFilterTaps), so the null is
    // measured in the configuration the operator hears rather than in WDSP's
    // 2048 default — which is also the one whose 200 Hz notch floor this PR
    // exists to get away from.
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

// A STOP IS NOT A CLOSE, and this is what says so in observable terms.
//
// Two things a close-and-reopen would have discarded, both readable from
// outside the class:
//
//   * the notch database — WDSP keeps it inside the channel, so a close frees
//     it and Hl2RxDsp::configure() has to replay every notch by hand;
//   * the allocation sequence — every rebuild re-plans FFTW and re-allocates
//     the buffers, which on this codebase is the expensive half of a connect.
//
// Across a stop/start neither moves. Across a reconfigure() — the close-and-
// reopen this class still does for a rate change (HERMES §13 Tier 4) — both
// do, and the second half of this test pins that contrast so the first half
// cannot pass by accident on a channel that was never really stopped.
bool runStartStopTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.mode = WdspChannel::Mode::Usb;
    // Blocking, as runVector() does and for the same reason: nothing here
    // paces the loop, so in the non-blocking form this thread outruns WDSP's
    // worker and every block underruns — the "audio came back" half of the
    // test would then pass vacuously against silence it caused itself.
    //
    // Safe across the stop, which is the part worth stating. fexchange2's wait
    // sits behind WDSP's exchange bit: while the down-slew is still running
    // the bit is set and the worker is still producing, so the wait is
    // satisfied; once the ramp finishes the bit clears and fexchange2 returns
    // without reaching the wait at all. A stopped channel never blocks here.
    config.blockForOutput = true;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str()) ||
        !require(channel->isRunning(), "a freshly opened channel was not running")) {
        return false;
    }

    const double tuneHz = 7'000'000.0;
    if (!require(channel->setNotchTuneFrequency(tuneHz),
                 "could not set the notch tune frequency")) {
        return false;
    }
    for (int index = 0; index < 3; ++index) {
        if (!require(channel->addNotch(index, tuneHz + 1000.0 * (index + 1),
                                       400.0, true),
                     "could not seed a notch before the stop")) {
            return false;
        }
    }
    if (!require(channel->notchCount() == 3, "the seeded notches did not land")) {
        return false;
    }

    std::vector<float> inputI(config.inputBlockSize);
    std::vector<float> inputQ(config.inputBlockSize);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());

    // ENERGY AND THE Ok COUNT TOGETHER, never energy alone. clock() accumulates
    // only on Ok, so a block that returns Busy, Underrun or InvalidBuffer
    // contributes exactly 0 and is otherwise invisible. Every silence assertion
    // below would then pass vacuously against a regression that made processIq()
    // fail for all 32 blocks: "correctly produced silence" and "produced nothing
    // at all" are the same number. Counting Ok separates them, which is the only
    // way the zero-fill contract these assertions exist for is actually pinned.
    // Raised in review of #5628.
    struct Clocked
    {
        double energy = 0.0;
        int okBlocks = 0;
    };
    const auto clock = [&](std::size_t blocks, std::size_t offset) {
        Clocked clocked;
        for (std::size_t block = 0; block < blocks; ++block) {
            fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                            (offset + block) * config.inputBlockSize);
            const WdspChannel::ProcessResult result =
                channel->processIq(inputI, inputQ, outputLeft, outputRight);
            if (result == WdspChannel::ProcessResult::Ok) {
                clocked.energy += rms(outputLeft) + rms(outputRight);
                ++clocked.okBlocks;
            }
        }
        return clocked;
    };

    if (!require(clock(64, 0).energy > 0.01,
                 "the running channel produced no audio")) {
        return false;
    }

    // ── Stop ──────────────────────────────────────────────────────────────
    const uint64_t allocationsBeforeStop = WdspChannel::allocationSequenceForTest();
    if (!require(channel->setRunning(false), "the channel refused to stop") ||
        !require(!channel->isRunning(), "the channel still reported itself running")) {
        return false;
    }

    // The ramp needs clocking to play out — that is the half of the contract
    // SetChannelState cannot perform on its own. Once it has, the output must
    // be SILENCE and not the last block before the stop held forever, which is
    // what fexchange2 leaves behind when it stops writing the buffer at all.
    clock(64, 64);
    const Clocked tail = clock(32, 128);
    if (!require(tail.okBlocks == 32,
                 "a stopped channel stopped accepting blocks — the silence "
                 "assertion below would have passed against nothing at all")) {
        std::cerr << "       " << tail.okBlocks << " of 32 blocks returned Ok\n";
        return false;
    }
    if (!require(tail.energy == 0.0,
                 "a stopped channel kept producing output — the buffer is stale, "
                 "not silent")) {
        return false;
    }

    // Everything a close would have thrown away is still here.
    double center = 0.0;
    double width = 0.0;
    bool active = false;
    if (!require(channel->notchCount() == 3,
                 "stopping the channel destroyed the notch database") ||
        !require(channel->notchAt(2, &center, &width, &active) &&
                     std::abs(center - (tuneHz + 3000.0)) < 1.0,
                 "a notch did not survive the stop intact")) {
        return false;
    }

    // ── Start again ───────────────────────────────────────────────────────
    if (!require(channel->setRunning(true), "the channel refused to start") ||
        !require(channel->isRunning(), "the channel did not report itself running")) {
        return false;
    }
    if (!require(WdspChannel::allocationSequenceForTest() == allocationsBeforeStop,
                 "a stop/start allocated — it rebuilt the channel instead of "
                 "changing its state")) {
        return false;
    }
    if (!require(clock(128, 160).energy > 0.01,
                 "the restarted channel produced no audio") ||
        !require(channel->notchCount() == 3,
                 "restarting the channel destroyed the notch database")) {
        return false;
    }

    // Setting the state it is already in succeeds and does nothing.
    if (!require(channel->setRunning(true), "a redundant start was refused") ||
        !require(WdspChannel::allocationSequenceForTest() == allocationsBeforeStop,
                 "a redundant start allocated")) {
        return false;
    }

    // ── The contrast: reconfigure() IS a close-and-reopen ──────────────────
    // Same config, so nothing about the channel's shape changes — and both
    // observables move anyway, because the channel was destroyed and rebuilt.
    if (!require(channel->reconfigure(config, &error), error.c_str()) ||
        !require(WdspChannel::allocationSequenceForTest() > allocationsBeforeStop,
                 "reconfigure() did not rebuild the channel") ||
        !require(channel->notchCount() == 0,
                 "reconfigure() kept the notch database — the contrast this "
                 "test rests on no longer holds")) {
        return false;
    }

    // And a rebuild restores the state it found rather than starting a stopped
    // channel behind the caller's back.
    if (!require(channel->setRunning(false), "the rebuilt channel refused to stop") ||
        !require(channel->reconfigure(config, &error), error.c_str()) ||
        !require(!channel->isRunning(),
                 "reconfigure() put a stopped channel back on the air")) {
        return false;
    }
    // Checked against the CHANNEL, not just the mirror: clock it and require
    // silence, so a reconfigure() that quietly restarted WDSP while isRunning()
    // still said "stopped" fails here rather than on the air.
    clock(32, 320);
    const Clocked afterReconfigure = clock(32, 352);
    if (!require(afterReconfigure.okBlocks == 32,
                 "a channel stopped across reconfigure() stopped accepting "
                 "blocks — the silence assertion below would be vacuous")) {
        std::cerr << "       " << afterReconfigure.okBlocks
                  << " of 32 blocks returned Ok\n";
        return false;
    }
    if (!require(afterReconfigure.energy == 0.0,
                 "a channel stopped across reconfigure() still produced audio")) {
        return false;
    }
    return true;
}

// A START TAKEN WITH A DOWN-RAMP STILL PENDING MUST NOT KILL THE CHANNEL.
//
// This is the case runStartStopTest cannot see, because it clocks 96 blocks
// between its stop and its start — well past the ramp. Raised in review of
// #5628 and fixed in the vendored source; this is the test that holds the fix.
//
// THE MECHANISM. WDSP's stop sets slew.downflag; the ramp only advances when
// the host clocks fexchange*. Upstream's SetChannelState case 1 armed the
// up-slew and re-armed exchange but never cleared downflag, and the two flags
// are read INDEPENDENTLY on opposite sides of fexchange2 — up gates the input,
// down gates the output. So a start taken before the host had clocked the ramp
// out left it pending on a channel WDSP now considered running. The next few
// blocks finished it, and downslew2's completion arm does
//     InterlockedBitTestAndReset (&ch[channel].exchange, 0);
// so finishing that ramp CLEARED EXCHANGE. Every later fexchange2 then failed
// its opening `if (exchange)` test and returned having written nothing and
// reported no error: the channel was permanently silent, isRunning() said true,
// and only a reconfigure() recovered it. AetherSDR patch 7 makes case 1 cancel
// the pending ramp (third_party/wdsp/AETHERSDR-PATCHES.md).
//
// HOW THIS GOES RED. Not on the mirror — isRunning() reports true either way,
// which is the whole complaint. It clocks the channel after the restart and
// requires real energy out of it, which is the only observable that separates
// "running" from "believes it is running". With patch 7 reverted every one of
// the three scenarios below fails on that assertion with energy exactly 0.
//
// All three ways in are covered, because they differ in WHERE the ramp is when
// the start arrives and a fix could plausibly catch one and miss another.
bool runRestartDuringRampTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.mode = WdspChannel::Mode::Usb;
    // Blocking, for the reason runStartStopTest gives: unpaced, the
    // non-blocking form outruns WDSP's worker and every block underruns, and
    // the "audio came back" assertion would pass or fail on silence this test
    // caused itself. Safe here too — a channel whose exchange bit is clear
    // returns from fexchange2 before it ever reaches the wait, which is exactly
    // the state the FAILING path leaves it in.
    config.blockForOutput = true;

    struct Scenario {
        const char* name;
        std::size_t blocksBetween;  // clocked between the stop and the start
        bool viaReconfigure;        // reach the stopped state through reconfigure()
    };
    // THE RAMP IS EXACTLY THREE BLOCKS LONG HERE, and the arithmetic matters
    // because the two failures this test pins live on opposite sides of it.
    // downslew2 spends one sample in BEGIN, ntdown + 1 = 481 in DOWNSLEW
    // (muteSlewDownSec 0.010 at 48 kHz), and out_size + 1 = 257 in ZERO, then
    // runs OFF to the end of whatever block it is in and clears downflag there:
    // 739 samples, so the third 256-sample block is the one that COMPLETES it.
    // Blocking mode makes that deterministic — every fexchange2 waits for
    // output, so every clocked block advances the ramp, with no underrun to
    // skip one.
    //
    // Spacings 0 and 1 are INSIDE the ramp: it is still pending at the start,
    // and AetherSDR patch 7's flush_slews() cancel is what makes them safe.
    // Spacings 3, 4 and 5 are AT and PAST its completion, which is a different
    // defect with a different fix: by then fexchange2 has already released
    // Sem_Flush, and the flushChannel thread — runnable, not necessarily
    // scheduled — will set exec_bypass whenever it gets a slot, possibly after
    // case 1 has cleared it. That is patch 8's window, and 3 is where it bites:
    // the probe behind that patch measured 20 of 20 blocking trials hung at
    // spacing 3 and none at 4 or 5, so 3 is the row with the mutation
    // sensitivity and 4 and 5 are the shoulders that say where it stops.
    //
    // Both spacings are what a T/R edge produces (§13 row 9a); which side of
    // the ramp it lands on is a question about the operator's timing, not
    // about this API, so both have to be safe.
    const Scenario scenarios[] = {
        {"stop then start with no clocking at all", 0, false},
        {"stop then start inside the down-slew window", 1, false},
        {"start after reconfigure() of a stopped channel", 0, true},
        {"stop, clock the ramp exactly out, then start with no gap", 3, false},
        {"stop, clock one block past the ramp, then start with no gap", 4, false},
        {"stop, clock two blocks past the ramp, then start with no gap", 5, false},
    };

    for (const Scenario& scenario : scenarios) {
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!require(channel != nullptr, error.c_str())) {
            return false;
        }

        std::vector<float> inputI(config.inputBlockSize);
        std::vector<float> inputQ(config.inputBlockSize);
        std::vector<float> outputLeft(channel->outputBlockSize());
        std::vector<float> outputRight(channel->outputBlockSize());
        std::size_t offset = 0;
        const auto clock = [&](std::size_t blocks) {
            double energy = 0.0;
            for (std::size_t block = 0; block < blocks; ++block, ++offset) {
                fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                                offset * config.inputBlockSize);
                if (channel->processIq(inputI, inputQ, outputLeft, outputRight) ==
                    WdspChannel::ProcessResult::Ok) {
                    energy += rms(outputLeft) + rms(outputRight);
                }
            }
            return energy;
        };
        // Every require() below is scenario-generic, so name the scenario on the
        // way out or a failure says nothing about which of the three it was.
        const auto fail = [&](const char* stage) {
            std::cerr << "       scenario: " << scenario.name << ", stage: "
                      << stage << '\n';
            return false;
        };

        if (!require(clock(64) > 0.01, "the running channel produced no audio")) {
            return fail("baseline");
        }

        if (scenario.viaReconfigure) {
            // The third way in, and the one no caller has to do anything odd to
            // reach: open() always starts the channel, so reconfigure() of a
            // STOPPED one has to stop it again afterwards — arming a down-ramp
            // from that moment with nothing left to clock it.
            if (!require(channel->setRunning(false),
                         "the channel refused to stop") ||
                !require(channel->reconfigure(config, &error), error.c_str()) ||
                !require(!channel->isRunning(),
                         "reconfigure() put a stopped channel back on the air")) {
                return fail("reconfigure");
            }
        } else {
            if (!require(channel->setRunning(false),
                         "the channel refused to stop")) {
                return fail("stop");
            }
            clock(scenario.blocksBetween);
        }

        const uint64_t allocationsBeforeStart =
            WdspChannel::allocationSequenceForTest();
        if (!require(channel->setRunning(true), "the channel refused to start") ||
            !require(channel->isRunning(),
                     "the channel did not report itself running") ||
            !require(WdspChannel::allocationSequenceForTest() ==
                         allocationsBeforeStart,
                     "the start rebuilt the channel instead of changing its "
                     "state — the recovery this test forbids")) {
            return fail("start");
        }

        // Discard the up-ramp (muteSlewUpSec 0.025 is under five blocks), then
        // ask the CHANNEL, not the mirror.
        //
        // ON ITS OWN THREAD, UNDER A DEADLINE, and that is not defensive
        // dressing. The past-the-ramp scenarios have two distinct failure
        // modes and only one of them is an assertion. Without patch 7 the
        // channel goes SILENT: exchange is clear, fexchange2 returns having
        // touched nothing, and the energy assertion below catches it. Without
        // patch 8 in blocking mode the channel HANGS: exec_bypass is set, so
        // wdspmain never reaches dexchange, Sem_OutReady is never released,
        // and fexchange2's `if (a->bfo) WaitForSingleObject (..., INFINITE)`
        // never returns. Clocked inline that is a ctest TIMEOUT — a red with
        // no message, at the suite's default cap, minutes later. Clocked here
        // it is a named failure in twenty seconds.
        //
        // _Exit rather than `return false`, because the clocking thread is
        // parked in the kernel on a semaphore nothing will ever release: it
        // cannot be joined, and unwinding past it would run ~WdspChannel on a
        // channel that thread is still inside. Exit codes are all ctest reads.
        std::atomic<bool> finished{false};
        double energy = 0.0;
        std::thread measurement([&] {
            clock(64);
            energy = clock(64);
            finished.store(true, std::memory_order_release);
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!finished.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished.load(std::memory_order_acquire)) {
            std::cerr << "FAIL: a restart taken in the flush window HUNG the "
                         "host — fexchange2 is parked on Sem_OutReady and the "
                         "worker is bypassed, so no block will ever come back\n"
                      << "       scenario: " << scenario.name << '\n';
            std::cerr.flush();
            std::_Exit(1);
        }
        measurement.join();
        if (!require(energy > 0.01,
                     "a channel restarted before its down-ramp had been clocked "
                     "out went permanently silent while isRunning() reported "
                     "true — WDSP finished the stale ramp and cleared exchange")) {
            std::cerr << "       scenario: " << scenario.name
                      << ", post-restart energy " << energy << '\n';
            return false;
        }
    }
    return true;
}

// close() must not hold the FFTW setup lock while WDSP's stop wait runs.
//
// close() asks WDSP to stop-and-flush in the BLOCKING form, and behind the
// control fence that wait always runs to WDSP's 100 ms timeout — nothing is
// left calling fexchange* to release Sem_Flush, so the flushChannel thread
// never wakes to clear the flag (see close()'s own comment). It used to sit out
// that timeout holding the process-global setup mutex, which exists only to
// serialise the FFTW planner, so N channels closing while running queued N
// timeouts end to end.
//
// PINNED WITHOUT A STOPWATCH. Hold the setup lock here, start a reconfigure()
// on another thread, and watch for the channel's run flag to go false. close()
// stores that flag between SetChannelState and CloseChannel, so it can only be
// observed from a thread holding the lock if the stop ran OUTSIDE it. Put the
// stop back under the lock and the flag stays true, the poll runs out, and this
// fails — with no timing margin to tune and nothing to go soft under load.
bool runCloseSetupLockTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str()) ||
        !require(channel->isRunning(),
                 "a freshly opened channel was not running")) {
        return false;
    }

    bool reconfigured = false;
    bool stoppedWhileLockHeld = false;
    {
        std::unique_lock<std::mutex> setupLock = WdspChannel::fftwSetupLock();
        std::thread closer([&] {
            // reconfigure() rather than the destructor, so the object is still
            // alive for the poll below. It holds beginControlOperation() across
            // its close(), which is the same fence the destructor raises.
            reconfigured = channel->reconfigure(config, nullptr);
        });
        // Generous, and only ever spent in full on the FAILING path: WDSP's
        // wait is 100 ms and has to complete exactly once.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!channel->isRunning()) {
                stoppedWhileLockHeld = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Before the join: reconfigure()'s CloseChannel and its re-open both
        // want this lock, so it cannot finish until we let go.
        setupLock.unlock();
        closer.join();
    }
    return require(stoppedWhileLockHeld,
                   "close() held the FFTW setup lock across WDSP's stop wait") &&
           require(reconfigured,
                   "reconfigure() failed while the FFTW setup lock was held");
}

// A channel stopped by its OWNER before teardown does not pay the 100 ms.
//
// This is the mechanism behind the production change in Hl2RxDsp/AnanRxDsp:
// SetChannelState no-ops when the state already matches, so close()'s blocking
// stop is skipped entirely on a channel that is already stopped. Measured
// against its own contrast — the same close, on a channel left running.
//
// MINIMUM of three runs, not a mean. The quantity being pinned is a fixed
// 100 ms constant inside WDSP; scheduling noise can only ever add to a sample,
// so the minimum is the load-robust estimator here.
bool runStoppedCloseTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;

    bool ok = true;
    const auto closeMs = [&](bool stopFirst) -> double {
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!require(channel != nullptr, error.c_str())) {
            ok = false;
            return 0.0;
        }
        if (stopFirst &&
            !require(channel->setRunning(false), "setRunning(false) was refused")) {
            ok = false;
            return 0.0;
        }
        if (!require(channel->isRunning() != stopFirst,
                     "the channel's run state did not follow setRunning()")) {
            ok = false;
            return 0.0;
        }
        const auto start = std::chrono::steady_clock::now();
        channel.reset();   // ~WdspChannel -> fence -> close()
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start).count();
    };

    double runningMs = std::numeric_limits<double>::max();
    double stoppedMs = std::numeric_limits<double>::max();
    for (int iteration = 0; iteration < 3 && ok; ++iteration) {
        runningMs = std::min(runningMs, closeMs(false));
        if (!ok) {
            return false;
        }
        stoppedMs = std::min(stoppedMs, closeMs(true));
    }
    if (!ok) {
        return false;
    }

    // Stated first, because the contrast is only meaningful if the timeout is
    // still there to be skipped. If WDSP ever learns to satisfy this wait on
    // its own, this is the line that says so rather than the one below quietly
    // passing for the wrong reason.
    if (!require(runningMs >= 80.0,
                 "closing a RUNNING channel no longer reaches WDSP's "
                 "stop-and-flush timeout")) {
        std::cerr << "       running close took " << runningMs << " ms\n";
        return false;
    }
    // ONE MEASUREMENT, NOT A DIFFERENCE OF TWO. This assertion used to be
    // `runningMs - stoppedMs >= 30`, and that shape compounds the noise of two
    // independent sets of runs: BOTH timings include CloseChannel — a
    // worker-thread join plus FFTW plan destruction under the process-global
    // g_setupMutex — which is large and variable, and the minimum is taken over
    // separate sets, so a stopped close that lands on an expensive plan teardown
    // while the running closes landed on cheap ones fails a test with nothing
    // wrong. Under the sanitizer lane, or on a box carrying concurrent builds
    // (this one routinely does), that is a flake for a quantity a stopwatch
    // cannot measure robustly. Raised in review of #5628 by ten9876.
    //
    // The absolute ceiling says the same thing with one sample instead of two:
    // it is the SAME 80 ms constant as the floor above, so the pair still proves
    // a 100 ms wait was skipped — runningMs is at least 80, stoppedMs is under
    // it — while noise on the stopped close now has ~80 ms of one-sided headroom
    // over a native close rather than having to stay inside another run's
    // budget. It still cannot pass if the wait is paid: paying it puts stoppedMs
    // at 100 or more.
    //
    // runCloseSetupLockTest pins the LOCK half of the same change stopwatch-free
    // and remains the stronger test; this one is what says the 100 ms is gone.
    if (!require(stoppedMs < 80.0,
                 "closing a STOPPED channel still paid WDSP's stop-and-flush "
                 "wait")) {
        std::cerr << "       running close " << runningMs << " ms, stopped close "
                  << stoppedMs << " ms\n";
        return false;
    }
    return true;
}

// CLOSING A CHANNEL THAT WAS STOPPED AND THEN CLOCKED MUST NOT RACE WDSP'S
// FLUSH THREAD.
//
// THE MECHANISM. A stop that is clocked to completion does three things at the
// completion of the down-ramp, inside fexchange0/fexchange2: it clears
// ch[].exchange, it releases the channel's Sem_Flush, and it thereby makes
// WDSP's per-channel flushChannel thread RUNNABLE. That thread takes csDSP and
// csEXCH and runs flush_iobuffs/flush_main, and flush_main walks the same
// rxa[channel] chain destroy_rxa frees.
//
// Nothing in CloseChannel waited for it. CloseChannel is exactly
// pre_main_destroy; destroy_main; post_main_destroy. AetherSDR patch 4's exit
// handshake in pre_main_destroy waits for the wdspmain WORKER. Upstream's
// flushChannel handshake existed too — but inside destroy_iobuffs, which
// post_main_destroy calls AFTER destroy_main. So destroy_main -> destroy_rxa
// freed the chain while flushChannel was inside flush_rxa on it.
//
// WHY THIS WAS NOT REACHABLE BEFORE THIS PR. Without setRunning() there was no
// way to stop a channel and then keep clocking it: the only stop was close()'s
// own, behind the control fence, with nothing left to call fexchange*. The ramp
// therefore never completed, Sem_Flush was never released, and flushChannel
// stayed parked for the whole of teardown. This PR's contract — "while stopped,
// processIq() still runs and returns silence" — is what makes the crashing
// shape a documented one, and docs/HERMES.md §13 row 9a (the T/R mute) is
// exactly it. Found by ten9876 in review of #5628; fixed as AetherSDR patch 9,
// which moves the handshake into pre_main_destroy.
//
// HOW THIS GOES RED: IT CRASHES OR IT HANGS. There is no assertion that can
// catch a use-after-free from inside the process that is committing it, so this
// case does not try to invent one — it drives the shape that was measured to
// fault and lets the fault be the signal. ctest reports the target's signal or
// its timeout; a reader who sees either should start here. That is why this
// case runs LAST of the WDSP lifecycle group in main(): everything before it
// has already reported.
//
// THE THREE VARIABLES THAT MATTER, all measured on this tree (macOS arm64,
// AppleClang, RelWithDebInfo, one trial per process, patch 9 reverted):
//
//   create -> clock -> destroy, never stopped          clean 15/15
//   create -> clock -> setRunning(false) -> destroy    clean 15/15
//   create -> clock -> setRunning(false) -> clock 32
//                                        -> destroy    CRASHED
//
// so the single distinguishing variable is CLOCKING AFTER THE STOP, which is
// what identifies the flush thread rather than the stop itself. A control that
// sleeps 50 ms before the destroy — giving that thread its slot — was clean
// 30/30 on the same binary. With patch 9 the crashing shape is clean 80/80
// across both output modes.
//
// NON-BLOCKING, unlike every other case in this file, and that is deliberate.
// The crash rate is scheduling-dependent and the non-blocking form is both the
// faster reproducer here and what production actually uses: Hl2RxDsp and
// AnanRxDsp leave Config::blockForOutput false. Measured per-cycle on this box
// with patch 9 reverted, 8 of 30 non-blocking against 1 of 30 blocking — both
// non-zero, so this is not a mode-specific defect, but the cheaper one is the
// right one to run every build. CYCLES is sized against that 8-in-30: at 24
// independent cycles an unfixed tree escapes with probability 0.73^24, under
// 0.1%. On a machine whose scheduling hides it more thoroughly this case can
// still pass against a broken tree, which is the honest limit of any test for a
// race — the probe behind the patch, not this, is the measurement.
bool runCloseAfterStoppedClockingTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.mode = WdspChannel::Mode::Usb;
    config.blockForOutput = false;

    constexpr int kCycles = 24;
    constexpr std::size_t kBlocks = 32;

    std::vector<float> inputI(config.inputBlockSize);
    std::vector<float> inputQ(config.inputBlockSize);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!require(channel != nullptr, error.c_str())) {
            return false;
        }
        std::vector<float> outputLeft(channel->outputBlockSize());
        std::vector<float> outputRight(channel->outputBlockSize());

        const auto clock = [&](std::size_t blocks, std::size_t offset) {
            for (std::size_t block = 0; block < blocks; ++block) {
                fillComplexTone(inputI, inputQ, config.inputSampleRate, 1000.0,
                                (offset + block) * config.inputBlockSize);
                (void)channel->processIq(inputI, inputQ, outputLeft, outputRight);
            }
        };

        // Prime the channel, so the stop below has a real ramp to run.
        clock(kBlocks, 0);
        if (!require(channel->setRunning(false),
                     "the channel refused to stop")) {
            return false;
        }
        // THE LINE THAT ARMS IT. The ramp is three blocks at this block size, so
        // 32 clocks it well past completion — which is what releases Sem_Flush
        // and wakes flushChannel. Anything at or past completion will do; more
        // blocks only widen the window in which the destroy below can land.
        clock(kBlocks, kBlocks);

        // AND THE LINE THAT USED TO CRASH. No gap, deliberately: a sleep here is
        // the control that passes, not the case under test.
        channel.reset();
    }
    return true;
}

// The minimum-phase path, which nothing else in this tree exercises:
// WdspChannel::Config::minimumPhase is false everywhere, so RXASetMP() always
// passes 0 and WDSP's mp == 1 branch is never entered by any other test.
//
// Two things are pinned here, and the second is why the first matters.
//
// 1. Turning minimum phase ON must still produce audio. The AetherSDR patch
//    that builds the minimum-phase workspace lazily
//    (third_party/wdsp/AETHERSDR-PATCHES.md) makes plan_fircore() build the
//    minimum-phase workspace only when the core's mp flag is set, and
//    calc_fircore() build it on first use. WdspChannel::open() calls RXASetNC()
//    BEFORE RXASetMP(), so with minimumPhase = true the six cores RXASetNC()
//    re-plans are planned at mp == 0 and only then flipped on: this test is the
//    one that walks the lazy-construction path. Before the patch the workspace
//    was always there; after it, getting the laziness wrong is a null
//    dereference inside mp_imp_exec() on the very first mask build.
//
// 2. Minimum phase must COST something. If the workspace were still built
//    unconditionally, a minimum-phase channel and a linear-phase one would hold
//    the same number of live WDSP allocations, and (1) could pass on a patch
//    that achieved nothing. The comparison is stated as a relation rather than
//    a count so it does not re-hardcode WDSP's internal fircore inventory.
bool runMinimumPhaseWorkspaceTest()
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
    config.agcMode = 0;
    config.agcFixedGainDb = 0.0;
    config.blockForOutput = true;
    // The tap count the HL2 runs (Hl2RxDsp::kRxFilterTaps), so the workspace
    // under test is the large one.
    config.filterTaps = 8192;

    // Live WDSP allocations held by one open channel, and the audio it makes.
    // Both are taken while the channel is alive; it is destroyed before return,
    // so runLeakChecked() still sees a clean balance.
    const auto openAndMeasure = [&](bool minimumPhase,
                                    uint64_t* liveAllocations,
                                    double* toneRms) -> bool {
        WdspChannel::Config channelConfig = config;
        channelConfig.minimumPhase = minimumPhase;
        const uint64_t before = WdspChannel::outstandingAllocationsForTest();
        std::string error;
        std::unique_ptr<WdspChannel> channel =
            WdspChannel::create(channelConfig, &error);
        if (!channel) {
            std::cerr << "FAIL: could not open a channel with minimumPhase="
                      << minimumPhase << ": " << error << '\n';
            return false;
        }
        *liveAllocations = WdspChannel::outstandingAllocationsForTest() - before;

        std::vector<float> inputI(config.inputBlockSize);
        std::vector<float> inputQ(config.inputBlockSize);
        std::vector<float> outputLeft(channel->outputBlockSize());
        std::vector<float> outputRight(channel->outputBlockSize());
        // Same geometry as runNotchAttenuationTest: RXA passes the opposite
        // sign to its passband bounds, so an in-passband tone is negative.
        constexpr double kToneHz = -1500.0;
        constexpr std::size_t kSettleBlocks = 40;
        constexpr std::size_t kTotalBlocks = 80;
        double energy = 0.0;
        for (std::size_t block = 0; block < kTotalBlocks; ++block) {
            fillComplexTone(inputI, inputQ, config.inputSampleRate, kToneHz,
                            block * config.inputBlockSize);
            if (channel->processIq(inputI, inputQ, outputLeft, outputRight) !=
                WdspChannel::ProcessResult::Ok) {
                std::cerr << "FAIL: processIq failed with minimumPhase="
                          << minimumPhase << '\n';
                return false;
            }
            if (block >= kSettleBlocks) {
                energy += rms(outputLeft);
            }
        }
        *toneRms = energy;
        return true;
    };

    uint64_t linearAllocations = 0;
    uint64_t minimumAllocations = 0;
    double linearTone = 0.0;
    double minimumTone = 0.0;
    if (!openAndMeasure(false, &linearAllocations, &linearTone) ||
        !openAndMeasure(true, &minimumAllocations, &minimumTone)) {
        return false;
    }

    if (!require(linearTone > 1.0e-4,
                 "the linear-phase channel produced no audio") ||
        !require(minimumTone > 1.0e-4,
                 "the minimum-phase channel produced no audio - the lazily "
                 "built minimum-phase workspace is wrong or absent")) {
        return false;
    }
    if (minimumAllocations <= linearAllocations) {
        std::cerr << "FAIL: minimum phase cost no extra WDSP allocations "
                     "(linear=" << linearAllocations
                  << " minimum=" << minimumAllocations
                  << ") - the minimum-phase workspace is still being built "
                     "for cores that do not use it\n";
        return false;
    }
    return true;
}

// ── Runtime filter length and phase mode ──────────────────────────────────
//
// The defect this is written against is a setter that is called, returns true,
// updates its cached Config and changes NOTHING in WDSP. Reading filterTaps
// back would pass against exactly that, so nothing here reads it back: every
// assertion below is a MEASUREMENT of the channel's group delay, taken from
// audio the channel actually produced.
//
// The measurement is tone onset. A linear-phase FIR of N taps delays by
// (N-1)/2 samples, so 2048 -> 1023.5 and 8192 -> 4095.5, and the DIFFERENCE is
// 3072 samples exactly. Differences are what this asserts: the absolute onset
// also carries WDSP's iobuffs pipeline (dspBlockSize + max(in, dsp) samples)
// and the startup mute ramp, and both are identical in every leg, so both
// cancel. That is deliberate -- pinning the absolute would pin two constants
// this change does not own.
//
// THE PRIMING MATTERS AND IS NOT OPTIONAL. iobuffs.c's upslew2 leaves its
// BEGIN state only `if ((I != 0.0) || (Q != 0.0))`, so clocking SILENCE to
// spend the startup mute ramp does not spend it -- it pins it, and the ramp
// then fires on the first sample of the probe tone, inside the measurement
// window. Every leg is primed with low-level NOISE instead, after any setter
// call (RXASetNC restarts the channel, which re-arms the ramp) and before the
// tone. Priming with noise and probing with a tone also keeps the onset
// threshold unambiguous: the primer's output is orders of magnitude below half
// the tone's steady level.
double onsetSamplesForLeg(int openTaps, int setTaps, bool setMinimumPhase,
                          bool* ok)
{
    *ok = false;
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.inputSampleRate = 48000;
    config.dspSampleRate = 48000;
    config.outputSampleRate = 48000;
    config.mode = WdspChannel::Mode::Usb;
    config.filterLowHz = 150.0;
    config.filterHighHz = 3000.0;
    // AGC off at unity. wcpagc.c's xwcpagc early-returns at mode 0 and never
    // enters its lookahead buffer, which otherwise contributes its own delay
    // (and would contribute it identically in every leg, but there is no
    // reason to measure through a stage this change does not touch).
    config.agcMode = 0;
    config.agcFixedGainDb = 0.0;
    config.blockForOutput = true;
    config.filterTaps = openTaps;

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!channel) {
        std::cout << "  channel creation failed: " << error << "\n";
        return 0.0;
    }
    if (setTaps != 0 && !channel->setFilterTaps(setTaps)) {
        std::cout << "  setFilterTaps(" << setTaps << ") returned false\n";
        return 0.0;
    }
    if (setMinimumPhase && !channel->setMinimumPhase(true)) {
        std::cout << "  setMinimumPhase(true) returned false\n";
        return 0.0;
    }

    std::vector<float> inputI(config.inputBlockSize);
    std::vector<float> inputQ(config.inputBlockSize);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());

    // Prime. 96 blocks = 24576 samples: longer than the 8192-tap filter's own
    // fill and far longer than the 0.035 s mute ramp.
    constexpr std::size_t kPrimeBlocks = 96;
    uint32_t lcg = 0x13579bdfu;
    for (std::size_t block = 0; block < kPrimeBlocks; ++block) {
        for (std::size_t sample = 0; sample < config.inputBlockSize; ++sample) {
            lcg = lcg * 1664525u + 1013904223u;
            inputI[sample] = static_cast<float>(
                1.0e-5 * (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0));
            lcg = lcg * 1664525u + 1013904223u;
            inputQ[sample] = static_cast<float>(
                1.0e-5 * (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0));
        }
        if (channel->processIq(inputI, inputQ, outputLeft, outputRight) !=
            WdspChannel::ProcessResult::Ok) {
            std::cout << "  primer block " << block << " did not process\n";
            return 0.0;
        }
    }

    // Probe. The tone is negative baseband because RXA as configured passes the
    // opposite sign to its passband bounds -- the same geometry
    // runNotchAttenuationTest measures in, and the one the HL2 runs.
    constexpr std::size_t kProbeBlocks = 128;
    std::vector<float> capture;
    capture.reserve(kProbeBlocks * outputLeft.size());
    for (std::size_t block = 0; block < kProbeBlocks; ++block) {
        fillComplexTone(inputI, inputQ, config.inputSampleRate, -1500.0,
                        block * config.inputBlockSize);
        if (channel->processIq(inputI, inputQ, outputLeft, outputRight) !=
            WdspChannel::ProcessResult::Ok) {
            std::cout << "  probe block " << block << " did not process\n";
            return 0.0;
        }
        capture.insert(capture.end(), outputLeft.begin(), outputLeft.end());
    }

    // Envelope: peak magnitude over one period of the 1500 Hz audio tone.
    // Steady level from the final quarter, which is long past any onset this
    // test can produce. NOT the peak over the whole capture -- a linear-phase
    // bandpass step response overshoots, and that peak is Gibbs ringing.
    constexpr std::size_t kPeriod = 32;   // 48000 / 1500
    if (capture.size() <= kPeriod) {
        return 0.0;
    }
    std::vector<double> envelope(capture.size() - kPeriod, 0.0);
    for (std::size_t sample = 0; sample < envelope.size(); ++sample) {
        double peak = 0.0;
        for (std::size_t k = 0; k < kPeriod; ++k) {
            peak = std::max(peak, std::abs(static_cast<double>(capture[sample + k])));
        }
        envelope[sample] = peak;
    }
    double steady = 0.0;
    const std::size_t steadyFrom = envelope.size() - envelope.size() / 4;
    for (std::size_t sample = steadyFrom; sample < envelope.size(); ++sample) {
        steady += envelope[sample];
    }
    steady /= static_cast<double>(envelope.size() - steadyFrom);
    if (steady <= 1.0e-4) {
        std::cout << "  the probe tone produced no steady output (" << steady << ")\n";
        return 0.0;
    }
    for (std::size_t sample = 0; sample < envelope.size(); ++sample) {
        if (envelope[sample] >= 0.5 * steady) {
            *ok = true;
            return static_cast<double>(sample);
        }
    }
    std::cout << "  the probe tone never reached half its steady level\n";
    return 0.0;
}

bool runFilterTapsGroupDelayTest()
{
    // Expected group-delay difference between 8192 and 2048 taps:
    // (8192-1)/2 - (2048-1)/2 = 3072 samples, 64.0 ms at 48 kHz.
    constexpr double kExpectedDelta = 3072.0;
    // One input block. The onset estimator quantises to its 32-sample envelope
    // window and the filter's ring-up is gradual, so this is loose against the
    // estimator and tight against the quantity: a setter that did nothing would
    // read a delta of 0, and a setter that moved the taps the wrong way would
    // read -3072.
    constexpr double kTolerance = 256.0;

    bool ok = false;
    const double open2048 = onsetSamplesForLeg(2048, 0, false, &ok);
    if (!require(ok, "the 2048-tap baseline leg did not measure")) {
        return false;
    }
    const double raised = onsetSamplesForLeg(2048, 8192, false, &ok);
    if (!require(ok, "the setFilterTaps(8192) leg did not measure")) {
        return false;
    }
    const double open8192 = onsetSamplesForLeg(8192, 0, false, &ok);
    if (!require(ok, "the 8192-tap baseline leg did not measure")) {
        return false;
    }
    const double lowered = onsetSamplesForLeg(8192, 2048, false, &ok);
    if (!require(ok, "the setFilterTaps(2048) leg did not measure")) {
        return false;
    }
    const double minimumPhase = onsetSamplesForLeg(8192, 0, true, &ok);
    if (!require(ok, "the setMinimumPhase(true) leg did not measure")) {
        return false;
    }

    std::cout << "  onset, samples after the probe tone starts:\n"
              << "    opened 2048                  " << open2048 << "\n"
              << "    opened 2048 -> setFilterTaps(8192) " << raised
              << "   (delta " << (raised - open2048) << ")\n"
              << "    opened 8192                  " << open8192 << "\n"
              << "    opened 8192 -> setFilterTaps(2048) " << lowered
              << "   (delta " << (lowered - open8192) << ")\n"
              << "    opened 8192 -> setMinimumPhase(true) " << minimumPhase
              << "   (delta " << (minimumPhase - open8192) << ")\n"
              << "  expected tap delta " << kExpectedDelta << " samples ("
              << (kExpectedDelta * 1000.0 / 48000.0) << " ms at 48 kHz)\n";

    bool result = true;
    // Raising. The measurement that a setter doing nothing fails.
    result = require(std::abs((raised - open2048) - kExpectedDelta) <= kTolerance,
                     "setFilterTaps(8192) did not add the group delay of an "
                     "8192-tap filter") && result;
    // Lowering. The same defect can hide in one direction only -- a setter that
    // applies a floor, or that only ever grows the filter, passes the leg above
    // and fails this one.
    result = require(std::abs((lowered - open8192) + kExpectedDelta) <= kTolerance,
                     "setFilterTaps(2048) did not remove the group delay of an "
                     "8192-tap filter") && result;
    // The setter arrives where open() arrives. Without these two, both deltas
    // above could be right while the channel sat at some third tap count.
    result = require(std::abs(raised - open8192) <= kTolerance,
                     "setFilterTaps(8192) did not reach the same group delay as "
                     "opening at 8192") && result;
    result = require(std::abs(lowered - open2048) <= kTolerance,
                     "setFilterTaps(2048) did not reach the same group delay as "
                     "opening at 2048") && result;
    // Minimum phase keeps the 8192 taps and front-loads their energy, so the
    // onset must collapse well below even the 2048-tap linear-phase figure.
    // Asserted as a relation, not a number: the exact figure is
    // frequency-dependent for a minimum-phase filter.
    result = require(minimumPhase < open2048,
                     "setMinimumPhase(true) did not reduce the group delay below "
                     "the 2048-tap linear-phase figure") && result;

    // The notch width floor is a function of the tap count -- 1600 / (nc/256) Hz
    // at 48 kHz -- and it is what the long filter is bought for. A caller that
    // raises the taps to honour a narrow notch has to see the floor move, so
    // pin that it follows the setter rather than the Config the channel opened
    // with.
    WdspChannel::Config config;
    config.filterTaps = 2048;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config);
    if (!require(channel != nullptr, "the notch-width channel did not open")) {
        return false;
    }
    const int channelIdBefore = channel->channelIdForTest();
    const double floorAt2048 = channel->minimumNotchWidthHz();
    result = require(std::abs(floorAt2048 - 200.0) < 0.5,
                     "the 2048-tap notch width floor is not 200 Hz") && result;
    result = require(channel->setFilterTaps(8192),
                     "setFilterTaps(8192) was refused on a live channel") && result;
    // The WDSP channel id is the process-global table slot. If it moved, the
    // channel was closed and reopened -- which is what reconfigure() does and
    // what takes the notch database with it. Same id is the cheap proof that
    // RXASetNC changed the filter IN PLACE.
    result = require(channel->channelIdForTest() == channelIdBefore,
                     "setFilterTaps closed and reopened the channel") && result;
    const double floorAt8192 = channel->minimumNotchWidthHz();
    result = require(std::abs(floorAt8192 - 50.0) < 0.5,
                     "the notch width floor did not follow setFilterTaps(8192) "
                     "down to 50 Hz") && result;
    result = require(channel->setFilterTaps(2048),
                     "setFilterTaps(2048) was refused on a live channel") && result;
    result = require(std::abs(channel->minimumNotchWidthHz() - 200.0) < 0.5,
                     "the notch width floor did not follow setFilterTaps(2048) "
                     "back up to 200 Hz") && result;
    std::cout << "  minimumNotchWidthHz: 2048 -> " << floorAt2048
              << " Hz, after setFilterTaps(8192) -> " << floorAt8192 << " Hz\n";

    // Refusals. nc >= size is WDSP's own requirement (fircore divides nc by
    // size); a shorter filter than one block gives nfor == 0 and a channel that
    // is silent rather than broken, which is the worst way to fail.
    result = require(!channel->setFilterTaps(0),
                     "setFilterTaps accepted zero taps") && result;
    result = require(!channel->setFilterTaps(-8192),
                     "setFilterTaps accepted a negative tap count") && result;
    result = require(!channel->setFilterTaps(
                         static_cast<int>(config.dspBlockSize) / 2),
                     "setFilterTaps accepted a filter shorter than one DSP block")
             && result;

    // AND THE HALF nc >= size DOES NOT COVER. fircore walks its overlap-save
    // ring with idxmask = nfor - 1 used as a POWER-OF-TWO MASK (firmin.c,
    // xfircore), and firmin.h states the contract on the field itself: "number
    // of filter coefficients, power of two, >= size". The first three below all
    // satisfy nc >= size, all used to return true, and all corrupt the filter
    // silently.
    //
    // dspBlockSize is 1024 here, so: 3072 gives nfor 3 and mask 2, at which
    // buffidx is pinned at 0 and one partition is never written or read; 6144
    // gives nfor 6 and mask 5, at which half the ring is skipped; 1536 is not a
    // multiple of the block at all, so nfor truncates to 1 and a third of the
    // impulse is discarded.
    //
    // MEASURED before the guard existed, dspBlockSize 1024, a 0.1-amplitude
    // tone in a 150-3000 Hz passband, steady-state peak in band (1500 Hz) and
    // out of band (6000 Hz):
    //
    //   1024 taps  0.39807 / 0.00000  = 139 dB rejection   sound
    //   1536 taps  0.39723 / 0.00032  =  62 dB             impulse truncated
    //   2048 taps  0.39807 / 0.00000  = 149 dB             sound
    //   3072 taps  0.00008 / 0.00044  = -15 dB             ring broken
    //   6144 taps  0.19966 / 0.03989  =  14 dB             ring broken
    //   8192 taps  0.39807 / 0.00000  = 161 dB             sound
    //
    // At 3072 the wanted signal comes out 74 dB down and the out-of-band tone
    // comes out LOUDER than it. The truncating counts keep their passband and
    // lose their stopband -- audio that sounds right and no longer filters,
    // which is the worse of the two failures because nothing sounds wrong.
    result = require(!channel->setFilterTaps(3072),
                     "setFilterTaps accepted 3072 taps, whose nfor of 3 is not a "
                     "power of two") && result;
    result = require(!channel->setFilterTaps(6144),
                     "setFilterTaps accepted 6144 taps, whose nfor of 6 is not a "
                     "power of two") && result;
    result = require(!channel->setFilterTaps(1536),
                     "setFilterTaps accepted 1536 taps, which is not a multiple "
                     "of the DSP block size") && result;
    result = require(!channel->setFilterTaps(128),
                     "setFilterTaps accepted 128 taps, below WDSP's own "
                     "min_notch_width divisor of 256") && result;
    // Refused and INERT: a rejected count must not have moved the channel.
    result = require(std::abs(channel->minimumNotchWidthHz() - 200.0) < 0.5,
                     "a refused setFilterTaps still moved the notch width floor")
             && result;

    // THE SAME DOOR THROUGH open(). validateConfig() did not look at filterTaps
    // at all, so create() and reconfigure() reached every corruption above
    // while the setter refused it. One predicate now guards both.
    for (const int bad : {3072, 6144, 1536, 128, 0, -8192}) {
        WdspChannel::Config badConfig;
        badConfig.filterTaps = bad;
        std::string badError;
        result = require(WdspChannel::create(badConfig, &badError) == nullptr,
                         "create() accepted a filter length fircore cannot "
                         "partition") && result;
    }
    // ... and the sound ones still open, across four tap/block pairings. 2048
    // at dspBlockSize 2048 and 256 at 256 are both nfor == 1, the tightest case
    // in the tree and the one runUnderrunTest already relies on.
    for (const auto [taps, block] : {std::pair<int, std::size_t>{2048, 1024},
                                     {8192, 256},
                                     {2048, 2048},
                                     {256, 256}}) {
        WdspChannel::Config goodConfig;
        goodConfig.filterTaps = taps;
        goodConfig.dspBlockSize = block;
        goodConfig.inputBlockSize = block;
        std::string goodError;
        result = require(WdspChannel::create(goodConfig, &goodError) != nullptr,
                         "create() refused a filter length fircore can "
                         "partition") && result;
    }

    // Transmit has none of the six cores RXASetNC and RXASetMP address.
    WdspChannel::Config txConfig;
    txConfig.direction = WdspChannel::Direction::Transmit;
    std::unique_ptr<WdspChannel> tx = WdspChannel::create(txConfig);
    if (require(tx != nullptr, "the transmit channel did not open")) {
        result = require(!tx->setFilterTaps(8192),
                         "setFilterTaps was accepted on a transmit channel") && result;
        result = require(!tx->setMinimumPhase(true),
                         "setMinimumPhase was accepted on a transmit channel") && result;
    } else {
        result = false;
    }

    return result;
}

// The property that makes a runtime tap change worth having at all, and the one
// reconfigure() cannot provide: RXASetNC reaches nbp0 through setNc_nbp ->
// calc_nbp_impulse, which rebuilds the mask FROM the notch database rather than
// replacing the database, so notches placed before the call survive it.
// reconfigure() closes and reopens the channel and takes the database with it.
bool runNotchSurvivesTapChangeTest()
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
    config.agcMode = 0;
    config.agcFixedGainDb = 0.0;
    config.blockForOutput = true;
    config.filterTaps = 2048;

    const double tuneHz = 7'000'000.0;
    const double toneBasebandHz = -1500.0;
    const double toneRfHz = tuneHz + 1500.0;

    // Same geometry as runNotchAttenuationTest. `notchRfHz == 0` measures the
    // unnotched control.
    const auto measure = [&](double notchRfHz, bool raiseTaps) -> double {
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config);
        if (!channel || !channel->setNotchTuneFrequency(tuneHz)) {
            return -1.0;
        }
        if (notchRfHz != 0.0) {
            // 400 Hz: above the 200 Hz floor at 2048 taps, so the notch is
            // placed at the width asked for and stays at it when the taps rise.
            if (!channel->addNotch(0, notchRfHz, 400.0, true) ||
                !channel->setNotchesEnabled(true)) {
                return -1.0;
            }
        }
        // AFTER the notch is placed. This is the ordering under test.
        if (raiseTaps && !channel->setFilterTaps(8192)) {
            return -1.0;
        }
        if (raiseTaps && channel->notchCount() != (notchRfHz != 0.0 ? 1 : 0)) {
            return -1.0;
        }
        std::vector<float> inputI(config.inputBlockSize);
        std::vector<float> inputQ(config.inputBlockSize);
        std::vector<float> outputLeft(channel->outputBlockSize());
        std::vector<float> outputRight(channel->outputBlockSize());
        double energy = 0.0;
        constexpr std::size_t kSettleBlocks = 60;
        constexpr std::size_t kTotalBlocks = 120;
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

    const double unnotched = measure(0.0, true);
    const double notched = measure(toneRfHz, true);
    const double mirror = measure(tuneHz - 1500.0, true);

    if (!require(unnotched > 1.0e-4 && notched >= 0.0 && mirror >= 0.0,
                 "the notch-survives-tap-change measurement failed to run")) {
        return false;
    }
    std::cout << "  after setFilterTaps(8192) with a notch already placed:"
              << " unnotched " << unnotched << ", notched " << notched
              << ", mirror " << mirror << "\n";
    return require(notched < unnotched * 0.25,
                   "a notch placed before setFilterTaps no longer attenuates "
                   "after it -- the tap change destroyed the notch database") &&
           require(mirror > unnotched * 0.75,
                   "raising the taps inverted the notch frequency axis");
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
// ── TXA driven the way the HL2 backend drives a transmit chain ────────────
//
// runVector(Direction::Transmit) above proves a TXA channel produces IQ, but it
// proves it in a configuration the live path cannot use: equal rates, equal
// block sizes, and blockForOutput = true. With bfo set, fexchange2()'s
// `*error += -2` branch is unreachable (iobuffs.c: `if (a->bfo)
// WaitForSingleObject(...INFINITE); if (a->bfo || doit)`), so that vector is
// structurally incapable of reporting an underrun, and every buffer
// relationship pre_main_build() computes differs from the live one.
//
// This case runs a transmit channel at the rates and block sizes
// Hl2TxDsp::Config and MetisClient actually use -- 24 kHz audio in, 48 kHz DSP,
// 48 kHz EP2 out -- with blockForOutput = false, and measures what comes out.
//
// Three things it pins that nothing else in the tree does:
//
//  * THE CALLER MUST BE PACED. A non-blocking channel has exactly one DSP
//    buffer of slack (create_iobuffs: r2_havesamps = (DSP_MULT - 1) * r2_size,
//    DSP_MULT = 2) and fexchange2 CLAMPS r2_havesamps at zero on a miss while
//    still advancing r2_outidx, so credit is destroyed rather than banked and a
//    caller that outruns the worker never recovers. A tight loop underruns
//    almost every block; at the live block period it underruns none after the
//    first.
//
//  * THE AUDIO MUST BE IN I. xpanel runs with inselect = 2, which evaluates
//    I = in[2i] * (inselect >> 1) and Q = in[2i+1] * (inselect & 1), so Q is
//    multiplied by zero. A caller that fills inputQ and leaves inputI empty
//    gets exact zeros out forever, with no error. fillAudioTone() writes the
//    same value into both planes, so runVector cannot distinguish the two.
//
//  * POSITIVE PASSBAND EDGES ALREADY GIVE THE HPSDR WIRE'S HANDEDNESS.
//    fir_bandpass builds the complex impulse as coef * (cos, -sin), i.e.
//    exp(-j*w_osc*pos), so a positive signed band selects the NEGATIVE
//    baseband half. TXA therefore emits, with no conjugation, the same
//    handedness Hl2TxDsp reaches by conjugating -- which is what
//    hl2_txdsp_test's "USB puts energy on the LOWER wire bin" asserts.

struct TransmitRun
{
    bool created = false;
    int okBlocks = 0;
    int underrunBlocks = 0;
    int otherBlocks = 0;
    int firstOkBlock = -1;
    int lastUnderrunBlock = -1;
    int firstNonZeroBlock = -1;
    double peakMagnitude = 0.0;
    // Wire-facing IQ from Ok blocks at or after `discardBlocks`, with each
    // sample's ABSOLUTE index in the output stream. The index matters: a
    // dropped block is a phase discontinuity, and correlating a concatenation
    // of non-adjacent blocks against a fixed tone measures nothing.
    std::vector<std::complex<float>> iq;
    std::vector<std::size_t> index;
    // Per-Ok-block correlation phase at the tone frequency, in degrees, taken
    // against each block's ABSOLUTE position in the output stream. A channel
    // whose r2 read pointer is aligned with its write pointer returns the same
    // phase for every block (the chain's fixed group delay). fexchange2
    // advances r2_outidx on a MISS as well as on a hit, so a run of underruns
    // de-phases the two pointers and a later successful read can return a
    // buffer the worker has not refreshed -- which shows up here, and only
    // here, as a block whose phase differs from its neighbours'.
    std::vector<double> blockPhaseDeg;
    std::vector<double> blockPhasePeak;   // that block's peak |sample|, for gating
};

// Live geometry. Hl2TxDsp::Config's 24 kHz audio in and 48 kHz EP2 out, with
// dspBlockSize in DSP-rate samples -- twice the input block, so the channel
// consumes exactly one input block per DSP pass. That is the mirror of the
// arithmetic Hl2RxDsp::configure already does for receive, and nothing in the
// tree does it for transmit today.
WdspChannel::Config liveTransmitConfig(WdspChannel::Mode mode,
                                       double lowHz, double highHz)
{
    WdspChannel::Config config;
    config.direction = WdspChannel::Direction::Transmit;
    config.inputSampleRate = 24000;
    config.dspSampleRate = 48000;
    config.outputSampleRate = 48000;
    config.inputBlockSize = 512;
    config.dspBlockSize = 1024;
    config.mode = mode;
    config.filterLowHz = lowHz;
    config.filterHighHz = highHz;
    config.blockForOutput = false;   // the LIVE setting, deliberately
    return config;
}

// `plane`: 0 = tone in I only (what a backend feeding mono audio would do),
// 1 = tone in Q only, 2 = both (what fillAudioTone does).
// `paceUs`: wall-clock delay between calls. 0 is a tight loop; the live value
// is inputBlockSize / inputSampleRate = 21333 us.
TransmitRun runTransmitChannel(int plane, double toneHz, WdspChannel::Mode mode,
                               double lowHz, double highHz,
                               std::size_t blocks, std::size_t discardBlocks,
                               int paceUs, double amplitude = 0.1)
{
    TransmitRun run;
    const WdspChannel::Config config = liveTransmitConfig(mode, lowHz, highHz);

    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str())) {
        return run;
    }
    run.created = true;

    std::vector<float> inputI(config.inputBlockSize, 0.0f);
    std::vector<float> inputQ(config.inputBlockSize, 0.0f);
    std::vector<float> outputLeft(channel->outputBlockSize());
    std::vector<float> outputRight(channel->outputBlockSize());
    const std::size_t outBlock = channel->outputBlockSize();

    for (std::size_t block = 0; block < blocks; ++block) {
        for (std::size_t sample = 0; sample < inputI.size(); ++sample) {
            const double phase = 2.0 * std::numbers::pi * toneHz *
                                 static_cast<double>(block * inputI.size() + sample) /
                                 static_cast<double>(config.inputSampleRate);
            const float value = static_cast<float>(amplitude * std::cos(phase));
            inputI[sample] = (plane == 1) ? 0.0f : value;
            inputQ[sample] = (plane == 0) ? 0.0f : value;
        }
        const WdspChannel::ProcessResult result =
            channel->processIq(inputI, inputQ, outputLeft, outputRight);
        switch (result) {
        case WdspChannel::ProcessResult::Ok:
            ++run.okBlocks;
            if (run.firstOkBlock < 0) {
                run.firstOkBlock = static_cast<int>(block);
            }
            break;
        case WdspChannel::ProcessResult::Underrun:
            ++run.underrunBlocks;
            run.lastUnderrunBlock = static_cast<int>(block);
            break;
        default:
            ++run.otherBlocks;
            break;
        }
        double blockPeak = 0.0;
        for (std::size_t k = 0; k < outBlock; ++k) {
            blockPeak = std::max(blockPeak,
                                 std::abs(static_cast<double>(outputLeft[k])));
            blockPeak = std::max(blockPeak,
                                 std::abs(static_cast<double>(outputRight[k])));
        }
        if (blockPeak > 0.0 && run.firstNonZeroBlock < 0) {
            run.firstNonZeroBlock = static_cast<int>(block);
        }
        run.peakMagnitude = std::max(run.peakMagnitude, blockPeak);
        if (result == WdspChannel::ProcessResult::Ok && blockPeak > 0.0) {
            std::complex<double> acc {0.0, 0.0};
            const double w = 2.0 * std::numbers::pi * toneHz / 48000.0;
            for (std::size_t k = 0; k < outBlock; ++k) {
                const double ph = w * static_cast<double>(block * outBlock + k);
                acc += std::complex<double>(outputLeft[k], outputRight[k]) *
                       std::complex<double>(std::cos(ph), std::sin(ph));
            }
            run.blockPhaseDeg.push_back(std::arg(acc) * 180.0 /
                                        std::numbers::pi);
            run.blockPhasePeak.push_back(blockPeak);
        }
        if (result != WdspChannel::ProcessResult::Ok) {
            // An underrun slips the output stream by one whole DSP buffer for
            // good (see the census in runTransmitLiveGeometryTest), so anything
            // collected before it is in a different phase frame from anything
            // collected after. Start again rather than correlate across the
            // seam: the alternative is a suppression figure that silently
            // averages two time origins.
            run.iq.clear();
            run.index.clear();
        } else if (block >= discardBlocks) {
            for (std::size_t k = 0; k < outBlock; ++k) {
                run.iq.emplace_back(outputLeft[k], outputRight[k]);
                run.index.push_back(block * outBlock + k);
            }
        }
        if (paceUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(paceUs));
        }
    }
    return run;
}

// The same run, retried if the caller was STARVED.
//
// runTransmitChannel restarts collection after any non-Ok block, because an
// underrun slips the output stream by a whole DSP buffer permanently and
// correlating across that seam averages two time origins. So a starved run does
// not announce itself as starvation -- it comes back with a SHORT capture, and
// every figure derived from it is worse than it should be because this
// instrument's floor rises as 1/sqrt(N).
//
// THIS IS NOT HYPOTHETICAL AND IT IS NOT ABOUT THE CHAIN. Measured on this
// machine under load averages between 180 and 343, with another agent's -j8
// build running: the spectral leg below underran 2 of 96 blocks at a 5 ms pace
// while the pacing census in the SAME case reported 0 of 64 at 1 ms, 5 ms and
// at the live 21.33 ms period. Retried, it comes back clean. A transient on a
// shared machine must not read as a transmit regression.
//
// THE PREDICATE IS "NO UNDERRUN", NOT "LONG ENOUGH", and the difference was
// found the hard way. A capture length guard was tried first and let the defect
// through: an underrun at block 31 of 104 leaves 72 contiguous blocks, 90% of
// the full length and a whole number of cycles of every tone -- and the LSB
// 2000 Hz point still read 81.91 dB where a clean run reads 289.61. The reason
// is that collection restarts IMMEDIATELY after the fault, with no second
// discard, so the retained span opens inside the post-underrun transient. A
// transient is amplitude modulation and amplitude modulation lands in the image
// bin: the figure is the envelope, not the filter. It is the same trap
// hl2_txdsp_test's kSettleSamples exists for, arriving through a different
// door. Any underrun invalidates the point however much of it survives.
//
// Retrying belongs in the harness and would be wrong inside Hl2TxDsp: pacing is
// the CALLER's responsibility, and a sleep-and-retry on the audio I/O thread
// would hide a timing fault behind a spin. The runtime path counts and logs the
// fault instead.
TransmitRun runTransmitChannelSettled(int plane, double toneHz,
                                      WdspChannel::Mode mode, double lowHz,
                                      double highHz, std::size_t blocks,
                                      std::size_t discardBlocks, int paceUs,
                                      double amplitude = 0.1)
{
    TransmitRun best;
    for (int attempt = 0; attempt < 4; ++attempt) {
        // BACK THE PACE OFF on each retry rather than rolling the dice again.
        // Starvation has a cause -- the caller is feeding faster than the
        // channel drains on a machine that is busy elsewhere -- and slowing the
        // caller addresses it, where a retry at the same pace only hopes the
        // machine is quieter. Capped below the LIVE 21.33 ms block period, so
        // even the last attempt is a pace the radio itself would beat.
        const int pace = paceUs * (attempt + 1);
        TransmitRun run = runTransmitChannel(plane, toneHz, mode, lowHz, highHz,
                                             blocks, discardBlocks, pace,
                                             amplitude);
        if (!run.created || run.underrunBlocks == 0) {
            return run;
        }
        const int had = run.underrunBlocks;
        if (run.iq.size() > best.iq.size()) {
            best = std::move(run);
        }
        std::cout << "  (starved at " << toneHz << " Hz: " << had
                  << " underrun(s) at " << pace << " us -- retrying slower)\n";
    }
    return best;
}

// Goertzel-style complex-bin correlation. The same instrument hl2_txdsp_test
// runs on the phasing modulator, extended only to take each sample's absolute
// index so a dropped block does not silently become a phase step.
double binPower(const std::vector<std::complex<float>>& iq,
                const std::vector<std::size_t>& index, double hz, double fs,
                std::size_t count = 0)
{
    if (count == 0 || count > iq.size()) {
        count = iq.size();
    }
    if (count == 0) {
        return 0.0;
    }
    std::complex<double> acc {0.0, 0.0};
    const double w = -2.0 * std::numbers::pi * hz / fs;
    for (std::size_t n = 0; n < count; ++n) {
        const double ph = w * static_cast<double>(index[n]);
        acc += std::complex<double>(iq[n].real(), iq[n].imag()) *
               std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(count);
}

double suppressionDb(double wanted, double unwanted)
{
    // Floor well below float32 quantization so a bin that measures at the
    // arithmetic floor prints its real value rather than a clamp artefact.
    return 20.0 * std::log10(std::max(1.0e-20, unwanted) /
                             std::max(1.0e-20, wanted));
}

// Longest prefix of a capture that is a WHOLE number of cycles of `hz`.
//
// THIS IS NOT TIDINESS, and without it the sweep below measures its own
// analysis window rather than the modulator.
//
// binPower correlates against a RECTANGULAR window. A strong component at -f
// leaks into the bin at +f through the Dirichlet kernel, and for a capture of N
// samples that leakage is about wanted * fs / (pi * 2f * N). At 1.5 s of 48 kHz
// output and a 150 Hz tone that is roughly 7e-4 of the wanted bin -- about
// 63 dB down. Anything genuinely quieter than that is invisible: the number
// reported is the window's.
//
// THIS IS NOT HYPOTHETICAL AND IT ALREADY BIT THIS BRANCH. The DIGU rows
// printed by runTransmitLiveGeometryTest -- 67.8 dB at 150 Hz, 66.1 dB at
// 200 Hz, 69.7 dB at 300 Hz -- sit within a decibel or two of that estimate at
// every one of those three tones, because 73728 samples is not a whole number
// of cycles of 150, 200 or 300 Hz. They are the instrument. Only the 1 kHz row
// escaped it, by accident: 73728 is exactly 1536 periods of 1 kHz at 48 kHz, so
// the kernel was already nulled there, which is why that row alone reads a
// floor figure rather than a filter figure.
//
// Truncating to a whole number of cycles puts the image bin exactly on a null
// of the kernel: the bin spacing fs/N then divides the 2f separation exactly
// and the leakage term vanishes. The period is fs/gcd(f, fs) samples, which is
// why every tone in the sweep is an integer number of hertz. The capture's
// absolute start index does not matter -- an offset only rotates the leakage
// term's phase, it does not stop it summing to zero.
//
// This is the same correction tests/hl2_txdsp_test.cpp's characterisation
// sweep makes with wholeCycles(). Both chains have to be measured through the
// same instrument or the comparison is not one.
std::size_t wholeCycleCount(std::size_t have, double hz, double fs)
{
    const long f = std::lround(hz);
    const long r = std::lround(fs);
    if (f <= 0 || r <= 0) {
        return 0;
    }
    const std::size_t period = static_cast<std::size_t>(r / std::gcd(f, r));
    return (have / period) * period;
}

// Input block period at the live rates, in microseconds: what
// Hl2TxDsp::processAudioBlock's accumulator hands the channel, paced by
// AudioEngine's 5 ms TX poll timer.
constexpr int kLivePaceUs = 512 * 1000000 / 24000;   // 21333
// Pace for the spectral runs. Four times FASTER than the live cadence, and
// still five times slower than the point the census below shows the worker
// keeping up, so these runs are not measuring a race. Keeping them off the live
// cadence keeps the case's wall-clock cost to a few seconds.
constexpr int kSpectralPaceUs = 5000;

bool runTransmitLiveGeometryTest()
{
    // 1. Underrun census against caller pacing. This is the measurement the
    //    original TXA attempt needed and the one runVector cannot make.
    std::cout << "  TX underrun census at the live geometry"
                 " (512 in / 1024 dsp, 24k->48k, bfo=0):\n";
    bool pacedClean = false;
    for (const int paceUs : {0, 1000, 5000, kLivePaceUs}) {
        // The unpaced leg costs no wall clock, so run it long enough to show
        // that the state does not clear itself.
        const std::size_t censusBlocks = (paceUs == 0) ? 256 : 64;
        const TransmitRun census =
            runTransmitChannel(0, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                               censusBlocks, censusBlocks, paceUs);
        if (!require(census.created, "live-geometry transmit channel was refused")) {
            return false;
        }
        // Only blocks at full amplitude: a block still inside the mute ramp or
        // bp0's fill has a meaningless phase, and including it would report a
        // priming transient as a pointer fault.
        double phaseSpread = 0.0;
        std::size_t settledBlocks = 0;
        {
            double reference = 0.0;
            bool haveReference = false;
            for (std::size_t n = 0; n < census.blockPhaseDeg.size(); ++n) {
                if (census.blockPhasePeak[n] < 0.5 * census.peakMagnitude) {
                    continue;
                }
                ++settledBlocks;
                if (!haveReference) {
                    reference = census.blockPhaseDeg[n];
                    haveReference = true;
                    continue;
                }
                double delta = census.blockPhaseDeg[n] - reference;
                while (delta > 180.0) { delta -= 360.0; }
                while (delta < -180.0) { delta += 360.0; }
                phaseSpread = std::max(phaseSpread, std::abs(delta));
            }
        }
        std::cout << "    pace " << paceUs << " us over " << censusBlocks
                  << " blocks: ok=" << census.okBlocks
                  << " underrun=" << census.underrunBlocks
                  << " other=" << census.otherBlocks
                  << " lastUnderrun=" << census.lastUnderrunBlock
                  << " signalBlocks=" << census.blockPhaseDeg.size()
                  << " settledBlocks=" << settledBlocks
                  << " maxPhaseSpread=" << phaseSpread << " deg\n";
        if (!require(census.otherBlocks == 0,
                     "a live-geometry transmit channel reported a hard error")) {
            return false;
        }
        if (paceUs == kLivePaceUs) {
            // At the live cadence the pipeline primes and stays primed. The
            // margin is deliberately loose -- what this falsifies is the
            // existing note's "Underrun on most blocks", not a tight bound.
            pacedClean = census.underrunBlocks * 20 <=
                         static_cast<int>(censusBlocks);
            // AND the output must not have SLIPPED. An underrun does not merely
            // drop a block: fexchange2 advances r2_outidx on the miss without
            // consuming, so the read pointer catches the write pointer up and
            // the stream thereafter runs one whole DSP buffer ahead -- measured
            // here as a 120 degree step at 1 kHz, which is exactly 1024 samples
            // at 48 kHz. A block of audio is discarded on top of the block of
            // silence, permanently, with nothing reported. Only asserted on a
            // leg that underran nothing, so a loaded machine reports the slip
            // rather than failing twice for one cause.
            if (census.underrunBlocks == 0 &&
                !require(phaseSpread < 1.0,
                         "a clean transmit channel's output slipped against "
                         "its input")) {
                return false;
            }
        }
    }
    if (!require(pacedClean,
                 "a transmit channel paced at the live block period underran "
                 "more than 5% of blocks")) {
        return false;
    }

    constexpr std::size_t kBlocks = 96;
    constexpr std::size_t kDiscard = 24;   // past the mute ramp and bp0's fill

    // 2. The backend's arrangement: mono audio in I, Q empty.
    const TransmitRun iOnly =
        runTransmitChannelSettled(0, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                           kBlocks, kDiscard, kSpectralPaceUs);
    if (!require(iOnly.created, "live-geometry transmit channel was refused")) {
        return false;
    }
    std::cout << "  TX live geometry (I only): ok=" << iOnly.okBlocks
              << " underrun=" << iOnly.underrunBlocks
              << " firstNonZero=" << iOnly.firstNonZeroBlock
              << " peak=" << iOnly.peakMagnitude << '\n';

    // 3. The same feed in the other plane. xpanel's inselect = 2 discards Q.
    const TransmitRun qOnly =
        runTransmitChannel(1, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                           24, 24, kSpectralPaceUs);
    if (!require(qOnly.created, "live-geometry transmit channel was refused")) {
        return false;
    }
    std::cout << "  TX live geometry (Q only): ok=" << qOnly.okBlocks
              << " peak=" << qOnly.peakMagnitude << '\n';

    if (!require(iOnly.iq.size() >= 4096,
                 "live-geometry transmit channel produced too few contiguous "
                 "Ok blocks to correlate") ||
        !require(iOnly.peakMagnitude > 0.0,
                 "audio in I produced no transmit IQ at the live geometry") ||
        !require(qOnly.peakMagnitude == 0.0,
                 "audio in Q produced transmit IQ; xpanel's inselect changed")) {
        return false;
    }

    // 4. Sideband and opposite-sideband suppression, measured with the same
    //    instrument and on the same convention hl2_txdsp_test uses: the
    //    wire-facing plane pair, against an audio tone, with nothing
    //    downstream. Two conjugations cannot cancel here.
    // Trimmed to a whole number of 1 kHz cycles, like every figure in
    // runTransmitSuppressionSweepTest. At this capture length it happens to be
    // a no-op -- 73728 samples is exactly 1536 periods of 1 kHz at 48 kHz --
    // but it is written rather than relied on, because the tones where it is
    // NOT a no-op are the ones that matter and the two places must agree.
    const std::size_t iOnlyTrim =
        wholeCycleCount(iOnly.iq.size(), 1000.0, 48000.0);
    const double upper = binPower(iOnly.iq, iOnly.index, 1000.0, 48000.0, iOnlyTrim);
    const double lower = binPower(iOnly.iq, iOnly.index, -1000.0, 48000.0, iOnlyTrim);
    std::cout << "  TX USB {300,2700} 1 kHz: +1 kHz " << upper
              << "  -1 kHz " << lower << "  suppression "
              << suppressionDb(std::max(upper, lower), std::min(upper, lower))
              << " dB over " << iOnly.iq.size() << " samples\n";

    // hl2_txdsp_test asserts exactly this of Hl2TxDsp's CONJUGATED output. TXA
    // reaches it with no conjugation, because fir_bandpass's exp(-j*w_osc*pos)
    // impulse makes a positive signed band select the negative baseband half.
    // So Hl2Backend::defaultTxPassbandForMode's positive-for-every-mode table
    // is already right for a TXA channel, and ADDING the conjugation would
    // transmit on the wrong sideband.
    if (!require(lower > upper,
                 "TXA with positive passband edges did not put the tone on the "
                 "LOWER wire bin")) {
        return false;
    }
    if (!require(suppressionDb(lower, upper) < -40.0,
                 "TXA opposite-sideband suppression below 40 dB")) {
        return false;
    }

    // 5. Negative passband edges mirror it. TXASetupBPFilters handles TXA_LSB
    //    and TXA_USB with the identical CalcBandpassFilter call, so the MODE
    //    does not select the sideband in TXA -- the passband sign does, exactly
    //    as it does in RXA.
    const TransmitRun mirrored =
        runTransmitChannelSettled(0, 1000.0, WdspChannel::Mode::Usb, -2700.0, -300.0,
                           kBlocks, kDiscard, kSpectralPaceUs);
    if (!require(mirrored.created && !mirrored.iq.empty(),
                 "mirrored-passband transmit channel produced nothing")) {
        return false;
    }
    const double mirroredUpper = binPower(mirrored.iq, mirrored.index, 1000.0, 48000.0);
    const double mirroredLower = binPower(mirrored.iq, mirrored.index, -1000.0, 48000.0);
    std::cout << "  TX USB {-2700,-300} 1 kHz: +1 kHz " << mirroredUpper
              << "  -1 kHz " << mirroredLower << "  suppression "
              << suppressionDb(std::max(mirroredUpper, mirroredLower),
                               std::min(mirroredUpper, mirroredLower)) << " dB\n";
    if (!require(mirroredUpper > mirroredLower,
                 "negative passband edges did not mirror the sideband")) {
        return false;
    }

    // 6. The DIGU/DIGL low edge used to be measured here, at 150, 200, 300 and
    //    1000 Hz, and the figures it printed -- 67.8, 66.1 and 69.7 dB -- WERE
    //    WRONG. They were the rectangular analysis window's Dirichlet leakage,
    //    not bp0's stopband: 73728 samples is not a whole number of cycles of
    //    150, 200 or 300 Hz, so the wanted bin bled into the image bin at
    //    roughly wanted * fs / (pi * 2f * N), which comes to within a decibel
    //    or two of each of those three numbers. Only the 1 kHz row escaped,
    //    because 73728 IS exactly 1536 periods of 1 kHz -- and that row alone
    //    read a floor figure rather than a filter figure, which was the clue.
    //
    //    Correctly trimmed, the same four points read 173.1, 172.8, 170.1 and
    //    283.2 dB. The whole low-edge comparison now lives in
    //    runTransmitSuppressionSweepTest, which measures all four modes at all
    //    fourteen of hl2_txdsp_test's tones through hl2_txdsp_test's own
    //    correction, so that the two chains are read by one instrument.

    // 7. Out-of-passband rejection -- the assertion that caught the wideband
    //    Hilbert bug on the phasing modulator.
    const TransmitRun outOfBand =
        runTransmitChannelSettled(0, 5000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                           kBlocks, kDiscard, kSpectralPaceUs);
    if (!require(outOfBand.created && !outOfBand.iq.empty(),
                 "out-of-band transmit channel produced nothing")) {
        return false;
    }
    const std::size_t outOfBandTrim =
        wholeCycleCount(outOfBand.iq.size(), 5000.0, 48000.0);
    const double leak =
        std::max(binPower(outOfBand.iq, outOfBand.index, 5000.0, 48000.0,
                          outOfBandTrim),
                 binPower(outOfBand.iq, outOfBand.index, -5000.0, 48000.0,
                          outOfBandTrim));
    std::cout << "  TX out-of-band 5 kHz against a 2700 Hz edge: "
              << suppressionDb(lower, leak) << " dB below an in-band tone\n";
    if (!require(suppressionDb(lower, leak) < -60.0,
                 "a 5 kHz tone leaked through a 2700 Hz transmit filter")) {
        return false;
    }

    return true;
}

// ── The characterisation sweep, run against a TXA channel ────────────────
//
// WHAT THIS IS. tests/hl2_txdsp_test.cpp carries a CHARACTERISATION SWEEP over
// four modes and fourteen tones that measures the PHASING MODULATOR's
// opposite-sideband suppression at the passband each mode actually gets. That
// sweep is the instrument this migration is judged by, and it was pointed at
// today's code first, deliberately, so that it was known to discriminate before
// it was asked to judge a replacement.
//
// THIS BLOCK IS THE OTHER HALF: the same measurement, the same fourteen tones,
// the same two passbands, the same rectangular-window correction -- run against
// a WDSP TXA channel opened at the LIVE HL2 transmit geometry.
//
// IT WAS WRITTEN AS A MEASUREMENT OF A CANDIDATE and it is now a measurement of
// the DEFAULT BUILD's transmit modulator: AETHER_HL2_TX_TXA is on, Hl2TxDsp
// opens a channel at this geometry, and the phasing modulator is compiled out.
// What this case still is, and why it stays separate from hl2_txdsp_test, is a
// measurement of the RAW CHANNEL -- opened here, by this file, with no level
// chain in front of it. hl2_txdsp_test measures the same quantity through the
// shipped stage. That the two agree (180.62 dB here against 180.6 there at
// DIGU 150 Hz) is worth more than either figure alone, because the two harnesses
// share no code but the arithmetic.
//
// WHERE IT DIFFERS FROM THE PHASING MODULATOR'S SWEEP, AND WHY. Three
// deliberate differences, each forced by what a TXA channel is:
//
//  1. THE PASSBAND SIGN CARRIES THE SIDEBAND, NOT THE MODE. TXASetupBPFilters
//     handles TXA_LSB and TXA_USB with the identical CalcBandpassFilter call,
//     so SetTXAMode does not choose a sideband for SSB -- SetTXABandpassFreqs
//     does, through the sign of its edges, exactly as it does in RXA. The LSB
//     and DIGL rows therefore run {-2700, -300} and {-3000, -150} where
//     hl2_txdsp_test's rows run the positive edges
//     Hl2Backend::defaultTxPassbandForMode returns today. The MODE is still set
//     per row, because a migration would set it.
//
//  2. THE ALC CANNOT BE SWITCHED OFF, SO IT IS MEASURED INSTEAD. The phasing
//     modulator's sweep runs with Hl2TxDsp's ALC off. A TXA channel has no such
//     switch: create_txa brings alc up with run = 1, max_gain = 1.0 and
//     out_targ = 1.0. It cannot BOOST -- max_gain 1.0 is unity -- but it will
//     pull down anything that reaches the target, and a sweep run through a
//     limiter measures gain-riding rather than a filter.
//
//     The amplitude is therefore hl2_txdsp_test's own 0.5, and the linearity
//     probe below is what licenses it: halving the audio halves the emitted
//     carrier exactly, which a limiter in circuit cannot do. THIS REPLACED AN
//     EARLIER AND WRONG REASON FOR RUNNING AT 0.1 -- that bp0's analytic gain
//     of 2.0 would put a 0.5 tone on the limiter's threshold. It does not.
//     The gain of 2.0 applies to the HALF of a real cosine that survives, so
//     the complex envelope comes out at the tone's own amplitude: a 0.5 tone
//     reaches 0.5 against out_targ = 1.0, with 6 dB in hand. Measured both
//     ways, the sweep's worst in-band point moves from 163.8 dB at 0.1 to
//     167.3 dB at 0.5 -- the ALC is idle at both and the choice changes
//     nothing. It is set to 0.5 so that not even the amplitude differs between
//     the two chains' tables.
//
//  3. THE CAPTURE IS INDEXED. runTransmitChannel records each sample's ABSOLUTE
//     position and restarts collection after any non-Ok block, because at
//     bfo = 0 an underrun slips the output stream by a whole DSP buffer
//     permanently (see the census in runTransmitLiveGeometryTest). The phasing
//     modulator has no such seam.
//
// AND THE WIRE CANNOT CARRY WHAT THIS MEASURES. MetisProtocol.cpp's
// ep2WriteTxIq packs I and Q as SIGNED 16-BIT samples -- `v * 32767.0f`, two
// bytes each -- so EP2 quantises the transmit stream at about 96 dB of dynamic
// range before a single sample reaches the radio, and less than that in
// practice because the modulator does not run at full scale. Every figure in
// the table below is therefore ABOVE THE WIRE, and above roughly 96 dB it
// describes a chain the HL2's transmit endpoint cannot deliver.
//
// That is not an argument against the migration; it is an argument about WHERE
// the migration pays. At 1 kHz the phasing modulator already measures 87.15 dB,
// which is at the wire's own floor, so TXA's advantage there is unusable. At
// 150 Hz on the DIGU/DIGL passband the incumbent measures 22.06 dB -- seventy
// decibels of headroom that the wire could carry and the modulator does not
// fill. THAT is the gap this replaces, and it is the gap in the modes WSJT-X
// transmits in.
//
// WHAT IS NOT MEASURED HERE, and it is most of what a transmitter does. No
// radio is keyed, no hpsdrsim runs, no socket opens. These are a WDSP channel's
// own emitted IQ read in wire order inside a unit test -- the same class of
// measurement hl2_txdsp_test makes, with the same blindness. It sees no PA, no
// wire, no antenna, no IMD, no EP2 pacing and no MetisClient queue. It says
// nothing about whether a TXA channel can be driven by the live caller for a
// whole over; that is the census's job, and the census is a 64-block run on an
// idle Mac, not an over.
struct SweepPoint
{
    bool measured = false;
    double wanted = 0.0;
    double image = 0.0;
    double suppDb = 0.0;
    double untrimmedSuppDb = 0.0;
    std::size_t samples = 0;        // after the whole-cycle trim
    std::size_t captured = 0;       // before it
    int underruns = 0;
};

SweepPoint measureSweepPoint(WdspChannel::Mode mode, double lowHz, double highHz,
                             bool wireUpper, double toneHz,
                             std::size_t blocks, std::size_t discardBlocks,
                             int paceUs, double amplitude)
{
    SweepPoint point;
    const TransmitRun run =
        runTransmitChannel(0, toneHz, mode, lowHz, highHz, blocks, discardBlocks,
                           paceUs, amplitude);
    point.underruns = run.underrunBlocks;
    if (!run.created || run.iq.empty()) {
        return point;
    }
    point.captured = run.iq.size();

    const double rawUpper = binPower(run.iq, run.index, toneHz, 48000.0);
    const double rawLower = binPower(run.iq, run.index, -toneHz, 48000.0);
    point.untrimmedSuppDb =
        -suppressionDb(wireUpper ? rawUpper : rawLower,
                       wireUpper ? rawLower : rawUpper);

    const std::size_t trimmed = wholeCycleCount(run.iq.size(), toneHz, 48000.0);
    if (trimmed == 0) {
        return point;
    }
    point.samples = trimmed;
    const double upper = binPower(run.iq, run.index, toneHz, 48000.0, trimmed);
    const double lower = binPower(run.iq, run.index, -toneHz, 48000.0, trimmed);
    point.wanted = wireUpper ? upper : lower;
    point.image = wireUpper ? lower : upper;
    point.suppDb = -suppressionDb(point.wanted, point.image);
    point.measured = true;
    return point;
}

// One sweep point, re-run if the modulator was STARVED.
//
// A starved run does not announce itself. runTransmitChannel restarts
// collection after any non-Ok block -- correlating across the seam would
// average two time origins -- so what comes back is a SHORTER capture that
// opens inside the post-underrun transient, with no second discard in front of
// it. Both of those corrupt the figure, and the second one dominates: a
// transient is amplitude modulation and amplitude modulation lands in the image
// bin. Measured under a load average of 252, the USB 2500 Hz point read
// 77.80 dB where a clean run reads 171.98, and the suite failed claiming TXA's
// suppression had fallen below the phasing modulator's best point. That is a
// true statement about the capture and a false one about the chain, and only
// the second one is what the assertion is for.
//
// THE PREDICATE IS "NO UNDERRUN", NOT "LONG ENOUGH". A length guard was tried
// first and let the defect through -- see runTransmitChannelSettled.
//
// Retrying is the right place for this and a retry inside Hl2TxDsp would not
// be: pacing is the CALLER's responsibility, and here the caller is this
// function. Four attempts, and each retry is PRINTED rather than hidden -- a
// point that needs retrying on an idle machine is evidence about the chain.
SweepPoint measureSweepPointSettled(WdspChannel::Mode mode, double lowHz,
                                    double highHz, bool wireUpper, double toneHz,
                                    std::size_t blocks, std::size_t discardBlocks,
                                    int paceUs, double amplitude)
{
    SweepPoint best;
    for (int attempt = 0; attempt < 4; ++attempt) {
        // The pace BACKS OFF on each retry -- see runTransmitChannelSettled.
        const int pace = paceUs * (attempt + 1);
        const SweepPoint point =
            measureSweepPoint(mode, lowHz, highHz, wireUpper, toneHz, blocks,
                              discardBlocks, pace, amplitude);
        if (point.measured && point.underruns == 0) {
            return point;
        }
        if (point.samples > best.samples) {
            best = point;
        }
        std::cout << "  (starved at " << toneHz << " Hz: " << point.underruns
                  << " underrun(s) at " << pace << " us -- retrying slower)\n";
    }
    return best;
}

bool runTransmitSuppressionSweepTest()
{
    // 104 blocks of 1024 output samples with the first 24 discarded leaves
    // 81920 samples -- 1.71 s at 48 kHz -- which is a whole number of cycles of
    // every tone in the table once trimmed. The pace is four times faster than
    // the live 21.3 ms block period and twice as fast as the slowest leg the
    // census shows running clean, so it is not measuring a race; it keeps the
    // 56 points to about twelve seconds of wall clock.
    constexpr std::size_t kBlocks = 104;
    constexpr std::size_t kDiscard = 24;
    // The FIRST attempt's pace. measureSweepPointSettled backs it off on each
    // retry, up to 8 ms, which is still under half the live 21.33 ms period.
    constexpr int kPaceUs = 2000;
    constexpr double kAmplitude = 0.5;   // hl2_txdsp_test's sweep amplitude

    // The floor of this instrument, not of the chain. Below about -200 dB the
    // image bin is at the numerical floor of a double-precision correlation
    // over float32 samples and the figure printed is not a filter measurement.
    // Rows at or past it are reported as a bound, never as a value.
    constexpr double kInstrumentFloorDb = 200.0;

    struct SweepBand
    {
        const char* name;
        WdspChannel::Mode mode;
        double lowHz;       // SIGNED, as SetTXABandpassFreqs wants it
        double highHz;
        bool wireUpper;     // which wire bin carries the wanted sideband
    };
    // The audio-domain passbands are hl2_txdsp_test's: {300, 2700} for the
    // voice modes and {150, 3000} for the digital ones, from
    // Hl2Backend::defaultTxPassbandForMode. The SIGNS are TXA's.
    const SweepBand bands[] = {
        {"USB",  WdspChannel::Mode::Usb,    300.0,  2700.0, false},
        {"LSB",  WdspChannel::Mode::Lsb,  -2700.0,  -300.0, true},
        {"DIGU", WdspChannel::Mode::Digu,   150.0,  3000.0, false},
        {"DIGL", WdspChannel::Mode::Digl, -3000.0,  -150.0, true},
    };
    // Integer hertz, so wholeCycleCount can null the analysis leakage exactly.
    // The same fourteen hl2_txdsp_test's sweep uses, including the two that sit
    // outside the voice passband on purpose.
    const double tones[] = {100.0,  150.0,  200.0,  250.0,  300.0,  400.0,
                            500.0,  700.0, 1000.0, 1500.0, 2000.0, 2500.0,
                           2700.0, 3000.0};

    std::cout << "  === TXA opposite-sideband characterisation sweep "
                 "(live geometry, 512 in / 1024 dsp, 24k->48k, bfo=0, "
                 "amplitude 0.5) ===\n";
    std::cout << "  mode passband      tone     wanted        image"
                 "     supp dB   untrimmed  samples\n";

    double worstInBandDb = 1.0e9;
    double worstInBandHz = 0.0;
    const char* worstInBandMode = "";
    double worstSettledDb = 1.0e9;

    for (const SweepBand& band : bands) {
        const double absLow = std::min(std::abs(band.lowHz), std::abs(band.highHz));
        const double absHigh = std::max(std::abs(band.lowHz), std::abs(band.highHz));
        for (const double toneHz : tones) {
            const SweepPoint point =
                measureSweepPointSettled(band.mode, band.lowHz, band.highHz,
                                         band.wireUpper, toneHz, kBlocks,
                                         kDiscard, kPaceUs, kAmplitude);
            std::string what = std::string("sweep ") + band.name + " at " +
                               std::to_string(static_cast<int>(toneHz)) +
                               " Hz produced analysable IQ";
            if (!require(point.measured, what.c_str())) {
                return false;
            }
            // STARVATION IS REPORTED AS STARVATION, and not folded into the
            // dB assertion below -- see measureSweepPointSettled. Named
            // separately so a failure here sends the reader to the machine's
            // load and a failure there sends them to the chain. On a shared
            // machine these are indistinguishable from the decibel figure
            // alone, and only one of them is about the transmitter.
            what = std::string("sweep ") + band.name + " at " +
                   std::to_string(static_cast<int>(toneHz)) +
                   " Hz was not starved (no underruns in the measured run)";
            if (!require(point.underruns == 0, what.c_str())) {
                return false;
            }

            // The dB figures and the handedness assertion apply IN-BAND only,
            // on the same reasoning hl2_txdsp_test gives -- and here the
            // reasoning is much stronger than it is there. bp0 is 2048 taps
            // against Hl2TxDsp's 255, so a tone below the low edge is not
            // merely attenuated, it is GONE: at 100 Hz on {300, 2700} both
            // bins land near 1e-11, which is the chain's own numerical floor
            // and has no handedness at all. The phasing modulator passes that
            // same tone about 12 dB down with 14.3 dB of sideband suppression,
            // so its sweep can meaningfully assert a wire bin out of band and
            // this one cannot. Out-of-band rows are printed as a REJECTION
            // figure, which is what they are evidence about.
            const bool inBand = (toneHz >= absLow && toneHz <= absHigh);

            char line[224];
            std::snprintf(line, sizeof(line),
                          "  %-4s %6.0f..%-6.0f %5.0f %12.5e %12.5e %9.2f %11.2f %8zu",
                          band.name, band.lowHz, band.highHz, toneHz,
                          point.wanted, point.image, point.suppDb,
                          point.untrimmedSuppDb, point.samples);
            std::cout << line;
            if (!inBand) {
                const double rejectionDb =
                    20.0 * std::log10(kAmplitude /
                                      std::max(1.0e-30,
                                               std::max(point.wanted, point.image)));
                std::cout << "   out of band, rejected " << rejectionDb << " dB";
            } else if (point.suppDb >= kInstrumentFloorDb) {
                std::cout << "   (image at the instrument floor)";
            }
            std::cout << '\n';

            what = std::string("sweep ") + band.name + " at " +
                   std::to_string(static_cast<int>(toneHz)) +
                   " Hz: the sideband is on the expected wire bin";
            if (inBand && !require(point.wanted > point.image, what.c_str())) {
                return false;
            }

            if (inBand) {
                if (point.suppDb < worstInBandDb) {
                    worstInBandDb = point.suppDb;
                    worstInBandHz = toneHz;
                    worstInBandMode = band.name;
                }
                if (toneHz >= 500.0) {
                    worstSettledDb = std::min(worstSettledDb, point.suppDb);
                }
            }
        }
    }

    std::cout << "  worst in-band point: " << worstInBandDb << " dB at "
              << worstInBandHz << " Hz (" << worstInBandMode << ")\n";
    std::cout << "  worst in-band point at or above 500 Hz: " << worstSettledDb
              << " dB\n";
    std::cout << "  READ THESE AS LOWER BOUNDS. Every in-band image bin above "
                 "is at or near this\n  instrument's own numerical floor, not "
                 "at a filter response -- see the halving probe\n  below. What "
                 "the table establishes is that TXA's opposite sideband is "
                 "nowhere\n  observable at the float32 interface "
                 "WdspChannel::processIq hands back.\n";

    // ONE FLOOR, NOT hl2_txdsp_test's TWO, and the difference is the finding.
    //
    // That sweep needs a second, tighter bound above 500 Hz because the phasing
    // modulator's curve VARIES -- 22 dB at the low edge, 87 dB at mid-band --
    // so a single floor set from the low edge is passed by degradations that
    // only show up where the filter has settled. TXA's curve does not vary:
    // every in-band point in the table above is against the instrument's floor,
    // the low edge included. A second tier would be measuring the same thing
    // twice.
    //
    // 100 dB is chosen to sit ABOVE THE PHASING MODULATOR'S BEST MEASURED
    // POINT. hl2_txdsp_test's sweep measures the incumbent at 87.15 dB at its
    // strongest (USB, 1 kHz) and 22.06 dB at its weakest (DIGU, 150 Hz). So
    // this assertion reads: TXA's WORST in-band point still beats the phasing
    // modulator's BEST one. Today it does so with 63 dB to spare. If a future
    // change brings TXA anywhere near the chain it is replacing, the migration
    // has lost the argument it was made on, and this is the line that says so.
    //
    // It is not a tight bound and is not meant to be. What it catches is a
    // channel that has been MIS-OPENED -- the wrong passband sign, bp0 not run,
    // a refused setFilter leaving create_txa's {-5000, -100} default in place,
    // audio put in Q -- every one of which lands tens of dB below it, most of
    // them below zero.
    constexpr double kSweepFloorDb = 100.0;
    if (!require(worstInBandDb > kSweepFloorDb,
                 "TXA in-band opposite-sideband suppression fell below 100 dB, "
                 "the phasing modulator's best measured point")) {
        return false;
    }

    // ── The ALC is not riding the sweep, and the floor is the instrument's ──
    //
    // Two checks on the method, not on the chain.
    //
    // FIRST: halving the amplitude must halve the emitted carrier exactly.
    // create_txa's alc runs with max_gain = 1.0 and out_targ = 1.0 and cannot be
    // turned off; if it were acting at the sweep's amplitude the two runs would
    // not scale, because a limiter is not a linear stage. This is the assertion
    // that makes "the ALC is idle here" a measurement rather than a reading of
    // TXA.c.
    //
    // SECOND: the deepest rows above read past 200 dB, which no float32 FIR
    // achieves. If that figure were a real filter response it would not move
    // when the capture is halved; if it is the numerical floor of the
    // correlation it rises by about 3 dB, because the floor is broadband and
    // averages down as 1/sqrt(N) while a real tone does not. The point of
    // printing it is to be able to say which, rather than quoting 297 dB at
    // anyone.
    {
        const SweepPoint full =
            measureSweepPoint(WdspChannel::Mode::Usb, 300.0, 2700.0, false,
                              1000.0, kBlocks, kDiscard, kPaceUs, kAmplitude);
        const SweepPoint half =
            measureSweepPoint(WdspChannel::Mode::Usb, 300.0, 2700.0, false,
                              1000.0, kBlocks, kDiscard, kPaceUs,
                              0.5 * kAmplitude);
        if (!require(full.measured && half.measured,
                     "the ALC linearity probe produced no IQ")) {
            return false;
        }
        const double ratio = half.wanted / std::max(1.0e-20, full.wanted);
        std::cout << "  ALC linearity: audio " << kAmplitude << " -> carrier "
                  << full.wanted << ", audio " << (0.5 * kAmplitude)
                  << " -> carrier " << half.wanted << " (ratio " << ratio
                  << "; an idle ALC gives 0.5)\n";
        if (!require(std::abs(ratio - 0.5) < 0.01,
                     "halving the transmit audio did not halve the emitted "
                     "carrier; the TXA ALC is acting at this level")) {
            return false;
        }

        const std::size_t halfLength =
            wholeCycleCount(full.samples / 2, 1000.0, 48000.0);
        const TransmitRun probe =
            runTransmitChannelSettled(0, 1000.0, WdspChannel::Mode::Usb, 300.0,
                                      2700.0, kBlocks, kDiscard, kPaceUs,
                                      kAmplitude);
        if (!require(probe.created && probe.iq.size() >= full.samples,
                     "the floor probe produced no IQ")) {
            return false;
        }
        const double imageFull =
            binPower(probe.iq, probe.index, 1000.0, 48000.0, full.samples);
        const double imageHalf =
            binPower(probe.iq, probe.index, 1000.0, 48000.0, halfLength);
        std::cout << "  instrument floor probe at USB 1 kHz: image over "
                  << full.samples << " samples " << imageFull << ", over "
                  << halfLength << " samples " << imageHalf << " (ratio "
                  << (imageHalf / std::max(1.0e-30, imageFull))
                  << "; a real tone gives 1, broadband floor gives ~1.41)\n";
    }

    return true;
}

// ── "Underrun on most blocks and zeros on the rest" ───────────────────────
//
// Hl2TxDsp.cpp's note records a TXA attempt that "returned Underrun on most
// blocks and zeros on the rest". S6 §3.6 ranks four candidates for the ZEROS
// half and says none of them is established. This case is the experiment §3.6
// names: a transmit channel at the live rates and block sizes, fed with the
// tone in ONE plane only, printing per-block RMS and ProcessResult for the
// first 200 blocks.
//
// It is deliberately a SIBLING of runTransmitLiveGeometryTest, not a
// replacement. That case establishes the Underrun half (caller cadence) and
// measures the channel's spectra; this one is about the zeros, and about which
// of §3.6's four candidates can and cannot produce them.
//
// The four candidates, and what each leg below does to it:
//
//  1. WRONG PLANE. xpanel runs with inselect = 2 -- create_panel's ninth
//     argument in txa.c's create_txa, commented "1 to use Q, 2 to use I for
//     input" -- and evaluates I = in[2i] * (inselect >> 1),
//     Q = in[2i+1] * (inselect & 1). Q is multiplied by zero. Leg Q feeds the
//     tone into inputQ and nothing into inputI.
//
//  2. PRIMING: the iobuffs mute ramp plus bp0's fill. create_slews sizes
//     ndelup = tdelayup * in_rate and ntup = tslewup * in_rate, and upslew2
//     sits in BEGIN emitting zeros until a non-zero sample arrives. Leg I
//     measures how long the priming actually lasts, in blocks and in samples,
//     against that arithmetic.
//
//  3. A REFUSED CONTROL CALL. WdspChannel::setFilter returns false on
//     lowHz >= highHz and on a control operation already in flight. Leg 3
//     refuses a call on a RUNNING channel and keeps feeding it.
//
//  4. xuslew. Not reachable for SSB: TXAuSlewCheck leaves runmode 0 and the
//     stage is a pass-through. Leg AM drives the same geometry in AM, where
//     ammod.run is 1, and reports what that does to the priming stretch.
//
// Note what upslew2 tests: `(I != 0.0) || (Q != 0.0)`, on the RAW input pair,
// BEFORE xpanel. So candidates 1 and 2 are separable rather than confounded --
// a Q-only feed opens the mute ramp exactly as an I-only feed does, and is
// then zeroed one stage later.

struct BlockRecord
{
    WdspChannel::ProcessResult result = WdspChannel::ProcessResult::Ok;
    double rms = 0.0;
    bool exactlyZero = true;
};

const char* resultName(WdspChannel::ProcessResult result)
{
    switch (result) {
    case WdspChannel::ProcessResult::Ok:                  return "Ok";
    case WdspChannel::ProcessResult::Underrun:            return "Underrun";
    case WdspChannel::ProcessResult::Busy:                return "Busy";
    case WdspChannel::ProcessResult::InvalidBuffer:       return "InvalidBuffer";
    case WdspChannel::ProcessResult::AllocationViolation: return "AllocationViolation";
    case WdspChannel::ProcessResult::EngineError:         return "EngineError";
    }
    return "?";
}

struct ZerosCensus
{
    bool created = false;
    std::vector<BlockRecord> blocks;
    int okBlocks = 0;
    int underrunBlocks = 0;
    int otherBlocks = 0;
    int firstNonZeroBlock = -1;
    // Index of the first non-zero sample in the OUTPUT stream, at the output
    // rate. Converted to input samples this is the quantity §3.6's mute-ramp
    // arithmetic predicts.
    long long firstNonZeroOutputSample = -1;
    int okBlocksThatWereZero = 0;
    int okBlocksAfterFirstNonZeroThatWereZero = 0;
    double peakRms = 0.0;
    // RMS of the Ok blocks only, in the order they were returned. On a starved
    // caller this is the OUTPUT STREAM's own opening sequence sampled one block
    // at a time, which is the whole point: it is what the caller sees, and it
    // is not the same thing as the first N calls.
    std::vector<double> okRms;
};

// plane: 0 = tone in I only, 1 = tone in Q only.
ZerosCensus runZerosCensus(int plane, double toneHz, WdspChannel::Mode mode,
                           double lowHz, double highHz, std::size_t blocks,
                           int paceUs)
{
    ZerosCensus census;
    const WdspChannel::Config config = liveTransmitConfig(mode, lowHz, highHz);
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, error.c_str())) {
        return census;
    }
    census.created = true;

    std::vector<float> inputI(config.inputBlockSize, 0.0f);
    std::vector<float> inputQ(config.inputBlockSize, 0.0f);
    const std::size_t outBlock = channel->outputBlockSize();
    std::vector<float> outputLeft(outBlock);
    std::vector<float> outputRight(outBlock);

    for (std::size_t block = 0; block < blocks; ++block) {
        for (std::size_t sample = 0; sample < inputI.size(); ++sample) {
            const double phase = 2.0 * std::numbers::pi * toneHz *
                                 static_cast<double>(block * inputI.size() + sample) /
                                 static_cast<double>(config.inputSampleRate);
            const float value = static_cast<float>(0.1 * std::cos(phase));
            inputI[sample] = (plane == 1) ? 0.0f : value;
            inputQ[sample] = (plane == 0) ? 0.0f : value;
        }
        const WdspChannel::ProcessResult result =
            channel->processIq(inputI, inputQ, outputLeft, outputRight);

        BlockRecord record;
        record.result = result;
        double sum = 0.0;
        for (std::size_t k = 0; k < outBlock; ++k) {
            const double l = outputLeft[k];
            const double r = outputRight[k];
            sum += l * l + r * r;
            if (l != 0.0 || r != 0.0) {
                if (record.exactlyZero) {
                    record.exactlyZero = false;
                    if (census.firstNonZeroOutputSample < 0) {
                        census.firstNonZeroOutputSample =
                            static_cast<long long>(block * outBlock + k);
                    }
                }
            }
        }
        record.rms = std::sqrt(sum / static_cast<double>(2 * outBlock));
        census.peakRms = std::max(census.peakRms, record.rms);
        if (!record.exactlyZero && census.firstNonZeroBlock < 0) {
            census.firstNonZeroBlock = static_cast<int>(block);
        }
        switch (result) {
        case WdspChannel::ProcessResult::Ok:
            ++census.okBlocks;
            census.okRms.push_back(record.rms);
            if (record.exactlyZero) {
                ++census.okBlocksThatWereZero;
                if (census.firstNonZeroBlock >= 0) {
                    ++census.okBlocksAfterFirstNonZeroThatWereZero;
                }
            }
            break;
        case WdspChannel::ProcessResult::Underrun:
            ++census.underrunBlocks;
            break;
        default:
            ++census.otherBlocks;
            break;
        }
        census.blocks.push_back(record);
        if (paceUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(paceUs));
        }
    }
    return census;
}

// Per-block print. Every block of the first `verbatim`, then only blocks that
// are not Ok or that change the zero/non-zero state, then a run-length line for
// each collapsed stretch -- so a 200-block leg that says the same thing 190
// times says it once, and nothing that CHANGES is ever hidden.
void printCensus(const char* label, const ZerosCensus& census,
                 std::size_t verbatim)
{
    std::cout << "    " << label << ":\n";
    std::size_t runStart = 0;
    for (std::size_t n = 0; n < census.blocks.size(); ++n) {
        const BlockRecord& record = census.blocks[n];
        const bool changed =
            n == 0 ||
            record.result != census.blocks[n - 1].result ||
            record.exactlyZero != census.blocks[n - 1].exactlyZero;
        if (changed && n > 0 && n > verbatim && n - runStart > 1) {
            std::cout << "      b" << runStart << "..b" << (n - 1) << ": "
                      << resultName(census.blocks[runStart].result)
                      << (census.blocks[runStart].exactlyZero
                              ? "  rms=0 (exactly)" : "  rms>0")
                      << "  (" << (n - runStart) << " blocks, same)\n";
        }
        if (changed) {
            runStart = n;
        }
        if (n < verbatim || changed) {
            std::cout << "      b" << n << ": " << resultName(record.result)
                      << "  rms=" << record.rms
                      << (record.exactlyZero ? "  (exactly zero)" : "") << '\n';
        }
    }
    const std::size_t last = census.blocks.size();
    if (last > verbatim && last - runStart > 1) {
        std::cout << "      b" << runStart << "..b" << (last - 1) << ": "
                  << resultName(census.blocks[runStart].result)
                  << (census.blocks[runStart].exactlyZero
                          ? "  rms=0 (exactly)" : "  rms>0")
                  << "  (" << (last - runStart) << " blocks, same)\n";
    }
    std::cout << "      totals: ok=" << census.okBlocks
              << " underrun=" << census.underrunBlocks
              << " other=" << census.otherBlocks
              << " firstNonZeroBlock=" << census.firstNonZeroBlock
              << " firstNonZeroOutputSample=" << census.firstNonZeroOutputSample
              << " okButZero=" << census.okBlocksThatWereZero
              << " okButZeroAfterSignalStarted="
              << census.okBlocksAfterFirstNonZeroThatWereZero
              << " peakRms=" << census.peakRms << '\n';
}

bool runTransmitZerosCensusTest()
{
    constexpr std::size_t kBlocks = 200;     // §3.6's own number
    constexpr std::size_t kVerbatim = 12;

    // The mute-ramp arithmetic §3.6 predicts, printed so the measurement below
    // can be read against it rather than against a remembered number.
    const WdspChannel::Config probe =
        liveTransmitConfig(WdspChannel::Mode::Usb, 300.0, 2700.0);
    const int ndelup = static_cast<int>(probe.muteDelayUpSec *
                                        probe.inputSampleRate);
    const int ntup = static_cast<int>(probe.muteSlewUpSec *
                                      probe.inputSampleRate);
    std::cout << "  TX zeros census at the live geometry (512 in / 1024 dsp, "
                 "24k->48k, bfo=0)\n";
    std::cout << "    create_slews arithmetic: ndelup=" << ndelup
              << " + ntup=" << ntup << " = " << (ndelup + ntup)
              << " input samples = "
              << static_cast<double>(ndelup + ntup) /
                     static_cast<double>(probe.inputBlockSize)
              << " input blocks; bp0 = " << probe.filterTaps << " taps over "
              << probe.dspBlockSize << "-sample DSP passes\n";

    // ── Leg I: the tone in inputI, paced at the live block period. ────────
    const ZerosCensus legI =
        runZerosCensus(0, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                       kBlocks, kLivePaceUs);
    if (!require(legI.created, "live-geometry transmit channel was refused")) {
        return false;
    }
    printCensus("leg I  (tone in inputI, inputQ zero, live pace)", legI,
                kVerbatim);

    // ── Leg Q: the same tone in inputQ. ───────────────────────────────────
    const ZerosCensus legQ =
        runZerosCensus(1, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                       kBlocks, kLivePaceUs);
    if (!require(legQ.created, "live-geometry transmit channel was refused")) {
        return false;
    }
    printCensus("leg Q  (tone in inputQ, inputI zero, live pace)", legQ,
                kVerbatim);

    // ── Leg U: I-only, unpaced. The historical caller's cadence. ──────────
    const ZerosCensus legU =
        runZerosCensus(0, 1000.0, WdspChannel::Mode::Usb, 300.0, 2700.0,
                       kBlocks, 0);
    if (!require(legU.created, "live-geometry transmit channel was refused")) {
        return false;
    }
    printCensus("leg U  (tone in inputI, unpaced tight loop)", legU, kVerbatim);

    // ── Leg AM: candidate 4's stage, reachable. ───────────────────────────
    const ZerosCensus legAm =
        runZerosCensus(0, 1000.0, WdspChannel::Mode::Am, -3000.0, 3000.0,
                       kBlocks, kLivePaceUs);
    if (!require(legAm.created, "AM live-geometry transmit channel was refused")) {
        return false;
    }
    printCensus("leg AM (tone in inputI, AM, live pace -- xuslew reachable)",
                legAm, kVerbatim);

    // ── What the four legs settle ─────────────────────────────────────────

    // CANDIDATE 1 is a real, silent, permanent zeros mechanism. Not "zeros for
    // a while": zeros for every block of the run, with every ProcessResult Ok,
    // so a caller watching the return value sees a healthy channel.
    if (!require(legQ.peakRms == 0.0,
                 "audio in Q produced transmit output; xpanel's inselect "
                 "changed")) {
        return false;
    }
    if (!require(legQ.otherBlocks == 0 && legQ.underrunBlocks == 0,
                 "the Q-only leg reported an error, so the wrong-plane fault "
                 "is not silent after all")) {
        return false;
    }
    if (!require(static_cast<std::size_t>(legQ.okBlocksThatWereZero) == kBlocks,
                 "the Q-only leg did not report Ok-and-zero on every block")) {
        return false;
    }

    // CANDIDATE 2 is real but BOUNDED, and the bound is what excludes it as an
    // explanation for zeros that persist. Output starts, and once started it
    // never returns to exact zero on an Ok block.
    if (!require(legI.firstNonZeroBlock >= 0,
                 "audio in I produced no transmit output at all")) {
        return false;
    }
    if (!require(legI.firstNonZeroBlock < 8,
                 "the priming stretch on an I-only feed ran past 8 blocks")) {
        return false;
    }
    if (!require(legI.okBlocksAfterFirstNonZeroThatWereZero == 0,
                 "an Ok block returned exact zeros AFTER the channel had "
                 "started producing output")) {
        return false;
    }

    // THE RECORDED SYMPTOM IS LEG U, AND ITS ZEROS ARE CANDIDATE 2's.
    // Hl2TxDsp.cpp's note says "Underrun on most blocks AND ZEROS ON THE REST"
    // -- the rest of the BLOCKS, not the rest of the run. Leg U reproduces both
    // halves at once with the audio in the RIGHT plane. The caller outruns the
    // worker, fexchange2 misses on nearly every call, and the few reads that do
    // land walk the OUTPUT STREAM's opening blocks one at a time -- so what the
    // caller gets back is leg I's priming stretch, spread over hundreds of
    // calls instead of five. The okRms sequence printed below is that walk.
    //
    // How far it walks is load-dependent, and deliberately not asserted: on a
    // busier machine fewer reads land and every one of them is zero (which is
    // the record verbatim); on a quieter one a late read reaches full
    // amplitude. What is asserted is the part that does not move -- most calls
    // underrun, and the first read a starved caller gets is exact zeros.
    //
    // So candidate 1 is NOT what the note describes. It cannot be: leg Q shows
    // a wrong-plane channel returns Ok on every block and underruns NOTHING,
    // which contradicts the "Underrun on most blocks" half of the record. It
    // remains a real, silent, permanent zeros mechanism -- just not this one.
    if (!require(legU.underrunBlocks * 2 >= static_cast<int>(kBlocks),
                 "an unpaced caller at the live geometry did not underrun; the "
                 "recorded symptom was not reproduced")) {
        return false;
    }
    if (!require(!legU.okRms.empty() && legU.okRms.front() == 0.0,
                 "the first block a starved transmit caller got back was not "
                 "exact zeros")) {
        return false;
    }
    std::cout << "    leg U Ok-block rms in order:";
    for (const double value : legU.okRms) {
        std::cout << ' ' << value;
    }
    std::cout << "\n    leg I block rms in order (first 8):";
    for (std::size_t n = 0; n < 8 && n < legI.blocks.size(); ++n) {
        std::cout << ' ' << legI.blocks[n].rms;
    }
    std::cout << '\n';
    std::cout << "    verdict:\n"
                 "      candidate 1 (leg Q): " << legQ.okBlocksThatWereZero
              << "/" << kBlocks << " Ok-and-exactly-zero, "
              << legQ.underrunBlocks << " underruns. A real, silent, permanent"
                 " zeros mechanism -- but it underruns NOTHING, so it does not"
                 " fit the recorded \"Underrun on most blocks\".\n"
                 "      candidate 2 (leg I): priming ends at output sample "
              << legI.firstNonZeroOutputSample << " ("
              << 1000.0 * static_cast<double>(legI.firstNonZeroOutputSample) /
                     static_cast<double>(probe.outputSampleRate)
              << " ms), of which create_slews accounts for "
              << 1000.0 * static_cast<double>(ndelup + ntup) /
                     static_cast<double>(probe.inputSampleRate)
              << " ms. Bounded: output never returns to exact zero after it"
                 " starts.\n"
                 "      leg U = the record: underrun=" << legU.underrunBlocks
              << "/" << kBlocks << ", ok=" << legU.okBlocks
              << ". The output stream advanced " << legU.okBlocks
              << " blocks in " << kBlocks << " calls, so candidate 2's bounded"
                 " priming is what a starved caller sees for the whole run.\n";

    // CANDIDATE 3 cannot be the mechanism either, and this is the leg that
    // shows why: a refused setFilter leaves a RUNNING channel running. The
    // channel is fed across the refusal, so "the caller stopped feeding" --
    // §3.6's own caveat -- is the only way a false becomes zeros, and that is a
    // property of the caller, not of the channel.
    {
        const WdspChannel::Config config =
            liveTransmitConfig(WdspChannel::Mode::Usb, 300.0, 2700.0);
        std::string error;
        std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
        if (!require(channel != nullptr, error.c_str())) {
            return false;
        }
        std::vector<float> inputI(config.inputBlockSize, 0.0f);
        std::vector<float> inputQ(config.inputBlockSize, 0.0f);
        const std::size_t outBlock = channel->outputBlockSize();
        std::vector<float> outputLeft(outBlock);
        std::vector<float> outputRight(outBlock);
        double rmsBefore = 0.0;
        double rmsAfter = 0.0;
        bool refused = false;
        bool accepted = true;
        for (std::size_t block = 0; block < 32; ++block) {
            for (std::size_t sample = 0; sample < inputI.size(); ++sample) {
                const double phase = 2.0 * std::numbers::pi * 1000.0 *
                    static_cast<double>(block * inputI.size() + sample) /
                    static_cast<double>(config.inputSampleRate);
                inputI[sample] = static_cast<float>(0.1 * std::cos(phase));
            }
            if (block == 16) {
                rmsBefore = rms(outputLeft);
                // Inverted edges: setFilter's own guard, on a live channel.
                refused = !channel->setFilter(2700.0, 300.0);
                accepted = channel->setFilter(400.0, 2600.0);
            }
            channel->processIq(inputI, inputQ, outputLeft, outputRight);
            if (block == 31) {
                rmsAfter = rms(outputLeft);
            }
            std::this_thread::sleep_for(
                std::chrono::microseconds(kLivePaceUs));
        }
        std::cout << "    leg 3  (setFilter refused mid-run): "
                     "setFilter(2700,300) refused=" << (refused ? "yes" : "no")
                  << "  setFilter(400,2600) accepted=" << (accepted ? "yes" : "no")
                  << "  rms before=" << rmsBefore << " after=" << rmsAfter << '\n';
        if (!require(refused, "setFilter accepted inverted passband edges") ||
            !require(accepted, "setFilter refused a valid passband on a "
                               "running channel") ||
            !require(rmsBefore > 0.0 && rmsAfter > 0.0,
                     "a channel that refused a setFilter stopped producing "
                     "output")) {
            return false;
        }
    }

    // CANDIDATE 4 -- the AM leg -- is reported, not asserted into a bound. The
    // point is only that the stage being reachable does not turn the priming
    // stretch into a permanent one.
    if (!require(legAm.firstNonZeroBlock >= 0,
                 "an AM transmit channel produced no output at all")) {
        return false;
    }
    if (!require(legAm.okBlocksAfterFirstNonZeroThatWereZero == 0,
                 "an AM Ok block returned exact zeros after output started")) {
        return false;
    }

    return true;
}

} // namespace

// FM DEVIATION IS AN AUDIO GAIN, AND THIS MEASURES IT AS ONE.
//
// Nothing in this tree could move the FM detector's deviation: create_rxa
// builds the fmd stage with a hard 5000.0 and no setter reached it. The risk
// in closing that gap is the defect class #5829 and #5859 record -- a value
// that is stored and acted on with nothing able to write it, and a mechanism
// with no call site -- in this shape: a setter that is called, returns true,
// and changes nothing. An API-only test — call it, read it back — cannot tell
// those apart, because the value it reads back is the one it just stored.
//
// So this drives a real FM signal through a real channel and measures the
// recovered audio. WDSP's detector emits `again * (fil_out - fmdc)` with
// `again = rate / (deviation * TWOPI)` (upstream/fmd.c), and `fil_out` is the
// PLL's instantaneous frequency in radians per sample — so for a signal
// deviated by D_sig and a detector told to assume D_set, recovered audio comes
// out proportional to D_sig / D_set. EVERYTHING ELSE IN THE PATH CANCELS: the
// de-emphasis curve, the audio bandpass, its afgain, and the bp1 gain are all
// linear and identical between the two measurements, and SetRXAMode turns the
// AGC OFF in FM (RXA.c, case RXA_FM: `agc.p->run = 0`) so nothing claws the
// level back. The prediction is therefore not a direction but a NUMBER:
// halving the assumed deviation doubles the audio, exactly.
//
// MEASURED ON THE SAME CHANNEL INSTANCE, never on two channels configured
// differently. Two channels would differ by their FFTW plans and their settle
// history as well as by the deviation, and the comparison would prove only
// that two things are not identical. One channel, one signal, one setter call
// between the measurements.
//
// BOTH DIRECTIONS, because a setter that only ever moves one way is half
// broken and reads as working: 5000 -> 2500 doubles the audio and 2500 -> 5000
// must bring it back to where it started.
//
// AND ACROSS A reconfigure(), which is the failure this would otherwise have
// shipped with. create_rxa builds the fmd stage with a hard 5000.0 and close()
// frees it, so a deviation held only in the runtime setter reverts to 5 kHz on
// the next sample-rate or block-size change — silently, with the stored value
// still reading 2500 and nothing an operator could see. WdspChannel carries it
// in Config and open() re-pushes it; the third measurement is what says so.
bool runFmDeviationTest()
{
    WdspChannel::Config config;
    config.inputBlockSize = 256;
    config.dspBlockSize = 256;
    config.inputSampleRate = 48000;
    config.dspSampleRate = 48000;
    config.outputSampleRate = 48000;
    config.mode = WdspChannel::Mode::Fm;
    // Symmetric about the carrier, and deliberately so: it comfortably passes
    // the modulated signal (Carson bandwidth here is 2*(2500+1000) = 7 kHz),
    // and a symmetric passband is immune to the sideband-handedness trap that
    // runNotchAttenuationTest has to reason about. Nothing in this measurement
    // should depend on which way round the spectrum sits.
    config.filterLowHz = -8000.0;
    config.filterHighHz = 8000.0;
    // Belt and braces, and nothing more than that. SetRXAMode's case RXA_FM
    // clears agc.p->run (RXA.c) and nothing switches it back: applyRxAgc calls
    // SetRXAAGCMode -- which writes agc.p->mode and calls loadWcpAGC, never
    // run -- plus the slope/top/fixed/attack/decay/hang setters, and the only
    // `->run =` writers in wcpAGC.c are create_wcpagc and the TX ALC and
    // leveler. So mode 0 at 0 dB does not turn the AGC off here; it makes the
    // path a constant linear gain if it ever did run.
    config.agcMode = 0;
    config.agcFixedGainDb = 0.0;
    config.blockForOutput = true;

    constexpr double kAudioHz = 1000.0;
    constexpr double kSignalDeviationHz = 2500.0;

    std::string error;
    auto channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, "FM deviation test failed to open a channel")) {
        return false;
    }

    // Long enough to flush the 2048-tap de-emphasis and audio FIRs, which hold
    // ~8 blocks of history at the PREVIOUS gain after a deviation change, plus
    // the channel's mute ramp and the PLL's own acquisition on the first run.
    constexpr std::size_t kSettleBlocks = 60;
    constexpr std::size_t kTotalBlocks = 140;
    std::size_t clock = 0;
    const auto measure = [&](WdspChannel& ch) -> double {
        std::vector<float> inputI(ch.config().inputBlockSize);
        std::vector<float> inputQ(ch.config().inputBlockSize);
        std::vector<float> outputLeft(ch.outputBlockSize());
        std::vector<float> outputRight(ch.outputBlockSize());
        double energy = 0.0;
        for (std::size_t block = 0; block < kTotalBlocks; ++block) {
            fillFmTone(inputI, inputQ, ch.config().inputSampleRate,
                       kAudioHz, kSignalDeviationHz, clock);
            clock += inputI.size();
            if (ch.processIq(inputI, inputQ, outputLeft, outputRight) !=
                WdspChannel::ProcessResult::Ok) {
                return -1.0;
            }
            if (block >= kSettleBlocks) {
                energy += rms(outputLeft);
            }
        }
        return energy / static_cast<double>(kTotalBlocks - kSettleBlocks);
    };

    const double atDefault = measure(*channel);
    if (!require(atDefault > 1.0e-4,
                 "the FM detector produced no audio at the default deviation")) {
        return false;
    }

    if (!require(channel->setFmDeviation(kSignalDeviationHz),
                 "setFmDeviation(2500) was refused on a receive channel")) {
        return false;
    }
    const double atHalf = measure(*channel);

    if (!require(channel->setFmDeviation(5000.0),
                 "setFmDeviation(5000) was refused")) {
        return false;
    }
    const double restored = measure(*channel);

    if (!require(atHalf > 0.0 && restored > 0.0,
                 "an FM deviation measurement failed to run")) {
        return false;
    }

    const double ratio = atHalf / atDefault;
    const double restoredRatio = restored / atDefault;
    std::cout << "FM deviation: assumed 5000 Hz -> " << atDefault
              << ", assumed 2500 Hz -> " << atHalf
              << ", back to 5000 Hz -> " << restored
              << "  (ratio " << ratio << ", expected 2, restored "
              << restoredRatio << ", expected 1)\n";

    // 5% either side. The prediction is exact arithmetic, not a fit, so the
    // tolerance is for the settle tail and float output quantisation and
    // nothing else -- it is deliberately far too tight for a no-op setter
    // (which would give 1.0) to slip through.
    bool ok = true;
    ok = require(ratio > 1.90 && ratio < 2.10,
                 "halving the assumed FM deviation did not double the recovered "
                 "audio -- SetRXAFMDeviation is not reaching the detector") && ok;
    ok = require(restoredRatio > 0.95 && restoredRatio < 1.05,
                 "restoring the FM deviation did not restore the audio level -- "
                 "the setter moves one way only") && ok;

    // Across a rebuild. A DIFFERENT block size, so this is a real close-and-
    // reopen rather than a no-op, and the Config carries the deviation the
    // operator chose.
    WdspChannel::Config rebuilt = config;
    rebuilt.inputBlockSize = 512;
    rebuilt.dspBlockSize = 512;
    rebuilt.fmDeviationHz = kSignalDeviationHz;
    if (!require(channel->reconfigure(rebuilt, &error),
                 "FM channel failed to reconfigure")) {
        return false;
    }
    const double afterRebuild = measure(*channel);
    const double rebuiltRatio = afterRebuild / atDefault;
    std::cout << "FM deviation across reconfigure(): " << afterRebuild
              << "  (ratio " << rebuiltRatio << ", expected 2)\n";
    ok = require(rebuiltRatio > 1.90 && rebuiltRatio < 2.10,
                 "reconfigure() lost the FM deviation -- the rebuilt fmd stage "
                 "is back on create_rxa's 5 kHz default") && ok;

    // Refusals. Zero and negative matter because WDSP divides by this value.
    ok = require(!channel->setFmDeviation(0.0),
                 "a zero FM deviation was accepted -- WDSP divides by it") && ok;
    ok = require(!channel->setFmDeviation(-2500.0),
                 "a negative FM deviation was accepted") && ok;
    ok = require(!channel->setFmDeviation(
                     std::numeric_limits<double>::quiet_NaN()),
                 "a non-finite FM deviation was accepted") && ok;
    ok = require(channel->config().fmDeviationHz == kSignalDeviationHz,
                 "a refused setFmDeviation() still overwrote the stored value") && ok;

    // A RANGE, NOT A SIGN. 1e-40 is positive and finite and a sign check waves
    // it through; again = rate / (deviation * TWOPI) then runs away and the
    // detector's float output goes to infinity. MEASURED on this branch before
    // Config::kMinFmDeviationHz existed, not reasoned about: a channel opened
    // at 1e-40 Hz was accepted and recovered `inf` from the same 2.5 kHz
    // signal that reads 1.66 at the 5 kHz default, and 1e-3 Hz was accepted
    // and recovered 8.3e+06 -- finite, and five million times too loud.
    ok = require(!channel->setFmDeviation(1.0e-40),
                 "a tiny positive FM deviation was accepted -- again = rate / "
                 "(deviation * TWOPI) overflows and the detector emits inf") && ok;
    ok = require(!channel->setFmDeviation(1.0e6),
                 "an FM deviation above the ceiling was accepted") && ok;
    ok = require(channel->config().fmDeviationHz == kSignalDeviationHz,
                 "an out-of-range setFmDeviation() overwrote the stored value") && ok;

    // USB rather than FM, and the filter edges a transmitter actually uses:
    // the assertion here is about DIRECTION, and giving it an exotic TX mode
    // would only add a way for it to fail for an unrelated reason.
    WdspChannel::Config transmit = config;
    transmit.direction = WdspChannel::Direction::Transmit;
    transmit.mode = WdspChannel::Mode::Usb;
    transmit.filterLowHz = 300.0;
    transmit.filterHighHz = 2700.0;
    auto txChannel = WdspChannel::create(transmit, &error);
    ok = require(txChannel && !txChannel->setFmDeviation(kSignalDeviationHz),
                 "setFmDeviation() was accepted on a transmit channel, which "
                 "has no RXA detector to set") && ok;

    WdspChannel::Config invalidDeviation = config;
    invalidDeviation.fmDeviationHz = 0.0;
    ok = require(WdspChannel::create(invalidDeviation, &error) == nullptr,
                 "a Config carrying a zero FM deviation was accepted") && ok;
    // The same door, and the one the registry pushes through: open() sends the
    // Config value straight to SetRXAFMDeviation, so a sign check here is the
    // same hole in a second place.
    invalidDeviation.fmDeviationHz = 1.0e-40;
    ok = require(WdspChannel::create(invalidDeviation, &error) == nullptr,
                 "a Config carrying a tiny positive FM deviation was accepted "
                 "-- open() pushes it straight into SetRXAFMDeviation") && ok;

    // ...and the bounds are inclusive, so a caller that asks for exactly the
    // documented limit is not refused by an off-by-one.
    ok = require(channel->setFmDeviation(WdspChannel::Config::kMinFmDeviationHz) &&
                     channel->setFmDeviation(WdspChannel::Config::kMaxFmDeviationHz),
                 "the FM deviation range refuses its own endpoints") && ok;

    // The refusal is RX-only in validateConfig(), like the WBFM refusal beside
    // it: a transmit Config never reaches SetRXAFMDeviation, so an
    // out-of-range value on one is not this check's business. The setter still
    // refuses a TX channel outright, which the assertion below covers.
    WdspChannel::Config transmitDeviation = config;
    transmitDeviation.direction = WdspChannel::Direction::Transmit;
    transmitDeviation.mode = WdspChannel::Mode::Usb;
    transmitDeviation.filterLowHz = 300.0;
    transmitDeviation.filterHighHz = 2700.0;
    transmitDeviation.fmDeviationHz = 0.0;
    ok = require(WdspChannel::create(transmitDeviation, &error) != nullptr,
                 "a transmit Config was refused for an FM deviation that never "
                 "reaches a TXA stage") && ok;

    return ok;
}

bool runTransmitDiscardTest()
{
    WdspChannel::Config config = liveTransmitConfig(WdspChannel::Mode::Usb, 300.0, 2700.0);
    std::string error;
    auto channel = WdspChannel::create(config, &error);
    if (!require(channel != nullptr, "discard test opens TXA")) {
        return false;
    }
    const int id = channel->channelId();
    if (!require(channel->discardTransmitData() && !channel->isRunning(),
                 "discard leaves TXA stopped") ||
        !require(channel->discardTransmitData(), "discard is repeatable") ||
        !require(channel->setRunning(true), "discarded TXA restarts") ||
        !require(channel->channelId() == id, "discard retains channel allocation") ||
        !require(channel->setRunning(false), "clocked stop is accepted") ||
        !require(!channel->discardTransmitData(), "discard refuses a pending asynchronous flush") ||
        !require(channel->setRunning(true), "pending fade can be cancelled")) {
        return false;
    }
    config.direction = WdspChannel::Direction::Receive;
    auto receiver = WdspChannel::create(config, &error);
    return require(receiver && !receiver->discardTransmitData(),
                   "TX discard cannot modify a receive channel");
}

int main()
{
    const uint64_t allocationBaseline = WdspChannel::outstandingAllocationsForTest();
    WdspChannel::Config invalid;
    invalid.inputSampleRate = 44100;
    invalid.dspSampleRate = 48000;
    std::string validationError;
    // NOT SHORT-CIRCUITED, and that is the point. These cases are independent
    // -- each builds and tears down its own channels -- but chaining them with
    // `||` meant the FIRST failure silently skipped every case after it.
    //
    // That is not hypothetical. `runVector` diverged under CPU load (#5734,
    // since fixed by AetherSDR WDSP patch 13 and pinned by the worker-handoff
    // cases) and it runs THIRD, so while it was firing none of the five
    // start/stop cases below ran at all. Those
    // five are the regression pins for AetherSDR WDSP patches 7, 8 and 9; a
    // regression in any of them would have been invisible behind an unrelated
    // red, which is the exact failure a pin exists to prevent. Run everything,
    // report everything, fail once at the end.
    //
    // The ORDER still matters even without the short circuit, and the
    // close-after-stopped-clocking case is still LAST for the reason it always
    // was: its failure mode is a signal or a timeout rather than a message, so
    // everything that can still report has already reported when it runs. The
    // difference is that this is now actually true rather than true only when
    // nothing ahead of it failed.
    bool ok = true;
    const auto check = [&ok](bool passed) { ok = passed && ok; };

    check(require(WdspChannel::create(invalid, &validationError) == nullptr,
                  "invalid non-integral rate configuration was accepted"));
    check(runLeakChecked("lifecycle test", runLifecycleTest));
    check(runLeakChecked("RX vector", [] {
        return runVector(WdspChannel::Direction::Receive);
    }));
    check(runLeakChecked("TX vector", [] {
        return runVector(WdspChannel::Direction::Transmit);
    }));
    check(runLeakChecked("RX worker handoff", [] {
        return runWorkerHandoffTest(WdspChannel::Direction::Receive);
    }));
    check(runLeakChecked("TX worker handoff", [] {
        return runWorkerHandoffTest(WdspChannel::Direction::Transmit);
    }));
    check(runLeakChecked("TX discard", runTransmitDiscardTest));
    check(runLeakChecked("TX live geometry", runTransmitLiveGeometryTest));
    check(runLeakChecked("TX suppression sweep", runTransmitSuppressionSweepTest));
    check(runLeakChecked("TX zeros census", runTransmitZerosCensusTest));
    check(runLeakChecked("underrun test", runUnderrunTest));
    check(runLeakChecked("reconfiguration test", runReconfigurationTest));
    check(runLeakChecked("start/stop test", runStartStopTest));
    check(runLeakChecked("restart-during-ramp test", runRestartDuringRampTest));
    check(runLeakChecked("close setup-lock test", runCloseSetupLockTest));
    check(runLeakChecked("stopped-close test", runStoppedCloseTest));
    check(runLeakChecked("notch index test", runNotchIndexTest));
    check(runLeakChecked("notch attenuation test", runNotchAttenuationTest));
    check(runLeakChecked("minimum-phase workspace test",
                         runMinimumPhaseWorkspaceTest));
    check(runLeakChecked("filter-taps group-delay test",
                         runFilterTapsGroupDelayTest));
    check(runLeakChecked("notch survives tap change test",
                         runNotchSurvivesTapChangeTest));
    check(runLeakChecked("FM deviation test", runFmDeviationTest));
    // LAST, and deliberately so: see the ordering note above. Anything added
    // later belongs ABOVE this line, not below it.
    check(runLeakChecked("close-after-stopped-clocking test",
                         runCloseAfterStoppedClockingTest));
    check(require(WdspChannel::outstandingAllocationsForTest() == allocationBaseline,
                  "WDSP test suite left allocations outstanding"));

    if (!ok) {
        return 1;
    }

    std::cout << "WDSP channel tests passed\n";
    return 0;
}
