// RTL-SDR WDSP receive audio is mono, L == R, through the production
// RtlReceivePipeline, offline (#7563).
//
// WDSP's create_rxa() leaves the output panel at copy 0 (L = I, R = Q): a 90
// degree pair in SSB/CW. RtlReceiveRecipe::applyDsp opens the sideband
// channels with binaural off, like Thetis, piHPSDR and the HL2 (#7528).
// AM/SAM keep copy 0: WdspChannel's AmReceive recipe already outputs the mean
// of L and R on both channels, calibrated against the Hilbert pair copy 0
// gives. One eight-receiver bank: USB, LSB, CW, CWR, AM+ANR, AM+ANF, SAM+ANR,
// SAM+ANF. Per receiver: the signal reaches L, R equals L sample for sample,
// and the mean (L+R)/2 equals L.

#include "core/backends/rtl/RtlDcBlocker.h"
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlReceiveRecipe.h"

#include <QCoreApplication>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using T = AetherSDR::rtl::RtlCaptureTransaction;
using Recipe = AetherSDR::rtl::RtlReceiveRecipe;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += ok ? 0 : 1;
}

constexpr int kRate = 240000;
constexpr int kChunk = 1200;
constexpr double kCenter = 100000000;
constexpr double kTau = 2 * std::numbers::pi;
constexpr int kReceivers = 8;

bool ready(Pipeline& pipeline)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = pipeline.service();
        if (status != Pipeline::Preparation::Pending) {
            return status == Pipeline::Preparation::Ready;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

struct Rx {
    T::Mode mode;
    bool nr;
    bool anf;
    const char* name;
    double toneHz;
};
constexpr std::array<Rx, kReceivers> kBank{{
    {T::Mode::Usb, false, false, "USB", 1100},      {T::Mode::Lsb, false, false, "LSB", 1300},
    {T::Mode::Cw, false, false, "CW", 600},         {T::Mode::Cwr, false, false, "CWR", 600},
    {T::Mode::Am, true, false, "AM+ANR", 700},      {T::Mode::Am, false, true, "AM+ANF", 900},
    {T::Mode::Sam, true, false, "SAM+ANR", 1500},   {T::Mode::Sam, false, true, "SAM+ANF", 1700},
}};
constexpr std::array<double, kReceivers> kOffsets{
    -84000., -60000., -36000., -12000., 12000., 36000., 60000., 84000.};

T::State bank()
{
    T::State result;
    result.token = {1, 1};
    result.capture = {1, 1, kCenter, kRate, kRate * .45, kRate * .45};
    result.hardware.centerHz = std::uint32_t(kCenter);
    result.hardware.sampleRateHz = kRate;
    for (int id = 0; id < kReceivers; ++id) {
        const auto [low, high] = Recipe::defaultFilter(kBank[id].mode);
        T::Receiver receiver{{id, kCenter + kOffsets[id], double(low), double(high), 0, 3000, 3000},
                             kBank[id].mode};
        receiver.agcMode = 0;
        receiver.nrEnabled = kBank[id].nr;
        receiver.nrLevel = 50;
        receiver.anfEnabled = kBank[id].anf;
        receiver.anfLevel = 50;
        result.receivers.push_back(receiver);
        result.receivingIds.push_back(id);
    }
    return result;
}

std::complex<double> am(double t, double carrier, double audio, double amplitude)
{
    return std::polar(amplitude * (1 + .5 * std::cos(kTau * audio * t)), kTau * carrier * t);
}

std::complex<double> signal(double t)
{
    std::complex<double> sum = std::polar(.015, kTau * (kOffsets[0] + kBank[0].toneHz) * t)
        + std::polar(.015, kTau * (kOffsets[1] - kBank[1].toneHz) * t)
        + std::polar(.015, kTau * kOffsets[2] * t) + std::polar(.015, kTau * kOffsets[3] * t);
    for (int id = 4; id < kReceivers; ++id) {
        sum += am(t, kOffsets[id], kBank[id].toneHz, .015);
    }
    return sum;
}

struct Channel {
    std::vector<float> left;
    std::vector<float> right;
    double maxDiff = 0;
};

std::array<Channel, kReceivers> render(double seconds)
{
    std::array<Channel, kReceivers> out;
    const auto pipelineOwner = std::make_unique<Pipeline>(kReceivers);
    Pipeline& pipeline = *pipelineOwner;
    if (!pipeline.prepare(bank(), true) || !ready(pipeline) || !pipeline.adopt()) {
        check(false, "the eight-receiver WDSP bank prepares and adopts");
        return out;
    }
    std::array<std::complex<float>, kChunk> iq{};
    AetherSDR::rtl::RtlDcBlocker dc;
    dc.configure(true, kRate);
    const std::uint64_t end = std::uint64_t(seconds * kRate);
    for (std::uint64_t position = 0; position < end; position += kChunk) {
        for (std::size_t index = 0; index < iq.size(); ++index) {
            const auto value = signal(double(position + index) / kRate);
            iq[index] = {float(value.real()), float(value.imag())};
        }
        dc.process(iq);
        if (!pipeline.process(position, iq)) {
            check(false, "production acquisition accepts the generated capture");
            break;
        }
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.slot < 0 || packet.slot >= kReceivers) {
                continue;
            }
            auto& channel = out[packet.slot];
            for (std::size_t index = 0; index < packet.frames; ++index) {
                // Past startup, AGC and carrier settling.
                if (packet.firstSample + index < 48000) {
                    continue;
                }
                const float l = packet.samples[2 * index];
                const float r = packet.samples[2 * index + 1];
                channel.left.push_back(l);
                channel.right.push_back(r);
                channel.maxDiff = std::max(channel.maxDiff, double(std::abs(l - r)));
            }
        }
        // Pace the real asynchronous WDSP worker; a burst tests its ring.
        std::this_thread::sleep_for(6ms);
    }
    pipeline.stop();
    return out;
}

double magnitude(std::span<const float> pcm, double hz)
{
    if (pcm.size() < 2) {
        return 0;
    }
    std::complex<double> sum{};
    double weight = 0;
    for (std::size_t index = 0; index < pcm.size(); ++index) {
        const double window = .5 - .5 * std::cos(kTau * index / (pcm.size() - 1));
        sum += double(pcm[index]) * window * std::polar(1.0, -kTau * hz * index / 48000);
        weight += window;
    }
    return 2 * std::abs(sum) / weight;
}

double rms(std::span<const float> pcm)
{
    double power = 0;
    for (float v : pcm) {
        power += double(v) * v;
    }
    return pcm.empty() ? 0 : std::sqrt(power / pcm.size());
}

double meanRms(const Channel& c)
{
    double power = 0;
    for (std::size_t i = 0; i < c.left.size() && i < c.right.size(); ++i) {
        const double m = 0.5 * (double(c.left[i]) + c.right[i]);
        power += m * m;
    }
    return c.left.empty() ? 0 : std::sqrt(power / c.left.size());
}

double db(double ratio) { return 20 * std::log10(std::max(ratio, 1e-12)); }

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    for (T::Mode mode : {T::Mode::Usb, T::Mode::Lsb, T::Mode::Cw, T::Mode::Cwr, T::Mode::Am,
                         T::Mode::Sam}) {
        T::Receiver receiver{{0, kCenter, 0, 0, 0, 3000, 3000}, mode};
        const auto [low, high] = Recipe::defaultFilter(mode);
        receiver.passband.filterLowHz = low;
        receiver.passband.filterHighHz = high;
        WdspChannel::Config dsp;
        Recipe::applyDsp(receiver, dsp);
        const bool want = !Recipe::amplitude(mode);
        if (dsp.rxPanelMono != want) {
            std::printf("  recipe mode %d: rxPanelMono %d, expected %d\n", int(mode),
                        int(dsp.rxPanelMono), int(want));
        }
        check(dsp.rxPanelMono == want,
              "SSB/CW recipes open the panel mono; AM/SAM keep AmReceive's own L/R mean");
    }

    const auto result = render(3);
    for (int id = 0; id < kReceivers; ++id) {
        const Channel& c = result[id];
        const double l = rms(c.left);
        const double r = rms(c.right);
        const double tone = magnitude(c.left, kBank[id].toneHz);
        std::printf("%-8s tone %.5f  L %.5f  R-L %+8.3f dB  mean-L %+8.3f dB  max|L-R| %.2g  frames %zu\n",
                    kBank[id].name, tone, l, db(r / std::max(l, 1e-12)),
                    db(meanRms(c) / std::max(l, 1e-12)), c.maxDiff, c.left.size());
        // ANF removes a coherent tone by design, so an ANF row only needs audio.
        const bool reaches = kBank[id].anf ? l > 1e-3 : tone > .004;
        check(c.left.size() > 48000 && reaches, "the signal reaches L");
        check(c.maxDiff <= 1e-5 * std::max(l, 1e-6), "R equals L sample for sample");
        check(std::abs(db(meanRms(c) / std::max(l, 1e-12))) < 0.01,
              "the mean (L+R)/2 equals L within 0.01 dB");
    }

    QThreadPool::globalInstance()->waitForDone();
    std::printf("rtl_rx_panel_mono_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
