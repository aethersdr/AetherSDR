// HL2 transmit output, ANT or RF1 (#4954). RF1 is the AD9866 output with the
// onboard PA held off: 0x09[19] clear. The claims under test are what reaches
// the wire and what the operator is told:
//   - a session that never leaves ANT sends exactly the banks it sent before;
//   - RF1 never carries the PA bit, at any level, through any path, and 0x09[18]
//     stays clear on both routes;
//   - the route rides the round robin once it has left ANT, so a lost bank is
//     re-sent, and a new session starts with drive 0 and no slot;
//   - a change is refused while keyed, under radio PTT, tuning or in the tail,
//     and every refusal republishes the actual route and warns;
//   - RF1 has its own drive and never reads or writes the band maps;
//   - SliceModel's listeners hear the route in force last, not the refused one.
// Socket-free: constructed objects, packets from the transport's own builder.
// Nothing is keyed on any radio.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2HardwareOptions.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QStringList>
#include <QThread>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c) { c.m_running = true; }
    static void dropOneShots(MetisClient& c) { c.m_oneShot.clear(); }
    static void setRadioPtt(MetisClient& c, bool ptt) { c.m_telemetry.ptt = ptt; }
    static void beginSession(MetisClient& c, TxRoute route)
    {
        c.m_params.txRoute = route;
        c.resetTxDriveForSession();
    }
    static Cc storedDriveBank(const MetisClient& c) { return c.m_ccTxDrive; }
    static bool atuTune(const MetisClient& c) { return c.m_atuTune; }
};

struct Hl2TxRouteTestAccess {
    static void prepare(Hl2Backend& b)
    {
        b.m_txAllowed = true;
        b.m_txTailHoldMs = 60 * 60 * 1000;   // parked; the test expires it
        QMetaObject::invokeMethod(b.m_metis, [m = b.m_metis] {
            m->enableTransmit(true);
            MetisClientTestAccess::setStreaming(*m);
        }, Qt::BlockingQueuedConnection);
    }
    static void twoReceivers(Hl2Backend& b)
    {
        b.m_ids.reset(2);
        b.m_rx.assign(2, Hl2Backend::Receiver{});
    }
    static std::array<std::uint8_t, kUsbPacketSize> packet(Hl2Backend& b)
    {
        std::array<std::uint8_t, kUsbPacketSize> p{};
        QMetaObject::invokeMethod(b.m_metis, [&p, m = b.m_metis] { p = m->buildNextControlPacket(); },
                                  Qt::BlockingQueuedConnection);
        return p;
    }
    static TxRoute clientRoute(Hl2Backend& b)
    {
        TxRoute r = TxRoute::Ant;
        QMetaObject::invokeMethod(b.m_metis, [&r, m = b.m_metis] { r = m->txRoute(); },
                                  Qt::BlockingQueuedConnection);
        return r;
    }
    static bool clientAtu(Hl2Backend& b)
    {
        bool on = false;
        QMetaObject::invokeMethod(b.m_metis, [&on, m = b.m_metis] {
            on = MetisClientTestAccess::atuTune(*m);
        }, Qt::BlockingQueuedConnection);
        return on;
    }
    static void setKeyed(Hl2Backend& b, bool v) { b.m_keyed = v; }
    static void setRadioPtt(Hl2Backend& b, bool v) { b.m_radioPtt = v; }
    static void setTuning(Hl2Backend& b, bool v) { b.m_tuning = v; }
    static void setTail(Hl2Backend& b, bool v) { b.m_txTailPending = v; }
    static void expireTxTail(Hl2Backend& b)
    {
        b.m_txTailTimer->stop();
        b.finishTxTail();
    }
    static void declareGatewareAtu(Hl2Backend& b)
    {
        Hl2HardwareOptions hw = b.m_hw;
        hw.atuGateware = true;
        b.m_hw = hw;
    }
    static void atuRequest(Hl2Backend& b, bool on) { b.applyAtuTuneRequest(on); }
    static int bandDrive(const Hl2Backend& b, const QString& band)
    {
        return b.m_driveByBand.value(band, -999);
    }
    static int tuneFor(const Hl2Backend& b, const QString& band)
    {
        return b.m_tuneByBand.value(band, -999);
    }
    static int driveDefault(const Hl2Backend& b) { return b.m_driveDefaultPercent; }
    static int rfPower(const Hl2Backend& b) { return b.m_rfPowerPercent; }
    static TxRoute backendRoute(const Hl2Backend& b) { return b.m_txRoute; }
    static void clientRefused(Hl2Backend& b, int actual) { b.onMetisTxRouteRefused(actual); }
};

} // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using A = Hl2TxRouteTestAccess;
using C = MetisClientTestAccess;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

namespace {

// Sub-frame 2 carries the one-shot or the rotation's bank; C0 bit 0 is MOX.
struct Bank {
    Cc cc{};
    bool mox = false;
};
Bank bankIn(const std::array<std::uint8_t, kUsbPacketSize>& p)
{
    const std::size_t fs = 8 + kFrameSize;
    Bank b;
    b.mox = (p[fs + 3] & kC0MoxBit) != 0;
    b.cc = {static_cast<std::uint8_t>(p[fs + 3] & ~kC0MoxBit), p[fs + 4], p[fs + 5], p[fs + 6],
            p[fs + 7]};
    return b;
}
bool isDrive(const Cc& cc) { return cc[0] == kC0TxDrive; }
bool paOn(const Cc& cc) { return (cc[2] & 0x08) != 0; }
bool trDisable(const Cc& cc) { return (cc[2] & 0x04) != 0; }

std::vector<Bank> banks(MetisClient& c, int n)
{
    std::vector<Bank> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(bankIn(c.buildNextControlPacket()));
    }
    return out;
}
int driveCount(const std::vector<Bank>& v)
{
    int n = 0;
    for (const Bank& b : v) {
        n += isDrive(b.cc) ? 1 : 0;
    }
    return n;
}
std::optional<Cc> lastDrive(const std::vector<Bank>& v)
{
    std::optional<Cc> out;
    for (const Bank& b : v) {
        if (isDrive(b.cc)) {
            out = b.cc;
        }
    }
    return out;
}

std::optional<Cc> drainBackend(Hl2Backend& b)
{
    QCoreApplication::processEvents();
    std::optional<Cc> out;
    for (int i = 0; i < 24; ++i) {
        const Bank bank = bankIn(A::packet(b));
        if (isDrive(bank.cc)) {
            out = bank.cc;
        }
    }
    return out;
}

void testBankBuilder()
{
    std::printf("\n  the 0x09 bank per route\n");
    bool antSame = true;
    bool rf1Clear = true;
    bool bit18Clear = true;
    bool gateClosed = true;
    for (const int level : {0, 1, 25, 128, 254, 255, 400, -3}) {
        for (const bool atu : {false, true}) {
            // Main's rule, verbatim: ccTxDrive(level, level > 0, atu).
            antSame = antSame
                && ccTxDriveForRoute(TxRoute::Ant, level, true, atu) == ccTxDrive(level, level > 0, atu);
            const Cc rf1 = ccTxDriveForRoute(TxRoute::Rf1, level, true, atu);
            rf1Clear = rf1Clear && !paOn(rf1) && rf1[1] == ccTxDrive(level)[1]
                && ((rf1[2] & 0x10) != 0) == atu;
            for (const TxRoute r : {TxRoute::Ant, TxRoute::Rf1}) {
                const Cc open = ccTxDriveForRoute(r, level, true, atu);
                const Cc shut = ccTxDriveForRoute(r, level, false, atu);
                bit18Clear = bit18Clear && !trDisable(open) && !trDisable(shut);
                gateClosed = gateClosed && shut[1] == 0 && !paOn(shut);
            }
        }
    }
    check(antSame, "RT1: ANT builds main's bank at every level and ATU state");
    check(rf1Clear, "RT2: RF1 keeps the drive in C1 and never sets the PA bit");
    check(bit18Clear, "RT2: 0x09[18] (tr_disable) is clear on both routes");
    check(gateClosed, "RT3: a closed gate is drive 0, PA off, on both routes");
}

void testClientRotation(TxTestAuthority& authority)
{
    std::printf("\n  MetisClient: route bank, rotation, session start\n");
    MetisClient c;
    C::setStreaming(c);
    c.enableTransmit(true);

    // A session that never left ANT: main's rotation, NCO / gain / ADC, no 0x09.
    const std::vector<Cc> expected = {ccRxFreq(0, MetisClient::Params{}.rxFrequencyHz),
                                      ccRxGain(MetisClient::Params{}.lnaGainDb), ccAdcAssign()};
    std::vector<Bank> v = banks(c, 12);
    bool same = true;
    for (std::size_t i = 0; i < v.size(); ++i) {
        same = same && v[i].cc == expected[i % expected.size()];
    }
    check(same, "RT10: never-left-ANT rotation is NCO, gain, ADC with no 0x09 slot");

    c.setTxDriveLevel(128);
    v = banks(c, 12);
    check(driveCount(v) == 1 && lastDrive(v) == ccTxDrive(128, true),
          "RT10: on ANT a drive change is one one-shot bank, PA on, as on main");

    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 200);
    v = banks(c, 1);
    check(lastDrive(v) && (*lastDrive(v))[1] == 200 && !paOn(*lastDrive(v)),
          "RT2: setTxRoute(RF1) sends route and level as one bank, PA off");
    v = banks(c, 16);
    check(driveCount(v) == 4, "RT10: after leaving ANT the 0x09 bank is one slot of four");
    check(lastDrive(v) && !paOn(*lastDrive(v)) && (*lastDrive(v))[1] == 200,
          "RT10: the rotated bank is RF1's: level 200, PA off");

    // RT4: the ATU restate rebuilds through the route. Main's `[1] > 0` would set
    // the PA bit here.
    // The restate's own bank, before the rotation could repair it.
    c.setAtuTuneRequest(true);
    v = banks(c, 1);
    check(lastDrive(v) && !paOn(*lastDrive(v)) && ((*lastDrive(v))[2] & 0x10) != 0,
          "RT4: the ATU restate on RF1 keeps the PA off");
    c.setAtuTuneRequest(false);

    // RT11: the bank is lost; the rotation repairs it within one rotation, keyed or not.
    c.setTxRoute(static_cast<int>(TxRoute::Ant), 100);
    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 100);
    C::dropOneShots(c);
    v = banks(c, 4);
    check(lastDrive(v) && (*lastDrive(v))[1] == 100 && !paOn(*lastDrive(v)),
          "RT11: a dropped route bank reaches the wire within one rotation");
    c.setMox(true, authority.operation);
    v = banks(c, 4);
    bool keyedSlot = false;
    for (const Bank& b : v) {
        keyedSlot = keyedSlot || (isDrive(b.cc) && b.mox && !paOn(b.cc));
    }
    check(keyedSlot, "RT11: the slot keeps running while keyed, PA off");
    c.setMox(false, authority.operation);
    QThread::msleep(100);

    // Back to ANT: the slot stays, so the change back is as reliable as the change away.
    c.setTxRoute(static_cast<int>(TxRoute::Ant), 100);
    v = banks(c, 16);
    check(driveCount(v) >= 4 && lastDrive(v) == ccTxDrive(100, true),
          "RT10: after the change back the slot stays, carrying ANT with the PA on");

    // RT10: a new session. The #4579 trap: a rotated bank carrying the old level.
    C::beginSession(c, TxRoute::Ant);
    v = banks(c, 12);
    check(driveCount(v) == 0, "RT10: a new ANT session has no 0x09 slot");
    check(C::storedDriveBank(c) == ccTxDrive(0, false), "RT10: a new session holds drive 0, PA off");
    C::beginSession(c, TxRoute::Rf1);
    v = banks(c, 8);
    check(driveCount(v) == 2 && lastDrive(v) == ccTxDriveForRoute(TxRoute::Rf1, 0, true, false),
          "RT7/RT10: a session started on RF1 rotates from its first frame at drive 0");

    // RT3 through the client: gate closed.
    C::beginSession(c, TxRoute::Ant);
    c.setTxDriveLevel(180);
    c.enableTransmit(false);
    c.setAtuTuneRequest(true);
    v = banks(c, 8);
    check(lastDrive(v) && (*lastDrive(v))[1] == 0 && !paOn(*lastDrive(v)),
          "RT3: the ATU restate with the gate closed is drive 0, PA off");
}

void testClientRefusal(TxTestAuthority& authority)
{
    std::printf("\n  MetisClient: the second refusal\n");
    MetisClient c;
    C::setStreaming(c);
    c.enableTransmit(true);
    std::vector<int> refused;
    QObject::connect(&c, &MetisClient::txRouteRefused, [&](int actual) { refused.push_back(actual); });

    c.setMox(true, authority.operation);
    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 50);
    check(c.isKeyed() && c.txRoute() == TxRoute::Ant && refused.size() == 1 && refused.back() == 0,
          "RT5: keyed, the client refuses and answers with ANT");
    c.setMox(false, authority.operation);
    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 50);
    check(c.txRoute() == TxRoute::Ant && refused.size() == 2, "RT5: inside the unkey tail, refused");
    QThread::msleep(100);
    C::setRadioPtt(c, true);
    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 50);
    check(c.txRoute() == TxRoute::Ant && refused.size() == 3, "RT5: under radio PTT, refused");
    C::setRadioPtt(c, false);
    c.setTxRoute(7, 50);
    check(c.txRoute() == TxRoute::Ant && refused.size() == 4, "RT5: an unknown route is refused");
    c.setTxRoute(static_cast<int>(TxRoute::Rf1), 50);
    check(c.txRoute() == TxRoute::Rf1 && refused.size() == 4, "RT5: after the tail, accepted");
}

struct Published {
    std::vector<std::optional<QString>> txAntenna{std::nullopt, std::nullopt};
    std::vector<std::optional<QStringList>> lists{std::nullopt, std::nullopt};
    std::vector<int> rfPower;
    int warnings = 0;
    void clearRfPower() { rfPower.clear(); }
};

void watch(Hl2Backend& b, Published& p)
{
    QObject::connect(&b, &IRadioBackend::sliceChanged, &b, [&p](int sliceId, const SliceDelta& d) {
        if (sliceId < 0 || sliceId > 1) {
            return;
        }
        if (d.txAntenna) {
            p.txAntenna[static_cast<std::size_t>(sliceId)] = *d.txAntenna;
        }
        if (d.txAntennaList) {
            p.lists[static_cast<std::size_t>(sliceId)] = *d.txAntennaList;
        }
    });
    QObject::connect(&b, &IRadioBackend::transmitChanged, &b, [&p](const TransmitDelta& d) {
        if (d.rfPower) {
            p.rfPower.push_back(*d.rfPower);
        }
    });
    QObject::connect(&b, &IRadioBackend::configurationWarning, &b,
                     [&p](const QString&) { ++p.warnings; });
}

bool bothSay(const Published& p, const char* token)
{
    const QStringList list{QStringLiteral("ANT"), QStringLiteral("RF1")};
    return p.txAntenna[0] == QString::fromLatin1(token) && p.txAntenna[1] == QString::fromLatin1(token)
        && p.lists[0] == list && p.lists[1] == list;
}

ReceiveDispatch route(Hl2Backend& b, const QString& token)
{
    return b.requestSliceControl(0, {SliceControlRequest::Field::TxAntenna, token});
}

void testBackendRequest()
{
    std::printf("\n  Hl2Backend: Field::TxAntenna\n");
    Hl2Backend b;
    A::twoReceivers(b);
    A::prepare(b);
    Published p;
    watch(b, p);

    check(b.capabilities().independentTxAntenna.has_value(),
          "RT8: HL2 declares its TX output independent of the RX select");
    check(route(b, QStringLiteral("RF1")) == ReceiveDispatch::Dispatched && bothSay(p, "RF1")
              && A::clientRoute(b) == TxRoute::Rf1 && p.warnings == 0,
          "RT6: RF1 is accepted, reaches the client, and every slice publishes it");
    check(route(b, QStringLiteral("ANT")) == ReceiveDispatch::Dispatched && bothSay(p, "ANT")
              && A::clientRoute(b) == TxRoute::Ant,
          "RT6: ANT is accepted and published");
    check(route(b, QStringLiteral("ANT2")) == ReceiveDispatch::Dispatched && bothSay(p, "ANT")
              && p.warnings == 1,
          "RT6: ANT2 is refused, warned, and the actual route republished");
    route(b, QStringLiteral("XVTA"));
    check(p.warnings == 2, "RT6: every refusal warns, not once per session");
    check(route(b, QString()) == ReceiveDispatch::Unsupported && A::backendRoute(b) == TxRoute::Ant,
          "RT6: an empty token is not a valid request and moves nothing");

    const auto refusedWhile = [&](auto set, const char* what) {
        const int before = p.warnings;
        set(true);
        const ReceiveDispatch r = route(b, QStringLiteral("RF1"));
        QCoreApplication::processEvents();
        const bool ok = r == ReceiveDispatch::Dispatched && A::backendRoute(b) == TxRoute::Ant
            && A::clientRoute(b) == TxRoute::Ant && bothSay(p, "ANT") && p.warnings == before + 1;
        set(false);
        check(ok, what);
    };
    refusedWhile([&](bool v) { A::setKeyed(b, v); }, "RT5: refused while keyed");
    refusedWhile([&](bool v) { A::setRadioPtt(b, v); }, "RT5: refused under radio PTT");
    refusedWhile([&](bool v) { A::setTuning(b, v); }, "RT5: refused while tuning");
    refusedWhile([&](bool v) { A::setTail(b, v); }, "RT5: refused inside the unkey tail");
    check(route(b, QStringLiteral("RF1")) == ReceiveDispatch::Dispatched
              && A::clientRoute(b) == TxRoute::Rf1 && bothSay(p, "RF1"),
          "RT5: accepted once nothing radiates");

    // The client's own refusal answers through a signal and the backend republishes.
    const int before = p.warnings;
    A::clientRefused(b, static_cast<int>(TxRoute::Ant));
    check(A::backendRoute(b) == TxRoute::Ant && bothSay(p, "ANT") && p.warnings == before + 1,
          "RT5: a client refusal reverts the backend, republishes and warns");
}

void testBackendDrive(TxTestAuthority& authority)
{
    std::printf("\n  Hl2Backend: RF1's own drive\n");
    Hl2Backend b;
    A::twoReceivers(b);
    A::prepare(b);
    b.setSliceMode(0, QStringLiteral("USB"));
    Published p;
    watch(b, p);
    const QString b20 = QStringLiteral("20m");
    const QString b40 = QStringLiteral("40m");

    b.setSliceFrequency(0, 14'074'000.0);
    b.setTxPower(60);
    b.setSliceFrequency(0, 7'074'000.0);
    b.setTxPower(30);
    b.setSliceFrequency(0, 14'074'000.0);
    check(A::bandDrive(b, b20) == 60 && A::bandDrive(b, b40) == 30 && A::rfPower(b) == 60
              && A::driveDefault(b) == 60,
          "premise: ANT band memory 20m 60 %, 40m 30 %, on 20m");

    p.clearRfPower();
    route(b, QStringLiteral("RF1"));
    std::optional<Cc> d = drainBackend(b);
    check(!p.rfPower.empty() && p.rfPower.back() == 0, "RT12: the slider shows RF1's start drive, 0 %");
    check(d && (*d)[1] == 0 && !paOn(*d), "RT12: RF1 starts at drive 0, PA off");

    b.setTxPower(20);
    d = drainBackend(b);
    check(d && (*d)[1] == 20 * kTxDriveMax / 100 && !paOn(*d), "RT12: the RF1 slider drives RF1, PA off");
    check(A::bandDrive(b, b20) == 60 && A::driveDefault(b) == 60 && A::rfPower(b) == 60,
          "RT12: the RF1 slider writes neither the band map nor the baseline");

    p.clearRfPower();
    b.setSliceFrequency(0, 7'074'000.0);
    d = drainBackend(b);
    check(p.rfPower.empty() && d && (*d)[1] == 20 * kTxDriveMax / 100 && !paOn(*d),
          "RT12: a band change on RF1 applies no stored drive");
    check(A::bandDrive(b, b20) == 60 && A::bandDrive(b, b40) == 30,
          "RT12: a band change on RF1 leaves both band drives as they were");

    const int tune40 = A::tuneFor(b, b40);
    b.requestTransmitControl({TransmitControlRequest::Field::TunePower, 77},
                             TunePowerContext::Deferred);
    check(A::tuneFor(b, b40) == tune40, "RT12: the tune slider on RF1 writes no tune memory");

    // TUNE on RF1 runs at RF1's drive, not the tune slider's.
    b.setTune(true, 80, authority.operation, {});
    d = drainBackend(b);
    check(d && (*d)[1] == 20 * kTxDriveMax / 100 && !paOn(*d), "RT12: TUNE on RF1 uses RF1's drive");
    check(A::tuneFor(b, b40) == tune40, "RT12: TUNE on RF1 records no tune power");
    b.setTune(false, 80, authority.operation, {});
    A::expireTxTail(b);
    d = drainBackend(b);
    check(d && (*d)[1] == 20 * kTxDriveMax / 100 && !paOn(*d), "RT12: after TUNE, RF1's drive stands");

    // The echo of RF1's value through setTxPower cannot reach ANT's map.
    b.setTxPower(20);
    check(A::bandDrive(b, b40) == 30, "RT12: an echo of RF1's drive does not become ANT's band drive");

    // The client's own unkey hold is wall-clock; let it pass.
    QThread::msleep(100);
    p.clearRfPower();
    route(b, QStringLiteral("ANT"));
    d = drainBackend(b);
    check(!p.rfPower.empty() && p.rfPower.back() == 30, "RT12: back on ANT the slider shows 40m's drive");
    check(d && (*d)[1] == 30 * kTxDriveMax / 100 && paOn(*d), "RT12: back on ANT 40m's drive returns, PA on");
}

void testBackendAtu()
{
    std::printf("\n  Hl2Backend: gateware ATU\n");
    Hl2Backend b;
    A::twoReceivers(b);
    A::prepare(b);
    A::declareGatewareAtu(b);
    route(b, QStringLiteral("RF1"));
    A::atuRequest(b, true);
    check(!A::clientAtu(b), "RT13: no ATU request is raised on RF1");
    route(b, QStringLiteral("ANT"));
    A::atuRequest(b, true);
    check(A::clientAtu(b), "RT13: on ANT the request is raised as before");
    A::atuRequest(b, false);
    check(!A::clientAtu(b), "RT13: clearing clears");
}

void testSliceModelOrdering()
{
    std::printf("\n  SliceModel: the last txAntennaChanged is the route in force\n");
    SliceModel s(0);
    SliceDelta init;
    init.txAntennaList = QStringList{QStringLiteral("ANT"), QStringLiteral("RF1")};
    init.txAntenna = QStringLiteral("ANT");
    s.applyChanges(init);
    QStringList heard;
    QObject::connect(&s, &SliceModel::txAntennaChanged, &s, [&](const QString& a) { heard << a; });
    // A backend that refuses answers synchronously with the route still in force.
    QObject::connect(&s, &SliceModel::controlRequested, &s, [&s](const SliceControlRequest& r) {
        if (r.field != SliceControlRequest::Field::TxAntenna) {
            return;
        }
        SliceDelta d;
        d.txAntenna = QStringLiteral("ANT");
        s.applyChanges(d);
    }, Qt::DirectConnection);
    s.setTxAntenna(QStringLiteral("RF1"));
    check(s.txAntenna() == QLatin1String("ANT") && !heard.isEmpty() && heard.last() == QLatin1String("ANT"),
          "RT6: listeners hear the refused value first and the actual route last");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-tx-route-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    std::printf("\n  HL2 transmit output, ANT or RF1 (#4954)\n");
    TxTestAuthority authority;

    testBankBuilder();
    testClientRotation(authority);
    testClientRefusal(authority);
    testBackendRequest();
    testBackendDrive(authority);
    testBackendAtu();
    testSliceModelOrdering();

    std::printf("\n  %s (%d failure%s)\n\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
