// WHAT AN EP2 FRAME IS ALLOWED TO CARRY. Two properties of the same packet
// builder, kept in one target because they are asserted on the same bytes:
//
//   sections 1-4  a band change must not put a stale sample rate in the later
//                 of an EP2 frame's two C&C banks (aethersdr/AetherSDR#4579);
//   section 5     the frame's audio slot stays untouched unless a codec has
//                 been DECLARED, because on a bare HL2 that slot is the
//                 extended address register.
//
// SOCKET-FREE ON PURPOSE. It drives MetisClient's own packet builder, which is
// public for exactly this reason ("Exists so the gate can be tested on the exact
// bytes that would go out"), rather than standing up a fake radio -- the
// fake-radio fixtures for this path are retired, and are kept in tests.cmake
// only as a bracket comment.
//
// WHAT IS UNDER TEST. buildNextControlPacket() puts LIVE m_ccConfig in bank A of
// every frame; a one-shot only ever fills bank B; and the radio applies bank B
// after bank A. So a COPY of m_ccConfig queued as a one-shot is both redundant
// -- bank A already carried the change -- and, because it is a snapshot, able to
// overwrite the live value for one frame when something else rebuilds the config
// register in between.
//
// WHAT IT DOES NOT TEST. That the radio really applies the second sub-frame last
// is a protocol fact read from the HPSDR frame layout, not something measured
// here: no radio is involved and nothing is keyed. The assertions below do not
// depend on it -- they require that no frame carry two config banks at all, so
// there is no second one to win or lose.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QByteArray>
#include <QCoreApplication>

#include <array>
#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "[ OK ] %s\n", what); }
}

using Ep2 = std::array<std::uint8_t, kUsbPacketSize>;

namespace AetherSDR::hl2 {
// The friend seam MetisClient.h declares, for one question section 5 cannot
// answer from the bytes: how much audio is SITTING IN the queue. Without it the
// three codec gates are only pinned together — @on8st removed each on its own
// and all three stayed green, because the other two mask it (#5867 review).
// Reading the queue makes the producer gate and the drain load-bearing
// individually, and leaves the packet builder's gate as the backstop it is.
struct MetisClientTestAccess {
    static std::size_t speakerQueued(const MetisClient& c) { return c.m_speakerAudio.size(); }
    // Withdraw the codec WITHOUT the drain setLocalCodec() performs, to produce
    // the one state the packet builder's own gate says it exists for: "a queue
    // that filled by mistake must still not reach the wire". Production cannot
    // reach it — which is exactly why the gate is otherwise untestable.
    static void forgetCodecWithoutDraining(MetisClient& c) { c.m_params.hasCodec = false; }
};
}  // namespace AetherSDR::hl2

// C&C sits SYNC(3) into each 512-byte frame. Frame 0 is bank A, frame 1 bank B.
static const std::uint8_t* bank(const Ep2& pkt, int which)
{
    return pkt.data() + (which == 0 ? 8 : 8 + kFrameSize) + 3;
}
// MOX rides C0 bit 0 of every bank, so it is masked off before the address is read.
static bool isConfigBank(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit) == kC0Config;
}
static int rateCodeOf(const std::uint8_t* cc) { return cc[1] & 0x03; }
// Open-collector outputs are C2[7:1] -- the one-bit shift ccConfig() applies.
static std::uint8_t ocByteOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[2] >> 1);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. bank A alone already carries the band change ----
    //
    // This pins the PREMISE of the fix rather than the fix: it passes with the
    // one-shot push present too. If it ever fails, deleting the push stops being
    // safe, so it is the assertion that has to hold for the rest to be honest.
    {
        MetisClient c;
        const Ep2 before = c.buildNextControlPacket();
        check(isConfigBank(bank(before, 0)), "bank A is the config register on every frame");
        check(ocByteOf(bank(before, 0)) == kOcNone, "no relay engaged before the band change");

        c.setBandFilter(kOcLpf80);
        const Ep2 after = c.buildNextControlPacket();
        check(isConfigBank(bank(after, 0)), "bank A is still the config register after the change");
        check(ocByteOf(bank(after, 0)) == kOcLpf80,
              "the new relay pattern reaches the wire on the very next frame, from bank A alone");
    }

    // ---- 2. the defect: a band change then a rate change, no frame in between ----
    {
        MetisClient c;                          // Params::sampleRate defaults to R48k
        c.setBandFilter(kOcLpf80);              // a snapshot taken here holds 48k
        c.setSampleRate(SampleRate::R192k);     // the live config register moves to 192k

        const Ep2 pkt = c.buildNextControlPacket();
        const std::uint8_t* a = bank(pkt, 0);
        const std::uint8_t* b = bank(pkt, 1);

        check(isConfigBank(a) && rateCodeOf(a) == static_cast<int>(SampleRate::R192k),
              "bank A carries the live sample rate");
        check(!isConfigBank(b),
              "bank B is not a second config bank, so nothing can overwrite bank A");
        if (isConfigBank(b)) {
            std::fprintf(stderr,
                         "       bank A rate code %d, bank B rate code %d"
                         " -- two config banks in one frame, and the radio ends on the later\n",
                         rateCodeOf(a), rateCodeOf(b));
        }
    }

    // ---- 3. and none appears in the frames that follow ----
    {
        MetisClient c;
        c.setBandFilter(kOcLpf80);
        c.setSampleRate(SampleRate::R192k);
        bool sawSecondConfig = false;
        for (int i = 0; i < 8; ++i) {
            const Ep2 pkt = c.buildNextControlPacket();
            sawSecondConfig = sawSecondConfig || isConfigBank(bank(pkt, 1));
        }
        check(!sawSecondConfig, "no frame in the next eight carries a second config bank");
    }

    // ---- 4. the cross-session half ----
    //
    // m_oneShot is cleared nowhere -- not in start() alongside m_txSeq,
    // m_roundRobin, m_haveRxSeq, m_drops and m_linkUp -- and stop() deliberately
    // preserves everything that is not an unfinished IO-board write. So a bank
    // queued by a band change while disconnected would ride the next session's
    // first frames. Queuing nothing is what closes that here.
    {
        MetisClient c;
        c.setBandFilter(kOcLpf80);
        c.stop();
        bool survived = false;
        for (int i = 0; i < 4; ++i) {
            const Ep2 pkt = c.buildNextControlPacket();
            survived = survived || isConfigBank(bank(pkt, 1));
        }
        check(!survived, "nothing a disconnected band change queued survives into the next session");
    }

    // ---- 5. the EP2 audio slot is gated on a DECLARED codec ----
    //
    // On a bare Hermes-Lite 2 the four bytes ep2WriteTxAudio() writes are not
    // audio: the first word of each frame's payload is the extended address
    // register, so a sample landing there is a register command. The gate in
    // buildNextControlPacket() is therefore `m_params.hasCodec`, not "do we
    // happen to have audio queued" -- and until now nothing asserted it. The
    // decision cannot be seen from Hl2HardwareOptions, which is where the
    // policy lives; it can only be seen on the bytes, which is here.
    {
        // The audio half of a sample slot is payload[k+0..3]; the IQ half is
        // [k+4..7]. Slot 0 of frame 0 is the one that is also EADDR.
        const auto audioHalf = [](const Ep2& pkt, int frame, std::size_t slot) {
            const std::size_t fs = (frame == 0 ? 8u : 8u + kFrameSize);
            return pkt.data() + fs + 8 + slot * kTxSampleBytes;
        };
        const auto audioSlotsAllZero = [&](const Ep2& pkt) {
            for (int f = 0; f < 2; ++f) {
                for (std::size_t slot = 0; slot < kFramePayload / kTxSampleBytes; ++slot) {
                    const std::uint8_t* p = audioHalf(pkt, f, slot);
                    if (p[0] || p[1] || p[2] || p[3])
                        return false;
                }
            }
            return true;
        };

        // Loud, and deliberately not symmetric, so a swapped channel or a
        // half-written sample cannot pass as silence.
        QByteArray block;
        for (int i = 0; i < kTxSamplesPerPacket; ++i) {
            const std::int16_t l = 0x4321;
            const std::int16_t r = 0x1234;
            block.append(reinterpret_cast<const char*>(&l), sizeof(l));
            block.append(reinterpret_cast<const char*>(&r), sizeof(r));
        }

        {
            MetisClient c;                       // Params::hasCodec defaults false
            c.submitSpeakerAudio(block);
            check(MetisClientTestAccess::speakerQueued(c) == 0,
                  "no declared codec: the audio is refused at the PRODUCER — it "
                  "never enters the queue, so the packet builder's gate is a "
                  "backstop rather than the only thing standing between a bare "
                  "HL2 and a register write");
            const Ep2 pkt = c.buildNextControlPacket();
            check(audioSlotsAllZero(pkt),
                  "no declared codec: not one audio byte reaches the EADDR slot");
        }

        {
            MetisClient c;
            c.setLocalCodec(true);
            c.submitSpeakerAudio(block);
            const Ep2 pkt = c.buildNextControlPacket();
            const std::uint8_t* p = audioHalf(pkt, 0, 0);
            check(p[0] == 0x43 && p[1] == 0x21 && p[2] == 0x12 && p[3] == 0x34,
                  "declared codec: the sample reaches the audio slot, L then R, big-endian");
            check(p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0,
                  "and the IQ half of the same slot is untouched");
        }

        // WITHDRAWING THE CODEC MUST DRAIN WHAT IS ALREADY QUEUED. Otherwise a
        // page that corrected a wrong codec declaration would keep feeding the
        // extended-address register from a queue filled while it was wrong.
        {
            MetisClient c;
            c.setLocalCodec(true);
            c.submitSpeakerAudio(block);
            c.setLocalCodec(false);
            check(MetisClientTestAccess::speakerQueued(c) == 0,
                  "codec withdrawn: the queue is DRAINED, not merely gated — the "
                  "samples that were legal a moment ago are gone");
            const Ep2 pkt = c.buildNextControlPacket();
            check(audioSlotsAllZero(pkt),
                  "codec withdrawn: the audio queued while it was declared does not leak out");
        }

        // THE BACKSTOP, on the state it was written for. The gate in
        // buildNextControlPacket() is a DECLARED codec rather than "do we happen
        // to have audio queued", and its comment says why: a queue that filled by
        // mistake must still not reach the wire. Nothing in production can
        // produce that state — setLocalCodec(false) drains — so it is produced
        // here, or the gate is pinned by nothing.
        {
            MetisClient c;
            c.setLocalCodec(true);
            c.submitSpeakerAudio(block);
            check(MetisClientTestAccess::speakerQueued(c) > 0, "audio is queued");
            MetisClientTestAccess::forgetCodecWithoutDraining(c);
            const Ep2 pkt = c.buildNextControlPacket();
            check(audioSlotsAllZero(pkt),
                  "a queue that filled by mistake still does not reach the wire — "
                  "the packet builder gates on the DECLARATION, not on the queue");
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_band_filter_frame_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
