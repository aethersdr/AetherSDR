#include "core/backends/hl2/MetisProtocol.h"

#include <cmath>

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

Cc ccTxFreq(std::uint32_t hz) noexcept
{
    return {kC0TxFreq,
            static_cast<std::uint8_t>((hz >> 24) & 0xFF),
            static_cast<std::uint8_t>((hz >> 16) & 0xFF),
            static_cast<std::uint8_t>((hz >> 8) & 0xFF),
            static_cast<std::uint8_t>(hz & 0xFF)};
}

Cc ccTxDrive(int level, bool paEnable) noexcept
{
    if (level < 0) level = 0;
    if (level > kTxDriveMax) level = kTxDriveMax;
    // C1 = DATA[31:24] drive level. C2 = DATA[23:16]; bit 3 of it is DATA[19],
    // the onboard PA enable. ATU tune, Alex filters and VNA stay zero — those
    // are separate decisions and none of them belong in a drive-level write.
    const auto c2 = static_cast<std::uint8_t>(paEnable ? 0x08 : 0x00);
    return {kC0TxDrive, static_cast<std::uint8_t>(level), c2, 0x00, 0x00};
}

void ep2WriteTxIq(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                  std::span<const std::complex<float>> iq) noexcept
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    std::size_t consumed = 0;
    for (const std::size_t fs : frameStarts) {
        std::uint8_t* payload = pkt.data() + fs + 8;     // after SYNC(3) + C&C(5)
        for (std::size_t k = 0; k + kRxSampleBytes <= kFramePayload; k += kRxSampleBytes) {
            // payload[k+0..3] is the Hermes headphone-audio slot. On the first
            // sample of each frame it is EADDR (extended address, base 0x3f),
            // NOT audio. We never write it, so it stays zero from ep2Packet's
            // zero fill -- which is exactly what "not using the extended
            // address space" must look like on the wire.
            std::int16_t i = 0;
            std::int16_t q = 0;
            if (consumed < iq.size()) {
                const auto clamp = [](float v) -> std::int16_t {
                    // Symmetric clamp: 32767, not 32768. Letting a full-scale
                    // sample wrap to the negative rail is a click at best.
                    if (v >  1.0f) v =  1.0f;
                    if (v < -1.0f) v = -1.0f;
                    return static_cast<std::int16_t>(v * 32767.0f);
                };
                i = clamp(iq[consumed].real());
                q = clamp(iq[consumed].imag());
                ++consumed;
            }
            const auto ui = static_cast<std::uint16_t>(i);
            const auto uq = static_cast<std::uint16_t>(q);
            payload[k + 4] = static_cast<std::uint8_t>((ui >> 8) & 0xFF);   // I high
            payload[k + 5] = static_cast<std::uint8_t>(ui & 0xFF);          // I low
            payload[k + 6] = static_cast<std::uint8_t>((uq >> 8) & 0xFF);   // Q high
            payload[k + 7] = static_cast<std::uint8_t>(uq & 0xFF);          // Q low
        }
    }
}

std::optional<Ep6Response> parseEp6Response(const std::uint8_t* frame) noexcept
{
    if (frame[0] != kSync || frame[1] != kSync || frame[2] != kSync)
        return std::nullopt;
    const std::uint8_t c0 = frame[3];
    Ep6Response r;
    r.ack = (c0 & 0x80) != 0;
    if (r.ack) {
        r.raddr = (c0 >> 1) & 0x3F;      // full 6 bits when answering a RQST
    } else {
        r.raddr = (c0 >> 3) & 0x0F;      // classic free-running cycle
        r.dot   = (c0 & 0x04) != 0;      // CW key tip; C0[1] Dash is always 0 here
    }
    r.ptt  = (c0 & 0x01) != 0;
    r.data = readBe32(frame + 4);
    return r;
}

void Hl2Telemetry::apply(const Ep6Response& r) noexcept
{
    ptt = r.ptt;
    switch (r.raddr) {
    case 0x00:
        firmwareVersion = static_cast<int>(r.data & 0xFF);
        adcOverload     = (r.data & (1u << 24)) != 0;
        // ACTIVE LOW on the wire: the bit is SET when transmit is permitted.
        // Decoded here so nothing above this layer has to remember the inversion.
        txInhibited     = (r.data & (1u << 25)) == 0;
        // TX IQ FIFO depth. hpsdrsim writes a 15-bit count as C2[6:0]:C3[7:0],
        // i.e. DATA[22:8], and that is what this decodes because it is what we
        // can actually verify.
        //
        // THE ORACLE DISAGREES: §6 lists [14:8] as "FIFO count MSBs" and [15:14]
        // as an under/overflow code, which overlaps bit 14 and cannot both be
        // right. The gateware RTL is the authority and this has NOT been checked
        // against it. Do not build FIFO-servoed TX pacing on this field until it
        // has been — a pacing loop driven by a misread depth is exactly the kind
        // of unverified assumption that wedged a radio once already.
        txFifoCount     = static_cast<int>((r.data >> 8) & 0x7FFF);
        txFifoUnderflow = ((r.data >> 14) & 0x3) == 0x2;
        txFifoOverflow  = ((r.data >> 14) & 0x3) == 0x3;
        break;
    case 0x01:
        temperatureRaw  = static_cast<int>((r.data >> 16) & 0xFFFF);
        forwardPowerRaw = static_cast<int>(r.data & 0xFFFF);
        break;
    case 0x02:
        reversePowerRaw = static_cast<int>((r.data >> 16) & 0xFFFF);
        biasCurrentRaw  = static_cast<int>(r.data & 0xFFFF);
        break;
    default:
        break;                            // 0x03/0x04 carry nothing we consume
    }
}

std::optional<double> swrFromRaw(int forwardRaw, int reverseRaw) noexcept
{
    // No carrier, no SWR. Returning 1.0 here would render as a perfect match
    // when the truth is that the question is meaningless.
    if (forwardRaw <= 0)
        return std::nullopt;
    // The counts are VOLTAGE-proportional, so rho is a plain ratio and there is
    // no square root. Establishing that mattered: the power form would have
    // reported roughly the square root of the true reflection coefficient, i.e.
    // a flattering SWR that hides a real mismatch.
    //
    // Evidence: hpsdrsim derives its reading as j proportional to
    // sqrt(txlevel), and txlevel is a sum of i^2+q^2 — a power — so the reported
    // count is proportional to voltage. pihpsdr's own meter.c is inconsistent
    // (one branch uses the voltage form (Vf+Vr)/(Vf-Vr), another a sqrt form
    // whose arguments are the wrong way round and would return a NEGATIVE SWR),
    // so it is not usable as the tie-breaker.
    const double fwd = static_cast<double>(forwardRaw);
    double rev = static_cast<double>(reverseRaw < 0 ? 0 : reverseRaw);
    // Reverse above forward is physically impossible; it means noise on a tiny
    // reading. Clamp rather than emit a negative or infinite SWR.
    if (rev >= fwd)
        rev = fwd * 0.999;
    return (fwd + rev) / (fwd - rev);
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
