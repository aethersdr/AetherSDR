#include "core/tnc/HdlcCodec.h"

#include "bitstream.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace lm = aether_libmodem_core;
using AetherSDR::HdlcCodec;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Feed a vector of NRZI bits into the codec and return the number of
// complete-frame transitions.
struct FeedResult {
    int frames{0};
    int goodFcs{0};
};

FeedResult feedBits(HdlcCodec& codec, const std::vector<uint8_t>& nrzi)
{
    FeedResult r;
    for (uint8_t bit : nrzi) {
        if (codec.processBit(bit)) {
            ++r.frames;
            if (codec.fcsValid())
                ++r.goodFcs;
        }
    }
    return r;
}

// Build a known AX.25 packet using libmodem and NRZI-encode it.
// Returns the NRZI bitstream (one byte per bit, values 0/1).
std::vector<uint8_t> makeNrziBitstream(const lm::packet& pkt,
                                       int preamble = 10,
                                       int postamble = 3)
{
    auto bs = lm::ax25::encode_bitstream(pkt, preamble, postamble);
    return bs;  // encode_bitstream already returns NRZI-encoded bits
}

// ── Tests ─────────────────────────────────────────────────────────────────────

void testSingleFrame()
{
    lm::packet pkt("N0CALL-9", "APRS", {"WIDE1-1", "WIDE2-1"}, ">Test payload 123");
    auto nrzi = makeNrziBitstream(pkt);

    HdlcCodec codec;
    auto r = feedBits(codec, nrzi);

    report("single frame: exactly one complete", r.frames == 1);
    report("single frame: FCS valid", r.goodFcs == 1);
}

void testFrameContents()
{
    lm::packet pkt("W5XD", "APRS", {}, ">Hello world");
    auto nrzi = makeNrziBitstream(pkt);

    HdlcCodec codec;
    bool gotFrame = false;
    for (uint8_t bit : nrzi) {
        if (codec.processBit(bit)) {
            gotFrame = true;
            break;
        }
    }

    report("frame contents: complete flag set", gotFrame && codec.complete());
    report("frame contents: FCS valid", codec.fcsValid());
    report("frame contents: at least 15 bytes", codec.frameSize() >= 15);

    // Verify via libmodem's AX.25 parser (passing frame without last 2 FCS bytes)
    if (codec.fcsValid() && codec.frameSize() >= 2) {
        lm::packet decoded;
        bool ok = lm::ax25::try_decode_frame_no_fcs(
            std::span<const uint8_t>(codec.frameData(), codec.frameSize() - 2),
            decoded);
        report("frame contents: libmodem parses callsigns", ok && decoded.from == "W5XD");
        report("frame contents: libmodem parses data",
               ok && decoded.data == ">Hello world");
    } else {
        report("frame contents: libmodem parses callsigns", false);
        report("frame contents: libmodem parses data", false);
    }
}

void testFcsMatchesLibmodem()
{
    lm::packet pkt("K5PTB", "BEACON", {"WIDE1-1"}, ">CRC cross-check");
    auto nrzi = makeNrziBitstream(pkt);

    // Break immediately when the frame closes so complete()/fcsValid() are
    // still true (they are reset at the start of the *next* processBit call).
    HdlcCodec codec;
    bool gotFrame = false;
    for (uint8_t bit : nrzi) {
        if (codec.processBit(bit)) {
            gotFrame = true;
            break;
        }
    }

    if (!gotFrame || !codec.fcsValid()) {
        report("FCS cross-check: codec decoded frame", false);
        return;
    }
    report("FCS cross-check: codec decoded frame", true);

    // libmodem's compute_crc over the frame body (excluding last 2 FCS bytes)
    auto span = std::span<const uint8_t>(codec.frameData(), codec.frameSize() - 2);
    auto libmodemFcs = lm::ax25::compute_crc(span.begin(), span.end());

    report("FCS cross-check: CRC low byte matches libmodem",
           codec.expectedFcs()[0] == libmodemFcs[0]);
    report("FCS cross-check: CRC high byte matches libmodem",
           codec.expectedFcs()[1] == libmodemFcs[1]);
    report("FCS cross-check: actual FCS == expected FCS",
           codec.actualFcs() == codec.expectedFcs());
}

void testBadFcs()
{
    // Corrupt a bit in the middle of the bitstream and verify FCS fails.
    lm::packet pkt("N0CALL", "APRS", {}, ">Corruption test");
    auto nrzi = makeNrziBitstream(pkt);

    // Flip a bit roughly in the middle (well away from preamble/postamble flags).
    size_t mid = nrzi.size() / 2;
    nrzi[mid] ^= 1;

    HdlcCodec codec;
    int frames = 0;
    int badFcs = 0;
    for (uint8_t bit : nrzi) {
        if (codec.processBit(bit)) {
            ++frames;
            if (!codec.fcsValid())
                ++badFcs;
        }
    }

    // We expect at least one complete frame (may not decode as valid AX.25)
    // but the FCS should be wrong.
    report("bad FCS: at least one frame candidate", frames >= 1);
    report("bad FCS: no valid FCS", badFcs == frames);
}

void testTwoConsecutiveFrames()
{
    lm::packet p1("AA1BB", "APRS", {}, ">First");
    lm::packet p2("CC3DD", "APRS", {}, ">Second");

    // Encode p1 normally, then encode p2 starting from the NRZI level where
    // p1 left off.  Without this, bs2 starts at level 0 while the codec's
    // prevNrzi is whatever bs1 ended on, causing mis-decoded first bits.
    auto bs1 = lm::ax25::encode_bitstream(p1, 5, 2);
    const uint8_t finalLevel = bs1.empty() ? 0 : bs1.back();
    auto bs2 = lm::ax25::encode_bitstream(p2, finalLevel, 5, 2);

    HdlcCodec codec;
    FeedResult r1 = feedBits(codec, bs1);
    FeedResult r2 = feedBits(codec, bs2);

    report("two frames: first decoded", r1.goodFcs == 1);
    report("two frames: second decoded", r2.goodFcs == 1);
}

void testPreambleCount()
{
    lm::packet pkt("N0CALL", "APRS", {}, ">preamble count");
    auto nrzi = makeNrziBitstream(pkt, 10, 3);

    HdlcCodec codec;
    for (uint8_t bit : nrzi)
        codec.processBit(bit);

    // Preamble count should be >= 10 (the requested preamble flags).
    report("preamble count >= 10", codec.preambleCount() >= 10);
}

void testReset()
{
    lm::packet pkt("N0CALL", "APRS", {}, ">Reset test");
    auto nrzi = makeNrziBitstream(pkt);

    HdlcCodec codec;
    feedBits(codec, nrzi);

    codec.reset();

    report("reset: searching after reset", codec.searching());
    report("reset: not complete after reset", !codec.complete());
    report("reset: zero frame size after reset", codec.frameSize() == 0);

    // Should decode again after reset.
    auto r = feedBits(codec, nrzi);
    report("reset: decodes again after reset", r.goodFcs == 1);
}

void testStateTransitions()
{
    HdlcCodec codec;
    report("initial state: searching", codec.searching());
    report("initial state: not in preamble", !codec.inPreamble());
    report("initial state: not in frame", !codec.inFrame());
    report("initial state: not complete", !codec.complete());

    // Feed one preamble flag: 0x7E = 0,1,1,1,1,1,1,0 (NRZ LSB-first)
    // NRZI encode it starting from level 0.
    uint8_t level = 0;
    const uint8_t nrzFlag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    for (int i = 0; i < 8; ++i) {
        if (nrzFlag[i] == 0)
            level ^= 1;
        codec.processBit(level);
    }
    report("after one flag: in preamble", codec.inPreamble());
}

void testLongPayload()
{
    // 256-byte payload — exercises the frame buffer boundary
    std::string data(256, 'X');
    data[0] = '>';
    lm::packet pkt("N0CALL", "APRS", {}, data);
    auto nrzi = makeNrziBitstream(pkt);

    HdlcCodec codec;
    auto r = feedBits(codec, nrzi);
    report("long payload: decoded", r.goodFcs == 1);
}

} // namespace

int main()
{
    testStateTransitions();
    testSingleFrame();
    testFrameContents();
    testFcsMatchesLibmodem();
    testBadFcs();
    testTwoConsecutiveFrames();
    testPreambleCount();
    testReset();
    testLongPayload();

    if (g_failed == 0)
        std::printf("All tests passed.\n");
    else
        std::printf("%d test(s) FAILED.\n", g_failed);

    return g_failed ? 1 : 0;
}
