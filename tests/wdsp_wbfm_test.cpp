#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <string>
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
    // RF amplitude changes must not change valid FM audio gain or stereo
    // orientation; these clean vectors are not a weak-noisy-station claim.
    const Measurement weak = run(*channel, Vector::Stereo, 5.0, 2.0, 0.0, 0.001);
    ok = require(weak.finite && channel->wbfmStereoDetected() == true
                 && std::abs(weak.left(0) / stereo.left(0) - 1.0) < 0.01
                 && std::abs(weak.right(1) / stereo.right(1) - 1.0) < 0.01,
                 "clean40dB lower RF amplitude retains FM level and stereo") && ok;
    const Measurement noise = run(*channel, Vector::Noise, 4.0, 2.0);
    std::cout << "No-signal noise peak=" << noise.peak << " pilot="
              << channel->wbfmStereoDetected().value_or(true) << '\n';
    ok = require(noise.finite && channel->wbfmStereoDetected() == false
                 && noise.clipped == 0 && noise.peak < 0.15,
                 "internal automatic squelch suppresses no-signal noise without a false pilot") && ok;
    ok = require(channel->setRunning(false) && channel->wbfmStereoDetected() == false,
                 "stop immediately invalidates previous stereo indication") && ok;
    if (!require(channel->setRunning(true), "WFM restarts")) { return false; }
    const Measurement resumed = run(*channel, Vector::Mono, 3.0, 2.0);
    ok = require(resumed.finite && channel->wbfmStereoDetected() == false
                 && resumed.peak < 0.95 && resumed.clipped == 0,
                 "restart on mono never retains old station's stereo flag") && ok;
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
    const bool filter = filterAndHeadroom();
    const bool response = deemphasis();
    const bool envelope = rfEnvelope();
    const bool stereo = stereoAndPilot();
    const bool conditions = clockPilotAndNoise();
    const bool validation = invalidConfiguration();
    const bool leaks = require(WdspChannel::outstandingAllocationsForTest() == allocations,
                               "WFM channel teardown frees tracked allocations");
    return filter && response && envelope && stereo && conditions && validation && leaks ? 0 : 1;
}
