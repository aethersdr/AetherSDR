// Hl2Backend's IRadioBackend seam on a default-constructed backend: no
// connectRadio(), no socket, no peer. Link edges are injected by emitting
// MetisClient's own signals on the I/O thread, timers are fired by hand, and
// the drive register is read both from the health rows and from the EP2
// packet MetisClient would send.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "gui/ClientFftSmoothingGate.h"
#include "core/AppSettings.h"

#include "core/backends/hl2/Hl2TxLevelPolicy.h"

#include "Hl2KeyOnRaddr1Fixture.h"
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c) { c.m_running = true; }
    static void feedDatagram(MetisClient& c, std::span<const std::uint8_t> bytes)
    {
        c.handleDatagram(bytes);
    }
    static void holdEmit(MetisClient& c) { c.m_telemetryEmitClock.restart(); }
    static void dueEmit(MetisClient& c) { c.m_telemetryEmitClock.invalidate(); }
    static qint64 emitIntervalMs() { return MetisClient::kTelemetryMinIntervalMs; }
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
    // Receiver 0 gets a configured chain on this thread, owned by the backend
    // (its destructor deletes m_rx[].dsp), so pushed verbs land in a real spectrum.
    static Hl2RxDsp* attachSpectrumDsp(Hl2Backend& b)
    {
        if (b.m_rx.empty())
            return nullptr;
        auto* dsp = new Hl2RxDsp(nullptr);
        Hl2RxDsp::Config cfg;
        cfg.inputSampleRateHz = 48000;
        cfg.audioSampleRateHz = 48000;
        cfg.dspBlockSize = 1024;
        cfg.fftSize = 256;
        cfg.blockForOutput = true;
        std::string err;
        if (!dsp->configure(cfg, &err)) {
            delete dsp;
            return nullptr;
        }
        b.m_rx[0].dsp = dsp;
        return dsp;
    }
    static int panAverage(const Hl2Backend& b) { return b.m_rx[0].panAverage; }
    static bool panWeighted(const Hl2Backend& b) { return b.m_rx[0].panWeightedAverage; }
    static int receiverCeiling(const Hl2Backend& b) { return b.receiverCeiling(); }
    static bool keyed(const Hl2Backend& b) { return b.m_keyed; }
    static void setKeyedFlag(Hl2Backend& b, bool keyed) { b.m_keyed = keyed; }
    static void publish(Hl2Backend& b, const Hl2Telemetry& t) { b.publishTelemetry(t); }
    static double fwdPeakReleaseAlpha() { return Hl2Backend::kFwdPeakReleaseAlpha; }
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
    // The radio's transmit tail after an unkey (Hl2Backend::kTxTailHoldMs):
    // parked so it cannot end on its own, and expired here by hand.
    static void parkTxTail(Hl2Backend& b) { b.m_txTailHoldMs = 60 * 60 * 1000; }
    static bool txTailPending(const Hl2Backend& b) { return b.m_txTailPending; }
    static void expireTxTail(Hl2Backend& b)
    {
        b.m_txTailTimer->stop();
        b.finishTxTail();
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
    // The HL2 averages its own panadapter (Hl2Spectrum, per FFT AVG), so it
    // engages the record that turns SpectrumWidget's EMA off (RFC #5782 §8).
    check(caps.backendPanAveraging.has_value()
              && caps.backendPanAveraging->msPerAverageStep == Hl2Backend::kMsPerAverageStep
              && Hl2Backend::kMsPerAverageStep == 10,
          "backendPanAveraging engaged at 10 ms per FFT AVG step");
    check(!AetherSDR::clientFftSmoothingEnabled(true, caps.backendPanAveraging.has_value()),
          "connected to an HL2, the widget's own EMA is skipped");
    check(AetherSDR::clientFftSmoothingEnabled(false, caps.backendPanAveraging.has_value())
              && AetherSDR::clientFftSmoothingEnabled(true, false),
          "control: disconnected, or a backend that does not average, keeps the EMA");
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
    Access::parkTxTail(b);

    b.setTxPower(100);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: RF power 100 reaches the drive register");
    b.setTune(true, 10, authority.operation);
    check(reg() == driveFor(10) && Access::wireDrive(b) == driveFor(10),
          "#4549: TUNE drives at tune power, not the RF power slider");
    // The radio still radiates the tune tone for its transmit tail after the
    // MOX-off, so RF power comes back only when the tail ends (#6104).
    b.setKeying(false, authority.operation);
    check(Access::txTailPending(b) && reg() == driveFor(10) && Access::wireDrive(b) == -1,
          "#4549: an unkey that bypasses setTune() holds the tune drive through the tail");
    Access::expireTxTail(b);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: an unkey that bypasses setTune() still restores RF power after the tail");

    b.setTune(true, 10, authority.operation);
    b.setTune(false, 10, authority.operation);
    check(Access::txTailPending(b) && reg() == driveFor(10) && Access::wireDrive(b) == driveFor(10),
          "#4549: releasing TUNE holds the tune drive through the tail");
    Access::expireTxTail(b);
    check(reg() == driveFor(100) && Access::wireDrive(b) == driveFor(100),
          "#4549: releasing TUNE restores RF power after the tail");

    b.setTune(true, 10, authority.operation);
    Access::wireDrive(b);
    b.setTxPower(40);
    check(reg() == driveFor(10) && Access::wireDrive(b) == -1,
          "#4549: a mid-tune power change leaves the tune carrier alone");
    b.setTune(false, 10, authority.operation);
    check(reg() == driveFor(10) && Access::wireDrive(b) == -1,
          "#4549: the power set during the tune waits for the tail");
    Access::expireTxTail(b);
    check(reg() == driveFor(40) && Access::wireDrive(b) == driveFor(40),
          "#4549: the tail's end applies the power set during the tune");

    // #4912: requested vs written, in a TX-capable session.
    b.setTxPower(60);
    check(health(b, "rfPowerPercent").toInt() == 60, "#4912: health reports the requested percent");
    check(reg() == driveFor(60) && Access::wireDrive(b) == reg(),
          "#4912: health reports the register the wire carries");
    check(health(b, "txDriveGated").isValid() && !health(b, "txDriveGated").toBool(),
          "#4912: drive is not reported gated in a TX-capable session");

    // #4912 inside a tail: the request moves, the register and wire do not.
    b.setTune(true, 10, authority.operation);
    b.setTune(false, 10, authority.operation);
    Access::wireDrive(b);
    b.setTxPower(70);
    check(health(b, "rfPowerPercent").toInt() == 70 && reg() == driveFor(10)
              && Access::wireDrive(b) == -1,
          "#4912: during the tail, health reports the tune register the wire still carries");
    Access::expireTxTail(b);
    check(reg() == driveFor(70) && Access::wireDrive(b) == reg(),
          "#4912: after the tail, health reports the restored register the wire carries");
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

// TX:FWDPWR takes the window peak only while keyed; unkeyed it keeps the last
// value. Only the m_keyed flag is set: no transmitter exists here.
static void forwardPowerWindowPeakWhileKeyed()
{
    Hl2Backend b;
    QSignalSpy meters(&b, &IRadioBackend::meterUpdate);
    auto fwdDbm = [&](bool keyed, int last, std::optional<int> peak) {
        Access::setKeyedFlag(b, keyed);
        hl2::Hl2Telemetry t;
        t.forwardPowerRaw = last;
        t.forwardPowerPeakRaw = peak;
        meters.clear();
        Access::publish(b, t);
        for (const auto& args : meters) {
            if (args.at(0).toString() == QStringLiteral("TX:FWDPWR"))
                return args.at(1).toDouble();
        }
        return std::nan("");
    };
    const double loud = fwdDbm(false, 3000, std::nullopt);
    const double quiet = fwdDbm(false, 100, std::nullopt);
    check(std::isfinite(loud) && std::isfinite(quiet) && loud > quiet,
          "fwd power: 3000 counts reads above 100 counts");
    check(fwdDbm(false, 100, 3000) == quiet,
          "fwd power unkeyed: the last value, not the window peak");
    check(fwdDbm(true, 100, 3000) == loud,
          "fwd power keyed: the window peak (3000), not the last value (100)");
    Access::setKeyedFlag(b, false);
    fwdDbm(false, 100, std::nullopt);
    check(fwdDbm(true, 100, std::nullopt) == quiet,
          "fwd power keyed with no RADDR 1 in the window: falls back to the last value");
    Access::setKeyedFlag(b, false);
}

// The Radio Health row for responses the peak window skipped: a session total,
// absent until a RADDR-1 response arrived. No radio: telemetry is handed in.
static void forwardPowerSkippedHealthRow()
{
    static const char* const kRow = "forwardPowerSkippedTotal";
    const auto publish = [](Hl2Backend& b, int kept, int skipped) {
        hl2::Hl2Telemetry t;
        t.forwardPowerRaw = 1200;
        if (kept > 0)
            t.forwardPowerPeakRaw = 1300;
        t.forwardPowerSamples = kept;
        t.forwardPowerSkipped = skipped;
        Access::publish(b, t);
    };
    Hl2Backend b;
    check(b.healthSnapshot().order.contains(QString::fromLatin1(kRow))
              && !health(b, kRow).isValid(),
          "skipped row: declared, and not reported before any RADDR-1 response");
    Access::publish(b, hl2::Hl2Telemetry{});
    check(!health(b, kRow).isValid(),
          "skipped row: telemetry without a RADDR-1 response leaves it not reported");
    publish(b, 19, 0);
    check(health(b, kRow).isValid() && health(b, kRow).toULongLong() == 0,
          "skipped row: 0 once responses arrived and none was skipped");
    publish(b, 17, 2);
    check(health(b, kRow).toULongLong() == 2, "skipped row: counts a window's skipped responses");
    publish(b, 19, 0);
    check(health(b, kRow).toULongLong() == 2,
          "skipped row: a session total, kept across a window that skipped nothing");
    publish(b, 0, 19);
    check(health(b, kRow).toULongLong() == 21,
          "skipped row: a window of skipped responses only adds all of them");

    // A unit whose temperature word reads 0 from the first response on.
    Hl2Backend dead;
    publish(dead, 0, 19);
    check(health(dead, kRow).isValid() && health(dead, kRow).toULongLong() == 19,
          "skipped row: reported when every response so far was skipped");
}

// What one key-on replay published, window by window.
struct KeyOnReplay {
    int windows = 0;
    int peakDiffersFromPlainMax = 0;   // windows whose peak is not the plain maximum
    int publishedTop = -1;             // highest forward word in any published window
    double shownMaxW = 0.0;            // highest TX:FWDPWR of the over
    double shownLastW = 0.0;
    double plainMaxHeldW = 0.0;        // the same over through a plain window maximum
    qulonglong skippedShown = 0;       // Radio Health's skipped total after the replay
    bool skippedReported = false;
};

// Recorded RADDR-1 responses through MetisClient's receive loop and
// Hl2Backend::publishTelemetry, keyed, with the first emit `phaseMs` after
// key-on and one every publish interval after it. No socket, no transmitter.
static KeyOnReplay replayKeyOn(std::span<const Hl2KeyOnRaddr1> rows, int phaseMs)
{
    using MetisAccess = hl2::MetisClientTestAccess;
    hl2::MetisClient client;
    MetisAccess::setStreaming(client);
    Hl2Backend b;
    Access::setKeyedFlag(b, true);
    QSignalSpy meters(&b, &IRadioBackend::meterUpdate);

    KeyOnReplay out;
    int plainMax = -1;
    double plainHeldW = 0.0;
    QObject::connect(&client, &hl2::MetisClient::telemetryUpdated,
                     [&](const hl2::Hl2Telemetry& t) {
        ++out.windows;
        if (t.forwardPowerPeakRaw.value_or(-1) != plainMax)
            ++out.peakDiffersFromPlainMax;
        out.publishedTop = std::max(out.publishedTop, plainMax);
        meters.clear();
        Access::publish(b, t);
        for (const QList<QVariant>& args : meters) {
            if (args.at(0).toString() == QStringLiteral("TX:FWDPWR"))
                out.shownLastW = std::pow(10.0, args.at(1).toDouble() / 10.0) / 1000.0;
        }
        out.shownMaxW = std::max(out.shownMaxW, out.shownLastW);
        plainHeldW = hl2::fwdPeakHoldStep(plainHeldW, hl2::directionalWatts(plainMax),
                                          /*keyed=*/true, Access::fwdPeakReleaseAlpha());
        out.plainMaxHeldW = std::max(out.plainMaxHeldW, plainHeldW);
        plainMax = -1;
    });

    const std::int64_t intervalUs = MetisAccess::emitIntervalMs() * 1000;
    std::int64_t dueUs = std::int64_t{phaseMs} * 1000;
    std::uint32_t seq = 0;
    for (const Hl2KeyOnRaddr1& row : rows) {
        std::vector<std::uint8_t> pkt(hl2::kUsbPacketSize, 0);
        pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
        pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
        pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
        ++seq;
        // Frame 1 carries the RADDR-1 response, frame 2 an empty RADDR-3 slot.
        const std::size_t f1 = 8;
        const std::size_t f2 = 8 + hl2::kFrameSize;
        pkt[f1] = pkt[f1 + 1] = pkt[f1 + 2] = 0x7F;
        pkt[f1 + 3] = 0x01 << 3;
        pkt[f1 + 4] = static_cast<std::uint8_t>(row.temperature >> 8);
        pkt[f1 + 5] = static_cast<std::uint8_t>(row.temperature & 0xFF);
        pkt[f1 + 6] = static_cast<std::uint8_t>(row.forward >> 8);
        pkt[f1 + 7] = static_cast<std::uint8_t>(row.forward & 0xFF);
        pkt[f2] = pkt[f2 + 1] = pkt[f2 + 2] = 0x7F;
        pkt[f2 + 3] = 0x03 << 3;

        plainMax = std::max(plainMax, static_cast<int>(row.forward));
        if (row.usAfterMox >= dueUs) {
            while (row.usAfterMox >= dueUs)
                dueUs += intervalUs;
            MetisAccess::dueEmit(client);
        } else {
            MetisAccess::holdEmit(client);
        }
        MetisAccess::feedDatagram(client, pkt);
    }
    Access::setKeyedFlag(b, false);
    const QVariant skipped = health(b, "forwardPowerSkippedTotal");
    out.skippedReported = skipped.isValid();
    out.skippedShown = skipped.toULongLong();
    return out;
}

// TX:FWDPWR at key-on, replayed from recorded RADDR-1 words (Hl2KeyOnRaddr1Fixture.h)
// at ten phases of the publish window. Nothing is keyed here.
static void forwardPowerKeyOnReplay()
{
    static constexpr int kPhasesMs[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    // 1. The 40 % tone over holds one forward word, 3.2 ms after key-on, far
    // above anything the carrier reaches. The gauge must show the carrier.
    {
        int carrierMax = 0;
        int headMax = 0;
        for (const Hl2KeyOnRaddr1& row : kHl2KeyOnTone40Lone) {
            if (row.usAfterMox >= 100'000)
                carrierMax = std::max(carrierMax, static_cast<int>(row.forward));
            else if (row.usAfterMox >= 0 && row.usAfterMox < 10'000)
                headMax = std::max(headMax, static_cast<int>(row.forward));
        }
        qulonglong marked = 0;
        for (const Hl2KeyOnRaddr1& row : kHl2KeyOnTone40Lone)
            marked += row.temperature == 0 ? 1 : 0;
        const double carrierW = hl2::directionalWatts(carrierMax);
        check(carrierMax > 1000 && headMax > carrierMax + 1000,
              "control: the recording holds a forward word in the first 10 ms far above its carrier");
        bool shownIsCarrier = true;
        bool plainMaxShowsIt = true;
        bool gaugeAlive = true;
        bool skippedCounted = true;
        double worstShownW = 0.0;
        double worstPlainW = 0.0;
        for (const int phase : kPhasesMs) {
            const KeyOnReplay r = replayKeyOn(kHl2KeyOnTone40Lone, phase);
            shownIsCarrier = shownIsCarrier && r.windows >= 9 && r.shownMaxW <= carrierW + 1e-9;
            plainMaxShowsIt = plainMaxShowsIt && r.plainMaxHeldW > 2.0 * carrierW;
            gaugeAlive = gaugeAlive && r.shownLastW > 0.9 * carrierW;
            skippedCounted = skippedCounted && r.skippedReported && r.skippedShown == marked;
            worstShownW = std::max(worstShownW, r.shownMaxW);
            worstPlainW = std::max(worstPlainW, r.plainMaxHeldW);
        }
        std::fprintf(stderr, "       tone 40 %%: carrier maximum %.3f W; shown at most %.3f W;"
                             " a plain window maximum shows %.3f W\n",
                     carrierW, worstShownW, worstPlainW);
        check(plainMaxShowsIt,
              "control: a plain window maximum displays that word, over twice the carrier");
        check(shownIsCarrier,
              "key-on replay, tone 40 %: TX:FWDPWR never exceeds the carrier's own maximum");
        check(gaugeAlive, "key-on replay, tone 40 %: the gauge ends the first second on the carrier");
        check(marked > 0 && skippedCounted,
              "key-on replay, tone 40 %: Radio Health counts the recording's marked responses, no more");
    }

    // 2. TUNE at 100 % and at 10 %: every window publishes the plain maximum,
    // so the rise and the level are what they were, window for window.
    const auto unchanged = [&](std::span<const Hl2KeyOnRaddr1> rows, const char* what) {
        bool same = true;
        double shownW = 0.0;
        for (const int phase : kPhasesMs) {
            const KeyOnReplay r = replayKeyOn(rows, phase);
            same = same && r.windows >= 3 && r.peakDiffersFromPlainMax == 0
                && std::abs(r.shownMaxW - hl2::directionalWatts(r.publishedTop)) < 1e-9
                && std::abs(r.shownMaxW - r.plainMaxHeldW) < 1e-9;
            shownW = std::max(shownW, r.shownMaxW);
        }
        std::fprintf(stderr, "       %s: shown at most %.3f W\n", what, shownW);
        check(same, what);
    };
    unchanged(kHl2KeyOnTune100,
              "key-on replay, TUNE 100 %: every window is the plain maximum, at every phase");
    unchanged(kHl2KeyOnTune10,
              "key-on replay, TUNE 10 %: every window is the plain maximum, at every phase");

    // 3. SYNTHETIC, not a recording: a speech-like envelope at 190 responses a
    // second with one-sample peaks between quiet neighbours, and a live
    // temperature word. Every such peak must still win its window.
    {
        std::vector<Hl2KeyOnRaddr1> speech;
        for (int i = 0; i < 380; ++i) {
            const bool peak = (i % 23) == 11;
            const int forward = peak ? 2200 + (i * 37) % 1200 : 150 + (i * 53) % 200;
            speech.push_back({i * 5263, 1050, static_cast<std::uint16_t>(forward)});
        }
        bool caught = true;
        bool noneSkipped = true;
        for (const int phase : kPhasesMs) {
            const KeyOnReplay r = replayKeyOn(speech, phase);
            noneSkipped = noneSkipped && r.skippedReported && r.skippedShown == 0;
            caught = caught && r.windows >= 19 && r.peakDiffersFromPlainMax == 0
                && r.publishedTop >= 2200
                && std::abs(r.shownMaxW - hl2::directionalWatts(r.publishedTop)) < 1e-9;
        }
        check(caught, "synthetic speech: every one-sample peak between polls still wins its window");
        check(noneSkipped, "synthetic speech: Radio Health reports 0 skipped responses");
    }
}

static void panAveragingSeam()
{
    // FFT AVG through the seam verbs RadioModel calls (RFC #5782 q2): stored
    // on the receiver for rebuilds, and pushed into the live spectrum.
    Hl2Backend b;
    hl2::Hl2RxDsp* dsp = Access::attachSpectrumDsp(b);
    check(dsp != nullptr, "receiver 0 holds a configured chain");
    if (!dsp)
        return;
    check(dsp->spectrumAverageMsApplied() == 0.0 && !dsp->spectrumLogAverageApplied(),
          "control: a fresh chain does not average, in power");
    b.setPanAverage(QString(), 30);
    b.setPanWeightedAverage(QString(), true);
    QCoreApplication::sendPostedEvents(dsp, QEvent::MetaCall);
    check(Access::panAverage(b) == 30 && Access::panWeighted(b),
          "FFT AVG and weighted are held on the receiver for a rebuilt chain");
    check(dsp->spectrumAverageMsApplied() == 300.0, "FFT AVG 30 reaches the spectrum as 300 ms");
    check(dsp->spectrumLogAverageApplied(), "weighted on reaches the spectrum as log-recursive");
    b.setPanAverage(QString(), 250);
    b.setPanWeightedAverage(QString(), false);
    QCoreApplication::sendPostedEvents(dsp, QEvent::MetaCall);
    check(Access::panAverage(b) == 100 && dsp->spectrumAverageMsApplied() == 1000.0,
          "FFT AVG above range clamps to 100 (1 s)");
    check(!dsp->spectrumLogAverageApplied(), "weighted off returns the spectrum to power");
    b.setPanAverage(QStringLiteral("no-such-pan"), 5);
    QCoreApplication::sendPostedEvents(dsp, QEvent::MetaCall);
    check(Access::panAverage(b) == 100 && dsp->spectrumAverageMsApplied() == 1000.0,
          "an unknown pan id changes nothing");
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
    forwardPowerWindowPeakWhileKeyed();
    forwardPowerSkippedHealthRow();
    forwardPowerKeyOnReplay();
    panAveragingSeam();

    std::fprintf(stderr, "hl2_backend_seam_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
