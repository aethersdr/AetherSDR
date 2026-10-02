// Hl2Backend's IRadioBackend seam on a default-constructed backend: no
// connectRadio(), no socket, no peer. Link edges are injected by emitting
// MetisClient's own signals on the I/O thread, timers are fired by hand, and
// the drive register is read both from the health rows and from the EP2
// packet MetisClient would send.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "core/AppSettings.h"

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QStringList>

#include <cmath>
#include <cstdio>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c) { c.m_running = true; }
};
struct Hl2TxGateTestAccess {
    // Emit a MetisClient signal on its own thread, then deliver the backend's
    // queued handler, exactly as a datagram-driven edge would arrive.
    static void metisEdge(Hl2Backend& b, const char* signal)
    {
        QMetaObject::invokeMethod(b.m_metis, signal, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static void connectFailed(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_metis, "connectFailed", Qt::BlockingQueuedConnection,
                                  Q_ARG(QString, QStringLiteral("injected: no EP6")));
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    // Run everything already queued on the I/O thread, then its replies here.
    static void drainIo(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_metis, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static int receiverCeiling(const Hl2Backend& b) { return b.receiverCeiling(); }
    static bool keyed(const Hl2Backend& b) { return b.m_keyed; }
    static bool hangArmed(const Hl2Backend& b) { return b.m_cwHangTimer->isActive(); }
    // Stops the timer first, so this proves what the callback decides, not
    // whether the timer was stopped.
    static void expireHang(Hl2Backend& b)
    {
        b.m_cwHangTimer->stop();
        QMetaObject::invokeMethod(b.m_cwHangTimer, "timeout", Qt::DirectConnection);
    }
    // End the zoom cooldown so the next request is a discrete gesture.
    static void settleZoom(Hl2Backend& b)
    {
        if (b.m_bandwidthThrottle)
            b.m_bandwidthThrottle->stop();
        b.m_pendingBandwidthHz = 0.0;
    }
    static bool zoomCooling(const Hl2Backend& b)
    {
        return b.m_bandwidthThrottle && b.m_bandwidthThrottle->isActive();
    }
    static void fireZoomCooldown(Hl2Backend& b)
    {
        b.m_bandwidthThrottle->stop();
        QMetaObject::invokeMethod(b.m_bandwidthThrottle, "timeout", Qt::DirectConnection);
    }
    // A session without a socket: MetisClient queues a drive bank only while
    // running (#4579), so wireDrive() would otherwise read nothing.
    static void startSession(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_metis, [metis = b.m_metis] {
            MetisClientTestAccess::setStreaming(*metis);
        }, Qt::BlockingQueuedConnection);
    }
    // The last drive level carried by the EP2 packets MetisClient would send
    // next, after every queued call has landed; -1 when none carries one.
    static int wireDrive(Hl2Backend& b)
    {
        int drive = -1;
        QMetaObject::invokeMethod(b.m_metis, [&drive, metis = b.m_metis] {
            for (int i = 0; i < 64; ++i) {
                const auto pkt = metis->buildNextControlPacket();
                for (const std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}}) {
                    if ((pkt[fs + 3] & ~kC0MoxBit) == kC0TxDrive)
                        drive = pkt[fs + 4];
                }
            }
        }, Qt::BlockingQueuedConnection);
        return drive;
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2TxGateTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
static int driveFor(int percent) { return percent * hl2::kTxDriveMax / 100; }

static QVariant health(const Hl2Backend& b, const char* key)
{
    return b.healthSnapshot().values.value(QString::fromLatin1(key));
}

static void capabilitiesSeam()
{
    Hl2Backend b;
    const RadioCapabilities caps = b.capabilities();
    check(caps.extensionNamespaces == QVector<QString>{QStringLiteral("hl2")},
          "advertises exactly the hl2 extension namespace");
    // Disconnected, the slice capacity is the receivers held; connected, it is
    // the receiver ceiling (board count capped by the link budget at the span).
    check(caps.maxSlices == 1, "disconnected: maxSlices is the one receiver held");
    Access::metisEdge(b, "linkUp");
    const RadioCapabilities live = b.capabilities();
    check(live.maxSlices == Access::receiverCeiling(b)
              && live.maxPanadapters == live.maxSlices,
          "connected: maxSlices is the receiver ceiling, not a fixed 1");
    Access::metisEdge(b, "linkDown");
}

static void linkLifecycle()
{
    Hl2Backend b;
    QSignalSpy connectedSpy(&b, &IRadioBackend::connected);
    QSignalSpy disconnectedSpy(&b, &IRadioBackend::disconnected);
    QSignalSpy spanSpy(&b, &IRadioBackend::panCenterBandwidthChanged);
    QSignalSpy limitsSpy(&b, &IRadioBackend::panBandwidthLimitsChanged);
    QStringList order;
    int firstLow = 0, firstHigh = 0;
    bool sawSlice = false;
    QObject::connect(&b, &IRadioBackend::panCenterBandwidthChanged, &b, [&] {
        if (!order.contains(QStringLiteral("geometry")))
            order << QStringLiteral("geometry");
    });
    QObject::connect(&b, &IRadioBackend::panBandwidthLimitsChanged, &b, [&] {
        if (!order.contains(QStringLiteral("limits")))
            order << QStringLiteral("limits");
    });
    QObject::connect(&b, &IRadioBackend::sliceChanged, &b, [&](int, const SliceDelta& d) {
        if (!sawSlice && d.filterLow && d.filterHigh) {
            sawSlice = true;
            firstLow = *d.filterLow;
            firstHigh = *d.filterHigh;
        }
    });

    Access::metisEdge(b, "linkUp");
    check(connectedSpy.count() == 1 && b.isConnected(), "first EP6 reports connected()");
    check(!spanSpy.isEmpty() && near(spanSpy.first().at(2).toDouble(), 0.048),
          "with nothing remembered the pan comes up on the 48 kHz span");
    check(!limitsSpy.isEmpty() && near(limitsSpy.first().at(1).toDouble(), 0.048)
              && near(limitsSpy.first().at(2).toDouble(), 0.384),
          "zoom limits are the real rate range, 48..384 kHz");
    check(order == QStringList({QStringLiteral("geometry"), QStringLiteral("limits")}),
          "the pan is announced (geometry) before its limits");
    check(sawSlice && firstLow == 100 && firstHigh == 2900,
          "the first slice report carries USB's 100..2900 passband");

    Access::metisEdge(b, "linkDown");
    check(disconnectedSpy.count() == 1 && !b.isConnected(), "link loss reports disconnected()");
    Access::metisEdge(b, "linkDown");
    b.disconnectRadio();
    Access::drainIo(b);
    check(disconnectedSpy.count() == 1, "a link already down never reports disconnected() twice");
}

static void connectWithoutEp6()
{
    Hl2Backend b;
    QSignalSpy errorSpy(&b, &IRadioBackend::connectionError);
    QSignalSpy connectedSpy(&b, &IRadioBackend::connected);
    Access::connectFailed(b);
    check(errorSpy.count() == 1, "a connect with no EP6 surfaces connectionError");
    check(connectedSpy.isEmpty() && !b.isConnected(), "and never reports connected()");
}

static void spanPolicy()
{
    Hl2Backend b;
    const QString pan;   // empty id addresses the first receiver
    QSignalSpy spanSpy(&b, &IRadioBackend::panCenterBandwidthChanged);

    struct Row { double requestMhz, expectMhz; const char* what; };
    const Row rows[] = {
        {0.384, 0.384, "the widest request stays at 384 kHz"},
        {0.192, 0.192, "an exact rate is taken exactly"},
        {0.100, 0.096, "100 kHz snaps down to 96 kHz"},
        // Between 96 and 192 kHz the geometric mean is 135.8 kHz and the
        // arithmetic mean 144 kHz: only ratio-nearest sends 140 kHz up.
        {0.140, 0.192, "140 kHz snaps to 192 kHz: nearest by ratio, not linear distance"},
        {0.048, 0.048, "the narrowest request reaches 48 kHz"},
        {5.400, 0.384, "a 5.4 MHz request clamps to the widest real rate"},
        {0.000001, 0.048, "an absurdly narrow request floors at 48 kHz"},
    };
    for (const Row& r : rows) {
        Access::settleZoom(b);
        spanSpy.clear();
        b.setPanBandwidth(pan, r.requestMhz * 1.0e6);
        check(!spanSpy.isEmpty() && near(spanSpy.last().at(2).toDouble(), r.expectMhz), r.what);
    }

    // A sweep inside one cooldown: the first applies now, the rest coalesce
    // and the trailing edge applies only the last.
    Access::settleZoom(b);
    spanSpy.clear();
    for (const double mhz : {0.048, 0.060, 0.096, 0.120, 0.192, 0.240, 0.300, 0.384})
        b.setPanBandwidth(pan, mhz * 1.0e6);
    check(spanSpy.count() == 1, "a zoom sweep applies only its leading request immediately");
    check(Access::zoomCooling(b), "the sweep is held in the cooldown");
    Access::fireZoomCooldown(b);
    check(spanSpy.count() == 2 && near(spanSpy.last().at(2).toDouble(), 0.384),
          "the cooldown's trailing edge applies the LAST request");
    Access::fireZoomCooldown(b);
    check(spanSpy.count() == 2, "an idle cooldown applies nothing");

    // The applied span persists as a nested object under the "Hl2" root.
    Access::settleZoom(b);
    b.setPanBandwidth(pan, 192000.0);
    check(near(Hl2Settings::spanMhz(), 0.192), "the applied span is persisted");
    auto& s = AppSettings::instance();
    check(s.value(QStringLiteral("Hl2"), QString{}).toString().contains(QLatin1String("spanMhz")),
          "persisted inside the \"Hl2\" object");
    check(s.value(QStringLiteral("Hl2SpanMhz"), QString{}).toString().isEmpty(),
          "no loose flat span key");

    // Low bandwidth mode caps requests and the advertised limits at 96 kHz.
    s.setValue(QStringLiteral("LowBandwidthConnect"), QStringLiteral("True"));
    Access::settleZoom(b);
    spanSpy.clear();
    b.setPanBandwidth(pan, 384000.0);
    check(!spanSpy.isEmpty() && near(spanSpy.last().at(2).toDouble(), 0.096),
          "low bandwidth holds a 384 kHz request at 96 kHz");
    {
        Hl2Backend capped;
        QSignalSpy limits(&capped, &IRadioBackend::panBandwidthLimitsChanged);
        Access::metisEdge(capped, "linkUp");
        check(!limits.isEmpty() && near(limits.first().at(2).toDouble(), 0.096),
              "low bandwidth caps the advertised max span at 96 kHz");
        Access::metisEdge(capped, "linkDown");
    }
    s.setValue(QStringLiteral("LowBandwidthConnect"), QStringLiteral("False"));
}

static void manualMoxCancelsCwHang()
{
    TxTestAuthority authority;
    Hl2Backend b;
    b.setSliceMode(0, QStringLiteral("CW"));
    QSignalSpy txSpy(&b, &IRadioBackend::transmitChanged);

    b.setCwKeying(true, true, 30, authority.operation);
    b.setCwKeying(false, true, 30, authority.operation);
    check(Access::keyed(b) && Access::hangArmed(b), "a break-in element leaves MOX up on its hang");
    b.setKeying(true, authority.operation);   // operator takes over inside the hang
    txSpy.clear();
    Access::expireHang(b);
    check(Access::keyed(b) && txSpy.isEmpty(),
          "manual MOX inside the hang is not dropped when the hang expires");
    b.setKeying(false, authority.operation);
    check(!Access::keyed(b) && !txSpy.isEmpty()
              && !txSpy.last().at(0).value<TransmitDelta>().mox.value_or(true),
          "the manual release unkeys");
}

static void tunePowerAndRestore()
{
    TxTestAuthority authority;
    Hl2Backend b;
    const auto reg = [&b] { return health(b, "txDriveRegister").toInt(); };
    Access::startSession(b);

    b.setTxPower(100);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: RF power 100 reaches the drive register");
    b.setTune(true, 10, authority.operation);
    check(reg() == driveFor(10) && Access::wireDrive(b) == driveFor(10),
          "#4549: TUNE drives at tune power, not the RF power slider");
    b.setKeying(false, authority.operation);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: an unkey that bypasses setTune() still restores RF power");

    b.setTune(true, 10, authority.operation);
    b.setTune(false, 10, authority.operation);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: releasing TUNE restores RF power");

    b.setTune(true, 10, authority.operation);
    Access::wireDrive(b);
    b.setTxPower(40);
    check(reg() == driveFor(10) && Access::wireDrive(b) == -1,
          "#4549: a mid-tune power change leaves the tune carrier alone");
    b.setTune(false, 10, authority.operation);
    check(reg() == driveFor(40) && Access::wireDrive(b) == driveFor(40),
          "#4549: the unkey applies the power set during the tune");

    // #4912: requested vs written, in a TX-capable session.
    b.setTxPower(60);
    check(health(b, "rfPowerPercent").toInt() == 60, "#4912: health reports the requested percent");
    check(reg() == driveFor(60) && Access::wireDrive(b) == reg(),
          "#4912: health reports the register the wire carries");
    check(health(b, "txDriveGated").isValid() && !health(b, "txDriveGated").toBool(),
          "#4912: drive is not reported gated in a TX-capable session");
}

static void driveGateHealthRows()
{
    // The automation bridge without ALLOW_TX closes the gate at construction.
    qputenv("AETHER_AUTOMATION", "1");
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    Hl2Backend b;
    qunsetenv("AETHER_AUTOMATION");
    Access::startSession(b);

    check(health(b, "txDriveGated").toBool(),
          "#4912: the gate is reported before any drive is commanded");
    check(health(b, "rfPowerPercent").toInt() == 100, "#4912: the request reads its default 100");
    check(!b.healthSnapshot().values.contains(QStringLiteral("txDriveRegister")),
          "#4912: no register row before anything was written");

    b.setTxPower(100);
    check(health(b, "rfPowerPercent").toInt() == 100,
          "#4912: the operator's requested percent is still reported");
    check(health(b, "txDriveRegister").toInt() == 0 && Access::wireDrive(b) == 0,
          "#4912: the closed gate writes 0, and the row matches the wire");
    check(health(b, "txDriveGated").toBool(), "#4912: the gate is named in the rows");
}

static void notchIdsAreNeverReused()
{
    Hl2Backend b;
    QSignalSpy changed(&b, &IRadioBackend::notchChanged);
    QSignalSpy removed(&b, &IRadioBackend::notchRemoved);
    b.createNotch(7'041'000.0, 200.0);
    const int first = changed.isEmpty() ? -1 : changed.last().at(0).toInt();
    b.removeNotch(first);
    check(removed.count() == 1, "#4780: a placed notch can be removed by its id");

    b.createNotch(7'041'000.0, 200.0);
    const int second = changed.last().at(0).toInt();
    check(second > first, "#4780: a removed notch's id is not handed out again");
    changed.clear();
    removed.clear();
    NotchDelta move;
    move.centerHz = 7'042'000.0;
    b.setNotch(first, move);
    b.removeNotch(first);
    check(changed.isEmpty() && removed.isEmpty(), "#4780: a retired id addresses nothing");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-backend-seam-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    qRegisterMetaType<SliceDelta>();
    qRegisterMetaType<TransmitDelta>();
    qRegisterMetaType<NotchDelta>();
    check(Hl2Settings::spanMhz() == 0.0, "isolated profile starts with no remembered span");

    capabilitiesSeam();
    linkLifecycle();
    connectWithoutEp6();
    spanPolicy();
    manualMoxCancelsCwHang();
    tunePowerAndRestore();
    driveGateHealthRows();
    notchIdsAreNeverReused();

    std::fprintf(stderr, "hl2_backend_seam_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
