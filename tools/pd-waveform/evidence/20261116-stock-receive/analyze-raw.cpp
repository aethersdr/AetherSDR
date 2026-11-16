#include "core/txlinearity/ImdAnalyzer.h"
#include "core/txlinearity/TxSpectrumAnalyzer.h"
#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv)
{
    using namespace AetherSDR::txlin;
    constexpr int size = 16384;
    constexpr int frames = 4;
    if (argc != 2 || std::endian::native != std::endian::little) { return 2; }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != size * frames * 2 * sizeof(float)) { return 3; }
    input.seekg(0);
    TxSpectrumAnalyzer analyzer(size, WindowKind::BlackmanHarris7);
    std::vector<float> raw(size * 2);
    std::vector<std::complex<float>> iq(size);
    for (int f = 0; f < frames; ++f) {
        if (!input.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(float))) { return 4; }
        for (int i = 0; i < size; ++i) {
            if (!std::isfinite(raw[2*i]) || !std::isfinite(raw[2*i+1])) { return 5; }
            iq[i] = {raw[2*i], raw[2*i+1]};
        }
        if (!analyzer.accumulate(iq)) { return 6; }
    }
    const LinearityResult result = measureTwoTone(analyzer, 48000, {}, false, false);
    if (!result.valid) { std::fprintf(stderr, "%s\n", result.invalidReason.c_str()); return 7; }
    std::printf("{\"valid\":true,\"clipping_known\":false,\"absolute_power_calibrated\":false,\"frames\":4,\"sample_rate_hz\":48000,\"fft_size\":16384,\"window\":\"blackman-harris-7\",\"tones_hz\":[%.15g,%.15g],\"fundamental_pep_db_raw_iq\":%.15g,\"products\":[",
                result.tone1.freqHz, result.tone2.freqHz, result.pepDb);
    bool first = true;
    for (const ImdProduct& product : result.products) {
        std::printf("%s{\"order\":%d,\"upper\":%s,\"dbc_per_tone\":%.15g,\"dbc_fundamental_pep\":%.15g,\"noise_limited\":%s}", first ? "" : ",",
                    product.order, product.upper ? "true" : "false", product.dbcPerTone, product.dbcPep, product.limitedByNoiseFloor ? "true" : "false");
        first = false;
    }
    std::puts("]}");
    return 0;
}
