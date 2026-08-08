// IcomCIV Phase 3 — audio payload test. Pins the codec round trips, the refusal
// of codecs we cannot decode, and the unequal 1364 + 556 transmit split.
//
// Pure protocol: no sockets, no Qt, no hardware.

#include "core/backends/icom/IcomAudio.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void testCodecSupport()
{
    check(codecSupported(AudioCodec::Lpcm1ch16), "LPCM 1ch 16-bit is what we negotiate");
    check(codecSupported(AudioCodec::ULaw1ch8), "uLaw is available for constrained links");
    // Requesting a codec we cannot decode and then feeding the bytes through
    // the LPCM path produces FULL-SCALE NOISE into the operator's headphones.
    // Refusing at negotiation is the only safe behaviour.
    check(!codecSupported(AudioCodec::Opus1ch), "Opus is refused, not mis-decoded");
    check(!codecSupported(AudioCodec::Adpcm1ch), "ADPCM is refused, not mis-decoded");

    // And an unsupported codec yields NO samples rather than garbage.
    const std::vector<std::uint8_t> bytes(64, 0x7F);
    check(decodeAudio(AudioCodec::Opus1ch, bytes).empty(),
          "decoding an unsupported codec produces nothing, never noise");
}

static void testLpcm16RoundTrip()
{
    std::vector<float> in{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
    auto bytes = encodeAudio(AudioCodec::Lpcm1ch16, in);
    check(bytes.size() == in.size() * 2, "16-bit is two bytes per sample");
    // Little-endian on the wire.
    check(bytes[0] == 0x00 && bytes[1] == 0x00, "silence encodes to zero");

    auto out = decodeAudio(AudioCodec::Lpcm1ch16, bytes);
    check(out.size() == in.size(), "sample count survives");
    for (std::size_t i = 0; i < in.size(); ++i)
        check(std::fabs(out[i] - in[i]) < 1e-4f, "LPCM16 round-trips within a quantum");

    // A truncated payload must drop the half sample, not synthesise one from
    // half a word — that would be a full-scale click.
    bytes.pop_back();
    auto truncated = decodeAudio(AudioCodec::Lpcm1ch16, bytes);
    check(truncated.size() == in.size() - 1, "an odd trailing byte is dropped");
}

static void testEightBitAndULaw()
{
    // 8-bit LPCM is UNSIGNED, centred on 128. Reading it as signed gives audio
    // inverted around the midpoint, which sounds like heavy distortion rather
    // than like a decode error — so it would get blamed on the radio.
    const std::vector<std::uint8_t> mid{128};
    check(std::fabs(decodeAudio(AudioCodec::Lpcm1ch8, mid)[0]) < 1e-6f,
          "8-bit LPCM is unsigned, centred on 128");
    const std::vector<std::uint8_t> top{255};
    check(decodeAudio(AudioCodec::Lpcm1ch8, top)[0] > 0.9f, "and 255 is full positive");

    std::vector<float> in{0.0f, 0.5f, -0.5f, 0.9f};
    auto encoded = encodeAudio(AudioCodec::ULaw1ch8, in);
    check(encoded.size() == in.size(), "uLaw is one byte per sample");
    auto out = decodeAudio(AudioCodec::ULaw1ch8, encoded);
    for (std::size_t i = 0; i < in.size(); ++i)
        check(std::fabs(out[i] - in[i]) < 0.05f, "uLaw round-trips within its own precision");
}

static void testTxSplit()
{
    TxPacketizer tx;
    check(tx.takeFrame().empty(), "nothing to send before any audio arrives");

    // A short submission must NOT produce a short frame — the radio's jitter
    // buffer reads one as a discontinuity.
    tx.submit(std::vector<float>(100, 0.1f));
    check(tx.takeFrame().empty(), "a partial frame is held, not sent short");

    // 960 samples == 1920 bytes == exactly one 20 ms frame at 48 kHz mono s16.
    TxPacketizer full;
    full.submit(std::vector<float>(960, 0.25f));
    auto chunks = full.takeFrame();
    check(chunks.size() == 2, "a 20 ms frame is split across TWO packets");
    check(chunks[0].bytes.size() == kAudioSplitLarge, "the first is 1364 bytes");
    check(chunks[1].bytes.size() == kAudioSplitSmall, "the second is 556 bytes");
    check(chunks[0].bytes.size() + chunks[1].bytes.size() == kAudioFrameBytes,
          "and together they are exactly 1920 bytes");
    check(full.pendingBytes() == 0, "the frame is consumed");
    check(full.takeFrame().empty(), "and not re-emitted");

    // The split is UNEQUAL, so the two packets do not represent 10 ms each —
    // they carry 14.2 ms and 5.8 ms. Anything deriving timing from packet size
    // will drift, which is why nothing here timestamps.
    check(kAudioSplitLarge != kAudioSplitSmall, "the split is deliberately unequal");
}

static void testTxOverflowAndFlush()
{
    TxPacketizer tx;
    // Far more than the cap. Overflow must drop the OLDEST: on transmit the
    // freshest audio is the one that matters, and keeping a backlog just means
    // the operator's voice arrives late and keeps getting later.
    tx.submit(std::vector<float>(40000, 0.5f));
    check(tx.pendingBytes() <= TxPacketizer::kMaxPendingBytes, "the queue is capped");

    // Flush on unkey: whatever is queued belongs to the transmission that ended.
    tx.flush();
    check(tx.pendingBytes() == 0, "flush discards everything pending");
    check(tx.takeFrame().empty(), "and nothing is emitted afterwards");
}

static void testLossConcealment()
{
    RxAssembler rx;
    // 10 ms per packet is the AVERAGE across the unequal pair, and averaging is
    // the right use of it here — we are filling a hole whose true length we
    // cannot know.
    auto fill = rx.concealLoss(3, 48000);
    check(fill.size() == 3 * 480, "three lost packets become ~30 ms of silence");
    check(fill[0] == 0.0f, "and it is silence, not a repeat");
    check(rx.concealLoss(0, 48000).empty(), "no loss, no fill");
    check(rx.concealLoss(-1, 48000).empty(), "a negative count is not a giant allocation");
}

int main()
{
    testCodecSupport();
    testLpcm16RoundTrip();
    testEightBitAndULaw();
    testTxSplit();
    testTxOverflowAndFlush();
    testLossConcealment();

    if (g_failures == 0)
        std::printf("icom_audio_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
