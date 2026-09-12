#include "core/backends/anan/P2Protocol.h"

#include <algorithm>
#include <cstring>

namespace AetherSDR::anan {

namespace {

std::uint16_t readU16be(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t readU32be(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)  |  static_cast<std::uint32_t>(p[3]);
}

void writeU16be(std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

void writeU32be(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

// Sign-extend a 24-bit two's-complement value carried in the low 24 bits of
// a 32-bit int.
int signExtend24(std::int32_t raw) noexcept
{
    if (raw & 0x800000)
        raw -= (1 << 24);
    return raw;
}

}  // namespace

std::array<std::uint8_t, 60> buildDiscovery() noexcept
{
    std::array<std::uint8_t, 60> pkt{};
    pkt[4] = 0x02;  // Discovery Command (spec p.42-43)
    return pkt;
}

std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> data) noexcept
{
    // Bounds-check BEFORE indexing (Principle VII) -- this parses
    // unauthenticated UDP. 24 bytes covers the header through byte 23.
    if (data.size() < 24)
        return std::nullopt;
    if (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 0)
        return std::nullopt;
    if (data[4] != 0x02 && data[4] != 0x03)
        return std::nullopt;

    DiscoveryReply r;
    r.streaming = data[4] == 0x03;
    for (std::size_t i = 0; i < r.mac.size(); ++i)
        r.mac[i] = data[5 + i];
    r.boardId = data[11];
    r.protoVersionRaw = data[12];
    r.firmwareVer = data[13];
    r.numDdc = data[20];
    r.freqIsPhaseWord = data[21] != 0;
    r.endianByteRaw = data[22];
    r.p2appBuild = data[23];
    return r;
}

std::array<std::uint8_t, 60> buildGeneral() noexcept
{
    std::array<std::uint8_t, 60> pkt{};
    pkt[4] = 0x00;  // Command byte for the General Packet (spec p.19)
    // Bytes 5-22 (port table) left zero: "if set to zero the default port
    // will be used" (p.19-20). No renegotiation in Phase 1b.
    pkt[37] = 0x08; // bit[3]: DDC/DUC frequency as PHASE WORD (p.21) --
                     // matches Discovery byte 21 == 1, not a guess.
    pkt[38] = 0x01; // bit[0]: hardware reset timer / watchdog ON (p.8, p.21).
                     // See buildGeneral()'s header comment for why this stays
                     // on deliberately.
    pkt[39] = 0x00; // BE + 3-byte -- the only format Discovery byte22==0
                     // declares (p.22, p.45). See discoveryDeclaresBigEndian3Byte().
    return pkt;
}

std::array<std::uint8_t, 1444> buildDdcSpecific(std::span<const DdcConfig> ddcs, int numAdcs,
                                                bool ditherEnabled, bool randomEnabled) noexcept
{
    std::array<std::uint8_t, 1444> pkt{};  // full spec length (80 DDCs); unused DDCs
                                            // stay zero/disabled
    pkt[4] = static_cast<std::uint8_t>(numAdcs);  // "number of ADCs the hardware
                                                   // supports" (p.25)
    // Bytes 5/6: one Dither/Random control on this radio, not a per-ADC
    // pair, so both ADC0 and ADC1 bits track it together regardless of
    // which ADC any given DDC actually selects.
    pkt[5] = ditherEnabled ? 0x03 : 0x00;
    pkt[6] = randomEnabled ? 0x03 : 0x00;

    // Clamp rather than trust the caller: writing rows for more DDCs than
    // kMaxDdcs would keep landing inside this 1444-byte packet (the spec has
    // room for 80), so this is not a buffer guard -- it is a refusal to
    // enable hardware receivers the rest of this backend is not driving.
    const std::size_t count =
        std::min(ddcs.size(), static_cast<std::size_t>(kMaxDdcs));

    std::uint8_t enableBits = 0;
    for (std::size_t n = 0; n < count; ++n) {
        enableBits |= static_cast<std::uint8_t>(1u << n);
        // One 6-byte row per DDC: ADC select, 2-byte rate, CIC1, CIC2,
        // sample size. Offsets confirmed against the single-DDC layout this
        // replaced (DDC0: 17 / 18-19 / 20-21 / 22), which is bench-proven.
        const std::size_t row = 17 + 6 * n;
        pkt[row] = static_cast<std::uint8_t>(ddcs[n].adcIndex);
        writeU16be(&pkt[row + 1], static_cast<std::uint16_t>(ddcs[n].rateKsps));
        // row+3, row+4 = CIC1/CIC2, "For Future use" (p.25) -- left zero.
        pkt[row + 5] = 24;   // sample size, 24 bits (default, made explicit)
    }
    pkt[7] = enableBits;   // bit[n] enables DDC n
    return pkt;
}

std::array<std::uint8_t, 1444> buildDdcSpecific(int ddc0RateKsps, int numAdcs,
                                                bool ditherEnabled, bool randomEnabled,
                                                int ddc0AdcIndex) noexcept
{
    // Delegates so the row layout has exactly one implementation; the
    // single-DDC output stays byte-identical to what this function built
    // before the multi-DDC overload existed (pinned by the existing tests).
    const DdcConfig one{ddc0RateKsps, ddc0AdcIndex};
    return buildDdcSpecific(std::span<const DdcConfig>(&one, 1),
                            numAdcs, ditherEnabled, randomEnabled);
}

std::array<std::uint8_t, 1444> buildHighPriority(bool run,
                                                  std::span<const std::uint32_t> ddcFreqWords,
                                                  bool bypassAdc0Filters,
                                                  bool bypassAdc1Filters) noexcept
{
    std::array<std::uint8_t, 1444> pkt{};  // full spec length; attenuator fields stay zero
    pkt[4] = run ? 0x01 : 0x00;  // bit[0] = run. Bits[1..4] = PTT0..3 -- there is no
                                  // parameter to set them; see this header's own comment.
    // One 4-byte frequency/phase word per DDC at 9 + 4*n (p.32). Same
    // clamp-and-ignore contract as buildDdcSpecific().
    const std::size_t count =
        std::min(ddcFreqWords.size(), static_cast<std::size_t>(kMaxDdcs));
    for (std::size_t n = 0; n < count; ++n)
        writeU32be(&pkt[9 + 4 * n], ddcFreqWords[n]);
    // Alex0 register, bytes 1432-1435 -- ANT1 (bit 24) always, HF Bypass
    // (bit 12) when requested. See this function's declaration comment.
    std::uint32_t alex0 = std::uint32_t{1} << 24;
    if (bypassAdc0Filters)
        alex0 |= std::uint32_t{1} << 12;
    writeU32be(&pkt[1432], alex0);
    // Alex1 (RX2/BPF2) register, bytes 1430-1431 -- a separate 16-bit word,
    // bit 12 "HF Bypass 2" (p.90-91's own Alex1 bit table).
    if (bypassAdc1Filters)
        writeU16be(&pkt[1430], std::uint16_t{1} << 12);
    return pkt;
}

std::array<std::uint8_t, 1444> buildHighPriority(bool run, std::uint32_t ddc0FreqWord,
                                                  bool bypassAdc0Filters,
                                                  bool bypassAdc1Filters) noexcept
{
    // Delegates, same reasoning as buildDdcSpecific()'s single-DDC overload:
    // one implementation of the row layout, and byte-identical output to
    // what this built before (pinned by the existing tests).
    return buildHighPriority(run, std::span<const std::uint32_t>(&ddc0FreqWord, 1),
                             bypassAdc0Filters, bypassAdc1Filters);
}

std::optional<int> ddcIndexForSenderPort(std::uint16_t senderPort, int numDdc,
                                          std::uint16_t basePort) noexcept
{
    if (numDdc <= 0 || senderPort < basePort)
        return std::nullopt;
    // Widened to int before subtracting: both operands are uint16_t, and the
    // guard above already rules out the wrap case, but doing the arithmetic
    // in int keeps that correctness local rather than resting on the guard.
    const int index = static_cast<int>(senderPort) - static_cast<int>(basePort);
    if (index >= numDdc)
        return std::nullopt;
    return index;
}

std::optional<DdcFrame> parseDdcFrame(std::span<const std::uint8_t> data) noexcept
{
    // Bounds-check BEFORE indexing (Principle VII) -- the declared sample
    // count is used to REJECT a malformed/foreign datagram below, never to
    // index past what the buffer actually contains.
    if (data.size() < static_cast<std::size_t>(kDdcHeaderLen))
        return std::nullopt;

    const std::uint32_t seq = readU32be(&data[0]);
    // bytes 4-11 (timestamp) are unused in Phase 1b -- header field only, p.54
    const std::uint16_t bitsPerSample = readU16be(&data[12]);
    const std::uint16_t samplesDeclared = readU16be(&data[14]);

    // STRICT shape check, not a clamp -- see this function's declaration
    // comment in P2Protocol.h for why.
    const std::size_t expectedLen = static_cast<std::size_t>(kDdcHeaderLen)
        + static_cast<std::size_t>(samplesDeclared) * static_cast<std::size_t>(kDdcSampleBytes);
    if (bitsPerSample != 24 || data.size() != expectedLen)
        return std::nullopt;

    DdcFrame frame;
    frame.seq = seq;
    frame.samples = samplesDeclared;
    frame.iqRaw = data.subspan(static_cast<std::size_t>(kDdcHeaderLen),
                               data.size() - static_cast<std::size_t>(kDdcHeaderLen));
    return frame;
}

std::complex<float> decodeIqSample(const std::uint8_t* be6) noexcept
{
    const int iRaw = (static_cast<std::int32_t>(be6[0]) << 16)
                    | (static_cast<std::int32_t>(be6[1]) << 8)
                    |  static_cast<std::int32_t>(be6[2]);
    const int qRaw = (static_cast<std::int32_t>(be6[3]) << 16)
                    | (static_cast<std::int32_t>(be6[4]) << 8)
                    |  static_cast<std::int32_t>(be6[5]);
    const float scale = 1.0f / static_cast<float>(kFullScale24Bit);
    return {static_cast<float>(signExtend24(iRaw)) * scale,
            static_cast<float>(signExtend24(qRaw)) * scale};
}

void decodeIq(const DdcFrame& frame, std::vector<std::complex<float>>& out)
{
    out.reserve(out.size() + static_cast<std::size_t>(frame.samples));
    for (int i = 0; i < frame.samples; ++i)
        out.push_back(decodeIqSample(&frame.iqRaw[static_cast<std::size_t>(i) * kDdcSampleBytes]));
}

}  // namespace AetherSDR::anan
