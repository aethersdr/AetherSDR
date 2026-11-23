// HL2 receive gain while transmitting (docs/HERMES.md §13 row 22). The claims:
//   - 0x0e carries [15] enable, [14] native mode and the cap's code exactly when
//     the receive gain is above kRxGainDuringTxCapDb and PureSignal is off;
//   - fed to a model of ad9866.v's FAST_LNA block, the AD9866 runs at the cap
//     for exactly the TX time and at the receive gain otherwise, with no host
//     action at either edge, including after a gain change while keyed;
//   - a receive gain at or below the cap sends today's bytes;
//   - nothing else on the wire changes: keyed packets differ only in the
//     0x0a and 0x0e payloads;
//   - the backend applies it to the effective gain (auto RF gain respected),
//     never writes the operator's baseline, band memory or session pin, and
//     the dBm reference follows the gain in force while transmitting;
//   - a T/R edge with a non-zero shortfall drops the averaged pan, and one
//     with no shortfall does not.
// Socket-free: constructed objects, packets from the transport's own builder.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Bands.h"
#include "core/backends/hl2/Hl2RxGainDuringTx.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QMap>
#include <QString>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c) { c.m_running = true; }
};

struct Hl2RxGainDuringTxTestAccess {
    static Cc txLnaBank(Hl2Backend& b)
    {
        Cc out{};
        QMetaObject::invokeMethod(b.m_metis, [&out, m = b.m_metis] {
            out = m->currentTxLnaGainBank();
        }, Qt::BlockingQueuedConnection);
        return out;
    }
    static Cc rxGainBank(Hl2Backend& b)
    {
        Cc out{};
        QMetaObject::invokeMethod(b.m_metis, [&out, m = b.m_metis] {
            out = m->currentGainBank();
        }, Qt::BlockingQueuedConnection);
        return out;
    }
    static void setKeyed(Hl2Backend& b, bool v) { b.m_keyed = v; }
    static void setRadioPtt(Hl2Backend& b, bool v) { b.m_radioPtt = v; }
    static void setPsEnabled(Hl2Backend& b, bool v) { b.m_psEnabled = v; }
    static void setBaseline(Hl2Backend& b, int db) { b.applyLnaGainDb(db); }
    static void setBandGain(Hl2Backend& b, const QString& band, int db)
    {
        b.m_lnaDbByBand.insert(band, db);
    }
    static QMap<QString, int> bandMap(const Hl2Backend& b) { return b.m_lnaDbByBand; }
    static void setSessionPin(Hl2Backend& b, bool v) { b.m_lnaSessionPin = v; }
    static bool sessionPin(const Hl2Backend& b) { return b.m_lnaSessionPin; }
    static void moveTo(Hl2Backend& b, double hz) { b.applyPerBandStateFor(hz, "test"); }
    static QString band(const Hl2Backend& b) { return b.m_currentBandKey; }
    static double refOffset(const Hl2Backend& b) { return b.m_dbRef.offsetDb(); }
    // One receiver whose chain is `dsp`, averaging at `average` (0..100).
    static void oneReceiver(Hl2Backend& b, Hl2RxDsp* dsp, int average)
    {
        b.m_ids.reset(1);
        b.m_rx.assign(1, Hl2Backend::Receiver{});
        b.m_rx[0].dsp = dsp;
        b.m_rx[0].panAverage = average;
    }
    static void releaseReceiver(Hl2Backend& b) { b.m_rx[0].dsp = nullptr; }
    static quint64 drops(const Hl2Backend& b) { return b.m_panAverageDrops; }
    // A key or radio-PTT edge as the backend publishes it.
    static void edge(Hl2Backend& b, bool keyed, bool radioPtt)
    {
        b.m_keyed = keyed;
        b.m_radioPtt = radioPtt;
        b.publishTransmitState();
    }
    // The radio's TX tail ends: the gateware is back on the receive gain.
    static void tailEnds(Hl2Backend& b)
    {
        b.m_txTailPending = true;
        b.finishTxTail();
    }
};

}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;
using Access = AetherSDR::hl2::Hl2RxGainDuringTxTestAccess;
using C = AetherSDR::hl2::MetisClientTestAccess;

namespace {

int g_failures = 0;
void check(bool cond, const char* what)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) {
        ++g_failures;
    }
}

using Packet = std::array<std::uint8_t, kUsbPacketSize>;
constexpr std::size_t kSub1 = 8;
constexpr std::size_t kSub2 = 8 + kFrameSize;

Cc bankAt(const Packet& p, std::size_t sub)
{
    return {static_cast<std::uint8_t>(p[sub + 3] & ~kC0MoxBit), p[sub + 4], p[sub + 5],
            p[sub + 6], p[sub + 7]};
}

// ad9866.v, FAST_LNA generate block (lab snapshot of gateware 883a338, lines
// 131-155): the 0x0a and 0x0e decodes and the per-clock gain choice.
struct GatewareGain {
    int rxCode = 0x1f;
    int txCode = 0x1f;
    bool enTx = false;
    int code = 0x1f;

    static int decode(int field7)
    {
        if (field7 & 0x40) {
            return field7 & 0x3F;
        }
        return (field7 & 0x20) ? (~field7 & 0x3F) : (0x20 | (field7 & 0x1F));
    }
    void write(const Cc& b)
    {
        const std::uint32_t d = (std::uint32_t(b[1]) << 24) | (std::uint32_t(b[2]) << 16)
                              | (std::uint32_t(b[3]) << 8) | std::uint32_t(b[4]);
        const int addr = b[0] >> 1;
        if (addr == 0x0a) {
            rxCode = decode(static_cast<int>(d & 0x7F));
        } else if (addr == 0x0e) {
            txCode = decode(static_cast<int>((d >> 8) & 0x7F));
            enTx = (d & 0x8000) != 0;
        }
    }
    void clock(bool txEn, bool cwOn)
    {
        for (int i = 0; i < 4; ++i) {
            if (rxCode != code && (!txEn || (!enTx && !cwOn))) {
                code = rxCode;
            } else if (txCode != code && txEn && enTx) {
                code = txCode;
            } else if (code != 0 && !enTx && cwOn) {
                code = 0;
            }
        }
    }
    void feed(const Packet& p, bool txEn, bool cwOn = false)
    {
        write(bankAt(p, kSub1));
        write(bankAt(p, kSub2));
        clock(txEn, cwOn);
    }
    int appliedDb() const { return code + kLnaGainMinDb; }
};

void run(MetisClient& c, GatewareGain& g, int packets, bool txEn, bool cwOn = false)
{
    for (int i = 0; i < packets; ++i) {
        g.feed(c.buildNextControlPacket(), txEn, cwOn);
    }
}

void testEncoding()
{
    std::printf("\n  the 0x0e bank\n");
    const Cc zero = ccAdcAssign();
    check(kRxGainDuringTxCapDb == 8, "the cap is +8 dB");
    check(ccTxLnaGain(48, false)
              == Cc{kC0AdcAssignOrTxGain, 0x00, 0x00,
                    static_cast<std::uint8_t>(0xC0 | (8 - kLnaGainMinDb)), 0x00},
          "48 dB: C3 = enable | native | code(+8), every other byte zero");
    check(ccTxLnaGain(kRxGainDuringTxCapDb + 1, false)[3] != 0, "one dB above the cap engages");
    check(ccTxLnaGain(kRxGainDuringTxCapDb, false) == zero, "at the cap: all-zero, as today");
    check(ccTxLnaGain(-12, false) == zero && ccTxLnaGain(0, false) == zero,
          "below the cap: all-zero, as today");
    check(ccTxLnaGain(48, true) == zero, "PureSignal on: all-zero, its 0x0a bank owns TX");
    check(rxGainDuringTxDb(48, false) == 8 && rxGainDuringTxDb(5, false) == 5
              && rxGainDuringTxDb(48, true) == 48,
          "keyed gain is min(receive, cap), PureSignal excepted");
    check(rxGainDuringTxShortfallDb(48, false) == 40 && rxGainDuringTxShortfallDb(5, false) == 0,
          "the shortfall is what the cap takes off");
}

void testEdges(TxTestAuthority& authority)
{
    std::printf("\n  the T/R edge, through the gateware model\n");
    MetisClient c;
    c.setLnaGainDb(48);
    C::setStreaming(c);
    c.enableTransmit(true);
    GatewareGain g;
    run(c, g, 8, false);
    check(g.appliedDb() == 48, "receive: the AD9866 runs at the operator's 48 dB");
    run(c, g, 8, true);
    check(g.appliedDb() == 8, "tx_en high, no host action: the AD9866 drops to +8 dB");
    run(c, g, 8, false);
    check(g.appliedDb() == 48, "tx_en low again: 48 dB restored, no host action");

    c.setMox(true, authority.operation);
    run(c, g, 8, true);
    check(g.appliedDb() == 8, "MOX packets carry the same bank: +8 dB keyed");
    c.setLnaGainDb(30);   // a band change while keyed
    run(c, g, 8, true);
    check(g.appliedDb() == 8, "gain raised to 30 while keyed: still +8 dB until unkey");
    c.setMox(false, authority.operation);
    run(c, g, 8, false);
    check(g.appliedDb() == 30, "unkey after the change: the NEW receive gain, 30 dB");

    c.setMox(true, authority.operation);
    run(c, g, 8, true);
    c.setLnaGainDb(5);    // to a band remembered below the cap
    run(c, g, 8, true);
    check(g.appliedDb() == 5, "gain lowered to 5 while keyed: applied at once, never raised");
    c.setMox(false, authority.operation);
    run(c, g, 8, false);
    check(g.appliedDb() == 5, "unkey: 5 dB, the receive gain");
}

// Every keyed packet after a gain change, not only the state eight packets on:
// across each rotation phase, for one and four receivers, in both directions.
void testKeyedChangePerPacket(TxTestAuthority& authority)
{
    std::printf("\n  a gain change while keyed, packet by packet\n");
    struct Step { int from; int to; const char* what; };
    const Step steps[] = {
        {40, 4, "40 -> 4 dB keyed: never above +8 dB on any packet"},
        {30, 4, "30 -> 4 dB keyed: never above +8 dB on any packet"},
        {5, 30, "5 -> 30 dB keyed: never above +8 dB on any packet"},
        {5, 48, "5 -> 48 dB keyed: never above +8 dB on any packet"},
    };
    for (const Step& s : steps) {
        int worst = -99;
        for (const int numRx : {1, 4}) {
            for (int phase = 0; phase < 6; ++phase) {
                MetisClient c;
                c.setReceiverCount(numRx);
                c.setLnaGainDb(s.from);
                C::setStreaming(c);
                c.enableTransmit(true);
                GatewareGain g;
                run(c, g, 8, false);
                c.setMox(true, authority.operation);
                run(c, g, 8 + phase, true);
                c.setLnaGainDb(s.to);
                for (int i = 0; i < 16; ++i) {
                    run(c, g, 1, true);
                    worst = std::max(worst, g.appliedDb());
                }
                c.setMox(false, authority.operation);
            }
        }
        if (worst > kRxGainDuringTxCapDb) {
            std::printf("    worst keyed gain: %d dB\n", worst);
        }
        check(worst <= kRxGainDuringTxCapDb, s.what);
    }

    // PureSignal dropping out while keyed (refreshPureSignalAvailability): the
    // cap must land before a 0x0a that now carries the 48 dB receive gain.
    int worst = -99;
    for (const int numRx : {1, 4}) {
        for (int phase = 0; phase < 6; ++phase) {
            MetisClient c;
            c.setReceiverCount(numRx);
            c.setLnaGainDb(48);
            c.setPureSignalTxGainDb(0);
            C::setStreaming(c);
            c.enableTransmit(true);
            c.setPureSignal(true, 1);
            GatewareGain g;
            run(c, g, 8, false);
            c.setMox(true, authority.operation);
            run(c, g, 8 + phase, true);
            c.setPureSignal(false, 1);
            for (int i = 0; i < 16; ++i) {
                run(c, g, 1, true);
                worst = std::max(worst, g.appliedDb());
            }
            c.setMox(false, authority.operation);
        }
    }
    if (worst > kRxGainDuringTxCapDb) {
        std::printf("    worst keyed gain: %d dB\n", worst);
    }
    check(worst <= kRxGainDuringTxCapDb,
          "PureSignal off while keyed at 48 dB: never above +8 dB on any packet");
}

void testOneShot()
{
    std::printf("\n  a change reaches the wire ahead of the rotation\n");
    MetisClient c;
    c.setLnaGainDb(5);
    C::setStreaming(c);
    c.setLnaGainDb(48);
    const Packet p = c.buildNextControlPacket();
    check(bankAt(p, kSub2) == ccTxLnaGain(48, false), "the next packet carries the new 0x0e");
    c.setLnaGainDb(40);
    const Packet q = c.buildNextControlPacket();
    check(bankAt(q, kSub2)[0] != kC0AdcAssignOrTxGain,
          "a change that keeps the bank queues nothing");
}

void testBelowCapIsToday()
{
    std::printf("\n  at or below the cap: today's bytes\n");
    for (const int db : {kRxGainDuringTxCapDb, 5, -12}) {
        MetisClient c;
        c.setLnaGainDb(db);
        C::setStreaming(c);
        const std::vector<Cc> expected = {ccRxFreq(0, MetisClient::Params{}.rxFrequencyHz),
                                          ccRxGain(db), ccAdcAssign()};
        bool same = true;
        for (int i = 0; i < 12; ++i) {
            same = same && bankAt(c.buildNextControlPacket(), kSub2) == expected[i % 3];
        }
        check(same, db == kRxGainDuringTxCapDb ? "+8 dB: NCO, gain, all-zero 0x0e"
                                               : "below +8 dB: NCO, gain, all-zero 0x0e");
    }
    // The gateware's own CW rule stays in force where 0x0e is all-zero.
    MetisClient c;
    c.setLnaGainDb(5);
    C::setStreaming(c);
    GatewareGain g;
    run(c, g, 8, true, true);
    check(g.appliedDb() == -12, "below the cap, jack CW keeps the gateware's -12 dB");
    MetisClient h;
    h.setLnaGainDb(48);
    C::setStreaming(h);
    GatewareGain gh;
    run(h, gh, 8, true, true);
    check(gh.appliedDb() == 8, "above the cap, jack CW runs at the cap (disclosed)");
}

void testTxPathUnchanged(TxTestAuthority& authority)
{
    std::printf("\n  nothing on the TX path changes\n");
    MetisClient hot;
    MetisClient low;
    hot.setLnaGainDb(48);   // engaged
    low.setLnaGainDb(5);    // inert, today's bytes
    for (MetisClient* m : {&hot, &low}) {
        C::setStreaming(*m);
        m->enableTransmit(true);
        m->setTxDriveLevel(128);
    }
    bool sameMox = true;
    bool onlyGainBanks = true;
    bool restSame = true;
    int keyedPackets = 0;
    for (int i = 0; i < 64; ++i) {
        if (i == 16) {
            hot.setMox(true, authority.operation);
            low.setMox(true, authority.operation);
        }
        if (i == 48) {
            hot.setMox(false, authority.operation);
            low.setMox(false, authority.operation);
        }
        Packet a = hot.buildNextControlPacket();
        Packet b = low.buildNextControlPacket();
        for (const std::size_t sub : {kSub1, kSub2}) {
            sameMox = sameMox && (a[sub + 3] & kC0MoxBit) == (b[sub + 3] & kC0MoxBit);
            keyedPackets += (sub == kSub2 && (a[sub + 3] & kC0MoxBit)) ? 1 : 0;
            const Cc ba = bankAt(a, sub);
            const Cc bb = bankAt(b, sub);
            if (ba == bb) {
                continue;
            }
            const bool gain = ba[0] == bb[0]
                && (ba[0] == kC0AdcGain || ba[0] == kC0AdcAssignOrTxGain);
            onlyGainBanks = onlyGainBanks && gain;
            for (std::size_t k = 4; k < 8; ++k) {
                a[sub + k] = 0;
                b[sub + k] = 0;
            }
        }
        restSame = restSame && a == b;
    }
    check(keyedPackets == 32, "32 of the 64 packets were keyed");
    check(sameMox, "MOX is identical in every sub-frame");
    check(onlyGainBanks, "where a bank differs it is 0x0a or 0x0e, at the same slot");
    check(restSame, "masking those two payloads, every packet is byte-identical (0x09, IQ)");
}

void testPureSignal(TxTestAuthority& authority)
{
    std::printf("\n  PureSignal keeps its own keyed gain\n");
    MetisClient c;
    c.setLnaGainDb(48);
    C::setStreaming(c);
    c.enableTransmit(true);
    c.setPureSignal(true, 1);
    check(c.currentTxLnaGainBank() == ccAdcAssign(), "PureSignal on: 0x0e all-zero");
    c.setMox(true, authority.operation);
    check(c.currentGainBank() == ccGain(c.pureSignalTxGainDb(), true),
          "keyed 0x0a still carries the PureSignal feedback gain");
    c.setMox(false, authority.operation);
    c.setPureSignal(false, 2);
    check(c.currentTxLnaGainBank() == ccTxLnaGain(48, false), "PureSignal off: the cap returns");
}

void testBackend()
{
    std::printf("\n  Hl2Backend: effective gain, memory untouched, dBm reference\n");
    Hl2Backend b;
    const QString b40 = bandKeyForHz(7'100'000.0);
    const QString b20 = bandKeyForHz(14'100'000.0);
    Access::moveTo(b, 7'100'000.0);
    Access::setBaseline(b, 48);
    check(Access::txLnaBank(b) == ccTxLnaGain(48, false), "48 dB baseline engages 0x0e");
    check(Access::rxGainBank(b) == ccRxGain(48), "0x0a keeps the operator's 48 dB");
    check(b.displayOffsetDb() == Access::refOffset(b), "unkeyed: the plain reference");

    Access::setKeyed(b, true);
    check(b.displayOffsetDb() == Access::refOffset(b) + 40.0,
          "keyed: reference referred to the +8 dB in force");
    Access::setKeyed(b, false);
    Access::setRadioPtt(b, true);
    check(b.displayOffsetDb() == Access::refOffset(b) + 40.0, "radio PTT counts as keyed");
    Access::setRadioPtt(b, false);

    // Band change while keyed: the new band's gain decides, and nothing the
    // feature does writes the band memory or the pin.
    Access::setBandGain(b, b20, 6);
    Access::setKeyed(b, true);
    Access::moveTo(b, 14'100'000.0);
    check(Access::band(b) == b20 && b.lnaBaselineDb() == 6, "moved to 20 m, its 6 dB");
    check(Access::txLnaBank(b) == ccAdcAssign(), "6 dB is below the cap: 0x0e all-zero");
    check(b.displayOffsetDb() == Access::refOffset(b), "keyed at 6 dB: no correction");
    Access::moveTo(b, 7'100'000.0);
    check(b.lnaBaselineDb() == 48 && Access::txLnaBank(b) == ccTxLnaGain(48, false),
          "back to 40 m while keyed: 48 dB, 0x0e engaged");
    Access::setKeyed(b, false);
    check(Access::rxGainBank(b) == ccRxGain(48) && b.displayOffsetDb() == Access::refOffset(b),
          "unkey: 0x0a and the reference are the operator's 48 dB");

    // Automatic RF gain: the cap acts on the effective gain, never the baseline.
    b.setLnaAutoOffsetDb(36);
    check(b.lnaEffectiveDb() == 12 && Access::txLnaBank(b) == ccTxLnaGain(12, false),
          "auto offset 36: effective 12 dB, still above the cap");
    Access::setKeyed(b, true);
    check(b.displayOffsetDb() == Access::refOffset(b) + 4.0, "keyed: 4 dB correction");
    b.setLnaAutoOffsetDb(42);
    check(Access::txLnaBank(b) == ccAdcAssign() && b.displayOffsetDb() == Access::refOffset(b),
          "auto offset 42: effective 6 dB, the cap stands aside");
    Access::setKeyed(b, false);
    b.setLnaAutoOffsetDb(0);
    check(b.lnaBaselineDb() == 48, "the baseline was never written");

    // Session pin and band memory survive a key cycle above the cap.
    Access::setSessionPin(b, true);
    const QMap<QString, int> before = Access::bandMap(b);
    Access::setKeyed(b, true);
    (void)b.displayOffsetDb();
    Access::setKeyed(b, false);
    check(Access::sessionPin(b) && Access::bandMap(b) == before && b.lnaBaselineDb() == 48,
          "a key cycle leaves the pin, the band map and the baseline as they were");

    Access::setPsEnabled(b, true);
    Access::setKeyed(b, true);
    check(b.displayOffsetDb() == Access::refOffset(b), "PureSignal on: no correction");
    Access::setKeyed(b, false);
    Access::setPsEnabled(b, false);
}

void testEdgeDropsAverage()
{
    std::printf("\n  Hl2Backend: T/R edges drop the averaged pan when the gain moves\n");
    Hl2Backend b;
    Hl2RxDsp dsp;
    Access::setBaseline(b, 48);
    Access::oneReceiver(b, &dsp, 50);
    quint64 n = Access::drops(b);
    Access::edge(b, true, false);
    check(Access::drops(b) == n + 1, "48 dB, key-down: the average is dropped");
    Access::edge(b, false, false);
    check(Access::drops(b) == n + 2, "48 dB, unkey: dropped again");
    Access::edge(b, false, true);
    Access::edge(b, false, false);
    check(Access::drops(b) == n + 4, "48 dB, radio PTT press and release: both drop");
    Access::edge(b, true, false);
    Access::edge(b, true, true);
    check(Access::drops(b) == n + 5, "PTT under a held key is no edge: no drop");
    Access::edge(b, false, false);

    Access::setBaseline(b, 5);
    n = Access::drops(b);
    Access::edge(b, true, false);
    Access::edge(b, false, false);
    check(Access::drops(b) == n, "5 dB (zero shortfall): key and unkey keep the average");

    Access::setBaseline(b, 48);
    Access::setPsEnabled(b, true);
    n = Access::drops(b);
    Access::edge(b, true, false);
    Access::edge(b, false, false);
    check(Access::drops(b) == n, "PureSignal on (zero shortfall): no drop");
    Access::setPsEnabled(b, false);

    // The unkey drop is reseeded from tail frames still at the keyed gain.
    n = Access::drops(b);
    Access::edge(b, true, false);
    Access::edge(b, false, false);
    Access::tailEnds(b);
    check(Access::drops(b) == n + 3, "48 dB, end of the TX tail: dropped once more");
    n = Access::drops(b);
    Access::edge(b, true, false);
    Access::edge(b, false, true);
    Access::tailEnds(b);
    check(Access::drops(b) == n + 1, "tail ends under radio PTT: still keyed, no drop");
    Access::edge(b, false, false);
    Access::setBaseline(b, 5);
    n = Access::drops(b);
    Access::edge(b, true, false);
    Access::edge(b, false, false);
    Access::tailEnds(b);
    check(Access::drops(b) == n, "5 dB (zero shortfall): the tail end keeps the average");
    Access::releaseReceiver(b);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-rx-gain-during-tx-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    TxTestAuthority authority;

    testEncoding();
    testEdges(authority);
    testKeyedChangePerPacket(authority);
    testOneShot();
    testBelowCapIsToday();
    testTxPathUnchanged(authority);
    testPureSignal(authority);
    testBackend();
    testEdgeDropsAverage();

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
