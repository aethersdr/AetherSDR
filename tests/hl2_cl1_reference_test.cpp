// The CL1 external reference, in the two places it can go wrong silently.
//
// Nothing about CL1 is readable back. The VersaClock takes writes on the
// radio's INTERNAL I2C bus and reports nothing, the discovery reply says
// nothing about the clock source, and a register readback — which the gateware
// does allow, cookie 0x07 — would only echo what the host wrote. So neither of
// the properties below can be observed on a radio, by us or by an operator:
// they are observable here or nowhere.
//
//   PART A  A radio this process switched to CL1 gets its OFF table even if
//           the first attempt was interrupted. The record that a still-powered
//           radio may be running from an external reference is released only
//           when the whole table has been CONFIRMED SENT, and it is kept per
//           radio so a second HL2 cannot erase the first one's.
//
//   PART B  A connect that restores "CL1 on" together with a non-zero manual
//           ppb normalises the pair before anything is computed from it.
//
// Both were found by review on aethersdr/AetherSDR#5923. Part A's defect was
// reproduced against the production client with a socket-free probe; this is
// that probe, kept.
//
// Socket-free and radio-free throughout: Part A injects transport state through
// the MetisClientTestAccess friend seam and never binds; Part B uses the
// boardMaxRx connect that skips the discovery socket. No event loop is pumped
// and nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2FreqCal.h"
#include "core/backends/hl2/Hl2HardwareOptions.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok)
        ++g_failures;
}

namespace AetherSDR::hl2 {
// The seam MetisClient.h declares a friend of. Same shape as the one in
// hl2_tx_gate_test: no start(), no bind, no peer, no datagram — inject the
// transport state the CL1 logic reads and drive the real packet builder.
struct MetisClientTestAccess {
    static void runWith(MetisClient& c, const QString& serial, bool cl1)
    {
        c.m_running = true;
        c.m_params.radioSerial = serial;
        c.m_params.cl1RefClock = cl1;
    }
    static void setSetting(MetisClient& c, bool cl1) { c.m_params.cl1RefClock = cl1; }
    // Deliver `n` packets, reporting each as sent (or refused, for accepted=false).
    static int deliver(MetisClient& c, int n, bool accepted = true)
    {
        int cl1BanksSeen = 0;
        for (int i = 0; i < n; ++i) {
            const auto pkt = c.buildNextControlPacket();
            // C&C bank B sits SYNC(3) into the second 512-byte frame.
            const std::uint8_t* b = pkt.data() + 8 + kFrameSize + 3;
            Cc bank{b[0], b[1], b[2], b[3], b[4]};
            if (MetisClient::isCl1Bank(bank))
                ++cl1BanksSeen;
            c.onControlPacketSent(accepted ? static_cast<qint64>(pkt.size()) : -1, 0);
        }
        return cl1BanksSeen;
    }
    static bool recoveryPending(const MetisClient& c) { return c.cl1RecoveryPending(); }
    static bool knows(const MetisClient& c, const QString& serial)
    {
        return c.m_cl1MaybeOn.contains(serial);
    }
    static std::size_t queued(const MetisClient& c) { return c.m_oneShot.size(); }
    // The recovery decision start() makes, without binding a socket.
    static bool startWouldQueueOff(MetisClient& c)
    {
        return !c.m_params.cl1RefClock && c.cl1RecoveryPending();
    }
};
}  // namespace AetherSDR::hl2

using hl2::MetisClient;
using hl2::MetisClientTestAccess;
using Access = hl2::MetisClientTestAccess;

namespace {

const QString kRadioA = QStringLiteral("AA:BB:CC:DD:EE:01");
const QString kRadioB = QStringLiteral("AA:BB:CC:DD:EE:02");

void partA_recoverySurvivesAnInterruptedOff()
{
    // ---- 1. switching on is remembered immediately -------------------------
    {
        MetisClient c;
        Access::runWith(c, kRadioA, /*cl1=*/false);
        c.setCl1RefClock(true);
        check(Access::queued(c) == hl2::kVersaClockCl1Banks,
              "on: all 24 banks are queued");
        check(Access::recoveryPending(c),
              "on: the radio is remembered from the moment the table is QUEUED, "
              "because an interrupted sequence may already have moved the part");
        const int sent = Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(sent == static_cast<int>(hl2::kVersaClockCl1Banks),
              "on: every one of the 24 banks reaches a packet");
        check(Access::queued(c) == 0, "on: the queue drains");
        check(Access::recoveryPending(c),
              "on: still remembered after the on table completes — it IS on CL1 now");
    }

    // ---- 2. THE DEFECT: an interrupted OFF must not forget ----------------
    {
        MetisClient c;
        Access::runWith(c, kRadioA, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));

        c.setCl1RefClock(false);
        check(Access::queued(c) == hl2::kVersaClockCl1Banks,
              "off: all 24 banks are queued");
        check(Access::recoveryPending(c),
              "off: QUEUEING the off table does not release the radio — this is the "
              "line that used to clear it before a single write left the host");

        // Interrupted after a few banks: stop() drops the remainder.
        Access::deliver(c, 3);
        check(Access::recoveryPending(c), "off: three banks in, still remembered");
        c.stop();
        check(Access::queued(c) == 0, "stop: the remaining banks are dropped");
        check(Access::recoveryPending(c),
              "stop: the radio is STILL remembered — the off table never finished, so "
              "the radio may still be running from the external reference");
        check(Access::startWouldQueueOff(c),
              "reconnect: start() would queue the off table again, with the setting "
              "already clear — the recovery resumes instead of being forgotten");
    }

    // ---- 3. a completed OFF does release it -------------------------------
    {
        MetisClient c;
        Access::runWith(c, kRadioA, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        c.setCl1RefClock(false);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(!Access::recoveryPending(c),
              "off complete: the radio is back on its crystal and no longer tracked");
        check(!Access::startWouldQueueOff(c),
              "and a later connect leaves the clock bus alone");
    }

    // ---- 4. a packet the transport REFUSED retires nothing ----------------
    //
    // buildNextControlPacket() pops the bank, so a send that failed would lose
    // it either way; what must not happen is the COUNTDOWN completing on writes
    // that never reached the wire and reporting the radio recovered.
    {
        MetisClient c;
        Access::runWith(c, kRadioA, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        c.setCl1RefClock(false);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks), /*accepted=*/false);
        check(Access::recoveryPending(c),
              "refused sends do not retire the off table, however many are built");
    }

    // ---- 5. two radios, two records --------------------------------------
    //
    // The old single-QString latch lost A's record the moment B was switched on.
    // A then never got its off table: it stayed on CL1 with nothing that knew.
    {
        MetisClient c;
        Access::runWith(c, kRadioA, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(Access::knows(c, kRadioA), "A is remembered");

        // A radio swap inside one process: same client, different serial.
        c.stop();
        Access::runWith(c, kRadioB, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(Access::knows(c, kRadioB), "B is remembered");
        check(Access::knows(c, kRadioA),
              "and A is STILL remembered — switching B on must not erase A's recovery");

        // Coming back to A with the box cleared recovers A, not B.
        c.stop();
        Access::runWith(c, kRadioA, /*cl1=*/false);
        check(Access::startWouldQueueOff(c), "returning to A queues A's off table");
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks) + 1);
        c.setCl1RefClock(false);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(!Access::knows(c, kRadioA), "A is released once its off table completes");
        check(Access::knows(c, kRadioB), "and B's record is untouched by A's recovery");
    }

    // ---- 6. no serial, no record, and it says so --------------------------
    {
        MetisClient c;
        Access::runWith(c, QString{}, /*cl1=*/false);
        c.setCl1RefClock(true);
        Access::deliver(c, static_cast<int>(hl2::kVersaClockCl1Banks));
        check(!Access::recoveryPending(c),
              "an unknown serial is tracked as nothing — not writing the clock bus "
              "is always the safe failure");
    }
}

// ---------------------------------------------------------------------------
// PART B — the two documents cannot disagree past a connect.
// ---------------------------------------------------------------------------
int connectedPpb(const QString& serial)
{
    hl2::Hl2Backend backend;
    RadioConnectRequest request;
    request.host = QStringLiteral("192.0.2.1");
    request.serial = serial;
    // Skips the unicast discovery socket; nothing binds and no peer is needed.
    request.params.insert(QStringLiteral("boardMaxRx"), 4);
    backend.connectRadio(request);

    int reported = -1;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
                     [&reported](quint64, const QVariant& result) {
        const QVariantMap m = result.toMap();
        if (m.contains(QStringLiteral("ppb")))
            reported = m.value(QStringLiteral("ppb")).toInt();
    });
    // freqcal.get answers synchronously in the backend.
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("freqcal.get"), 1, {});
    backend.disconnectRadio();
    return reported;
}

void partB_inconsistentDocumentIsNormalisedOnConnect()
{
    // ---- 1. the inconsistent pair --------------------------------------
    //
    // Written as two separate rows, which is how it happens: the hardware
    // options and the calibration are persisted by independent operations, so an
    // interruption between them leaves exactly this.
    {
        const QString serial = QStringLiteral("AA:BB:CC:DD:EE:11");
        const RadioSettingsScope scope(QStringLiteral("hl2"), serial);
        Hl2HardwareOptions hw;
        hw.cl1RefClock = true;
        Hl2HardwareOptions::save(scope, hw);
        Hl2FreqCal::savePpb(scope, 500);
        check(Hl2FreqCal::loadPpb(scope) == 500, "the inconsistent pair is on disk");

        check(connectedPpb(serial) == 0,
              "connect with CL1 on and 500 ppb standing reports 0 ppb — the session "
              "does not come up correcting a disciplined clock");
        check(Hl2FreqCal::loadPpb(scope) == 0,
              "and the forced zero is PERSISTED, so it does not come back next connect");
        check(Hl2HardwareOptions::load(scope).cl1RefClock,
              "CL1 itself is untouched: the operator's declaration wins, the stale "
              "correction is what gives way");
    }

    // ---- 2. the consistent pair is left alone ---------------------------
    {
        const QString serial = QStringLiteral("AA:BB:CC:DD:EE:12");
        const RadioSettingsScope scope(QStringLiteral("hl2"), serial);
        Hl2HardwareOptions hw;           // cl1RefClock defaults false
        Hl2HardwareOptions::save(scope, hw);
        Hl2FreqCal::savePpb(scope, 500);
        check(connectedPpb(serial) == 500,
              "a manual calibration with CL1 OFF survives the connect untouched");
        check(Hl2FreqCal::loadPpb(scope) == 500, "and stays on disk");
    }

    // ---- 3. CL1 on with no calibration needs no repair -------------------
    {
        const QString serial = QStringLiteral("AA:BB:CC:DD:EE:13");
        const RadioSettingsScope scope(QStringLiteral("hl2"), serial);
        Hl2HardwareOptions hw;
        hw.cl1RefClock = true;
        Hl2HardwareOptions::save(scope, hw);
        check(connectedPpb(serial) == 0, "CL1 on, never calibrated: 0 ppb, nothing to do");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-cl1-reference"));
    if (!profile.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    std::fprintf(stderr, "-- Part A: the OFF-table recovery latch\n");
    partA_recoverySurvivesAnInterruptedOff();
    std::fprintf(stderr, "-- Part B: CL1 against a stale manual calibration\n");
    partB_inconsistentDocumentIsNormalisedOnConnect();

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_cl1_reference_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
