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

Cc ccConfig(SampleRate rate, int numRx, std::uint8_t ocFilterByte) noexcept
{
    const auto c1 = static_cast<std::uint8_t>((static_cast<std::uint8_t>(rate) & 0x03) | kConfigMercury);
    if (numRx < 1) numRx = 1;
    if (numRx > kMaxReceivers) numRx = kMaxReceivers;
    // Open collector outputs are DATA[23:17] == C2[7:1]. The one-bit shift is
    // the whole reason this cannot be a straight assignment: DATA[16] is not
    // part of the field, and writing the byte unshifted would put the 160 m
    // relay's bit there and every real selection one filter too low.
    const auto c2 = static_cast<std::uint8_t>((ocFilterByte & 0x7F) << 1);
    // Receiver count is DATA[6:3] — a FOUR-bit field (0000=1 .. 1011=12), so the
    // mask is 0x0F. It was 0x07 while only one receiver ever ran, which silently
    // capped the encodable count at 8 and would have wrapped 9..12 into 1..4.
    const auto c4 = static_cast<std::uint8_t>(kConfigDuplex | (((numRx - 1) & 0x0F) << 3));
    return {kC0Config, c1, c2, 0x00, c4};
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
        // TX IQ FIFO status. CHECKED against the gateware at 883a338, which is
        // what the comment that stood here asked for and did not have. It said
        // hpsdrsim writes a 15-bit count at DATA[22:8], that the oracle's §6
        // disagreed with itself about bit 14, and that nothing should servo on
        // this field until someone read the RTL. Read; all three of the old
        // fields were wrong.
        //
        // control.v:472 builds this slot as
        //
        //   data = {6'b000111, ~ext_txinhibit, (&clip_cnt), 8'h00,
        //    bits    31:26     25              24           23:16
        //           dsiq_status, VERSION_MAJOR}
        //           15:8         7:0
        //
        // The FIFO field is DATA[15:8] and nothing more. DATA[23:16] is a
        // constant zero, which is exactly why the old expression survived:
        // (data >> 8) & 0x7FFF returns dsiq_status itself, the right number
        // under a name claiming fifteen bits of sample count. Nothing it ever
        // displayed looked absurd, so nothing ever prompted the read.
        //
        // dsiq_fifo composes the byte at fifos.v:100-110 as
        // {recovery_flag_d1, rd_count[6:0]}, so:
        //
        //   [7]   one recovery flag, set by the FIFO running empty OR by its
        //         writes being blocked after it filled. The gateware carries no
        //         under/overflow distinction, so the two old booleans were
        //         decoding a difference that is not on the wire — and they
        //         disagreed with each other on the same event depending on
        //         fill-level bit 6, reporting "underflow" at 0x80 and
        //         "overflow" at 0xC0.
        //   [6:0] the TOP 7 bits of the read-side fill level: coarse occupancy,
        //         not a count of samples.
        //
        // Still NOT established, and so still not safe to servo on: what one
        // unit of [6:0] is worth. rdbits is 12 for this board's
        // DSIQ_FIFO_DEPTH of 16384 (hermeslite_core.v:136), making the unit 32
        // read-side words, but words-to-samples is an inference. #17 needs that
        // number measured before a pacing loop uses this field.
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
    // 0x02 idle, 0x03 already sending. NOT only that: on the HL2 gateware at
    // 883a338 this byte is
    //
    //   usopenhpsdr1.v:266
    //   discover_data_next = usethasmi_erase_done ? 8'h03
    //                      : (usethasmi_send_more ? 8'h04
    //                      : (run ? 8'h03 : 8'h02));
    //
    // so 0x03 means "streaming" OR "a gateware flash erase just completed", and
    // 0x04 — which nothing here decodes — means a flash write is in progress.
    // Reading 0x03 as `streaming` is therefore a judgement, not what the byte
    // says: an application that discovers while someone is flashing the radio
    // will be told the radio is busy sending IQ. Harmless while nobody flashes
    // over Ethernet, wrong the moment anybody does, and named here so the next
    // reader does not have to re-derive it from the RTL.
    r.streaming = (pkt[2] == 0x03);
    for (std::size_t i = 0; i < 6; ++i)
        r.mac[i] = pkt[3 + i];
    r.gatewareVersion = pkt[9];
    r.boardId = pkt[10];
    // OFFSET 19 (0x13), not 20. This was off by one, and the byte it was
    // actually reading is a real field with plausible values — so the error
    // could not show up as an obviously wrong answer.
    //
    // Settled against all three tiers of the source-precedence ladder, which
    // agree:
    //
    //   gateware  usopenhpsdr1.v emits the discovery reply from a DOWN-counting
    //             state, so the packet offset is 0x3B - state. Anchor it on two
    //             knowns — `6'h32: VERSION_MAJOR` is offset 9 and `6'h31:
    //             idhermeslite ? 8'h06 : 8'h01` is offset 10, both fixed by the
    //             map below — and `6'h28: ... NR` lands at 0x3B-0x28 = 0x13.
    //   wiki      discovery map, offset 0x13 = "Number of hardware receivers".
    //   hpsdrsim  writes `buffer[19] = 4` for a Hermes-Lite 2.
    //
    // Offset 20 (0x14) is `{BANDSCOPE_BITS, BOARD[5:0]}` — the wideband format
    // in [7:6] and the board build id in [5:0]. On a build-5 board with the
    // wideband bits set that reads as a receiver count in the dozens, which the
    // caller then clamps to the register maximum. So the old code did not fail
    // loudly on real hardware; it quietly authorised more receivers than the
    // board has, and the extra ones stream correctly framed, correctly paced,
    // all-ZERO IQ — indistinguishable from a dead antenna.
    //
    // Short replies omit it; leave 0 so callers apply their own default.
    if (pkt.size() > 19)
        r.numRx = pkt[19];

    // ---- Telemetry, offsets 0x17-0x29 ----
    //
    // The radio has been sending all of this at every discovery and we have
    // been discarding it since the parser was written. It is the same set the
    // EP6 response cycle carries, in the same raw units — but obtainable
    // WITHOUT a stream, which is the only way to read the radio while another
    // client holds it or while our own stream is broken. Roadmap item #15; it
    // needs nothing from item #13's RQST/ACK machinery, because none of these
    // are command responses. resp_control is a combinational assign
    // (control.v:899) and the discovery path has no `run` gate
    // (dsopenhpsdr1.v:185-207).
    //
    // Offsets come from usopenhpsdr1.v:261-307, which emits the reply from a
    // DOWN-counting state: offset = 0x3B - dbyte_no. Anchored on the two bytes
    // parsed above — 6'h32 (VERSION_MAJOR) at 9 and 6'h31 (board) at 10 — with
    // the same arithmetic putting 6'h28 (NR) at 0x13. hermeslite.py decodes the
    // same packet identically and is the cross-check, not the source.
    //
    // Everything here stays absent unless the reply is long enough to have
    // carried it. A gateware built without EXTENDED_RESP (control.v:826) sends
    // hard zeros in these bytes rather than readings, and we cannot tell that
    // apart from a genuine zero at this layer — a caller that needs to must
    // compare across polls, and this comment is the warning that it is not
    // free. Our board sets EXTENDED_RESP(1)
    // (gateware/variants/hl2b5up_main/hermeslite.v:110).
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
        // TWO MEANINGS, and which one applies depends on whether the radio is
        // streaming. `clip_cnt` is cleared on every EP6 packet (control.v:465)
        // and by NOTHING else, so:
        //   streaming  -> clip windows in the last EP6 interval (~2.6 ms), 0-3
        //   idle       -> "clipped at least once since the last stream ended",
        //                 saturated at 3 and unclearable by a discovery poller
        // It is also not a count: rxclip is a sticky rail latch added as a
        // LEVEL (control.v:479, ad9866.v:232-241), so three clock edges
        // saturate it. Treat this as a flag with a range, never as a rate — the
        // window length in wall-clock terms is not established. A caller must
        // pair it with `streaming` above before showing it to anyone.
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

}  // namespace AetherSDR::hl2
