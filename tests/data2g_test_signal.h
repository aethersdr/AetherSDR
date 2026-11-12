#pragma once

// Test signal for the Data2G receive tests: the vendored core's own Engine
// transmits one broadcast frame, and the helpers hand back what a receiver
// would hear at AetherModem's 24 kHz tap. Needs the core's include root, so
// only tests that link aether_data2g_core may include this.

#include "arq/engine.hpp"
#include "audio/filters.hpp"
#include "config/config.hpp"
#include "kisslink/kisslink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace AetherSDR::test {

// An AX.25 UI frame W1AW>APRS carrying "!beacon", as a KISS client hands it in.
inline std::vector<std::uint8_t> uiFrame()
{
    std::vector<std::uint8_t> ui;
    const std::pair<std::string, bool> addresses[] = {{"APRS", false}, {"W1AW", true}};
    for (const auto& [call, last] : addresses) {
        for (std::size_t i = 0; i < 6; ++i)
            ui.push_back(static_cast<std::uint8_t>((i < call.size() ? call[i] : ' ') << 1));
        ui.push_back(static_cast<std::uint8_t>(0x60 | (last ? 1 : 0)));
    }
    ui.push_back(0x03);
    ui.push_back(0xF0);
    for (char c : std::string("!beacon"))
        ui.push_back(static_cast<std::uint8_t>(c));
    return ui;
}

// One station sends a broadcast frame into a quiet channel. Returns what a
// receiver hears at 24 kHz: the burst over noise, about 12 dB SNR in 2.5 kHz,
// scaled so the burst peaks near -18 dBFS.
inline std::vector<float> broadcastOnAir(const std::vector<std::uint8_t>& frame, bool& sent)
{
    namespace cfg = data2g::config;
    constexpr int kBlock = cfg::FS / 10;
    constexpr double kSnrDb = 12.0;
    constexpr double kSignal = 2.2;

    data2g::kisslink::KissLink kiss;
    data2g::arq::EngineConfig engineConfig;
    engineConfig.seed = 41;
    engineConfig.kiss = &kiss;
    data2g::arq::Engine station("W1AW", engineConfig);
    kiss.enqueue(frame);

    std::mt19937 rng(5);
    const double sigma = std::sqrt((cfg::FS / 2.0) / cfg::SNR_REF_BW_HZ / std::pow(10.0, kSnrDb / 10.0));
    std::normal_distribution<double> noise(0.0, sigma);

    std::vector<double> atFs;
    std::vector<double> heardByStation(kBlock);
    bool keyed = false;
    int quietBlocksAfter = 0;
    for (int block = 0; block < 60 * 10 && quietBlocksAfter < 30; ++block) {
        for (double& v : heardByStation)
            v = noise(rng);
        const data2g::arq::Engine::Out out = station.step(heardByStation);
        keyed = keyed || out.ptt;
        if (keyed && !out.ptt && kiss.queue.empty())
            ++quietBlocksAfter;
        for (int i = 0; i < kBlock; ++i) {
            const double tx = i < static_cast<int>(out.audio.size()) ? out.audio[i] : 0.0;
            atFs.push_back(kSignal * tx + noise(rng));
        }
    }
    sent = keyed && kiss.queue.empty();

    data2g::audio::Interpolator to24k(24000);
    const std::vector<double> at24k = to24k(atFs);

    // The burst peaks at kSignal before this; put that at -18 dBFS.
    const double gain = std::pow(10.0, -18.0 / 20.0) / kSignal;
    std::vector<float> pcm(at24k.size());
    std::transform(at24k.begin(), at24k.end(), pcm.begin(),
                   [gain](double v) { return static_cast<float>(v * gain); });
    return pcm;
}

} // namespace AetherSDR::test
