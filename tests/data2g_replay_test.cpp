// Data2gMonitor against a recording it did not make (RFC #7159, Stage 1).
//
// The other Data2G tests transmit with the vendored core's Engine inside the
// test. This one replays tests/fixtures/data2g/session-0.4.3.wav: two
// upstream data2g-host 0.4.3 processes, N0CALL and W1AW, working each other
// through pipes — a KISS broadcast, then a connect, data both ways, a
// disconnect and both ID frames. See the README beside the recording.
//
// The expectations below are upstream's own data2g-monitor reading of the
// same file, so the adapter is held to the reference receiver, burst for
// burst. It is not an off-air recording: no radio, no ionosphere.

#include "core/hfdata/Data2gMonitor.h"

#include "audio/filters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

using AetherSDR::hfdata::Data2gBurst;
using AetherSDR::hfdata::Data2gMonitor;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

constexpr int kRecordedRate = 8000;
constexpr int kTapRate = 24000;

// 16-bit mono PCM at 8 kHz with the canonical 44-byte header, as samples in
// [-1, 1]. Empty on anything else.
std::vector<double> readRecording(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto u16 = [&bytes](std::size_t at) {
        return static_cast<unsigned>(static_cast<unsigned char>(bytes[at]))
            | static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 8;
    };
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVEfmt ", 8) != 0 || std::memcmp(bytes.data() + 36, "data", 4) != 0
        || u16(20) != 1 || u16(22) != 1 || u16(24) != kRecordedRate || u16(26) != 0 || u16(34) != 16)
        return {};
    std::vector<double> samples;
    samples.reserve((bytes.size() - 44) / 2);
    for (std::size_t at = 44; at + 1 < bytes.size(); at += 2)
        samples.push_back(static_cast<std::int16_t>(u16(at)) / 32767.0);
    return samples;
}

// What AetherModem's 24 kHz tap would hand over: the recording at `peakDbfs`,
// over white noise `snrDb` below the bursts in a 2.5 kHz bandwidth. A
// negative snrDb adds no noise.
std::vector<float> atTap(const std::vector<double>& recorded, double peakDbfs, double snrDb)
{
    double power = 0.0;
    double peak = 0.0;
    std::size_t keyed = 0;
    for (double v : recorded) {
        peak = std::max(peak, std::abs(v));
        if (v != 0.0) {
            power += v * v;
            ++keyed;
        }
    }
    std::vector<double> channel = recorded;
    if (snrDb >= 0.0 && keyed > 0) {
        const double sigma = std::sqrt(power / static_cast<double>(keyed) * (kRecordedRate / 2.0) / 2500.0
                                       / std::pow(10.0, snrDb / 10.0));
        std::mt19937 rng(7);
        std::normal_distribution<double> noise(0.0, sigma);
        for (double& v : channel)
            v += noise(rng);
    }
    data2g::audio::Interpolator toTap(kTapRate);
    const std::vector<double> at24k = toTap(channel);
    const double gain = peak > 0.0 ? std::pow(10.0, peakDbfs / 20.0) / peak : 1.0;
    std::vector<float> pcm(at24k.size());
    std::transform(at24k.begin(), at24k.end(), pcm.begin(), [gain](double v) { return static_cast<float>(v * gain); });
    return pcm;
}

std::vector<Data2gBurst> replay(const std::vector<float>& pcm)
{
    static constexpr std::size_t kChunks[] = {4800, 1, 479, 2401, 7, 1024, 257};
    Data2gMonitor monitor(kTapRate);
    std::vector<Data2gBurst> bursts;
    std::size_t offset = 0;
    std::size_t turn = 0;
    while (offset < pcm.size()) {
        const std::size_t n = std::min(kChunks[turn++ % std::size(kChunks)], pcm.size() - offset);
        for (Data2gBurst& burst : monitor.feed(std::span<const float>(pcm.data() + offset, n)))
            bursts.push_back(std::move(burst));
        offset += n;
    }
    check(monitor.faultCount() == 0, "the core raised no fault");
    return bursts;
}

struct Heard {
    double startSeconds;
    const char* mode;      // as the first line prints it, between the stamp and the SNR
    const char* says;      // the line that names the burst
    bool fragile;          // 256-QAM: found on a weak channel, but not read
};

// data2g-monitor 0.4.3 on the recording.
constexpr Heard kSession[] = {
    {0.5, "qpsk-r1/5 x2", "BCAST \"KISS 0\": 1 frame(s)", false},
    {4.5, "qpsk-r1/5 x1", "CONNECT N0CALL>W1AW v5 nonce 4438 cap 2 t_turn 1.0 s -> key 922b", false},
    {6.5, "qpsk-r1/5 x1", "CONNECT_ACK W1AW>N0CALL nonce 4438 cap 2", false},
    {8.5, "ack-4f x2", "ARQ N0CALL>W1AW key 922b  b0 acted 7 cum 0 K 0 rec w48-256l-r5/8 max", false},
    {10.5, "w48-256l-r5/8 x1", "ARQ W1AW>N0CALL key 922b  b0 acted 0 cum 0 K 0 rec w48-256l-r5/8 max", true},
    {11.6, "w48-256l-r5/8 x1", "ARQ N0CALL>W1AW key 922b  b1 acted 0 cum 1 K 0 rec w48-256l-r5/8 max", true},
    {12.9, "ack-1f x2", "ARQ W1AW>N0CALL key 922b  b1 acted 1 cum 1 K 0 rec w48-256l-r5/8 max", false},
    {14.0, "qpsk-r1/5 x1", "DISC N0CALL>W1AW key 922b", false},
    {16.0, "qpsk-r1/5 x1", "DISC_ACK W1AW>N0CALL key 922b", false},
    {17.6, "qpsk-r1/5 x1", "ID W1AW (session key 922b)", false},
    {20.4, "qpsk-r1/5 x1", "ID N0CALL (session key 922b)", false},
};

std::vector<std::uint8_t> bytesOf(const std::string& text)
{
    return {text.begin(), text.end()};
}

// The payload whose label starts with `label`, if every byte of it was heard.
const std::vector<std::uint8_t>* payload(const Data2gBurst& burst, const std::string& label)
{
    for (const auto& p : burst.payloads) {
        if (p.label.rfind(label, 0) == 0
            && std::none_of(p.unknown.begin(), p.unknown.end(), [](bool u) { return u; }))
            return &p.bytes;
    }
    return nullptr;
}

// Every burst of the session, in order, at the reference receiver's time and
// in its mode. A burst that was read carries the reference's description; one
// that was only found (`fragileRead` false) says so and claims no data.
void compare(const std::vector<Data2gBurst>& bursts, bool fragileRead, const std::string& when)
{
    check(bursts.size() == std::size(kSession),
          when + ": " + std::to_string(std::size(kSession)) + " bursts heard, got " + std::to_string(bursts.size()));
    for (std::size_t i = 0; i < std::min(bursts.size(), std::size(kSession)); ++i) {
        const Data2gBurst& burst = bursts[i];
        const Heard& heard = kSession[i];
        const std::string which = when + ": burst " + std::to_string(i + 1) + " (" + heard.says + ")";
        const std::string firstLine = burst.head.substr(0, burst.head.find('\n'));
        check(firstLine.find(std::string("  ") + heard.mode + "  SNR ") != std::string::npos, which + " mode");
        check(std::abs(burst.startSeconds - heard.startSeconds) < 0.1, which + " start time");
        const bool described = burst.head.find(std::string("\n  ") + heard.says + "\n") != std::string::npos;
        if (fragileRead || !heard.fragile) {
            check(described, which + " description");
        } else {
            check(!described && burst.head.find("\n  no decode") != std::string::npos, which + " reported as not decoded");
            check(payload(burst, "W1AW>N0CALL stream") == nullptr && payload(burst, "N0CALL>W1AW stream") == nullptr,
                  which + " claims no data");
        }
    }
}

// The whole session at a comfortable level: every burst, in order, with the
// reference receiver's wording, timing and payload bytes.
void clean(const std::vector<double>& recorded)
{
    const std::vector<Data2gBurst> bursts = replay(atTap(recorded, -18.0, -1.0));
    compare(bursts, true, "clean");
    if (bursts.size() == std::size(kSession)) {
        const auto* frame = payload(bursts[0], "frame 1 (AX.25 N0CALL>APRS, UI)");
        const std::vector<std::uint8_t> ui = {0x82, 0xA0, 0xA4, 0xA6, 0x40, 0x40, 0x60, 0x9C, 0x60, 0x86, 0x82, 0x98,
                                              0x98, 0x61, 0x03, 0xF0, '!',  'b',  'e',  'a',  'c',  'o',  'n'};
        check(frame && *frame == ui, "clean: the broadcast AX.25 frame, byte for byte");
        const auto* toCaller = payload(bursts[4], "W1AW>N0CALL stream");
        check(toCaller && *toCaller == bytesOf("Hello N0CALL, copied.\r"), "clean: the answering station's data");
        const auto* toAnswerer = payload(bursts[5], "N0CALL>W1AW stream");
        check(toAnswerer && *toAnswerer == bytesOf("Hello W1AW, this is the AetherSDR replay fixture.\r"),
              "clean: the calling station's data");
    }
    if (g_failures) {
        for (const Data2gBurst& burst : bursts)
            std::fputs(burst.text.c_str(), stderr);
    }
}

// The same session 32 dB down and 8 dB over the noise: every burst is still
// found, the robust frames (connect, acks, disconnect, IDs, the broadcast) are
// still read, and the two 256-QAM data bursts are reported as not decoded
// rather than as data.
void weak(const std::vector<double>& recorded)
{
    const int before = g_failures;
    const std::vector<Data2gBurst> bursts = replay(atTap(recorded, -50.0, 8.0));
    compare(bursts, false, "weak");
    if (g_failures != before) {
        for (const Data2gBurst& burst : bursts)
            std::fputs(burst.text.c_str(), stderr);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: data2g_replay_test RECORDING.wav\n");
        return 2;
    }
    Data2gMonitor::setDecodeThreads(1);
    const std::vector<double> recorded = readRecording(argv[1]);
    check(recorded.size() > 20u * kRecordedRate, "the recording is 8 kHz 16-bit mono and over 20 s long");
    if (recorded.empty())
        return 1;

    clean(recorded);
    weak(recorded);
    std::printf("%s\n", g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
