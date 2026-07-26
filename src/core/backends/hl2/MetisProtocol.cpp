#include "core/backends/hl2/MetisProtocol.h"

namespace AetherSDR::hl2 {

namespace {

// Decode a 24-bit signed big-endian sample (I/Q wire format) into int32.
inline std::int32_t decode24be(const std::uint8_t* p) noexcept
{
    std::int32_t v = (std::int32_t(p[0]) << 16) | (std::int32_t(p[1]) << 8) | std::int32_t(p[2]);
    if (v & 0x00800000)                                  // sign-extend 24 -> 32
        v |= static_cast<std::int32_t>(0xFF000000u);
    return v;
}

inline bool isEp6Header(std::span<const std::uint8_t> pkt) noexcept
{
    return pkt.size() >= kUsbPacketSize && pkt[0] == 0xEF && pkt[1] == 0xFE
        && pkt[2] == 0x01 && pkt[3] == 0x06;
}

inline std::uint32_t readBe32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
         | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

constexpr std::uint8_t kSync = 0x7F;

}  // namespace

int sampleRateHz(SampleRate rate) noexcept
{
    switch (rate) {
    case SampleRate::R48k:  return 48000;
    case SampleRate::R96k:  return 96000;
    case SampleRate::R192k: return 192000;
    case SampleRate::R384k: return 384000;
    }
    return 48000;
}

Cc ccConfig(SampleRate rate, int numRx) noexcept
{
    const auto c1 = static_cast<std::uint8_t>((static_cast<std::uint8_t>(rate) & 0x03) | kConfigMercury);
    if (numRx < 1) numRx = 1;
    if (numRx > 8) numRx = 8;
    const auto c4 = static_cast<std::uint8_t>(kConfigDuplex | (((numRx - 1) & 0x07) << 3));
    return {kC0Config, c1, 0x00, 0x00, c4};
}

Cc ccRx1Freq(std::uint32_t hz) noexcept
{
    return {kC0Rx1Freq,
            static_cast<std::uint8_t>((hz >> 24) & 0xFF),
            static_cast<std::uint8_t>((hz >> 16) & 0xFF),
            static_cast<std::uint8_t>((hz >> 8) & 0xFF),
            static_cast<std::uint8_t>(hz & 0xFF)};
}

Cc ccRxGain(int db) noexcept
{
    int code = db + 12;                                  // -12 dB -> 0, +48 dB -> 60
    if (code < 0) code = 0;
    if (code > 60) code = 60;
    return {kC0AdcGain, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(0x40 | code)};
}

Cc ccAdcAssign() noexcept
{
    // RX1..RX7 -> ADC0, TX attenuation 0. All-zero payload is the correct value
    // for a single-ADC Phase-1 receiver; what matters is that the bank is sent.
    return {kC0AdcAssignOrTxGain, 0x00, 0x00, 0x00, 0x00};
}

Cc ccPipelineReset() noexcept
{
    // DATA[7:4] = 0x8 -> C4 = 0x80. Everything else stays zero, which is "no
    // action" for the other command nibbles in this register.
    return {kC0Sync, 0x00, 0x00, 0x00, 0x80};
}

std::array<std::uint8_t, 64> metisCommand(std::uint8_t cmd) noexcept
{
    std::array<std::uint8_t, 64> out{};                  // zero-filled pad
    out[0] = 0xEF; out[1] = 0xFE; out[2] = 0x04; out[3] = cmd;
    return out;
}

std::array<std::uint8_t, 63> discoveryRequest() noexcept
{
    std::array<std::uint8_t, 63> out{};
    out[0] = 0xEF; out[1] = 0xFE; out[2] = 0x02;
    return out;
}

std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> pkt) noexcept
{
    if (pkt.size() < 11 || pkt[0] != 0xEF || pkt[1] != 0xFE)
        return std::nullopt;
    DiscoveryReply r;
    r.streaming = (pkt[2] == 0x03);                      // 0x02 idle, 0x03 already sending
    for (std::size_t i = 0; i < 6; ++i)
        r.mac[i] = pkt[3 + i];
    r.gatewareVersion = pkt[9];
    r.boardId = pkt[10];
    // Byte 20 carries the board's receiver count on full-length replies. Short
    // replies omit it; leave 0 so callers fall back to a single receiver.
    if (pkt.size() > 20)
        r.numRx = pkt[20];
    return r;
}

std::array<std::uint8_t, kUsbPacketSize> ep2Packet(std::uint32_t seq, const Cc& a, const Cc& b) noexcept
{
    std::array<std::uint8_t, kUsbPacketSize> pkt{};      // zero-filled (TX payload is all zero)
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x02;
    pkt[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    // Two 512-byte frames: SYNC(3) + C&C(5) + 504 zero bytes.
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    const Cc* ccs[2] = {&a, &b};
    for (int f = 0; f < 2; ++f) {
        std::uint8_t* fr = pkt.data() + frameStarts[f];
        fr[0] = kSync; fr[1] = kSync; fr[2] = kSync;
        for (std::size_t i = 0; i < 5; ++i)
            fr[3 + i] = (*ccs[f])[i];
    }
    return pkt;
}

std::optional<std::uint32_t> ep6Seq(std::span<const std::uint8_t> pkt) noexcept
{
    if (!isEp6Header(pkt))
        return std::nullopt;
    return readBe32(pkt.data() + 4);
}

int ep6Samples(std::span<const std::uint8_t> pkt, std::vector<std::complex<float>>& out) noexcept
{
    if (!isEp6Header(pkt))
        return -1;
    constexpr float kInvFullScale = 1.0f / static_cast<float>(kFullScale);
    int appended = 0;
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        const std::uint8_t* frame = pkt.data() + fs;
        if (frame[0] != kSync || frame[1] != kSync || frame[2] != kSync)
            continue;                                    // skip a corrupt frame, keep the good one
        const std::uint8_t* payload = frame + 8;         // after SYNC(3) + C&C(5)
        for (std::size_t k = 0; k + kRxSampleBytes <= kFramePayload; k += kRxSampleBytes) {
            const float i = static_cast<float>(decode24be(payload + k)) * kInvFullScale;
            const float q = static_cast<float>(decode24be(payload + k + 3)) * kInvFullScale;
            out.emplace_back(i, q);                       // payload[k+6..7] = mic, ignored
            ++appended;
        }
    }
    return appended;
}

}  // namespace AetherSDR::hl2
