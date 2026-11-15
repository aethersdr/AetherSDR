// Real bundled libspecbleach, no radio, device, network, or model fixture.
#include "core/SpecbleachFilter.h"
#include <specbleach_denoiser.h>
#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <memory>
#include <vector>

namespace {
int g_failures{0};
void check(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++g_failures;
    }
}

QByteArray inputBlock(int rate, int start, bool noise)
{
    const int frames = rate / 100;
    QByteArray result(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(result.data());
    uint32_t state = static_cast<uint32_t>(start) + 17;
    for (int frame = 0; frame < frames; ++frame) {
        state = state * 1664525u + 1013904223u;
        const float value = noise
            ? 0.2f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f)
            : 0.2f * std::sin(2.0 * std::numbers::pi * 16000.0 * (start + frame) / rate);
        samples[2 * frame] = value;
        samples[2 * frame + 1] = value;
    }
    return result;
}

void checkRate(int rate)
{
    AetherSDR::SpecbleachFilter filter(rate);
    check(filter.isValid() && filter.sampleRate() == rate, "supported specbleach domain initializes");
    double inputPower = 0;
    double outputPower = 0;
    bool valid = filter.isValid();
    for (int start = 0; start < rate * 4; start += rate / 100) {
        const QByteArray input = inputBlock(rate, start, true);
        const QByteArray output = filter.process(input);
        if (output.size() != input.size()) {
            valid = false;
            break;
        }
        if (start < rate * 2) {
            continue;
        }
        const auto* in = reinterpret_cast<const float*>(input.constData());
        const auto* out = reinterpret_cast<const float*>(output.constData());
        for (int sample = 0; sample < input.size() / static_cast<int>(sizeof(float)); ++sample) {
            valid = valid && std::isfinite(out[sample]);
            inputPower += in[sample] * in[sample];
            outputPower += out[sample] * out[sample];
        }
    }
    const double gain = std::sqrt(outputPower / std::max(inputPower, 1e-20));
    std::printf("rate=%d stationary-noise amplitude ratio=%.6f\n", rate, gain);
    check(valid && gain > 0.01 && gain < 0.95, "real enabled algorithm suppresses noise with finite equal-length output");
}

void checkNativeLibraryContract()
{
    // Independently configure the published C API at48000. With duplicated
    // mono and zero reduction, its output must equal the wrapper after the
    // wrapper's existing 25-packet learning period.
    const std::unique_ptr<void, decltype(&specbleach_free)> reference(
        specbleach_initialize(48000, 40.0f), specbleach_free);
    SpectralBleachDenoiserParameters params{};
    params.adaptive_noise = 1;
    params.masking_depth = 0.5f;
    params.suppression_strength = 0.5f;
    if (!reference || !specbleach_load_parameters(reference.get(), params)) {
        check(false, "reference libspecbleach initializes");
        return;
    }
    AetherSDR::SpecbleachFilter wrapper(48000);
    wrapper.setReductionAmount(0);
    std::vector<float> mono(480);
    std::vector<float> output(480);
    double maxError = 0;
    for (int packet = 0; packet < 200; ++packet) {
        const QByteArray input = inputBlock(48000, packet * 480, true);
        const auto* stereo = reinterpret_cast<const float*>(input.constData());
        for (int frame = 0; frame < 480; ++frame) {
            mono[frame] = stereo[2 * frame];
        }
        specbleach_process(reference.get(), 480, mono.data(), output.data());
        const QByteArray wrapped = wrapper.process(input);
        const auto* actual = reinterpret_cast<const float*>(wrapped.constData());
        if (packet >= 25) {
            for (int frame = 0; frame < 480; ++frame) {
                maxError = std::max(maxError, std::abs(static_cast<double>(
                    actual[2 * frame] - output[frame])));
            }
        }
    }
    check(maxError < 1e-7, "wrapper output agrees with independently configured48000 C API");
}
}

int main()
{
    AetherSDR::SpecbleachFilter invalid(44100);
    check(!invalid.isValid(), "unsupported specbleach domain fails initialization");
    AetherSDR::SpecbleachFilter legacy;
    AetherSDR::SpecbleachFilter explicit24(24000);
    bool equal = true;
    for (int start = 0; start < 72000; start += 240) {
        const QByteArray input = inputBlock(24000, start, true);
        equal = equal && legacy.process(input) == explicit24.process(input);
    }
    check(equal, "default and explicit24 preserve identical output");
    checkRate(24000);
    checkRate(48000);
    checkNativeLibraryContract();
    AetherSDR::SpecbleachFilter native48(48000);
    native48.setReductionAmount(0);
    double power = 0;
    int frames = 0;
    for (int start = 0; start < 144000; start += 480) {
        const QByteArray result = native48.process(inputBlock(48000, start, false));
        if (start < 96000) {
            continue;
        }
        const auto* values = reinterpret_cast<const float*>(result.constData());
        for (int frame = 0; frame < result.size() / (2 * static_cast<int>(sizeof(float))); ++frame) {
            power += values[2 * frame] * values[2 * frame];
            ++frames;
        }
    }
    check(frames > 0 && std::abs(std::sqrt(power / frames) - 0.141421356) < 0.003,
          "native48 preserves16k at zero reduction");
    return g_failures == 0 ? 0 : 1;
}
