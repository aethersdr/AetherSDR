// A band change must not put a stale sample rate in the later of an EP2 frame's
// two C&C banks. (aethersdr/AetherSDR#4579)
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

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_band_filter_frame_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
