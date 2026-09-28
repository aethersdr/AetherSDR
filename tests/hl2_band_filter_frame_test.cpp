// WHAT AN EP2 FRAME IS ALLOWED TO CARRY. Two properties of the same packet
// builder, kept in one target because they are asserted on the same bytes:
//
//   sections 1-4  a band change must not put a stale sample rate in the later
//                 of an EP2 frame's two C&C banks (aethersdr/AetherSDR#4579);
//   section 5     the frame's audio slot stays untouched unless a codec has
//                 been DECLARED, because on a bare HL2 that slot is the
//                 extended address register;
//   section 6     a live-control setter called while STOPPED queues nothing,
//                 so the first frames of the next session are its own
//                 start-up state and not a bank left over from before it
//                 (the remainder of #4579).
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
    // Section 6. How many one-shot banks are waiting, read directly: the bytes
    // alone cannot tell a queued RX1 bank from the rotation's own RX1 slot.
    static std::size_t oneShotQueued(const MetisClient& c) { return c.m_oneShot.size(); }
    // The session boundary without a socket, as hl2_tx_gate_test does it. What
    // start() adds beyond m_running is Params, the counters and the wire; it
    // does not touch m_oneShot, so for the question section 6 asks -- what is
    // already queued when the first frame is built -- this is the same state.
    static void setStreaming(MetisClient& c) { c.m_running = true; }
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
static std::uint8_t addrOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit);
}
static std::uint32_t payloadOf(const std::uint8_t* cc)
{
    return (std::uint32_t(cc[1]) << 24) | (std::uint32_t(cc[2]) << 16)
         | (std::uint32_t(cc[3]) << 8) | std::uint32_t(cc[4]);
}
// 0x09 C2: bit 3 is the PA enable (DATA[19]), bit 4 the ATU tune request (DATA[20]).
static bool paEnabledIn(const std::uint8_t* cc) { return (cc[2] & 0x08) != 0; }
static bool tuneRequestedIn(const std::uint8_t* cc) { return (cc[2] & 0x10) != 0; }
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

        // A zero speaker level stops new submissions in Hl2Backend, but audio
        // already queued would otherwise remain audible until the pacer drains
        // it. Clearing the queue must silence the very next packet.
        {
            MetisClient c;
            c.setLocalCodec(true);
            c.submitSpeakerAudio(block);
            check(MetisClientTestAccess::speakerQueued(c) > 0,
                  "speaker audio is queued before the level reaches zero");
            c.clearSpeakerAudio();
            check(MetisClientTestAccess::speakerQueued(c) == 0,
                  "silencing the speaker discards audio already queued");
            check(audioSlotsAllZero(c.buildNextControlPacket()),
                  "the next packet is silent after the speaker queue is cleared");
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

    // ---- 6. a setter called while stopped reaches no later session ----
    //
    // #4579's remainder. m_oneShot is cleared by neither start() nor stop() --
    // stop() on purpose keeps "unrelated one-shot setup" -- and four setters
    // pushed into it with no m_running guard: setRxFrequencyHz,
    // setTxFrequencyHz, setTxDriveLevel and setAtuTuneRequest. Whatever they
    // queued while the radio was stopped went out in the next session's
    // priming burst, ahead of anything that session asserted for itself. The
    // drive bank carries the PA enable and the ATU bank the tune request, and
    // start() clears m_atuTune precisely because "re-asserting a tune nobody
    // asked for would start one".
    //
    // Guarded at the push, not cleared at start(): see
    // MetisClient::queueOneShotIfRunning().
    {
        constexpr std::uint32_t kRxHz = 14'074'000;
        constexpr std::uint32_t kTxHz = 14'076'000;
        constexpr int kDrive = 200;

        MetisClient c;                          // never started: m_running is false
        c.enableTransmit(true);                 // so the drive is not clamped to 0
        c.setRxFrequencyHz(kRxHz);
        c.setTxFrequencyHz(kTxHz);
        c.setTxDriveLevel(kDrive);
        c.setAtuTuneRequest(true);
        check(MetisClientTestAccess::oneShotQueued(c) == 0,
              "stopped: the four live-control setters queue no one-shot bank");

        // The next session. Its rotation is RX1 NCO, gain, ADC assignment
        // (one receiver), so the first frames' bank B must be exactly that.
        MetisClientTestAccess::setStreaming(c);
        const Ep2 first = c.buildNextControlPacket();
        const Ep2 second = c.buildNextControlPacket();
        check(addrOf(bank(first, 1)) == kC0Rx1Freq && payloadOf(bank(first, 1)) == kRxHz,
              "the first frame of the next session carries the new RX1 frequency");
        check(addrOf(bank(second, 1)) == kC0AdcGain,
              "and the second is the rotation's gain slot -- the frequency came "
              "from the rotation, not from a one-shot queued while stopped");

        bool sawTxFreq = false, sawDrive = false, sawPa = false, sawTune = false;
        const Ep2* firstTwo[2] = {&first, &second};
        const auto scan = [&](const Ep2& pkt) {
            for (int w = 0; w < 2; ++w) {
                const std::uint8_t* cc = bank(pkt, w);
                sawTxFreq = sawTxFreq || addrOf(cc) == kC0TxFreq;
                if (addrOf(cc) == kC0TxDrive) {
                    sawDrive = true;
                    sawPa = sawPa || paEnabledIn(cc);
                    sawTune = sawTune || tuneRequestedIn(cc);
                }
            }
        };
        for (const Ep2* pkt : firstTwo)
            scan(*pkt);
        for (int i = 0; i < 16; ++i)            // several rotations past the priming burst
            scan(c.buildNextControlPacket());
        check(!sawTxFreq, "no TX frequency bank set while stopped reaches the next session");
        check(!sawDrive, "no drive bank set while stopped reaches the next session");
        check(!sawPa, "the PA enable asked for while stopped is not on the wire");
        check(!sawTune, "the ATU tune request made while stopped is not on the wire");
    }

    // ---- 6b. and the guard is not wider than the stop ----
    //
    // The same four setters on a RUNNING client still go out on the very next
    // frame, ahead of the rotation. Without this, a guard that refused
    // everything would pass section 6.
    {
        constexpr std::uint32_t kRxHz = 7'074'000;
        constexpr std::uint32_t kTxHz = 7'076'000;
        constexpr int kDrive = 120;

        MetisClient c;
        c.enableTransmit(true);
        MetisClientTestAccess::setStreaming(c);

        c.setTxFrequencyHz(kTxHz);
        const Ep2 tx = c.buildNextControlPacket();
        check(addrOf(bank(tx, 1)) == kC0TxFreq && payloadOf(bank(tx, 1)) == kTxHz,
              "running: a TX frequency change is on the next frame");

        c.setTxDriveLevel(kDrive);
        const Ep2 drive = c.buildNextControlPacket();
        check(addrOf(bank(drive, 1)) == kC0TxDrive && bank(drive, 1)[1] == kDrive
                  && paEnabledIn(bank(drive, 1)),
              "running: a drive change is on the next frame, PA enabled");

        c.setAtuTuneRequest(true);
        const Ep2 tune = c.buildNextControlPacket();
        check(addrOf(bank(tune, 1)) == kC0TxDrive && tuneRequestedIn(bank(tune, 1)),
              "running: an ATU tune request is on the next frame");

        // Drain what is left of the queue first, so the RX1 bank below cannot
        // be the rotation's own slot arriving by coincidence of phase.
        check(MetisClientTestAccess::oneShotQueued(c) == 0, "running: nothing else was queued");
        c.setRxFrequencyHz(kRxHz);
        check(MetisClientTestAccess::oneShotQueued(c) == 1,
              "running: an RX frequency change queues exactly one bank");
        const Ep2 rx = c.buildNextControlPacket();
        check(addrOf(bank(rx, 1)) == kC0Rx1Freq && payloadOf(bank(rx, 1)) == kRxHz,
              "running: and it is on the next frame");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_band_filter_frame_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
