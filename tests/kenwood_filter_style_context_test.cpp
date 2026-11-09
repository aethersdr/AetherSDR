// Socket-free style/bank ownership through actual backend/model/protocol routes.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RigctlProtocol.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciProtocol.h"
#endif
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& messages)
    {
        const quint64 ingress = ++backend.m_controlIngressSequence;
        for (const QByteArray& message : messages) { backend.adopt(message, ingress); }
    }
    static bool axesPending(const KenwoodBackend& backend) { return backend.m_filterBankAxesPending; }
    static QVariantMap nativeStatus(const KenwoodBackend& backend) { return backend.nativeFilterStatus(); }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
using Access = kenwood::KenwoodBackendTestAccess;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void pump()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}
bool containsAxisSetter(const QList<QByteArray>& commands)
{
    for (const QByteArray& command : commands) {
        if ((command.startsWith("SL0") || command.startsWith("SH0")) && command.size() > 4) { return true; }
    }
    return false;
}
struct Fixture {
    QList<QByteArray> writes;
    RadioModel model;
    Backend* backend = nullptr;
    explicit Fixture(char mode = '2')
    {
        auto owned = std::make_unique<Backend>(Backend::Transport{
            [this](const QByteArray& command) { writes.append(command); return true; },
            [](const QByteArray&) { return false; }});
        backend = owned.get();
        model.setBackendForTest(std::move(owned), "kenwood");
        Access::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT0"),
                QByteArray("TS0"), QByteArray("MO20"), QByteArray("SF000014074000") + mode, QByteArray("RX"), QByteArray("NR0"),
                QByteArray("EX00611 000"), QByteArray("EX00612 000")}) { observe(message); }
        observe("EX00610 003");
        observe("FL001");
        observe("MO20");
        require(slice() && slice()->receiveControlsConfirmed(), "actual backend creates one confirmed receiver");
        if (mode == '3') { observe("SL010"); observe("SH0080"); }
        else { observe("SL002"); observe("SH0024"); }
        require(slice()->receiveObservation().filterLowHz && slice()->receiveObservation().filterHighHz,
            "fixture starts with actual complete filter observation");
        writes.clear();
    }
    SliceModel* slice() { return model.slice(0); }
    void observe(const QByteArray& message) { Access::observe(*backend, message); }
    void newStyleAxes() { observe("SL004"); observe("SH0002"); }
    void holdNewBank()
    {
        backend->invokeExtension("kenwood", "filter.banks.get", 1);
        observe("EX00610 003"); observe("FL001");
        observe("SL002"); observe("SH0024");
        observe("FL011");
        require(Access::axesPending(*backend), "new active bank waits for fresh native axes");
        writes.clear();
    }
};
QPair<int, int> actualEdges(SliceModel* slice)
{
    const SliceModel::ReceiveObservation observation = slice->receiveObservation();
    require(observation.filterLowHz && observation.filterHighHz, "actual edges are complete");
    return {*observation.filterLowHz, *observation.filterHighHz};
}
void nestedReadyStyleSwitch()
{
    Fixture fixture;
    fixture.holdNewBank();
    fixture.observe("SL004");
    QList<QPair<int, int>> publications;
    bool switched = false;
    QObject::connect(fixture.backend, &IRadioBackend::sliceChanged, &fixture.model,
        [&](int id, const SliceDelta& delta) {
            if (id == 0 && delta.filterLow && delta.filterHigh) { publications.append({*delta.filterLow, *delta.filterHigh}); }
        });
    QObject::connect(fixture.backend, &IRadioBackend::capabilitiesChanged, &fixture.model, [&] {
        if (!switched && !Access::axesPending(*fixture.backend)) {
            switched = true;
            fixture.observe("EX00611 001");
            fixture.newStyleAxes();
            require(actualEdges(fixture.slice()) == QPair<int, int>(50, 250),
                "nested style is fully qualified before old publisher resumes");
        }
    });
    // Both indexes are valid in both layouts: old low/high=300/800Hz;
    // new width/shift=200/150Hz gives50..250Hz. Missing revision fencing
    // can therefore publish a plausible but wrong old-style interval.
    fixture.observe("SH0002");
    require(switched && publications == QList<QPair<int, int>>{{50, 250}}
            && actualEdges(fixture.slice()) == QPair<int, int>(50, 250),
        "fully qualified nested style emits exactly one current projection and no outer stale or duplicate edges");
}
void activeStyleRetirementAndBatch()
{
    for (const char mode : {'2', '1', 'D', 'C'}) {
        Fixture fixture(mode);
        const QByteArray style = mode == 'D' || mode == 'C' ? "EX00612 001" : "EX00611 001";
        const quint64 epoch = fixture.slice()->receiveSessionGeneration();
        const quint64 filterContext = fixture.slice()->receiveFilterContextGeneration();
        fixture.slice()->recordTuneIntent(14.0741);
        Access::batch(*fixture.backend, {style, "SL002", "SH0024"});
        require(fixture.slice()->receiveSessionGeneration() == epoch
                && fixture.slice()->receiveFilterContextGeneration() > filterContext
                && std::abs(fixture.slice()->incrementalTuneTarget(0.0) - 14.0741) < 1e-9
                && !fixture.slice()->receiveObservation().filterLowHz && !fixture.slice()->receiveObservation().filterHighHz,
            "active style retires only filter context, preserves pending tune and rejects same-batch old indexes");
        fixture.observe("SL004");
        require(!fixture.slice()->receiveObservation().filterLowHz && !fixture.slice()->receiveObservation().filterHighHz,
            "one new-style axis cannot restore an old-style companion");
        fixture.observe("SH0002");
        const bool lower = mode == '1' || mode == 'C';
        require(actualEdges(fixture.slice()) == (lower ? QPair<int, int>(-250, -50) : QPair<int, int>(50, 250)),
            "fresh complete active style restores only its actual signed projection");
    }
}
void reentrantFilterIntentRetired()
{
    Fixture fixture;
    bool switched = false;
    int requests = 0;
    QObject::connect(fixture.slice(), &SliceModel::receiveFilterRequested, &fixture.model,
        [&](const SliceFilterRequest&) { ++requests; });
    QObject::connect(fixture.slice(), &SliceModel::filterCommandIssued, &fixture.model, [&](int, int) {
        if (!switched) {
            switched = true;
            fixture.observe("EX00611 001"); fixture.newStyleAxes();
        }
    });
    fixture.writes.clear();
    fixture.slice()->setFilterWidth(100, 2800);
    require(switched && requests == 0 && !containsAxisSetter(fixture.writes)
            && actualEdges(fixture.slice()) == QPair<int, int>(50, 250),
        "actual style-context change retires outer model filter intent before backend dispatch");
}
void queuedProtocolFiltersRetired()
{
    for (const bool tci : {false, true}) {
#ifndef HAVE_WEBSOCKETS
        if (tci) { continue; }
#endif
        Fixture fixture;
        RigctlProtocol rig(&fixture.model);
#ifdef HAVE_WEBSOCKETS
        TciProtocol protocol(&fixture.model);
        if (tci) {
            protocol.handleCommand("rx_filter_band:0,100,1100");
            protocol.handleCommand("rx_nr_enable:0,true");
            require(protocol.pendingNotification().isEmpty(), "confirmed queued TCI filter has no optimistic notification");
        } else
#endif
        { require(rig.handleLine("M USB 1000") == "RPRT 0\n", "old-style same-mode Rigctl filter request is initially admissible"); }
        const quint64 epoch = fixture.slice()->receiveSessionGeneration();
        const quint64 filterContext = fixture.slice()->receiveFilterContextGeneration();
        fixture.observe("EX00611 001"); fixture.newStyleAxes();
        require(fixture.slice()->receiveSessionGeneration() == epoch
                && fixture.slice()->receiveFilterContextGeneration() > filterContext
                && Access::nativeStatus(*fixture.backend).value("editable").toBool()
                && actualEdges(fixture.slice()) == QPair<int, int>(50, 250),
            "new style is fully qualified and editable before old protocol queue is delivered");
        fixture.writes.clear(); pump();
        require(!containsAxisSetter(fixture.writes)
                && actualEdges(fixture.slice()) == QPair<int, int>(50, 250),
            "old queued Rigctl or TCI filter cannot mutate fully qualified replacement style");
        require(fixture.writes.contains(tci ? QByteArray("NR1;") : QByteArray("OM02;")),
            "filter-only context change preserves queued nonfilter mode or noise-reduction intent");
    }
}
void queuedFilterIntentsShareOnlyContextOwnership()
{
    for (const bool tci : {false, true}) {
#ifndef HAVE_WEBSOCKETS
        if (tci) { continue; }
#endif
        Fixture fixture;
        RigctlProtocol rig(&fixture.model);
        const quint64 context = fixture.slice()->receiveFilterContextGeneration();
        const QPair<int, int> observed = actualEdges(fixture.slice());
#ifdef HAVE_WEBSOCKETS
        TciProtocol protocol(&fixture.model);
        if (tci) {
            protocol.handleCommand("rx_filter_band:0,100,1200");
            protocol.handleCommand("rx_filter_band:0,100,1600");
        } else
#endif
        {
            require(rig.handleLine("M USB 1000") == "RPRT 0\n" && rig.handleLine("M USB 1500") == "RPRT 0\n",
                "two current-context Rigctl filter requests are admitted before event delivery");
        }
        fixture.writes.clear(); pump();
        require(fixture.writes.contains(tci ? QByteArray("SH0006;") : QByteArray("SH0005;"))
                && fixture.writes.contains("SH0010;")
                && fixture.slice()->receiveFilterContextGeneration() == context && actualEdges(fixture.slice()) == observed,
            "ordinary filter intent revision does not cancel newer queued request or invent radio readback");
    }
}
void confirmedFilterQueriesUseAcceptedEdges()
{
    const auto known = [](Fixture& fixture, int low, int high) {
        RigctlProtocol rig(&fixture.model);
        const int width = high - low; // All positive fixtures are small physical intervals.
        const QString modeReply = rig.handleLine("m");
        if (modeReply != QStringLiteral("USB\n%1\n").arg(width)) {
            std::cerr << "Expected accepted width " << width << ", got: " << modeReply.toStdString();
        }
        require(modeReply == QStringLiteral("USB\n%1\n").arg(width), "Rigctl GET uses current accepted width");
        require(rig.handleLine("\\get_vfo_info VFOA") == QStringLiteral("14074000\nUSB\n%1\n0\n0\n").arg(width),
            "Rigctl VFO info uses the same accepted width");
        rig.setExtendedMode(true);
        require(rig.handleLine("m").contains(QStringLiteral("Passband: %1\n").arg(width))
                && rig.handleLine("\\get_vfo_info VFOA").contains(QStringLiteral("Width: %1\n").arg(width)),
            "extended Rigctl mode and VFO info preserve accepted width formatting");
#ifdef HAVE_WEBSOCKETS
        TciProtocol protocol(&fixture.model);
        const QString response = QStringLiteral("rx_filter_band:0,%1,%2;").arg(low).arg(high);
        require(protocol.handleCommand("rx_filter_band:0") == response && protocol.generateInitBurst().contains(response),
            "TCI GET and init use the same accepted filter pair");
#endif
    };
    const auto unknown = [](Fixture& fixture) {
        RigctlProtocol rig(&fixture.model);
        require(rig.handleLine("m") == "RPRT -11\n" && rig.handleLine("\\get_vfo_info VFOA") == "RPRT -11\n",
            "known mode with unknown or invalid width returns unavailable, never normal zero or old display width");
        rig.setExtendedMode(true);
        require(rig.handleLine("m") == "RPRT -11\n" && rig.handleLine("\\get_vfo_info VFOA") == "RPRT -11\n",
            "extended queries cannot advertise partial success for unknown width");
#ifdef HAVE_WEBSOCKETS
        TciProtocol protocol(&fixture.model);
        require(protocol.handleCommand("rx_filter_band:0").isEmpty() && !protocol.generateInitBurst().contains("rx_filter_band:"),
            "unknown or invalid accepted filter pair is omitted from TCI GET and init");
#endif
    };
    Fixture fixture;
    known(fixture, 100, 3000);
    fixture.slice()->setFilterWidth(100, 1100);
    require(fixture.writes.contains("SH0005;"), "pending native setter was actually dispatched");
    known(fixture, 100, 3000);
    fixture.observe("TS1"); unknown(fixture);
    fixture.observe("TS0"); unknown(fixture);
    for (const QByteArray& fresh : {QByteArray("EX00610 003"), QByteArray("FL001"), QByteArray("MO20"),
            QByteArray("SL002"), QByteArray("SH0024")}) { fixture.observe(fresh); }
    known(fixture, 100, 3000);
    fixture.observe("EX00611 001");
    require(fixture.slice()->filterLow() == 100 && fixture.slice()->filterHigh() == 3000,
        "retirement fixture retains old UI edges, so omission cannot pass by clearing the display");
    unknown(fixture);
    fixture.observe("SL004"); unknown(fixture);
    fixture.observe("SH0002"); known(fixture, 50, 250);
    fixture.holdNewBank(); unknown(fixture);
    fixture.newStyleAxes(); known(fixture, 50, 250);
    // Normalized partial observations also exercise a same-valued companion:
    // low arrives first; high is exactly the retained legacy display high.
    Fixture partial;
    SliceDelta retired; retired.receiveFilterContextChanged = true;
    emit partial.backend->sliceChanged(0, retired); unknown(partial);
    SliceDelta low; low.filterLow = 200;
    emit partial.backend->sliceChanged(0, low); unknown(partial);
    SliceDelta high; high.filterHigh = 3000;
    emit partial.backend->sliceChanged(0, high); known(partial, 200, 3000);
    for (const QPair<int, int>& pair : {QPair<int, int>{400, 100}, QPair<int, int>{100, 100},
            QPair<int, int>{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}}) {
        Fixture invalid;
        SliceDelta delta; delta.filterLow = pair.first; delta.filterHigh = pair.second;
        emit invalid.backend->sliceChanged(0, delta); unknown(invalid);
    }
    fixture.backend->disconnectRadio();
    RigctlProtocol disconnected(&fixture.model);
    require(disconnected.handleLine("m") == "RPRT -8\n" && disconnected.handleLine("\\get_vfo_info VFOA") == "RPRT -8\n",
        "disconnected mode retains existing unavailable-mode response without exposing old width");
#ifdef HAVE_WEBSOCKETS
    TciProtocol protocol(&fixture.model);
    require(protocol.handleCommand("rx_filter_band:0").isEmpty() && !protocol.generateInitBurst().contains("rx_filter_band:"),
        "disconnected TCI queries and init cannot reuse a retained filter observation");
#endif
}
void inactiveStylePreservesCurrentAxes()
{
    for (const QByteArray& style : {QByteArray("EX00611 001"), QByteArray("EX00612 001")}) {
        Fixture fixture('3');
        const quint64 epoch = fixture.slice()->receiveSessionGeneration();
        const quint64 filterContext = fixture.slice()->receiveFilterContextGeneration();
        const QPair<int, int> edges = actualEdges(fixture.slice());
        fixture.observe(style);
        require(fixture.slice()->receiveSessionGeneration() == epoch
                && fixture.slice()->receiveFilterContextGeneration() == filterContext && actualEdges(fixture.slice()) == edges
                && !Access::axesPending(*fixture.backend) && fixture.writes.isEmpty(),
            "inactive SSB/data style metadata preserves CW axes and epoch without querying or mutating them");
    }
    Fixture fixture;
    const quint64 epoch = fixture.slice()->receiveSessionGeneration();
    const quint64 filterContext = fixture.slice()->receiveFilterContextGeneration();
    const QPair<int, int> edges = actualEdges(fixture.slice());
    fixture.observe("EX00612 001");
    require(fixture.slice()->receiveSessionGeneration() == epoch
            && fixture.slice()->receiveFilterContextGeneration() == filterContext && actualEdges(fixture.slice()) == edges
            && fixture.writes.isEmpty(), "inactive data style preserves active ordinary SSB axes");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("kenwood-filter-style-context"));
    require(settings.isValid(), "isolated settings profile");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    nestedReadyStyleSwitch();
    activeStyleRetirementAndBatch();
    reentrantFilterIntentRetired();
    queuedProtocolFiltersRetired();
    queuedFilterIntentsShareOnlyContextOwnership();
    confirmedFilterQueriesUseAcceptedEdges();
    inactiveStylePreservesCurrentAxes();
    std::cout << "Kenwood filter style context checks passed\n";
}
