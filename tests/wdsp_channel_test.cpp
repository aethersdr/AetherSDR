#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
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
    // NOT SHORT-CIRCUITED, and that is the point. These cases are independent
    // -- each builds and tears down its own channels -- but chaining them with
    // `||` meant the FIRST failure silently skipped every case after it.
    //
    // That is not hypothetical. `runVector` diverges under CPU load (#5734,
    // pre-existing and unrelated to anything here) and it runs THIRD, so while
    // it was firing none of the five start/stop cases below ran at all. Those
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
    check(runLeakChecked("underrun test", runUnderrunTest));
    check(runLeakChecked("reconfiguration test", runReconfigurationTest));
    check(runLeakChecked("start/stop test", runStartStopTest));
    check(runLeakChecked("restart-during-ramp test", runRestartDuringRampTest));
    check(runLeakChecked("close setup-lock test", runCloseSetupLockTest));
    check(runLeakChecked("stopped-close test", runStoppedCloseTest));
    check(runLeakChecked("notch index test", runNotchIndexTest));
    check(runLeakChecked("notch attenuation test", runNotchAttenuationTest));
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
