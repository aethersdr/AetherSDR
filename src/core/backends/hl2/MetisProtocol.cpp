#include "core/backends/hl2/MetisProtocol.h"
#include "core/backends/hl2/Hl2BandMemoryPolicy.h"

#include <algorithm>   // std::max, for the variance clamp in Ep4Stats::rmsDbfs
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

// The wideband bandscope's header, `EF FE 01 04`. Same shape as isEp6Header
// and the same length: the bandscope datagram is also 1032 bytes (usopenhpsdr1.v
// `START` sets `udp_tx_length_next = 'd1032` before entering `WIDE1`), it just
// spends all 1024 payload bytes on raw ADC codes instead of framed IQ rounds.
inline bool isEp4Header(std::span<const std::uint8_t> pkt) noexcept
{
    return pkt.size() >= kUsbPacketSize && pkt[0] == 0xEF && pkt[1] == 0xFE
        && pkt[2] == 0x01 && pkt[3] == 0x04;
}

// One 12-bit ADC code out of a little-endian 16-bit wire word holding it
// shifted left by four.
//
// Written as a logical shift of a NON-NEGATIVE value followed by an explicit
// 12-bit sign extension, rather than `int16_t(w) >> 4`. Both the narrowing
// conversion and the right shift of a negative integer were only fully pinned
// down in C++20; this form is portable to every standard and reads the same.
inline int decodeEp4Code(const std::uint8_t* p) noexcept
{
    const unsigned w = static_cast<unsigned>(p[0])
                     | (static_cast<unsigned>(p[1]) << 8);
    int code = static_cast<int>((w >> 4) & 0x0FFFu);
    if (code & 0x800)
        code -= 0x1000;                                  // sign-extend 12 -> 32
    return code;
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

std::uint8_t ocFilterByteForHz(double hz) noexcept
{
    const double mhz = hz / 1.0e6;
    // Ordered low to high; the first range that contains the frequency wins.
    // Boundaries sit in the gaps BETWEEN amateur bands, so every band lands
    // wholly inside one range — hl2_band_filter_test asserts that against
    // Quisk's table rather than trusting the arithmetic here.
    if (mhz < 1.6)   return kOcNone;                        // LW/MW: HPF would gut it
    if (mhz < 2.5)   return kOcLpf160;                      // 160 m — HPF out (spurs)
    if (mhz < 4.5)   return kOcHpfAmBc | kOcLpf80;          // 80 m
    if (mhz < 8.5)   return kOcHpfAmBc | kOcLpf60_40;       // 60 m, 40 m
    if (mhz < 16.5)  return kOcHpfAmBc | kOcLpf30_20;       // 30 m, 20 m
    if (mhz < 22.5)  return kOcHpfAmBc | kOcLpf17_15;       // 17 m, 15 m
    if (mhz <= 30.0) return kOcHpfAmBc | kOcLpf12_10;       // 12 m, 10 m
    return kOcNone;                                          // 6 m and up: no filter fitted
}

const char* ocFilterName(std::uint8_t oc) noexcept
{
    switch (static_cast<std::uint8_t>(oc & 0x7F)) {
    case kOcNone:                       return "none (bypass)";
    case kOcLpf160:                     return "160m LPF";
    case kOcHpfAmBc | kOcLpf80:         return "HPF + 80m LPF";
    case kOcHpfAmBc | kOcLpf60_40:      return "HPF + 60/40m LPF";
    case kOcHpfAmBc | kOcLpf30_20:      return "HPF + 30/20m LPF";
    case kOcHpfAmBc | kOcLpf17_15:      return "HPF + 17/15m LPF";
    case kOcHpfAmBc | kOcLpf12_10:      return "HPF + 12/10m LPF";
    default:                            return "custom";
    }
}

Cc ccConfig(SampleRate rate, int numRx, std::uint8_t ocFilterByte,
            bool dither, bool random) noexcept
{
    const auto c1 = static_cast<std::uint8_t>((static_cast<std::uint8_t>(rate) & 0x03) | kConfigMercury);
    if (numRx < 1) numRx = 1;
    if (numRx > kMaxReceivers) numRx = kMaxReceivers;
    // Open collector outputs are DATA[23:17] == C2[7:1]. The one-bit shift is
    // the whole reason this cannot be a straight assignment: DATA[16] is not
    // part of the field, and writing the byte unshifted would put the 160 m
    // relay's bit there and every real selection one filter too low.
    const auto c2 = static_cast<std::uint8_t>((ocFilterByte & 0x7F) << 1);
    // Receiver count is DATA[6:3], a FOUR-bit field (0000=1 .. 1011=12): mask 0x0F.
    const auto c4 = static_cast<std::uint8_t>(kConfigDuplex | (((numRx - 1) & 0x0F) << 3));
    // C3's dither bit is the band-voltage output on a bare HL2 and the speaker
    // switch on HL2+/SquareSDR 2 (see kConfigDither). It shares this register
    // with rate and receiver count, so every rebuild must carry it rather than
    // re-default it; MetisClient::Params holds it, as it holds ocFilterByte.
    const auto c3 = static_cast<std::uint8_t>((dither ? kConfigDither : 0u)
                                            | (random ? kConfigRandom : 0u));
    return {kC0Config, c1, c2, c3, c4};
}

Cc ccRxFreq(int rxIndex, std::uint32_t hz) noexcept
{
    // RX1 is register 0x02 and the receivers are contiguous from there: RX2..RX7
    // at 0x03..0x08. C0 is the address shifted left one, because C0 bit 0 is MOX.
    //
    // RX8..RX12 live at 0x12..0x16 and are NOT contiguous with this run — they
    // are deliberately not encoded here rather than being reached by arithmetic
    // that happens to be wrong past RX7. kMaxReceivers is 12 for the config
    // field; this encoder answers for the seven the shipping gateware can use.
    if (rxIndex < 0) rxIndex = 0;
    if (rxIndex > 6) rxIndex = 6;
    const auto c0 = static_cast<std::uint8_t>(kC0Rx1Freq + (rxIndex << 1));
    return {c0,
            static_cast<std::uint8_t>((hz >> 24) & 0xFF),
            static_cast<std::uint8_t>((hz >> 16) & 0xFF),
            static_cast<std::uint8_t>((hz >> 8) & 0xFF),
            static_cast<std::uint8_t>(hz & 0xFF)};
}

Cc ccRx1Freq(std::uint32_t hz) noexcept
{
    return ccRxFreq(0, hz);
}

Cc ccRxGain(int db) noexcept
{
    // Clamp before adding the bias, which would overflow for INT_MAX.
    // Bit 6 selects the native six-bit path in ad9866.v (gateware 883a338).
    const int code = clampDb(kLnaGainMinDb, db, kLnaGainMaxDb) - kLnaGainMinDb;
    return {kC0AdcGain, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(0x40 | code)};
}

Cc ccAdcAssign() noexcept
{
    // RX1..RX7 -> ADC0, TX attenuation 0. All-zero payload is the correct value
    // for a single-ADC Phase-1 receiver; what matters is that the bank is sent.
    return {kC0AdcAssignOrTxGain, 0x00, 0x00, 0x00, 0x00};
}

Cc ccRegister(int addr, std::uint32_t data) noexcept
{
    // Masked, not clamped: an out-of-range address is a caller bug, and the
    // callers that matter (Hl2ControlRequest::arm) refuse it outright before
    // reaching here. Masking is the belt to that brace — what it must never do
    // is let bit 6 of an address spill into the RQST flag at C0[7], or bit 0 of
    // the shifted byte become MOX.
    const auto c0 = static_cast<std::uint8_t>((addr & kMaxRegisterAddress) << 1);
    return {c0,
            static_cast<std::uint8_t>((data >> 24) & 0xFF),
            static_cast<std::uint8_t>((data >> 16) & 0xFF),
            static_cast<std::uint8_t>((data >> 8) & 0xFF),
            static_cast<std::uint8_t>(data & 0xFF)};
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

Cc ccTxDrive(int level, bool paEnable, bool atuTune) noexcept
{
    if (level < 0) level = 0;
    if (level > kTxDriveMax) level = kTxDriveMax;
    // C1 = DATA[31:24] drive level. C2 bit 3 = DATA[19] onboard PA enable, bit 4
    // = DATA[20] ATU tune request. Alex filters and VNA stay zero.
    // DATA[18] (tr_disable, control.v) must stay clear: with the PA off it holds
    // the internal T/R relay in receive via `pa_inttr = int_tx_on & ~vna &
    // (pa_enable | ~tr_disable)`. DATA[17] (exttuner.v bypass) stays clear too.
    const auto c2 = static_cast<std::uint8_t>((paEnable ? 0x08 : 0x00)
                                            | (atuTune ? 0x10 : 0x00));
    return {kC0TxDrive, static_cast<std::uint8_t>(level), c2, 0x00, 0x00};
}

Cc ccI2c2Write(std::uint8_t chip, std::uint8_t reg, std::uint8_t data) noexcept
{
    // The chip address is MASKED to 7 bits rather than asserted, because C2
    // bit 7 is the stop flag: an 8-bit I2C address passed by a caller who
    // pre-shifted it would otherwise clear the stop bit and leave the bus
    // held between transactions.
    return {kC0I2c2,
            kI2cCookieWrite,
            static_cast<std::uint8_t>(kI2cStopAtEnd | (chip & 0x7F)),
            reg,
            data};
}

Cc ccI2c1Write(std::uint8_t chip, std::uint8_t reg, std::uint8_t data) noexcept
{
    // Identical shape to ccI2c2Write, different bus byte. Written out rather
    // than factored into a shared helper taking the C0 value, because the two
    // buses have different consequences for a wrong write (see the header) and
    // a single function parameterised by "which bus" is exactly the thing a
    // caller gets wrong.
    return {kC0I2c1,
            kI2cCookieWrite,
            static_cast<std::uint8_t>(kI2cStopAtEnd | (chip & 0x7F)),
            reg,
            data};
}

std::array<Cc, kVersaClockCl1Banks> versaClockCl1Banks(bool externalRef) noexcept
{
    // Register/value pairs, in send order. VERBATIM from the Hermes-Lite 2
    // project by way of piHPSDR and deskHPSDR — see the header for why these
    // are not derived and must not be "tidied".
    static constexpr std::uint8_t kOn[kVersaClockCl1Banks * 2] = {
        0x10, 0xc0, 0x13, 0x03, 0x10, 0x40, 0x2d, 0x01, 0x2e, 0x20, 0x22, 0x03,
        0x23, 0x00, 0x24, 0x00, 0x25, 0x00, 0x19, 0x00, 0x1A, 0x00, 0x1B, 0x00,
        0x18, 0x00, 0x17, 0x12, 0x62, 0x3b, 0x2c, 0x00, 0x31, 0x81, 0x3d, 0x09,
        0x3e, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x63, 0x01,
    };
    static constexpr std::uint8_t kOff[kVersaClockCl1Banks * 2] = {
        0x10, 0xc0, 0x13, 0x00, 0x10, 0x80, 0x2d, 0x01, 0x2e, 0x10, 0x22, 0x00,
        0x23, 0x00, 0x24, 0x00, 0x25, 0x00, 0x19, 0x00, 0x1A, 0x00, 0x1B, 0x00,
        0x18, 0x40, 0x17, 0x04, 0x62, 0x5b, 0x2c, 0x00, 0x31, 0x00, 0x3d, 0x00,
        0x3e, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x63, 0x00,
    };
    const std::uint8_t* table = externalRef ? kOn : kOff;
    std::array<Cc, kVersaClockCl1Banks> out{};
    for (std::size_t i = 0; i < kVersaClockCl1Banks; ++i) {
        out[i] = ccI2c1Write(kVersaClockI2cAddr, table[2 * i], table[2 * i + 1]);
    }
    return out;
}

std::array<Cc, kIoBoardTxFreqBanks> ccIoBoardTxFrequency(std::uint64_t hz) noexcept
{
    std::array<Cc, kIoBoardTxFreqBanks> out{};
    for (std::size_t i = 0; i < kIoBoardTxFreqBanks; ++i) {
        const auto reg = static_cast<std::uint8_t>(kIoBoardRegTxFreqMsb + i);
        // Register 0 carries bits 39:32 and register 4 bits 7:0, so the shift
        // counts DOWN as the register number counts up. Writing this as
        // (8 * i) would invert the byte order and hand the board a frequency
        // in the wrong endianness, which reads as a wildly wrong band rather
        // than as a small error.
        const unsigned shift = 8u * static_cast<unsigned>(kIoBoardRegTxFreqLsb - reg);
        out[i] = ccI2c2Write(kIoBoardI2cAddr, reg,
                             static_cast<std::uint8_t>((hz >> shift) & 0xFFu));
    }
    return out;
}

void ep2WriteTxIq(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                  std::span<const std::complex<float>> iq) noexcept
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    std::size_t consumed = 0;
    for (const std::size_t fs : frameStarts) {
        std::uint8_t* payload = pkt.data() + fs + 8;     // after SYNC(3) + C&C(5)
        for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload; k += kTxSampleBytes) {
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

void ep2WriteTxAudio(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                     std::span<const std::int16_t> audio) noexcept
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    std::size_t consumed = 0;                 // in SAMPLES, not int16s
    for (const std::size_t fs : frameStarts) {
        std::uint8_t* payload = pkt.data() + fs + 8;     // after SYNC(3) + C&C(5)
        for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload; k += kTxSampleBytes) {
            // Stop at the end of the span rather than padding: the remaining
            // slots keep ep2Packet's zero fill, which on a codec radio is
            // silence and on the FIRST slot of a frame is also a zero EADDR.
            // That last part is why underrun is safe here — a short block
            // degrades toward the bare-HL2 behaviour instead of away from it.
            if (2 * consumed + 1 >= audio.size())
                return;
            const auto l = static_cast<std::uint16_t>(audio[2 * consumed]);
            const auto r = static_cast<std::uint16_t>(audio[2 * consumed + 1]);
            ++consumed;
            payload[k + 0] = static_cast<std::uint8_t>((l >> 8) & 0xFF);   // L high
            payload[k + 1] = static_cast<std::uint8_t>(l & 0xFF);          // L low
            payload[k + 2] = static_cast<std::uint8_t>((r >> 8) & 0xFF);   // R high
            payload[k + 3] = static_cast<std::uint8_t>(r & 0xFF);          // R low
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
    // C0[0] is ptt_resp in BOTH branches of control.v's iresp composition, so
    // an ACK still carries it honestly. MetisClient routes ACKs to
    // ingestControlResponse, not here; this public decoder takes PTT from an
    // ACK for its other callers and tests.
    ptt = r.ptt;
    // Everything below reads `raddr` as a free-running telemetry slot. In an ACK
    // it is a command address and `data` is our own echo — see the header.
    if (r.ack)
        return;
    switch (r.raddr) {
    case 0x00:
        firmwareVersion = static_cast<int>(r.data & 0xFF);
        adcOverload     = (r.data & (1u << 24)) != 0;
        // ACTIVE LOW on the wire: the bit is SET when transmit is permitted.
        // Decoded here so nothing above this layer has to remember the inversion.
        txInhibited     = (r.data & (1u << 25)) == 0;
        // TX IQ FIFO status (gateware 883a338). control.v:472 builds this slot
        // as {6'b000111, ~ext_txinhibit[25], &clip_cnt[24], 8'h00[23:16],
        // dsiq_status[15:8], VERSION_MAJOR[7:0]}. dsiq_status (fifos.v:100-110)
        // is {recovery_flag, rd_count[6:0]}: [7] is set by underrun OR blocked
        // writes (no under/overflow distinction on the wire); [6:0] are the top
        // 7 bits of the read-side fill level. One unit is 32 read-side words
        // (DSIQ_FIFO_DEPTH 16384, hermeslite_core.v:136); words-to-samples is
        // unmeasured, so don't servo on this yet (#17).
        txFifoFillMsbs  = static_cast<int>((r.data >> 8) & 0x7F);
        txFifoRecovery  = ((r.data >> 15) & 0x1) != 0;
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

double directionalWatts(int raw) noexcept
{
    // Quisk's `power_meter_std_calibrations['HL2FilterE3']` verbatim
    // (quisk_conf_defaults.py): [ADC count, watts] for an HL2 with an N2ADR
    // filter board rev E3. Not a per-unit calibration (coupler and diode vary
    // between boards): right order of magnitude only, so the meters are
    // labelled uncalibrated (defineMeters). swrFromRaw() shares this curve.
    struct Point { double counts; double watts; };
    static constexpr Point kCurve[] = {
        {    0.000000, 0.000000 }, {   25.865385, 0.002550 },
        {  101.024540, 0.012752 }, {  265.290123, 0.050601 },
        {  647.915584, 0.216458 }, { 1196.593548, 0.665480 },
        { 1603.703226, 1.155723 }, { 2012.327160, 1.811892 },
        { 2616.772727, 3.008585 }, { 3173.818182, 4.392743 },
        { 3382.792208, 4.979133 }, { 3721.071429, 6.024751 },
        { 4093.178571, 7.289948 }, { 4502.496429, 8.820838 },
        { 4952.746071, 10.673214 },
    };
    constexpr std::size_t kN = std::size(kCurve);

    const double counts = static_cast<double>(raw);
    if (counts <= kCurve[0].counts)
        return 0.0;
    // Above the top of the table, extrapolate along the last segment rather
    // than clamping. Clamping would pin the meter at 10.7 W and hide the one
    // reading an operator most needs to see — that they are past where the
    // curve was ever measured.
    if (counts >= kCurve[kN - 1].counts) {
        const Point& a = kCurve[kN - 2];
        const Point& b = kCurve[kN - 1];
        const double slope = (b.watts - a.watts) / (b.counts - a.counts);
        return b.watts + (counts - b.counts) * slope;
    }
    for (std::size_t i = 1; i < kN; ++i) {
        if (counts <= kCurve[i].counts) {
            const Point& a = kCurve[i - 1];
            const Point& b = kCurve[i];
            const double span = b.counts - a.counts;
            if (span <= 0.0)
                return b.watts;
            return a.watts + (counts - a.counts) * (b.watts - a.watts) / span;
        }
    }
    return kCurve[kN - 1].watts;
}



double detectorVolts(int raw) noexcept
{
    // The inverse of the count->power curve, taken back to VOLTAGE, in
    // arbitrary units — only ratios of two of these are ever used.
    //
    // sqrt() of directionalWatts() rather than a second table, deliberately:
    // the interpolation scheme is then the SAME scheme by construction, and the
    // two functions cannot drift apart when a per-unit calibration replaces the
    // points. A separate voltage table would be a second thing to keep in step.
    const double w = directionalWatts(raw);
    return w > 0.0 ? std::sqrt(w) : 0.0;
}

std::optional<double> swrFromRaw(int forwardRaw, int reverseRaw) noexcept
{
    // No carrier, no SWR. Returning 1.0 here would render as a perfect match
    // when the truth is that the question is meaningless.
    if (forwardRaw <= 0)
        return std::nullopt;
    // rho is a ratio of detector VOLTAGES, but the diode's k = counts/sqrt(W)
    // rises from 512 at 26 counts to ~1516 above ~1200, so a raw count ratio
    // reads SWR optimistic (#4578). detectorVolts() undoes the curve first.
    // Near the noise floor one-LSB differences still diverge (20/19 -> 78.0);
    // callers gate on kMinForwardCountsForSwr.
    const double fwd = detectorVolts(forwardRaw);
    if (!(fwd > 0.0))
        return std::nullopt;          // below the bottom of the curve entirely
    double rev = detectorVolts(reverseRaw < 0 ? 0 : reverseRaw);
    // Reverse above forward is physically impossible; it means noise on a tiny
    // reading. Clamp rather than emit a negative or infinite SWR.
    if (rev >= fwd)
        rev = fwd * 0.999;
    const double rho = rev / fwd;
    return (1.0 + rho) / (1.0 - rho);
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
    // 0x02 idle, 0x03 streaming. On gateware 883a338 (usopenhpsdr1.v:266) 0x03
    // also means "flash erase just completed" and 0x04 (not decoded) a flash
    // write in progress, so discovering during an Ethernet flash reads as
    // streaming.
    r.streaming = (pkt[2] == 0x03);
    for (std::size_t i = 0; i < 6; ++i)
        r.mac[i] = pkt[3 + i];
    r.gatewareVersion = pkt[9];
    r.boardId = pkt[10];
    // Receiver count is offset 0x13 (19): usopenhpsdr1.v emits the reply from a
    // down-counting state, offset = 0x3B - state, so `6'h28: NR` lands at 0x13;
    // the wiki map and hpsdrsim (`buffer[19] = 4`) agree. Offset 0x14 is
    // {BANDSCOPE_BITS, BOARD[5:0]}; misreading it authorises receivers that
    // stream all-zero IQ. Short replies leave 0 so callers apply a default.
    if (pkt.size() > 19)
        r.numRx = pkt[19];

    // Telemetry, offsets 0x17-0x29: the EP6 response set in the same raw units,
    // readable without a stream (control.v:899 is combinational; no `run` gate
    // in dsopenhpsdr1.v:185-207). Offsets per usopenhpsdr1.v:261-307, offset =
    // 0x3B - dbyte_no. Fields stay absent on short replies. Gateware without
    // EXTENDED_RESP (control.v:826) sends hard zeros here, indistinguishable from
    // real zeros; hl2b5up_main sets EXTENDED_RESP(1) (hermeslite.v:110).
    const auto be16 = [](std::span<const std::uint8_t> p, std::size_t at) {
        return static_cast<int>((std::uint32_t(p[at]) << 8) | std::uint32_t(p[at + 1]));
    };

    if (pkt.size() > 0x1a) {
        r.responseData = (std::uint32_t(pkt[0x17]) << 24) | (std::uint32_t(pkt[0x18]) << 16)
                       | (std::uint32_t(pkt[0x19]) << 8)  |  std::uint32_t(pkt[0x1a]);
    }
    if (pkt.size() > 0x1b) {
        // control.v:899:
        //   resp_control = {ext_cwkey, ptt_resp, pa_exttr, pa_inttr,
        //                   tx_on, cw_on, clip_cnt}
        const std::uint8_t c = pkt[0x1b];
        r.extCwKey = (c & 0x80) != 0;
        r.ptt      = (c & 0x40) != 0;          // ptt_resp = cw_on | ext_ptt (control.v:456)
        r.paExtTr  = (c & 0x20) != 0;
        r.paIntTr  = (c & 0x10) != 0;
        r.txOn     = (c & 0x08) != 0;
        r.cwOn     = (c & 0x04) != 0;
        // clip_cnt is cleared only on each EP6 packet (control.v:465): while
        // streaming it covers the last ~2.6 ms; idle it means "clipped since the
        // stream ended", saturated at 3. rxclip is a sticky level (control.v:479,
        // ad9866.v:232-241), so this is a flag, not a rate; pair with `streaming`.
        r.adcClipCount = static_cast<int>(c & 0x03);
    }
    // The four slow-ADC readings, each 12 bits in a big-endian pair with a zero
    // top nibble. Same converter and same scaling as the EP6 cycle's
    // temperatureRaw / forwardPowerRaw / reversePowerRaw / biasCurrentRaw, so
    // the stream-free reading and the in-band reading are directly comparable
    // with no conversion — which is what makes the two a cross-check on each
    // other rather than two unrelated numbers.
    if (pkt.size() > 0x23) {
        r.temperatureRaw  = be16(pkt, 0x1c);
        r.forwardPowerRaw = be16(pkt, 0x1e);
        r.reversePowerRaw = be16(pkt, 0x20);
        r.biasCurrentRaw  = be16(pkt, 0x22);
    }
    if (pkt.size() > 0x24) {
        // dsiq_status, identical to the byte the EP6 path decodes at DATA[15:8]
        // — one recovery flag covering underrun AND blocked writes, then the
        // top 7 bits of the fill level. See Hl2Telemetry::apply().
        r.txFifoRecovery = (pkt[0x24] & 0x80) != 0;
        r.txFifoFillMsbs = static_cast<int>(pkt[0x24] & 0x7F);
    }
    if (pkt.size() > 0x26)
        r.txBufferLatencyMs = static_cast<int>(pkt[0x26] & 0x7F);   // 6'h15: {1'b0, [6:0]}
    if (pkt.size() > 0x28) {
        // 6'h13: {cw_hang_time[9:8], 1'b0, ptt_hang_time[4:0]}. The mask is
        // 0x1F and not a byte, and that is load-bearing rather than tidy: 31 in
        // this field does not mean "the longest hang time", it DISABLES the
        // gateware's PTT auto-unkey altogether (softerhardware/Hermes-Lite2
        // issue #178). A decode that let cw_hang_time's two high bits bleed in
        // would report a disabled dead-man's switch as some other number, or
        // some other number as disabled.
        r.pttHangTimeMs = static_cast<int>(pkt[0x28] & 0x1F);
    }
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

namespace {

// Shared round walker for both ep6Samples() and ep6SamplesMulti().
//
// Returns rounds appended (== samples per receiver), or -1 on a bad header.
// `sink(rx, i, q)` is called for each receiver within each round.
template <typename Sink>
int ep6DecodeRounds(std::span<const std::uint8_t> pkt, int numRx, Sink&& sink) noexcept
{
    if (!isEp6Header(pkt))
        return -1;
    constexpr float kInvFullScale = 1.0f / static_cast<float>(kFullScale);
    const std::size_t roundBytes = ep6RoundBytes(numRx);
    int rounds = 0;
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        const std::uint8_t* frame = pkt.data() + fs;
        if (frame[0] != kSync || frame[1] != kSync || frame[2] != kSync)
            continue;                                    // skip a corrupt frame, keep the good one
        const std::uint8_t* payload = frame + 8;         // after SYNC(3) + C&C(5)
        // `k + roundBytes <= kFramePayload` is the host-side mirror of the
        // gateware's own "is there room for another round?" test, so the loop
        // stops exactly where the hardware switched to zero padding. Reading the
        // pad as samples would inject a burst of digital silence per packet.
        for (std::size_t k = 0; k + roundBytes <= kFramePayload; k += roundBytes) {
            for (int rx = 0; rx < numRx; ++rx) {
                const std::uint8_t* s = payload + k + static_cast<std::size_t>(rx) * kRxIqBytes;
                sink(rx,
                     static_cast<float>(decode24be(s)) * kInvFullScale,
                     static_cast<float>(decode24be(s + 3)) * kInvFullScale);
            }
            ++rounds;   // the round's trailing 2 mic bytes are ignored
        }
    }
    return rounds;
}

}  // namespace

int ep6Samples(std::span<const std::uint8_t> pkt, std::vector<std::complex<float>>& out) noexcept
{
    return ep6DecodeRounds(pkt, 1, [&out](int, float i, float q) { out.emplace_back(i, q); });
}

int ep6SamplesMulti(std::span<const std::uint8_t> pkt,
                    std::span<std::vector<std::complex<float>>> out) noexcept
{
    const int numRx = static_cast<int>(out.size());
    if (numRx < 1 || numRx > kMaxReceivers)
        return -1;
    return ep6DecodeRounds(pkt, numRx, [&out](int rx, float i, float q) {
        out[static_cast<std::size_t>(rx)].emplace_back(i, q);
    });
}

double Ep4Stats::peakDbfs() const noexcept
{
    if (samples <= 0 || peakAbs <= 0)
        return kEp4FloorDbfs;
    return 20.0 * std::log10(static_cast<double>(peakAbs)
                             / static_cast<double>(kEp4FullScale));
}

double Ep4Stats::rmsDbfs() const noexcept
{
    if (samples <= 0 || sumSquares <= 0.0)
        return kEp4FloorDbfs;
    // RMS about the mean, so a converter DC offset does not deflate the crest
    // factor (#5802).
    const double mean = sum / static_cast<double>(samples);
    // Exact for the production sample counts (512, 2048: powers of two, sums
    // under 2^53); the clamp keeps arbitrary counts from rounding negative
    // and feeding NaN to std::sqrt.
    const double var = std::max(0.0, sumSquares / static_cast<double>(samples)
                                         - mean * mean);
    const double rms = std::sqrt(var);
    // A record with no AC content at all — a DC pedestal and nothing else —
    // reaches here with rms == 0 and takes the same floor an all-zero block
    // does. That is the honest answer: it has no deviation to report.
    if (rms <= 0.0)
        return kEp4FloorDbfs;
    return 20.0 * std::log10(rms / static_cast<double>(kEp4FullScale));
}

std::optional<double> Ep4Stats::crestDb() const noexcept
{
    // Both terms must be real levels. peakDbfs() and rmsDbfs() return
    // kEp4FloorDbfs EXACTLY when they have nothing to report, so testing
    // against it is exact rather than an epsilon judgement. The test is `<=`
    // and not `==` so that it also rejects an RMS computed BELOW the floor,
    // for the same reason it rejects the sentinel: a deviation under half a
    // code is not a quantity to take a ratio against. See the header.
    const double peak = peakDbfs();
    const double rms  = rmsDbfs();
    if (peak <= kEp4FloorDbfs || rms <= kEp4FloorDbfs)
        return std::nullopt;
    return peak - rms;
}

void Ep4Stats::merge(const Ep4Stats& other) noexcept
{
    samples += other.samples;
    // Peak is the MAX and not a sum, which is what lets a block's stats and a
    // packet's stats be the same type: merging four packets of a block gives
    // the peak of the 2048-sample record, not four times one packet's.
    if (other.peakAbs > peakAbs)
        peakAbs = other.peakAbs;
    sumSquares += other.sumSquares;
    // Plain addition, exactly as for sumSquares: both are linear in the
    // record, which is what lets a merged block's mean and variance be those
    // of the 2048-sample concatenation rather than an average over packets.
    sum += other.sum;
    clippedSamples += other.clippedSamples;
}

std::optional<std::uint32_t> ep4Seq(std::span<const std::uint8_t> pkt) noexcept
{
    if (!isEp4Header(pkt))
        return std::nullopt;
    // Byte 4 is a hardwired 8'h00 and byte 5 carries only ep4_seq_no[19:16], so
    // a well-formed header needs no mask. It is applied anyway: this value
    // seeds the gap arithmetic, which assumes everything it sees is inside
    // kEp4SeqModulus, and a header that is not well-formed must not be able to
    // walk that state outside the modulus.
    return readBe32(pkt.data() + 4) & (kEp4SeqModulus - 1);
}

int ep4Samples(std::span<const std::uint8_t> pkt, std::vector<float>& out) noexcept
{
    if (!isEp4Header(pkt))
        return -1;
    constexpr float kInvFullScale = 1.0f / static_cast<float>(kEp4FullScale);
    const std::uint8_t* p = pkt.data() + 8;
    for (std::size_t i = 0; i < kEp4SamplesPerPacket; ++i)
        out.push_back(static_cast<float>(decodeEp4Code(p + 2 * i)) * kInvFullScale);
    return static_cast<int>(kEp4SamplesPerPacket);
}

std::optional<Ep4Stats> ep4Stats(std::span<const std::uint8_t> pkt) noexcept
{
    if (!isEp4Header(pkt))
        return std::nullopt;
    Ep4Stats s;
    const std::uint8_t* p = pkt.data() + 8;
    for (std::size_t i = 0; i < kEp4SamplesPerPacket; ++i) {
        const int code = decodeEp4Code(p + 2 * i);
        const int mag = code < 0 ? -code : code;
        if (mag > s.peakAbs)
            s.peakAbs = mag;
        s.sumSquares += static_cast<double>(code) * static_cast<double>(code);
        // The SIGNED code, alongside its square: rmsDbfs() removes the mean,
        // and a magnitude sum would not be one. (#5802.)
        s.sum += static_cast<double>(code);
        // The gateware's own rails, not a symmetric threshold. See
        // Ep4Stats::clippedSamples in the header.
        if (code >= kEp4FullScale - 1 || code <= -kEp4FullScale)
            ++s.clippedSamples;
    }
    s.samples = static_cast<int>(kEp4SamplesPerPacket);
    return s;
}

}  // namespace AetherSDR::hl2
