// HL2 hardware-variant options — the policy that decides what a bare
// Hermes-Lite 2, an HL2+ (AK4951 companion board) and a SquareSDR 2 each get
// on the wire, plus the three wire encodings those choices reach.
//
// WHY THIS TEST EXISTS. The three radios are indistinguishable over Protocol 1
// and NONE of what this file pins is readable back from any of them: the
// dither bit, the open-collector filter byte and the ATU request are all
// write-only. A wrong value is therefore not a failed command that reports
// itself — it is a loudspeaker nailed on, a codec the gateware stops believing
// in, or a receive path with relays engaged that are not in it. There is no
// runtime evidence to fall back on, so the evidence has to be here.
//
// The reference values are deskHPSDR's, which carries them from piHPSDR and
// the Hermes-Lite 2 project. They are compared against, not re-derived: the
// N2ADR band groupings are a property of that board and nothing in a datasheet
// would let a reader recompute them.
//
// Pure policy and pure wire — no Qt, no sockets, no hardware.

#include "core/backends/hl2/Hl2HardwareOptions.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

using AetherSDR::Hl2HardwareOptions;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// The dither bit: one bit, three meanings, ONE owner.
//
// The bit at 0x00[11] means something different on each of the three boards —
// band volts on a bare HL2, the codec's loudspeaker on the two that have one —
// but on none of them does it mean "a codec is present", and on none of them
// may this code take it away from the operator. That claim is checked against
// the Hermes-Lite 2 gateware rather than against deskHPSDR, which forces the
// bit for HL2_CODEC_AK4951 on the strength of a comment in old_protocol.c:
//
//   i2c_bus2.v   `ifdef AK4951, cmd_addr 0x00:
//                ak4951_spon_next = cmd_data[11];   // reuse Dither
//                data1_next = 8'h2e | (cmd_data[11] ? 8'h80 : 8'h00);
//   control.v    band_volts_enabled <= cmd_data[11];
//   localaudio.v assign i2s_pdn = ~clk_i2c_rst;     // codec PDN, not the bit
//   i2c.v        STATE_AK4951S8: {8'h02, 8'hae}     // speaker already ON
//
// Those are the only two consumers of cmd_data[11] on address 0x00, and the
// codec is instantiated on the bitstream parameter AK4951, never on the bit.
// If this test is ever changed back to pin a force, it has to cite a gateware
// build that actually withholds the codec while the bit is low.
// ---------------------------------------------------------------------------
static void testDitherMeaning()
{
    Hl2HardwareOptions o;

    // Bare HL2: the operator owns it — it is the band-voltage output.
    o.codec = Hl2HardwareOptions::Codec::None;
    o.ditherBit = false;
    check(!o.ditherBitOnWire(), "bare HL2: dither follows the operator (off)");
    o.ditherBit = true;
    check(o.ditherBitOnWire(), "bare HL2: dither follows the operator (on)");
    check(!o.hasLocalCodec(), "bare HL2 has no local codec");

    // HL2+: the operator owns it too, because here it is the AK4951's
    // loudspeaker. NOT forced — forcing it would nail the speaker on and take
    // the only off switch the board has away from whoever is wearing headphones.
    o.codec = Hl2HardwareOptions::Codec::Ak4951;
    o.ditherBit = false;
    check(!o.ditherBitOnWire(), "HL2+: speaker off stays off (NOT forced high)");
    o.ditherBit = true;
    check(o.ditherBitOnWire(), "HL2+: speaker on");
    check(o.hasLocalCodec(), "HL2+ has a local codec");

    // SquareSDR 2: the same bit, the same owner, the same speaker.
    o.codec = Hl2HardwareOptions::Codec::SquareSdr2;
    o.ditherBit = false;
    check(!o.ditherBitOnWire(), "SquareSDR 2: speaker off stays off (NOT forced)");
    o.ditherBit = true;
    check(o.ditherBitOnWire(), "SquareSDR 2: speaker on");
    check(o.hasLocalCodec(), "SquareSDR 2 has a local codec");

    // NO VARIANT OVERRIDES THE OPERATOR, walked through clampCodec() over
    // 0..kCodecCount-1 rather than over a hand-written list. The list was the
    // defect: it claimed a fourth codec would fail here, and @on8st showed it
    // would not — `Codec::Fourth = 3` with its dither bit forced high passed
    // every check (#5867 review). What actually stops that now is the
    // static_assert at the foot of Hl2HardwareOptions.h, which refuses to
    // compile until a new enumerator has been given a dither rule; this loop
    // then covers it without being edited.
    for (int raw = 0; raw < Hl2HardwareOptions::kCodecCount; ++raw) {
        const auto codec = Hl2HardwareOptions::clampCodec(raw);
        o.codec = codec;
        o.ditherBit = false;
        check(!o.ditherBitOnWire(), "no codec forces the bit high");
        o.ditherBit = true;
        check(o.ditherBitOnWire(), "no codec forces the bit low");
    }

    // The stored intent survives a change of board, in both directions: the
    // document keeps one bit and the codec never writes to it.
    o.codec = Hl2HardwareOptions::Codec::Ak4951;
    o.ditherBit = false;
    o.codec = Hl2HardwareOptions::Codec::None;
    check(!o.ditherBit && !o.ditherBitOnWire(),
          "a cleared bit is still cleared after the board changes");
    o.ditherBit = true;
    o.codec = Hl2HardwareOptions::Codec::SquareSdr2;
    check(o.ditherBit && o.ditherBitOnWire(),
          "a set bit is still set after the board changes");
}

// ---------------------------------------------------------------------------
// What the dither bit becomes when the operator declares a DIFFERENT board.
//
// The bit means different things on the three variants, so it cannot simply
// carry across a change of board. This is the rule that decides, and it is
// pinned here rather than in the dialog because the dialog is where it was
// wrong twice: the first version clobbered the operator's stored intent from
// refreshDither(), and its replacement seeded only the AK4951 case — so
// declaring an AK4951 and then correcting it to None left the bit high and
// persisted it, and a bare Hermes-Lite 2 came up driving its band-voltage
// output (#5867 review, @on8st, both rounds).
// ---------------------------------------------------------------------------
static void testCodecChangeSeed()
{
    using Codec = Hl2HardwareOptions::Codec;
    const auto seed = &Hl2HardwareOptions::ditherBitOnCodecChange;

    // None: always off. The bit is the band-voltage output on the CL2 jack, and
    // nobody may get a DC level on an antenna jack by declaring what codec they
    // do not have.
    check(!seed(Codec::None, false), "-> None seeds the bit off (was off)");
    check(!seed(Codec::None, true),  "-> None seeds the bit off (was ON) — the "
                                     "band-volts regression, pinned");

    // AK4951: always on, because the gateware's init already turned that
    // speaker on (i2c.v STATE_AK4951S8 writes 0x02 = 0xae) and only rewrites
    // the register on a change.
    check(seed(Codec::Ak4951, false), "-> AK4951 seeds the bit on (was off)");
    check(seed(Codec::Ak4951, true),  "-> AK4951 seeds the bit on (was on)");

    // SquareSDR 2: the operator's value, untouched. It is a loudspeaker on that
    // board too, so carrying a speaker setting onto a speaker is harmless in
    // the way carrying it onto band volts is not — and no gateware citation
    // exists for its power-on state, so seeding either way would assert
    // something nobody here has established.
    check(!seed(Codec::SquareSdr2, false), "-> SquareSDR 2 carries the operator's off");
    check(seed(Codec::SquareSdr2, true),   "-> SquareSDR 2 carries the operator's on");

    // THE REGRESSION ITSELF, walked as the operator walks it: declare a codec,
    // then correct the declaration. Every ordered pair, so no route back to
    // None can leave band volts on however the operator got there.
    for (int from = 0; from < Hl2HardwareOptions::kCodecCount; ++from) {
        for (int to = 0; to < Hl2HardwareOptions::kCodecCount; ++to) {
            const Codec a = Hl2HardwareOptions::clampCodec(from);
            const Codec b = Hl2HardwareOptions::clampCodec(to);
            const bool afterFirst  = seed(a, false);
            const bool afterSecond = seed(b, afterFirst);
            if (b == Codec::None) {
                check(!afterSecond,
                      "declaring any board and then correcting to None leaves the "
                      "band-voltage output off");
            }
            if (b == Codec::Ak4951) {
                check(afterSecond,
                      "declaring any board and then choosing the AK4951 leaves its "
                      "speaker on, matching the gateware's power-on state");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The open-collector filter byte, receive and transmit.
// ---------------------------------------------------------------------------
static void testFilterBoard()
{
    // deskHPSDR's n2adr_oc_settings(), verbatim: 160 m = 1 (no HPF), and
    // 80 m..10 m = the band's low-pass bit ORed with 64, the AM-broadcast
    // high-pass. Transcribed as the decimal values that file uses so the
    // comparison is against the source rather than against our own constants.
    struct Band { double hz; unsigned oc; const char* name; };
    static const Band kN2adr[] = {
        {  1'900'000.0,  1, "160 m" },
        {  3'700'000.0, 66, "80 m"  },
        {  5'350'000.0, 68, "60 m"  },
        {  7'100'000.0, 68, "40 m"  },
        { 10'120'000.0, 72, "30 m"  },
        { 14'100'000.0, 72, "20 m"  },
        { 18'100'000.0, 80, "17 m"  },
        { 21'200'000.0, 80, "15 m"  },
        { 24'930'000.0, 96, "12 m"  },
        { 28'500'000.0, 96, "10 m"  },
    };

    // ---- the default: receive and transmit, the boxed HL2 ----
    //
    // THE DEFAULT IS LOAD-BEARING. This backend drove exactly this pattern
    // unconditionally before any of these options existed, so a default of
    // anything else would silently change the front end of every HL2 already
    // on the air.
    Hl2HardwareOptions rxtx;
    check(rxtx.filterBoard == Hl2HardwareOptions::FilterBoard::N2adrRxTx,
          "default filter board is N2ADR receive+transmit");
    for (const Band& b : kN2adr) {
        check(rxtx.ocReceiveByteForHz(b.hz) == b.oc, b.name);
        check(rxtx.ocTransmitByteForHz(b.hz) == b.oc, b.name);
        // ...and identical to the unconditional rule this replaced.
        check(rxtx.ocReceiveByteForHz(b.hz) == ocFilterByteForHz(b.hz),
              "receive byte matches the historical unconditional rule");
    }

    // ---- no board: every relay released, both directions ----
    Hl2HardwareOptions none;
    none.filterBoard = Hl2HardwareOptions::FilterBoard::None;
    none.n2adrHpf = true;             // must not resurrect anything
    for (const Band& b : kN2adr) {
        check(none.ocReceiveByteForHz(b.hz) == kOcNone, "no board: receive released");
        check(none.ocTransmitByteForHz(b.hz) == kOcNone, "no board: transmit released");
    }

    // ---- transmit only: the SquareSDR 2's wiring ----
    //
    // TRANSMIT IS UNCHANGED. The low-pass is what keeps harmonics off the air,
    // so it is not the operator's convenience to decline — only declaring the
    // board absent releases it.
    Hl2HardwareOptions txOnly;
    txOnly.filterBoard = Hl2HardwareOptions::FilterBoard::N2adrTxOnly;
    txOnly.n2adrHpf = false;
    for (const Band& b : kN2adr) {
        check(txOnly.ocTransmitByteForHz(b.hz) == b.oc, "transmit-only: transmit unchanged");
        check(txOnly.ocReceiveByteForHz(b.hz) == kOcNone,
              "transmit-only, HPF off: receive is bare");
    }

    // With the high-pass on, receive gets the HPF ALONE — deskHPSDR's
    // n2adr_oc_settings_tx() sets OCrx to 64 on 80 m..10 m and 0 on 160 m.
    txOnly.n2adrHpf = true;
    for (const Band& b : kN2adr) {
        const unsigned want = (b.hz < 2'500'000.0) ? 0u : 64u;
        check(txOnly.ocReceiveByteForHz(b.hz) == want,
              "transmit-only, HPF on: receive is the high-pass alone (160 m excepted)");
        check(txOnly.ocTransmitByteForHz(b.hz) == b.oc,
              "transmit-only, HPF on: transmit still the full pattern");
    }

    // The two frequencies where the high-pass must stay out are NOT restated in
    // the transmit-only path — they are masked out of the per-band pattern — so
    // pin that the inherited exceptions really are inherited.
    check(txOnly.ocReceiveByteForHz(900'000.0) == kOcNone,
          "transmit-only: no high-pass below 1.6 MHz (it would remove the signal)");
    check(txOnly.ocReceiveByteForHz(1'900'000.0) == kOcNone,
          "transmit-only: no high-pass on 160 m (supply spurs couple into it)");
    check(txOnly.ocReceiveByteForHz(50'000'000.0) == kOcNone,
          "transmit-only: nothing above 30 MHz, the board has no filter there");
}

// ---------------------------------------------------------------------------
// The radio's own speaker level.
// ---------------------------------------------------------------------------
static void testSpeakerLevel()
{
    Hl2HardwareOptions o;
    // UNITY BY DEFAULT, so enabling a codec does not also quietly attenuate it.
    check(o.speakerLevelPercent == 100, "speaker level defaults to unity");

    // NO CODEC, NO SPEAKER. speakerGain() answers 0 whatever the level says,
    // so a caller that forgets to check hasLocalCodec() produces silence
    // rather than writing samples into a bare HL2's EADDR.
    o.codec = Hl2HardwareOptions::Codec::None;
    check(o.speakerGain() == 0.0f, "no codec: gain is zero even at level 100");

    o.codec = Hl2HardwareOptions::Codec::SquareSdr2;
    check(o.speakerGain() == 1.0f, "codec at 100 is unity");
    o.speakerLevelPercent = 50;
    check(o.speakerGain() == 0.5f, "linear: 50 is half");
    o.speakerLevelPercent = 0;
    check(o.speakerGain() == 0.0f, "0 is silent");

    // Clamped at both ends — a hand-edited settings file must not command a
    // gain above unity, which would clip the speaker feed with no control
    // anywhere in the application able to bring it back.
    check(Hl2HardwareOptions::clampSpeakerLevel(-5) == 0, "clamp below 0");
    check(Hl2HardwareOptions::clampSpeakerLevel(250) == 100, "clamp above 100");
    check(Hl2HardwareOptions::clampSpeakerLevel(73) == 73, "in range is untouched");
    o.speakerLevelPercent = 250;
    check(o.speakerGain() == 1.0f, "gain is clamped, not just the setter");
    o.speakerLevelPercent = -10;
    check(o.speakerGain() == 0.0f, "negative gain clamps to silence");

    // The level is INDEPENDENT of the dither bit, which on a SquareSDR 2 is
    // the speaker's on/off switch. Two different controls for two different
    // things: the gateware bit cuts the speaker, this one sets its level.
    o.speakerLevelPercent = 80;
    o.ditherBit = false;
    const float quiet = o.speakerGain();
    o.ditherBit = true;
    check(o.speakerGain() == quiet, "speaker level does not move with the dither bit");
}

// ---------------------------------------------------------------------------
// Clamping a round-tripped settings document.
// ---------------------------------------------------------------------------
static void testClamps()
{
    check(Hl2HardwareOptions::clampCodec(0) == Hl2HardwareOptions::Codec::None, "codec 0");
    check(Hl2HardwareOptions::clampCodec(1) == Hl2HardwareOptions::Codec::Ak4951, "codec 1");
    check(Hl2HardwareOptions::clampCodec(2) == Hl2HardwareOptions::Codec::SquareSdr2, "codec 2");
    // A value from a newer build, a hand-edited file, or a truncated write
    // falls back to the BARE BOARD — the answer that cannot write into EADDR.
    check(Hl2HardwareOptions::clampCodec(3) == Hl2HardwareOptions::Codec::None, "codec 3 -> None");
    check(Hl2HardwareOptions::clampCodec(-1) == Hl2HardwareOptions::Codec::None, "codec -1 -> None");

    check(Hl2HardwareOptions::clampFilterBoard(0) == Hl2HardwareOptions::FilterBoard::None,
          "filter board 0");
    check(Hl2HardwareOptions::clampFilterBoard(1) == Hl2HardwareOptions::FilterBoard::N2adrRxTx,
          "filter board 1");
    check(Hl2HardwareOptions::clampFilterBoard(2) == Hl2HardwareOptions::FilterBoard::N2adrTxOnly,
          "filter board 2");
    // NOTE THE DIFFERENT FALLBACK, and it is the point of this pair of checks:
    // an unrecognised filter board must NOT become "no board". That would
    // release the relays on a radio that has one, leaving the receiver hearing
    // the whole of HF through a bypassed front end.
    check(Hl2HardwareOptions::clampFilterBoard(7) == Hl2HardwareOptions::FilterBoard::N2adrRxTx,
          "filter board 7 -> receive+transmit, NOT None");
    check(Hl2HardwareOptions::clampFilterBoard(-4) == Hl2HardwareOptions::FilterBoard::N2adrRxTx,
          "filter board -4 -> receive+transmit, NOT None");
}

// ---------------------------------------------------------------------------
// The config register's C3.
// ---------------------------------------------------------------------------
static void testConfigDitherRandom()
{
    check(ccConfig(SampleRate::R48k, 1, kOcNone, false, false)[3] == 0x00, "C3 clear");
    check(ccConfig(SampleRate::R48k, 1, kOcNone, true, false)[3] == 0x08, "C3 dither = 0x08");
    check(ccConfig(SampleRate::R48k, 1, kOcNone, false, true)[3] == 0x10, "C3 random = 0x10");
    check(ccConfig(SampleRate::R48k, 1, kOcNone, true, true)[3] == 0x18, "C3 both");

    // The default is CLEAR, so every existing caller that omits the arguments
    // keeps the byte this encoder emitted before the parameters existed.
    check(ccConfig(SampleRate::R48k, 1, kOcNone)[3] == 0x00, "C3 defaults clear");

    // And it must disturb NOTHING else in the register — the sample rate, the
    // receiver count and the filter byte share it, and a dither change that
    // moved any of them would be a band change or a DDC-rate change nobody
    // asked for.
    const Cc off = ccConfig(SampleRate::R192k, 4, kOcLpf30_20, false, false);
    const Cc on  = ccConfig(SampleRate::R192k, 4, kOcLpf30_20, true, true);
    check(off[0] == on[0] && off[1] == on[1] && off[2] == on[2] && off[4] == on[4],
          "dither/random touch C3 only");
}

// ---------------------------------------------------------------------------
// The ATU tune request on the drive register.
// ---------------------------------------------------------------------------
static void testAtuBit()
{
    // PA enable is 0x09[19] = C2 bit 3; the tune request is 0x09[20] = C2 bit 4.
    check(ccTxDrive(120, false, false)[2] == 0x00, "no PA, no tune");
    check(ccTxDrive(120, true,  false)[2] == 0x08, "PA only");
    check(ccTxDrive(120, false, true)[2]  == 0x10, "tune only");
    check(ccTxDrive(120, true,  true)[2]  == 0x18, "PA and tune");
    // Defaulted off: starting an antenna tuner is never something a caller
    // should get by omission.
    check(ccTxDrive(120, true)[2] == 0x08, "tune defaults off");
    // DATA[18] — "an external tuner is in charge" — must stay clear, or two
    // tuners are asked to start at once.
    check((ccTxDrive(120, true, true)[2] & 0x04) == 0, "DATA[18] stays clear");
    // The drive level is untouched by either bit.
    check(ccTxDrive(200, true, true)[1] == 200, "drive level intact");
}

// ---------------------------------------------------------------------------
// The EP2 audio slot.
// ---------------------------------------------------------------------------
static void testEp2Audio()
{
    // Sample layout is audio(4) then I(2) Q(2), 63 samples per 512-byte frame,
    // two frames per packet. The first payload byte of frame 0 is at
    // 8 (packet header) + 3 (sync) + 5 (C&C) = 16.
    constexpr std::size_t kFrame0 = 16;
    constexpr std::size_t kFrame1 = 8 + 512 + 8;

    std::vector<std::int16_t> audio;
    for (int i = 0; i < kTxSamplesPerPacket * 2; ++i)
        audio.push_back(static_cast<std::int16_t>(0x0100 + i));

    auto pkt = ep2Packet(0, ccConfig(SampleRate::R48k), ccRxGain(20));
    ep2WriteTxAudio(pkt, audio);
    check(pkt[kFrame0 + 0] == 0x01 && pkt[kFrame0 + 1] == 0x00, "frame 0 sample 0 left");
    check(pkt[kFrame0 + 2] == 0x01 && pkt[kFrame0 + 3] == 0x01, "frame 0 sample 0 right");
    // 63 samples into the stream is the first sample of frame 1: L = 0x0100+126.
    check(pkt[kFrame1 + 0] == 0x01 && pkt[kFrame1 + 1] == 0x7E, "frame 1 sample 0 left");
    // The IQ half of the same sample is untouched.
    check(pkt[kFrame0 + 4] == 0 && pkt[kFrame0 + 5] == 0
       && pkt[kFrame0 + 6] == 0 && pkt[kFrame0 + 7] == 0, "IQ half untouched by the audio write");

    // AN EMPTY SPAN MUST LEAVE THE SLOT AT ZERO. The first word of each frame
    // is EADDR on a bare HL2; underrun therefore has to degrade TOWARD that
    // safe state rather than toward a stale repeat.
    auto silent = ep2Packet(0, ccConfig(SampleRate::R48k), ccRxGain(20));
    ep2WriteTxAudio(silent, {});
    check(silent[kFrame0 + 0] == 0 && silent[kFrame0 + 1] == 0
       && silent[kFrame0 + 2] == 0 && silent[kFrame0 + 3] == 0,
          "empty audio leaves EADDR zero");

    // A SHORT span fills what it can and leaves the rest zero.
    auto part = ep2Packet(0, ccConfig(SampleRate::R48k), ccRxGain(20));
    std::vector<std::int16_t> two{0x1234, 0x5678};
    ep2WriteTxAudio(part, two);
    check(part[kFrame0 + 0] == 0x12 && part[kFrame0 + 1] == 0x34, "short span: first sample written");
    check(part[kFrame0 + 8] == 0 && part[kFrame0 + 9] == 0, "short span: the rest stays zero");

    // Audio and IQ compose: the two write disjoint halves of every sample, and
    // a transmitting codec radio needs both in the same packet.
    auto both = ep2Packet(0, ccConfig(SampleRate::R48k), ccRxGain(20));
    std::vector<std::complex<float>> iq(kTxSamplesPerPacket, {1.0f, -1.0f});
    ep2WriteTxIq(both, iq);
    ep2WriteTxAudio(both, audio);
    check(both[kFrame0 + 0] == 0x01 && both[kFrame0 + 1] == 0x00, "composed: audio intact");
    check(both[kFrame0 + 4] == 0x7F && both[kFrame0 + 5] == 0xFF, "composed: I = +32767");
    check(both[kFrame0 + 6] == 0x80 && both[kFrame0 + 7] == 0x01, "composed: Q = -32767");
}

int main()
{
    testDitherMeaning();
    testCodecChangeSeed();
    testFilterBoard();
    testSpeakerLevel();
    testClamps();
    testConfigDitherRandom();
    testAtuBit();
    testEp2Audio();
    if (g_failures == 0)
        std::printf("hl2_hardware_options_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
