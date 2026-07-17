#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <cmath>
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
        !require(WdspChannel::outstandingAllocationsForTest() == allocationBaseline,
                 "WDSP test suite left allocations outstanding")) {
        return 1;
    }

    std::cout << "WDSP channel tests passed\n";
    return 0;
}
