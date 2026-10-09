#include "core/dsp/WdspChannel.h"
#include "../third_party/wdsp/include/aether_wdsp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kInputRate = 384000;
constexpr int kOutputRate = 48000;
constexpr std::size_t kInputBlock = 2048;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

bool require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; }
    return condition;
}

struct Measurement {
    std::array<std::complex<double>, 3> leftTones{};
    std::array<std::complex<double>, 3> rightTones{};
    std::array<double, 3> frequencies{1000.0, 2000.0, 15000.0};
    double peak = 0.0;
    double differenceEnergy = 0.0;
    std::size_t frames = 0;
    std::size_t clipped = 0;
    bool finite = true;
    void add(float left, float right, double time)
    {
        finite = finite && std::isfinite(left) && std::isfinite(right);
        peak = std::max({peak, std::abs(static_cast<double>(left)), std::abs(static_cast<double>(right))});
        clipped += std::abs(left) >= 1.0f || std::abs(right) >= 1.0f;
        differenceEnergy += (left - right) * (left - right);
        for (std::size_t tone = 0; tone < frequencies.size(); ++tone) {
            const std::complex<double> oscillator = std::polar(1.0, -kTwoPi * frequencies[tone] * time);
            leftTones[tone] += static_cast<double>(left) * oscillator;
            rightTones[tone] += static_cast<double>(right) * oscillator;
        }
        ++frames;
    }
    double left(std::size_t tone) const { return 2.0 * std::abs(leftTones[tone]) / frames; }
    double right(std::size_t tone) const { return 2.0 * std::abs(rightTones[tone]) / frames; }
};

WdspChannel::Config config(double low = -100000.0, double high = 100000.0)
{
    WdspChannel::Config value;
    value.inputSampleRate = kInputRate;
    value.dspSampleRate = 192000;
    value.outputSampleRate = kOutputRate;
    value.inputBlockSize = kInputBlock;
    value.dspBlockSize = 1024;
    value.mode = WdspChannel::Mode::Wbfm;
    value.wbfmReceive = WdspChannel::WbfmReceive{};
    value.filterLowHz = low;
    value.filterHighHz = high;
    value.agcMode = 0;
    value.agcFixedGainDb = 0;
    value.blockForOutput = true;
    return value;
}

enum class Vector { Mono, Adjacent, Stereo, StereoLowAudio, StereoHighAudio, StereoHighModulation, Bandwidth, Carrier, Noise };

Measurement run(WdspChannel& channel, Vector vector, double duration = 4.0,
                double measurementStart = 2.0, double carrierHz = 0.0, double amplitude = 0.1,
                double clockRatio = 1.0, double pilotHz = 19000.0, double noiseAmplitude = 0.0)
{
    std::vector<float> inputI(kInputBlock), inputQ(kInputBlock);
    std::vector<float> left(channel.outputBlockSize()), right(channel.outputBlockSize());
    Measurement result;
    if (vector == Vector::StereoLowAudio) { result.frequencies = {300.0, 500.0, 1000.0}; }
    if (vector == Vector::StereoHighAudio) { result.frequencies = {14000.0, 15000.0, 1000.0}; }
    for (double& frequency : result.frequencies) { frequency *= clockRatio; }
    const double physicalPilotHz = pilotHz * clockRatio;
    const double subcarrierHz = 2.0 * physicalPilotHz;
    const std::uint64_t allocations = WdspChannel::allocationSequenceForTest();
    const std::size_t blocks = static_cast<std::size_t>(duration * kInputRate / kInputBlock);
    for (std::size_t block = 0; block < blocks; ++block) {
        for (std::size_t index = 0; index < kInputBlock; ++index) {
            const double time = static_cast<double>(block * kInputBlock + index) / kInputRate;
            // Integrate the continuous FM waveform analytically. Rectangular
            // phase accumulation at the DSP rate cancels the discriminator's
            // sinc response and can falsely claim better stereo separation.
            const auto sineIntegral = [time](double hz) {
                return -std::cos(kTwoPi * hz * time) / hz;
            };
            const auto stereoIntegral = [time, subcarrierHz](double hz) {
                return 0.5 * (std::sin(kTwoPi * (subcarrierHz - hz) * time) / (subcarrierHz - hz)
                    - std::sin(kTwoPi * (subcarrierHz + hz) * time) / (subcarrierHz + hz));
            };
            double integrated = vector == Vector::Carrier ? 0.0 : 0.9 * sineIntegral(1000.0 * clockRatio);
            if (vector == Vector::Stereo || vector == Vector::StereoLowAudio || vector == Vector::StereoHighAudio
                || vector == Vector::StereoHighModulation) {
                // ITU-R BS.450-4 section 2.2.2.5: sin(theta) pilot and
                // sin(2*theta) suppressed subcarrier, not cosine quadrature.
                const double program = vector == Vector::StereoHighModulation ? 0.45 : 0.225;
                integrated = program * (sineIntegral(result.frequencies[0]) + sineIntegral(result.frequencies[1])
                    + stereoIntegral(result.frequencies[0]) - stereoIntegral(result.frequencies[1]))
                    + 0.1 * sineIntegral(physicalPilotHz);
            } else if (vector == Vector::Bandwidth) {
                integrated = 0.08 * (sineIntegral(1000.0 * clockRatio) + sineIntegral(15000.0 * clockRatio));
            }
            const double phase = 75000.0 * clockRatio * integrated;
            std::complex<double> sample = amplitude * std::polar(1.0, phase);
            if (vector == Vector::Adjacent) {
                // Desired low-deviation FM at DC, stronger adjacent FM at
                // +60 kHz. Narrowing the RF filter must select the desired
                // 1 kHz audio despite FM's capture effect.
                const double desiredPhase = 5.0 * std::sin(kTwoPi * 1000.0 * time);
                const double adjacentPhase = kTwoPi * 60000.0 * time
                    + 2.5 * std::sin(kTwoPi * 2000.0 * time);
                sample = 0.1 * std::polar(1.0, desiredPhase)
                    + 0.3 * std::polar(1.0, adjacentPhase);
            }
            if (vector == Vector::Noise || noiseAmplitude > 0.0) {
                // Deterministic independent I/Q hash noise: no transport, seed
                // state, or platform-dependent random distribution involved.
                const auto noise = [](std::uint32_t word) {
                    word ^= word >> 16; word *= 0x7feb352dU;
                    word ^= word >> 15; word *= 0x846ca68bU;
                    word ^= word >> 16;
                    return static_cast<double>(word) / 2147483648.0 - 1.0;
                };
                const std::uint32_t sampleIndex = static_cast<std::uint32_t>(block * kInputBlock + index);
                const std::complex<double> disturbance(noise(2 * sampleIndex), noise(2 * sampleIndex + 1));
                if (vector == Vector::Noise) { sample = amplitude * disturbance; }
                else { sample += noiseAmplitude * disturbance; }
            }
            sample *= std::polar(1.0, kTwoPi * carrierHz * time);
            inputI[index] = static_cast<float>(sample.real());
            inputQ[index] = static_cast<float>(sample.imag());
        }
        if (channel.processIq(inputI, inputQ, left, right) != WdspChannel::ProcessResult::Ok) {
            result.finite = false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            const double time = static_cast<double>(block * left.size() + index) / kOutputRate;
            if (time >= measurementStart) { result.add(left[index], right[index], time); }
        }
    }
    result.finite = result.finite && WdspChannel::allocationSequenceForTest() == allocations;
    return result;
}

bool filterAndHeadroom()
{
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    const Measurement wide = run(*channel, Vector::Adjacent);
    if (!require(channel->setFilter(-30000.0, 30000.0), "narrow WFM RF filter accepted")) { return false; }
    const Measurement narrow = run(*channel, Vector::Adjacent);
    const double selectedDb = 20.0 * std::log10(std::max(narrow.left(0), 1.0e-15)
        / std::max(narrow.left(1), 1.0e-15));
    std::cout << "RF wide: desired=" << wide.left(0) << " adjacent=" << wide.left(1)
              << " narrow: desired=" << narrow.left(0) << " adjacent=" << narrow.left(1)
              << " selection_db=" << selectedDb << '\n';
    bool ok = require(wide.finite && narrow.finite, "RF outputs finite and successful");
    ok = require(wide.left(1) > 5 * wide.left(0), "wide filter admits adjacent capture") && ok;
    ok = require(selectedDb >= 40.0, "selected WFM RF filter rejects stronger adjacent channel >=40 dB") && ok;
    channel.reset();
    channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    const Measurement level = run(*channel, Vector::Mono);
    std::cout << "Mono normalized peak=" << level.peak << " clipped_frames=" << level.clipped << '\n';
    ok = require(level.finite && level.peak > 0.1 && level.peak < 0.95 && level.clipped == 0,
                 "90 percent WFM modulation retains headroom without clipping") && ok;
    return ok;
}

bool deemphasis()
{
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    const Measurement us75 = run(*channel, Vector::Bandwidth);
    if (!require(channel->setWbfmDeemphasis(WdspChannel::WbfmReceive::Deemphasis::Us50),
                 "50 us control accepted")) { return false; }
    const Measurement us50 = run(*channel, Vector::Bandwidth);
    const double ratio = us50.left(2) / us75.left(2);
    const double bandwidthRatio = us75.left(2) / us75.left(0);
    std::cout << "15k amplitude: 75us=" << us75.left(2) << " 50us=" << us50.left(2)
              << " ratio=" << ratio << " bandwidth_15k_to_1k=" << bandwidthRatio << '\n';
    // Independent analog response ratio at15 kHz: sqrt((1+(2*pi*f*75us)^2)
    // /(1+(2*pi*f*50us)^2)) =1.482; includes tolerance for bilinear warping.
    bool ok = require(us75.finite && us50.finite && ratio > 1.43 && ratio < 1.54,
                      "50us changes actual 15 kHz response from 75us");
    // Nominal 75 us analog deemphasis gives 0.155 at15k relative to1k.
    // The broadcast audio FIR may add <=1 dB at15k; a24k producer cannot pass.
    ok = require(bandwidthRatio > 0.138 && bandwidthRatio < 0.170,
                 "broadcast audio retains15k bandwidth with expected deemphasis") && ok;
    // Changing one channel must not alter another owner's regional response.
    std::unique_ptr<WdspChannel> other = WdspChannel::create(config(), &error);
    if (!require(other != nullptr, error.c_str())) { return false; }
    const Measurement isolated75 = run(*other, Vector::Bandwidth);
    ok = require(std::abs(isolated75.left(2) / us75.left(2) - 1.0) < 0.01,
                 "deemphasis remains per-channel") && ok;
    return ok;
}
// The ADC meter observes post-prefilter, post384->192k decimator IQ before
// FM removes amplitude. This measures RF insertion/rejection rather than
// mistaking the FM capture effect for a filter response measurement.
bool rfEnvelope()
{
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(-30000, 30000), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    // The ADC meter averages power over100ms. Two seconds clears previous
    // in-band energy below the65dB stopband threshold before readback.
    const auto power = [&](double hz) {
        const Measurement result = run(*channel, Vector::Carrier, 2.0, 1.8, hz);
        if (!result.finite) { return std::numeric_limits<double>::quiet_NaN(); }
        return channel->meter(WdspChannel::Meter::AdcAverage);
    };
    const double reference = power(0);
    bool ok = true;
    for (const double hz : {-33000.0, -27000.0, 27000.0, 33000.0}) {
        const double relative = power(hz) - reference;
        std::cout << "RF 60k filter tone=" << hz << " relative_db=" << relative << '\n';
        ok = require(std::isfinite(relative) && (std::abs(hz) < 30000
                ? std::abs(relative) < 0.25 : relative < -65.0),
            "RF filter preserves passband and rejects stopband across3k guard") && ok;
    }
    if (!require(channel->setFilter(-20000, 40000), "asymmetric RF filter accepted")) { return false; }
    const double positive = power(30000) - reference;
    const double negative = power(-30000) - reference;
    std::cout << "RF asymmetric: +30k=" << positive << " -30k=" << negative << '\n';
    ok = require(std::abs(positive) < 0.25 && negative < -65.0,
                 "asymmetric RF filter follows mathematical IQ frequency sign") && ok;
    if (!require(channel->setFilter(-100000, 100000), "broadcast RF filter accepted")) { return false; }
    for (const double hz : {75000.0, 85000.0, 90000.0, 93000.0, 96000.0, 100000.0, 103000.0}) {
        const double relative = power(hz) - reference;
        std::cout << "RF 200k requested tone=" << hz << " relative_db=" << relative << '\n';
        ok = require(std::isfinite(relative), "overall RF response remains finite") && ok;
        if (hz <= 85000.0) {
            ok = require(std::abs(relative) < 0.5,
                         "broadcast RF path retains wanted spectrum through85k") && ok;
        }
    }
    return ok;
}

bool stereoAndPilot()
{
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    bool ok = require(channel->outputBlockSize() == 256,
                      "2048 complex384k input frames produce256 stereo48k frames");
    ok = require(channel->wbfmStereoDetected() == false, "fresh channel has no stereo observation") && ok;
    const Measurement stereo = run(*channel, Vector::Stereo, 6.0);
    const double leftSeparation = 20.0 * std::log10(stereo.left(0) / stereo.right(0));
    const double rightSeparation = 20.0 * std::log10(stereo.right(1) / stereo.left(1));
    std::cout << "Stereo separation L=" << leftSeparation << " R=" << rightSeparation
              << " peak=" << stereo.peak << '\n';
    ok = require(stereo.finite && leftSeparation >= 40 && rightSeparation >= 40,
                 "reference stereo separates correctly oriented L/R >=40dB") && ok;
    ok = require(channel->wbfmStereoDetected() == true, "real pilot acquires stereo indication") && ok;
    const auto acquired = channel->wbfmReceptionDiagnostics();
    ok = require(acquired && acquired->valid && acquired->pilotLocked
        && acquired->pilotMagnitude > acquired->pilotEngageThreshold
        && acquired->pilotEngageThreshold == 0.06 && acquired->pilotReleaseThreshold == 0.03
        && acquired->engageBlocks == 93 && acquired->releaseBlocks == 187
        && acquired->consecutiveHighBlocks >= acquired->engageBlocks
        && acquired->consecutiveLowBlocks == 0
        && acquired->lockLossCount == 0 && acquired->reacquisitionCount == 0
        && acquired->observationDurationMs >= 5900 && acquired->lockDurationMs >= 5000
        && acquired->stableDurationMs == 5000,
        "completed decoder snapshot reports real pilot thresholds, hysteresis and sample-clock duration") && ok;
    ok = require(stereo.peak < 0.95 && stereo.clipped == 0,
                 "stereo reference preserves unclipped headroom") && ok;
    for (const Vector vector : {Vector::StereoLowAudio, Vector::StereoHighAudio, Vector::StereoHighModulation}) {
        const Measurement stress = run(*channel, vector, 4.0);
        const double lDb = 20.0 * std::log10(stress.left(0) / stress.right(0));
        const double rDb = 20.0 * std::log10(stress.right(1) / stress.left(1));
        std::cout << (vector == Vector::StereoLowAudio ? "300/500Hz stereo"
            : vector == Vector::StereoHighAudio ? "14/15k stereo" : "97.5 percent stereo")
                  << " separation L=" << lDb << " R=" << rDb << " peak=" << stress.peak << '\n';
        // RFC reference qualification is >=40 dB. Near-full modulation is
        // a headroom/stability stress, with its separation reported explicitly:
        // 192 kHz pre-discriminator IQ conversion loses sidebands near96k.
        if (vector != Vector::StereoHighModulation) {
            ok = require(lDb >= 40 && rDb >= 40,
                         "low and wide-audio reference stereo separates >=40dB") && ok;
        }
        ok = require(stress.finite && stress.clipped == 0 && stress.peak < 0.95,
                     "low/wide-audio/high-modulation stereo remains finite and unclipped") && ok;
    }
    const Measurement lost = run(*channel, Vector::Mono, 3.0, 2.0);
    ok = require(channel->wbfmStereoDetected() == false && lost.finite
                 && std::sqrt(lost.differenceEnergy / lost.frames) < 1.0e-6,
                 "pilot loss returns truthful mono with identical L/R") && ok;
    const auto loss = channel->wbfmReceptionDiagnostics();
    ok = require(loss && loss->valid && !loss->pilotLocked && loss->lockDurationMs == 0
        && loss->lockLossCount == 1 && loss->reacquisitionCount == 0
        && acquired && loss->observationSequence != acquired->observationSequence
        && loss->pilotMagnitude < loss->pilotReleaseThreshold
        && loss->consecutiveLowBlocks >= loss->releaseBlocks,
        "pilot loss is counted in DSP even between UI snapshots") && ok;
    // RF amplitude changes must not change valid FM audio gain or stereo
    // orientation; these clean vectors are not a weak-noisy-station claim.
    const Measurement weak = run(*channel, Vector::Stereo, 5.0, 2.0, 0.0, 0.001);
    ok = require(weak.finite && channel->wbfmStereoDetected() == true
                 && std::abs(weak.left(0) / stereo.left(0) - 1.0) < 0.01
                 && std::abs(weak.right(1) / stereo.right(1) - 1.0) < 0.01,
                 "clean40dB lower RF amplitude retains FM level and stereo") && ok;
    const auto reacquired = channel->wbfmReceptionDiagnostics();
    ok = require(reacquired && reacquired->pilotLocked && reacquired->lockLossCount == 1
        && reacquired->reacquisitionCount == 1,
        "reacquisition excludes first acquisition and retains measured loss") && ok;
    const Measurement noise = run(*channel, Vector::Noise, 4.0, 2.0);
    std::cout << "No-signal noise peak=" << noise.peak << " pilot="
              << channel->wbfmStereoDetected().value_or(true) << '\n';
    ok = require(noise.finite && channel->wbfmStereoDetected() == false
                 && noise.clipped == 0 && noise.peak < 0.15,
                 "internal automatic squelch suppresses no-signal noise without a false pilot") && ok;
    ok = require(channel->setRunning(false) && channel->wbfmStereoDetected() == false,
                 "stop immediately invalidates previous stereo indication") && ok;
    ok = require(!channel->wbfmReceptionDiagnostics(), "stop invalidates completed-block diagnostics") && ok;
    // A stop only arms WDSP's down-slew. An immediate restart cancels it,
    // retaining decoder history because no flush occurred. Clock its 480-frame
    // fade and 256-frame zero tail through three blocking 256-frame exchanges;
    // setRunning(true) then waits for the completed fade's real flush worker.
    std::array<float, kInputBlock> drainInput{};
    std::vector<float> drainLeft(channel->outputBlockSize()), drainRight(channel->outputBlockSize());
    for (int block = 0; block < 3; ++block) {
        if (!require(channel->processIq(drainInput, drainInput, drainLeft, drainRight)
                == WdspChannel::ProcessResult::Ok,
                "stop clocks the real down-slew before reception-history flush")) { return false; }
        ok = require(!channel->wbfmReceptionDiagnostics(),
                     "clocking the stopped channel does not republish reception") && ok;
    }
    if (!require(channel->setRunning(true), "WFM restarts")) { return false; }
    const Measurement resumed = run(*channel, Vector::Mono, 3.0, 2.0);
    ok = require(resumed.finite && channel->wbfmStereoDetected() == false
                 && resumed.peak < 0.95 && resumed.clipped == 0,
                 "restart on mono never retains old station's stereo flag") && ok;
    const auto fresh = channel->wbfmReceptionDiagnostics();
    ok = require(fresh && !fresh->pilotLocked && fresh->lockLossCount == 0
        && fresh->reacquisitionCount == 0 && fresh->observationDurationMs < 3100,
        "flush starts new reception counters and sample-clock history") && ok;
    auto forced = config(); forced.wbfmReceive->forceMono = true;
    if (!require(channel->reconfigure(forced, &error), error.c_str())) { return false; }
    const Measurement mono = run(*channel, Vector::Stereo, 4.0);
    const auto monoReception = channel->wbfmReceptionDiagnostics();
    ok = require(mono.finite && mono.peak > 0.001 && mono.differenceEnergy == 0.0
        && channel->wbfmStereoDetected() == false && monoReception && monoReception->pilotLocked,
        "Force Mono produces identical nonzero L/R while truthfully observing the actual pilot") && ok;
    if (!require(channel->reconfigure(config(), &error), error.c_str())) { return false; }
    const Measurement automatic = run(*channel, Vector::Stereo, 4.0);
    ok = require(automatic.finite && automatic.left(0) > 100 * automatic.right(0)
        && automatic.right(1) > 100 * automatic.left(1),
        "return to Auto Stereo restores the decoder matrix") && ok;
    return ok;
}

bool clockPilotAndNoise()
{
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    struct Condition {
        const char* name;
        Vector vector;
        double clockRatio;
        double pilotHz;
        double amplitude;
        double noiseAmplitude;
        std::optional<bool> expectedPilot;
    };
    const std::array<Condition, 7> conditions{{
        {"clock -100ppm", Vector::Stereo, 0.9999, 19000.0, 0.1, 0.0, true},
        {"clock +100ppm", Vector::Stereo, 1.0001, 19000.0, 0.1, 0.0, true},
        {"coherent pilot -2Hz", Vector::Stereo, 1.0, 18998.0, 0.1, 0.0, true},
        {"coherent pilot +2Hz", Vector::Stereo, 1.0, 19002.0, 0.1, 0.0, true},
        {"noisy weak FM", Vector::Stereo, 1.0, 19000.0, 0.001, 0.0001, true},
        {"marginal noisy FM", Vector::Stereo, 1.0, 19000.0, 0.001, 0.0005, std::nullopt},
        {"marginal pilot absent", Vector::Mono, 1.0, 19000.0, 0.001, 0.0005, false}
    }};
    bool ok = true;
    for (const Condition& condition : conditions) {
        const Measurement measured = run(*channel, condition.vector, 5.0, 2.0, 0.0,
            condition.amplitude, condition.clockRatio, condition.pilotHz, condition.noiseAmplitude);
        const bool observed = channel->wbfmStereoDetected().value_or(false);
        std::cout << condition.name << ": pilot=" << observed << " peak=" << measured.peak
                  << " separation L=" << 20.0 * std::log10(std::max(measured.left(0), 1.0e-15)
                      / std::max(measured.right(0), 1.0e-15))
                  << " R=" << 20.0 * std::log10(std::max(measured.right(1), 1.0e-15)
                      / std::max(measured.left(1), 1.0e-15)) << '\n';
        ok = require(measured.finite && measured.clipped == 0 && measured.peak < 0.95,
                     "clock/pilot/marginal conditions remain finite and unclipped") && ok;
        if (condition.expectedPilot) {
            ok = require(observed == *condition.expectedPilot,
                         "bounded pilot offsets acquire and absent pilot is never reported stereo") && ok;
        }
        if (condition.expectedPilot == false) {
            ok = require(std::sqrt(measured.differenceEnergy / measured.frames) < 1.0e-6,
                         "marginal pilot loss produces paired mono") && ok;
        }
    }
    return ok;
}

// These barriers are test-thread observations, never acquisition policy. A
// deadline only detects a stuck worker; accepted exchange count is independent
// of CPU timing. Holds are released before any return can destroy the channel.
template<class Predicate>
bool awaitWorker(Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct WorkerHold {
    explicit WorkerHold(bool beforeCopy = false) : copy(beforeCopy)
    {
        if (copy) { WdspChannel::setWorkerOutputCopyHoldForTest(true); }
        else { WdspChannel::setWorkerHandoffHoldForTest(true); }
    }
    ~WorkerHold() { release(); }
    void release()
    {
        if (copy) { WdspChannel::setWorkerOutputCopyHoldForTest(false); }
        else { WdspChannel::setWorkerHandoffHoldForTest(false); }
    }
    bool entered() const
    {
        return copy ? WdspChannel::workerOutputCopyHeldForTest()
                    : WdspChannel::workerHandoffHeldForTest();
    }
    bool copy;
};

struct ExchangeVector {
    static constexpr std::size_t kBlocks = 480;
    static constexpr std::size_t kAudioBlock = 256;
    std::vector<float> i = std::vector<float>(kBlocks * kInputBlock);
    std::vector<float> q = std::vector<float>(kBlocks * kInputBlock);
    ExchangeVector()
    {
        for (std::size_t frame = 0; frame < i.size(); ++frame) {
            const double time = static_cast<double>(frame) / kInputRate;
            const auto sineIntegral = [time](double hz) {
                return -std::cos(kTwoPi * hz * time) / hz;
            };
            const auto stereoIntegral = [time](double hz) {
                return 0.5 * (std::sin(kTwoPi * (38000.0 - hz) * time) / (38000.0 - hz)
                    - std::sin(kTwoPi * (38000.0 + hz) * time) / (38000.0 + hz));
            };
            // Non-harmonic-to-block L/R tones distinguish temporal order as
            // well as channel order. Analytically integrated continuous FM.
            const double phase = 75000.0 * (0.225 * (sineIntegral(731.0)
                + sineIntegral(1279.0) + stereoIntegral(731.0) - stereoIntegral(1279.0))
                + 0.1 * sineIntegral(19000.0));
            i[frame] = static_cast<float>(0.1 * std::cos(phase));
            q[frame] = static_cast<float>(0.1 * std::sin(phase));
        }
    }
    WdspChannel::ProcessResult process(WdspChannel& channel, std::size_t block,
                                       std::span<float> left, std::span<float> right) const
    {
        return channel.processIq(std::span(i).subspan(block * kInputBlock, kInputBlock),
            std::span(q).subspan(block * kInputBlock, kInputBlock), left, right);
    }
};

bool exchangeOrder()
{
    const ExchangeVector signal;
    constexpr std::size_t audioBlock = ExchangeVector::kAudioBlock;
    constexpr std::size_t extraBlocks = 6;
    constexpr std::size_t heldBlock = 384; // >2 seconds of real pilot acquisition.
    constexpr int readySamples = 7 * audioBlock;
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    bool ok = require(channel->outputSamplesReadyForTest() == audioBlock,
                      "blocking WFM retains depth2 startup geometry");
    std::vector<float> referenceLeft(signal.kBlocks * audioBlock);
    std::vector<float> referenceRight(referenceLeft.size());
    for (std::size_t block = 0; block < signal.kBlocks; ++block) {
        if (!require(signal.process(*channel, block,
                std::span(referenceLeft).subspan(block * audioBlock, audioBlock),
                std::span(referenceRight).subspan(block * audioBlock, audioBlock))
                == WdspChannel::ProcessResult::Ok, "blocking ordered-audio reference succeeds")) {
            return false;
        }
    }
    WdspChannel::Config native = config();
    native.blockForOutput = false;
    if (!require(channel->reconfigure(native, &error), error.c_str())) { return false; }
    std::array<float, audioBlock> left{}, right{};
    // Reuse the same owner/slot twice; each preparation must restore the
    // exact prefix, and all wraps and queued input after release are compared.
    for (int preparation = 0; preparation < 2; ++preparation) {
        if (preparation != 0 && !require(channel->reconfigure(native, &error), error.c_str())) {
            return false;
        }
        if (!require(channel->outputSamplesReadyForTest() == readySamples,
                     "nonblocking WFM prepares seven credits on every rebuild")) { return false; }
        double maximumError = 0.0;
        bool finite = true;
        double burstEnergy = 0.0;
        double stereoEnergy = 0.0;
        const std::uint64_t allocations = WdspChannel::allocationSequenceForTest();
        for (std::size_t block = 0; block < signal.kBlocks; ++block) {
            static_assert(heldBlock + 8 < ExchangeVector::kBlocks);
            std::optional<WorkerHold> hold;
            if (block == heldBlock) {
                // Ready count alone precedes the previous handoff's hook. Do
                // not let the new hold accidentally claim that previous block.
                channel->synchronizeWorkerForTest();
                hold.emplace();
            }
            const std::size_t count = hold ? 8 : 1;
            for (std::size_t part = 0; part < count; ++part) {
                const std::size_t current = block + part;
                if (!require(signal.process(*channel, current, left, right)
                        == WdspChannel::ProcessResult::Ok,
                        "prepared nonblocking WFM accepts ordered bounded burst")) { return false; }
                if (block == heldBlock && part == 0
                    && !require(awaitWorker([] { return WdspChannel::workerHandoffHeldForTest(); }),
                                "ordered burst starts after acknowledged worker handoff")) { return false; }
                finite = finite && std::ranges::all_of(left, [](float sample) { return std::isfinite(sample); })
                    && std::ranges::all_of(right, [](float sample) { return std::isfinite(sample); });
                if (current >= extraBlocks) {
                    for (std::size_t frame = 0; frame < audioBlock; ++frame) {
                        const std::size_t expected = (current - extraBlocks) * audioBlock + frame;
                        maximumError = std::max({maximumError,
                            std::abs(static_cast<double>(left[frame]) - referenceLeft[expected]),
                            std::abs(static_cast<double>(right[frame]) - referenceRight[expected])});
                        if (block == heldBlock) {
                            burstEnergy += left[frame] * left[frame] + right[frame] * right[frame];
                            stereoEnergy += (left[frame] - right[frame]) * (left[frame] - right[frame]);
                        }
                    }
                }
            }
            if (hold) { hold->release(); }
            if (!require(awaitWorker([&] { return channel->outputSamplesReadyForTest() == readySamples; }),
                         "worker publishes all completed burst output without extra input")) { return false; }
            block += count - 1;
        }
        std::cout << "WFM_EXCHANGE_ORDER preparation=" << preparation << " extra_frames="
                  << extraBlocks * audioBlock << " max_error=" << maximumError
                  << " burst_energy=" << burstEnergy << " stereo_energy=" << stereoEnergy << '\n';
        ok = require(finite && maximumError < 2.0e-5 && burstEnergy > 1.0 && stereoEnergy > 0.5,
                     "nonzero paired output matches blocking reference with exactly1536 added frames") && ok;
        ok = require(WdspChannel::allocationSequenceForTest() == allocations,
                     "prepared bursts allocate no WDSP memory") && ok;
    }
    return ok;
}

// A held copy must not grant credit for bytes it has not written. This also
// tests the cold zero prefix after a genuine clocked stop/flush. A faulted
// channel is never fed again: release, then reprepare off the acquisition path.
bool unpublishedOutput(WdspChannel& channel, const ExchangeVector& signal)
{
    constexpr int audioBlock = ExchangeVector::kAudioBlock;
    std::array<float, audioBlock> left{}, right{};
    channel.synchronizeWorkerForTest();
    WorkerHold hold(true);
    bool ok = true;
    for (std::size_t block = 0; block < 7; ++block) {
        if (!require(signal.process(channel, block, left, right) == WdspChannel::ProcessResult::Ok,
                     "seven seeded output blocks remain available while next copy is held")) { return false; }
        if (block == 0) {
            if (!require(awaitWorker([&] { return hold.entered(); }),
                         "worker acknowledged before actual output memcpy")) { return false; }
            ok = require(channel.outputSamplesReadyForTest() == 6 * audioBlock,
                         "unpublished output never grants a premature credit") && ok;
        }
        ok = require(std::ranges::all_of(left, [](float sample) { return sample == 0.0f; })
                     && std::ranges::all_of(right, [](float sample) { return sample == 0.0f; }),
                     "prepared or flushed prefix contains actual paired silence") && ok;
    }
    ok = require(signal.process(channel, 7, left, right) == WdspChannel::ProcessResult::Underrun,
                 "eighth exchange underruns while output memcpy is unpublished") && ok;
    return ok;
}

bool exchangePreparation()
{
    const ExchangeVector signal;
    WdspChannel::Config native = config(); native.blockForOutput = false;
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(native, &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    bool ok = true;
    const std::uint64_t allocations = WdspChannel::allocationSequenceForTest();
    // These calls reach the WDSP linked into this executable. Under
    // AETHER_SHARED_CORE that is the test's own copy, not the core's: the
    // refusals are still proved, but the "existing ring intact" check below
    // then observes a copy the calls never touched. The static-core build has
    // one copy, and there the check is real.
    for (const int depth : {-1, 0, 1, 9, std::numeric_limits<int>::max()}) {
        ok = require(OpenChannelWithExchangeDepth(channel->channelId(), 2048, 1024,
                         384000, 192000, 48000, 0, 0, 0.01, 0.025, 0.0, 0.01, 0, depth) == 0,
                     "invalid prepared depth refuses before changing a live channel") && ok;
    }
    for (const int id : {-1, 32}) {
        ok = require(OpenChannelWithExchangeDepth(id, 2048, 1024,
                         384000, 192000, 48000, 0, 0, 0.01, 0.025, 0.0, 0.01, 0, 8) == 0,
                     "prepared creation rejects invalid channel identity") && ok;
    }
    ok = require(WdspChannel::allocationSequenceForTest() == allocations
                 && channel->outputSamplesReadyForTest() == 7 * 256,
                 "refused preparation leaves existing ring and allocations intact") && ok;
    ok = unpublishedOutput(*channel, signal) && ok;
    for (int round = 0; round < 2; ++round) {
        if (!require(channel->reconfigure(native, &error), error.c_str())) { return false; }
        std::array<float, ExchangeVector::kAudioBlock> left{}, right{};
        for (std::size_t block = 0; block < 64; ++block) {
            if (!require(signal.process(*channel, block, left, right) == WdspChannel::ProcessResult::Ok
                    && awaitWorker([&] { return channel->outputSamplesReadyForTest() == 7 * 256; }),
                         "flush fixture warms actual worker with bounded exchanges")) { return false; }
        }
        if (!require(channel->setRunning(false), "clocked flush stop accepted")) { return false; }
        // Config's 480-frame down-slew plus the WDSP 256-frame zero tail
        // completes within three 256-frame exchanges. Each handoff is awaited;
        // setRunning(true) then waits for that completed fade's flush worker.
        for (std::size_t block = 0; block < 3; ++block) {
            if (!require(signal.process(*channel, block, left, right) == WdspChannel::ProcessResult::Ok
                    && awaitWorker([&] { return channel->outputSamplesReadyForTest() == 7 * 256; }),
                         "stop clocks real fade and flush without starving worker")) { return false; }
        }
        if (!require(channel->setRunning(true), "flushed WFM restarts without rebuilding")) { return false; }
        ok = require(channel->outputSamplesReadyForTest() == 7 * 256,
                     "flush retains prepared depth8 credits") && ok;
        ok = unpublishedOutput(*channel, signal) && ok;
    }
    // The same slot must not retain its former depth when a legacy owner opens
    // it. Blocking WFM, legacy WBFM, ordinary receive and transmit stay at2.
    std::array<WdspChannel::Config, 4> legacy{config(), config(), WdspChannel::Config{}, WdspChannel::Config{}};
    legacy[1].wbfmReceive.reset(); legacy[1].blockForOutput = false;
    legacy[3].direction = WdspChannel::Direction::Transmit;
    for (const WdspChannel::Config& candidate : legacy) {
        if (!require(channel->reconfigure(candidate, &error), error.c_str())) { return false; }
        ok = require(channel->outputSamplesReadyForTest() == static_cast<int>(channel->outputBlockSize()),
                     "legacy and blocking owners reset reused slot to depth2") && ok;
    }
    return ok;
}

bool invalidConfiguration()
{
    using Deemphasis = WdspChannel::WbfmReceive::Deemphasis;
    std::string error;
    bool ok = true;
    const auto rejected = [&](const WdspChannel::Config& candidate) {
        return WdspChannel::create(candidate, &error) == nullptr;
    };
    WdspChannel::Config candidate = config();
    candidate.outputSampleRate = 24000;
    ok = require(rejected(candidate), "broadcast recipe refuses24k audio interim") && ok;
    candidate = config(); candidate.dspSampleRate = 384000;
    ok = require(rejected(candidate), "broadcast recipe refuses wrong DSP rate") && ok;
    candidate = config(); candidate.inputBlockSize = 1024;
    ok = require(rejected(candidate), "broadcast recipe refuses unqualified input geometry") && ok;
    candidate = config(); candidate.fmReceive = WdspChannel::FmReceive{};
    ok = require(rejected(candidate), "narrow and broadcast recipes are mutually exclusive") && ok;
    for (const double gain : {-1.0, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
        candidate = config(); candidate.wbfmReceive->outputGain = gain;
        ok = require(rejected(candidate), "broadcast output gain bounded and finite") && ok;
    }
    candidate = config(); candidate.wbfmReceive->deemphasis = static_cast<Deemphasis>(99);
    ok = require(rejected(candidate), "unknown deemphasis refused") && ok;
    candidate = config(); candidate.filterHighHz = 190000;
    ok = require(rejected(candidate), "RF filter includes input transition allowance") && ok;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config(), &error);
    if (!require(channel != nullptr, error.c_str())) { return false; }
    ok = require(!channel->setMode(WdspChannel::Mode::Fm)
                 && !channel->setWbfmDeemphasis(static_cast<Deemphasis>(99))
                 && !channel->setFilter(-190000, 100000),
                 "invalid runtime controls cannot mutate broadcast recipe") && ok;
    return ok;
}
} // namespace

int main()
{
    const std::uint64_t allocations = WdspChannel::outstandingAllocationsForTest();
    const bool order = exchangeOrder();
    const bool preparation = exchangePreparation();
    const bool filter = filterAndHeadroom();
    const bool response = deemphasis();
    const bool envelope = rfEnvelope();
    const bool stereo = stereoAndPilot();
    const bool conditions = clockPilotAndNoise();
    const bool validation = invalidConfiguration();
    const bool leaks = require(WdspChannel::outstandingAllocationsForTest() == allocations,
                               "WFM channel teardown frees tracked allocations");
    const bool ok = order && preparation && filter && response && envelope && stereo && conditions && validation && leaks;
    std::cout << "WDSP_WBFM_TEST completed success=" << ok << '\n';
    return ok ? 0 : 1;
}
