// #5904: the production model bindings, not a second implementation of them.
// Injected state/dispatch only: no sockets, firmware peer, hardware or keying.
// WAN fixtures inject connected state without opening the TLS socket.
#include "TestSettingsProfile.h"
#include "core/backends/flex/WanConnection.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <QSignalSpy>
#include <QPointer>
#include <QPointer>
#include <QStringList>
#include <QtLogging>
#include <cstdio>
#include <functional>
#include <thread>
#include <array>
#include <limits>
#include <memory>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void publishCapabilities(RadioModel& radio) { radio.publishCapabilities(true); }
    static void wire(RadioModel& radio, SliceModel* slice)
    {
        radio.wireSliceReceiveIntentsToBackend(slice);
    }
    static void refreshPolicy(RadioModel& radio, SliceModel* slice)
    {
        radio.wireSliceObservationsAndTxIntentToBackend(slice);
    }
    // Bypass the live handshake; exercise the production WAN predicate.
    static void setWanConnection(RadioModel& radio, WanConnection* wan)
    {
        radio.m_wanConn = wan;
    }
};
class WanConnectionTestAccess {
public:
    static void setConnected(WanConnection& wan, bool connected)
    {
        wan.m_connected = connected;
    }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
thread_local QStringList warningMessages;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class Backend final : public IRadioBackend {
public:
    bool connected = false;
    bool confirmed = false;
    QList<int> pitches;
    QList<bool> ritEnables;
    QList<int> ritOffsets;
    void setSliceRitEnabled(int id, bool enabled) override
    { ritEnables.append(enabled); if (observe) { observe(id); } }
    void setSliceRitOffset(int, int hz) override { ritOffsets.append(hz); }
    ReceiveControlPolicy receiveControlPolicy() const override
    { return confirmed ? ReceiveControlPolicy::Confirmed : ReceiveControlPolicy::Optimistic; }
    void setCwPitch(int hz) override { pitches.append(hz); }
    QList<SliceTuneRequest> tunes;
    QList<SliceFilterRequest> filters;
    QList<SliceAgcRequest> agcs;
    QStringList modes;
    QList<SliceDspRequest> dsps;
    QList<SliceAudioRequest> audio;
    QList<SliceSquelchRequest> squelch;
    QStringList antennas;
    QList<bool> locks;
    ReceiveDispatch receiveResult = ReceiveDispatch::Dispatched;
    int pairedAgcCalls = 0;
    int keyCalls = 0;
    std::function<void(int)> observe;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        if (confirmed) { caps.receiveCwPitchControl = ReceiveCwPitchControl{}; }
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    bool tuneAccepted = true;
    bool requestReceiveTune(int id, double hz, ReceiveTuneView view) override
    {
        if (!tuneAccepted) { return false; }
        requestSliceTune(id, {hz, view == ReceiveTuneView::Center
            ? SliceTuneRequest::PanIntent::AllowRecenter
            : SliceTuneRequest::PanIntent::PreservePan});
        return true;
    }
    void requestSliceTune(int id, const SliceTuneRequest& request) override
    {
        tunes.append(request);
        IRadioBackend::requestSliceTune(id, request);
    }
    void requestSliceFilter(int id, const SliceFilterRequest& request) override
    {
        filters.append(request);
        IRadioBackend::requestSliceFilter(id, request);
    }
    void requestSliceAgc(int id, const SliceAgcRequest& request) override
    {
        agcs.append(request);
        IRadioBackend::requestSliceAgc(id, request);
    }
    void setSliceFrequency(int id, double) override { if (observe) { observe(id); } }
    void setSliceMode(int id, const QString& mode) override
    {
        modes.append(mode);
        if (observe) { observe(id); }
    }
    void setSliceFilter(int id, int, int) override { if (observe) { observe(id); } }
    void setSliceAgc(int id, const QString&, int) override
    {
        ++pairedAgcCalls;
        if (observe) { observe(id); }
    }
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    ReceiveDispatch requestSliceDsp(int id, const SliceDspRequest& request) override
    {
        dsps.append(request);
        if (observe) { observe(id); }
        return receiveResult;
    }
    ReceiveDispatch requestSliceAudio(int id, const SliceAudioRequest& request) override
    {
        audio.append(request);
        if (observe) { observe(id); }
        return receiveResult;
    }
    ReceiveDispatch requestSliceSquelch(int id, const SliceSquelchRequest& request) override
    {
        squelch.append(request);
        if (observe) { observe(id); }
        return receiveResult;
    }
    ReceiveDispatch requestSliceRxAntenna(int id, const QString& antenna) override
    {
        antennas.append(antenna);
        if (observe) { observe(id); }
        return receiveResult;
    }
    ReceiveDispatch requestSliceLock(int id, bool locked) override
    {
        locks.append(locked);
        if (observe) { observe(id); }
        return receiveResult;
    }
    int receiveCalls() const
    { return dsps.size() + audio.size() + squelch.size() + antennas.size() + locks.size(); }
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    {
        ++keyCalls;
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    int calls() const { return tunes.size() + filters.size() + agcs.size() + modes.size(); }
};

SliceDelta report(double frequency = 14.2)
{
    SliceDelta delta;
    delta.panId = QStringLiteral("receiver");
    delta.frequency = frequency;
    delta.mode = QStringLiteral("USB");
    delta.filterLow = 100;
    delta.filterHigh = 2800;
    delta.agcMode = QStringLiteral("med");
    delta.agcThreshold = 65;
    delta.agcOffLevel = 10;
    delta.inUse = true;
    return delta;
}

struct Fixture {
    RadioModel radio;
    Backend* backend;
    explicit Fixture(bool commandPlaneCreation = false, bool confirmed = false)
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        backend->confirmed = confirmed;
        radio.setBackendForTest(std::move(owned), QStringLiteral("receive-intent-test"));
        if (commandPlaneCreation) {
            check(radio.automationApplySliceFixture(0, QStringLiteral("A")),
                  "command-plane construction path creates a disconnected fixture");
        }
        backend->connected = true;
        emit backend->sliceChanged(0, report());
        check(radio.slice(0) != nullptr, "production status materializes slice zero");
    }
    SliceModel* slice() { return radio.slice(0); }
};

void confirmedReceiveAntennaAuthority()
{
    {
        Fixture f(false, true); SliceModel* slice = f.slice();
        const QString initialDisplay = slice->rxAntenna();
        slice->setRxAntenna(initialDisplay);
        check(f.backend->antennas == QStringList{initialDisplay} && slice->rxAntenna() == initialDisplay,
                "unknown default-matching antenna request is intent, not invented confirmed readback or a dropped request");
        SliceDelta readback; readback.rxAntenna = initialDisplay; emit f.backend->sliceChanged(0, readback);
        const int confirmed = f.backend->antennas.size(); slice->setRxAntenna(initialDisplay);
        check(f.backend->antennas.size() == confirmed, "same-value request is idempotent only after an actual antenna report");
        SliceDelta source; source.receiveTuneContextChanged = true; emit f.backend->sliceChanged(0, source);
        slice->setRxAntenna(initialDisplay);
        check(f.backend->antennas.size() == confirmed + 1 && slice->rxAntenna() == initialDisplay,
                "new physical source retires antenna knowledge without fabricating another live value");
        emit f.backend->sliceChanged(0, readback); slice->invalidateFrequencyObservation();
        emit f.backend->sliceChanged(0, report());
        slice->setRxAntenna(initialDisplay);
        check(f.backend->antennas.size() == confirmed + 2, "whole observation retirement clears same-default antenna knowledge");
    }
    {
        Fixture f(false, true); SliceModel* slice = f.slice();
        SliceDelta initial; initial.rxAntenna = QStringLiteral("ANT1"); emit f.backend->sliceChanged(0, initial);
        QSignalSpy notifications(slice, &SliceModel::rxAntennaChanged);
        slice->setRxAntenna(QStringLiteral("ANT2"));
        check(slice->rxAntenna() == QStringLiteral("ANT1") && f.backend->antennas == QStringList{"ANT2"}
            && notifications.size() == 1 && notifications[0][0].toString() == QStringLiteral("ANT1"),
            "confirmed antenna setter restores observed UI value and emits intent without optimistic live state");
        f.backend->receiveResult = ReceiveDispatch::Unsupported;
        slice->setRxAntenna(QStringLiteral("RX-ANT"));
        check(slice->rxAntenna() == QStringLiteral("ANT1"), "backend refusal cannot change confirmed antenna property");
        const int before = f.backend->antennas.size();
        SliceDelta observed; observed.rxAntenna = QStringLiteral("ANT2"); emit f.backend->sliceChanged(0, observed);
        check(slice->rxAntenna() == QStringLiteral("ANT2") && f.backend->antennas.size() == before,
            "actual antenna report alone adopts live value without echoing another request");
    }
    {
        Fixture f(false, true); SliceModel* slice = f.slice();
        for (const QString& invalid : {QString{}, QString(129, QLatin1Char('X'))}) {
            SliceDelta actual; actual.rxAntenna = QStringLiteral("ANT1"); emit f.backend->sliceChanged(0, actual);
            SliceDelta malformed; malformed.rxAntenna = invalid; emit f.backend->sliceChanged(0, malformed);
            const int before = f.backend->antennas.size(); slice->setRxAntenna(QStringLiteral("ANT1"));
            check(slice->rxAntenna() == QStringLiteral("ANT1") && f.backend->antennas.size() == before + 1,
                    "invalid antenna observation cannot fabricate known state or replace last display");
        }
    }
    for (bool cancelToObserved : {false, true}) {
        Fixture f(false, true); SliceModel* slice = f.slice();
        SliceDelta initial; initial.rxAntenna = QStringLiteral("ANT1"); emit f.backend->sliceChanged(0, initial);
        bool nested = false;
        QObject::connect(slice, &SliceModel::rxAntennaChanged, &f.radio, [&](const QString&) {
            if (!nested) { nested = true; slice->setRxAntenna(cancelToObserved ? QStringLiteral("ANT1") : QStringLiteral("RX-ANT")); }
        });
        slice->setRxAntenna(QStringLiteral("ANT2"));
        check(nested && slice->rxAntenna() == QStringLiteral("ANT1")
            && f.backend->antennas == (cancelToObserved ? QStringList{} : QStringList{QStringLiteral("RX-ANT")}),
            "reentrant newer antenna intent including observed-value cancellation retires outer producer");
    }
    for (int context = 0; context < 4; ++context) {
        Fixture f(false, true); SliceModel* slice = f.slice();
        SliceDelta initial; initial.rxAntenna = QStringLiteral("ANT1"); emit f.backend->sliceChanged(0, initial);
        bool retired = false;
        QObject::connect(slice, &SliceModel::rxAntennaChanged, &f.radio, [&](const QString&) {
            if (!retired) {
                retired = true; SliceDelta change;
                if (context == 0) { change.receiveTuneContextChanged = true; }
                if (context == 1) { change.mode = QStringLiteral("AM"); }
                if (context == 2) { change.panId = QStringLiteral("other-source"); }
                if (context == 3) { change.rxAntenna = QStringLiteral("RX-ANT"); }
                emit f.backend->sliceChanged(0, change);
            }
        });
        slice->setRxAntenna(QStringLiteral("ANT2"));
        check(retired && f.backend->antennas.isEmpty(), "reentrant source/mode/pan or newer antenna observation prevents stale antenna dispatch");
    }
    {
        auto f = std::make_unique<Fixture>(false, true);
        SliceModel* slice = f->slice(); SliceDelta initial; initial.rxAntenna = QStringLiteral("ANT1"); emit f->backend->sliceChanged(0, initial);
        QPointer<SliceModel> alive(slice); QObject context; int intents = 0;
        QObject::connect(slice, &SliceModel::receiveRxAntennaRequested, &context, [&](const QString&) { ++intents; });
        QObject::connect(slice, &SliceModel::rxAntennaChanged, &context, [&](const QString&) { f.reset(); });
        slice->setRxAntenna(QStringLiteral("ANT2"));
        check(!alive && intents == 0, "deletion during confirmed old-value antenna notification prevents stale intent");
    }
    {
        Fixture legacy; SliceModel* slice = legacy.slice();
        slice->setRxAntenna(QStringLiteral("ANT2"));
        check(slice->rxAntenna() == QStringLiteral("ANT2") && legacy.backend->antennas == QStringList{"ANT2"},
            "legacy optimistic antenna behavior remains unchanged");
    }
}

void confirmedIncrementalTuneCursor()
{
    Fixture f(false, true);
    SliceModel* s = f.slice();
    const double initial = s->frequency();
    QSignalSpy changed(s, &SliceModel::frequencyChanged);
    const double first = s->incrementalTuneTarget(0.0001);
    s->setFrequency(first);
    const double second = s->incrementalTuneTarget(0.0001);
    s->setFrequency(second);
    check(f.backend->tunes.size() == 2
              && std::abs(f.backend->tunes.last().frequencyHz - (initial * 1e6 + 200)) < 0.01,
          "production dispatch sends both accumulated wheel requests");
    check(std::abs(second - initial - 0.0002) < 1e-9,
          "two delayed-readback ticks accumulate two frequency steps");
    check(s->frequency() == initial && changed.isEmpty(),
          "pending tune cursor never publishes optimistic live frequency");
    emit f.backend->sliceChanged(0, report(initial));
    check(std::abs(s->incrementalTuneTarget(0.0) - second) < 1e-9,
          "unchanged polling report preserves outstanding tune cursor");
    emit f.backend->sliceChanged(0, report(first));
    check(std::abs(s->incrementalTuneTarget(0.0001) - initial - 0.0003) < 1e-9,
          "intermediate readback preserves the newer tune target");
    emit f.backend->sliceChanged(0, report(second));
    check(std::abs(s->incrementalTuneTarget(0.0) - second) < 1e-9,
          "latest acknowledgment reconciles pending intent with radio truth");
    const quint64 olderToken = s->recordTuneIntent(second + 0.0001);
    s->recordTuneIntent(second + 0.0002);
    s->retireTuneIntent(olderToken);
    check(std::abs(s->incrementalTuneTarget(0.0) - second - 0.0002) < 1e-9,
          "an older rejected dispatch cannot retire a newer nested intent");
    s->recordTuneIntent(second + 0.0001);
    emit f.backend->sliceChanged(0, report(7.1));
    check(std::abs(s->incrementalTuneTarget(0.0001) - 7.1001) < 1e-9,
          "unsolicited radio tuning retires the outstanding cursor");
    s->recordTuneIntent(7.2);
    SliceDelta roleChanged;
    roleChanged.receiveTuneContextChanged = true;
    roleChanged.frequency = 7.1;
    emit f.backend->sliceChanged(0, roleChanged);
    check(std::abs(s->incrementalTuneTarget(0.0001) - 7.1001) < 1e-9,
          "same-frequency receiver-role switch retires the previous tune cursor");
    s->recordTuneIntent(7.2);
    SliceDelta moved;
    moved.panId = QStringLiteral("another-receiver");
    emit f.backend->sliceChanged(0, moved);
    check(std::abs(s->incrementalTuneTarget(0.0001) - 7.1001) < 1e-9,
          "pan reassignment retires the previous tune cursor");
    s->recordTuneIntent(7.2);
    f.backend->tuneAccepted = false;
    s->setFrequency(7.3);
    check(std::abs(s->incrementalTuneTarget(0.0) - 7.1) < 1e-9,
          "dispatch rejection retires pending tune intent");
    s->recordTuneIntent(7.2);
    s->invalidateFrequencyObservation();
    check(std::abs(s->incrementalTuneTarget(0.0) - s->frequency()) < 1e-9,
          "disconnect invalidation retires pending tune intent");
    s->recordTuneIntent(7.2);
    QThread::msleep(1010);
    check(std::abs(s->incrementalTuneTarget(0.0) - s->frequency()) < 1e-9,
          "expired pending intent cannot seed a later operator gesture");
}

void confirmedRecenterPreservesBackendViewport()
{
    Fixture f(false, true);
    SliceModel* s = f.slice();
    const double observed = s->frequency();
    QSignalSpy changed(s, &SliceModel::frequencyChanged);
    s->tuneAndRecenter(observed + 0.01);
    check(f.backend->tunes.size() == 1
              && f.backend->tunes.last().panIntent == SliceTuneRequest::PanIntent::PreservePan,
          "confirmed generic recenter retains the prior backend-preserve viewport semantics");
    check(s->frequency() == observed && changed.isEmpty(),
          "confirmed recenter leaves live frequency radio-authoritative");
}

void confirmedFilterContextRetirement()
{
    for (const bool panChanged : {false, true}) {
        Fixture f(false, true);
        SliceModel* slice = f.slice();
        QSignalSpy observations(slice, &SliceModel::receiveObservationChanged);
        const quint64 epoch = slice->receiveSessionGeneration();
        SliceDelta changed;
        if (panChanged) { changed.panId = QStringLiteral("new-receiver"); }
        else { changed.receiveTuneContextChanged = true; }
        emit f.backend->sliceChanged(0, changed);
        check(!slice->receiveObservation().filterLowHz && !slice->receiveObservation().filterHighHz
                  && observations.size() == 1 && slice->receiveSessionGeneration() > epoch,
              "confirmed source change retires both accepted filter edges before publication");
        SliceDelta low;
        low.filterLow = 200;
        emit f.backend->sliceChanged(0, low);
        check(slice->receiveObservation().filterLowHz == 200 && !slice->receiveObservation().filterHighHz,
              "new-source low edge cannot qualify an old-source high edge");
        SliceDelta high;
        high.filterHigh = 2600;
        emit f.backend->sliceChanged(0, high);
        check(slice->receiveObservation().filterLowHz == 200 && slice->receiveObservation().filterHighHz == 2600,
              "fresh complete new-source edges restore the accepted filter observation");
    }
    for (const bool modeChanged : {false, true}) {
        Fixture f(false, true);
        SliceModel* slice = f.slice();
        QSignalSpy requested(slice, &SliceModel::receiveFilterRequested);
        QObject::connect(slice, &SliceModel::filterCommandIssued, slice, [&](int, int) {
            SliceDelta changed;
            if (modeChanged) { changed.mode = QStringLiteral("CW"); }
            else { changed.receiveTuneContextChanged = true; }
            emit f.backend->sliceChanged(0, changed);
        });
        slice->setFilterWidth(200, 2600);
        check(requested.isEmpty() && f.backend->filters.isEmpty(),
              "reentrant source or mode change retires the outer confirmed filter intent before dispatch");
    }
    Fixture optimistic;
    SliceDelta optimisticSource;
    optimisticSource.receiveTuneContextChanged = true;
    emit optimistic.backend->sliceChanged(0, optimisticSource);
    check(optimistic.slice()->receiveObservation().filterLowHz == 100
              && optimistic.slice()->receiveObservation().filterHighHz == 2800,
          "optimistic legacy source handling retains its established filter policy");
}

void filterOnlyContextOwnership()
{
    Fixture f(false, true);
    SliceModel* slice = f.slice();
    const double observed = slice->frequency();
    slice->setFrequency(slice->incrementalTuneTarget(0.0001));
    slice->setFrequency(slice->incrementalTuneTarget(0.0001));
    const double pending = slice->incrementalTuneTarget(0.0);
    const quint64 receiverEpoch = slice->receiveSessionGeneration();
    const quint64 filterEpoch = slice->receiveFilterContextGeneration();
    QSignalSpy frequencyChanged(slice, &SliceModel::frequencyChanged);
    QSignalSpy observations(slice, &SliceModel::receiveObservationChanged);
    SliceDelta boundary;
    boundary.receiveFilterContextChanged = true;
    emit f.backend->sliceChanged(0, boundary);
    check(slice->receiveSessionGeneration() == receiverEpoch
              && slice->receiveFilterContextGeneration() > filterEpoch,
          "filter-only boundary advances filter ownership without changing the receiver epoch");
    check(!slice->receiveObservation().filterLowHz && !slice->receiveObservation().filterHighHz
              && observations.size() == 1 && slice->receiveObservation().mode == QStringLiteral("USB"),
          "filter-only boundary removes accepted edges while preserving observed mode");
    check(std::abs(slice->incrementalTuneTarget(0.0) - pending) < 1e-9
              && slice->frequency() == observed && frequencyChanged.isEmpty(),
          "filter-bank boundary preserves the accumulated delayed-readback tune cursor without optimistic frequency");
    slice->setFrequency(slice->incrementalTuneTarget(0.0001));
    check(f.backend->tunes.size() == 3
              && std::abs(f.backend->tunes.last().frequencyHz - (observed * 1e6 + 300)) < 0.01,
          "next production wheel intent continues the existing burst across a filter-only boundary");
    const quint64 currentFilterEpoch = slice->receiveFilterContextGeneration();
    slice->setFilterWidth(200, 2600);
    slice->setFilterWidth(300, 2500);
    check(f.backend->filters.size() == 2 && slice->receiveFilterContextGeneration() == currentFilterEpoch,
          "ordinary operator filter A then B both dispatch without advancing filter context generation");
    SliceDelta low; low.filterLow = 300;
    SliceDelta high; high.filterHigh = 2500;
    emit f.backend->sliceChanged(0, low);
    check(slice->receiveObservation().filterLowHz == 300 && !slice->receiveObservation().filterHighHz,
          "new filter low readback cannot recover an old-context high edge");
    emit f.backend->sliceChanged(0, high);
    check(slice->receiveFilterContextGeneration() == currentFilterEpoch
              && slice->receiveObservation().filterHighHz == 2500,
          "ordinary partial filter readbacks restore authority without creating another context boundary");
    const quint64 beforeModeReceiver = slice->receiveSessionGeneration();
    SliceDelta mode; mode.mode = QStringLiteral("CW");
    emit f.backend->sliceChanged(0, mode);
    check(slice->receiveSessionGeneration() > beforeModeReceiver
              && slice->receiveFilterContextGeneration() > currentFilterEpoch,
          "observed mode transition advances receiver and filter contexts together");
    const quint64 beforeSourceReceiver = slice->receiveSessionGeneration();
    const quint64 beforeSourceFilter = slice->receiveFilterContextGeneration();
    SliceDelta source; source.receiveTuneContextChanged = true;
    emit f.backend->sliceChanged(0, source);
    check(slice->receiveSessionGeneration() > beforeSourceReceiver
              && slice->receiveFilterContextGeneration() > beforeSourceFilter
              && std::abs(slice->incrementalTuneTarget(0.0) - observed) < 1e-9,
          "physical-source transition still retires tune cursor and both queue contexts");
    const quint64 beforeReset = slice->receiveFilterContextGeneration();
    slice->invalidateFrequencyObservation();
    check(slice->receiveFilterContextGeneration() > beforeReset,
          "whole-observation reset retires the filter context");
    const quint64 beforePolicy = slice->receiveFilterContextGeneration();
    f.backend->confirmed = false;
    RadioModelSliceLifecycleTestAccess::refreshPolicy(f.radio, slice);
    check(slice->receiveFilterContextGeneration() > beforePolicy,
          "actual receive-policy replacement fences old filter ownership");
    const quint64 samePolicy = slice->receiveFilterContextGeneration();
    RadioModelSliceLifecycleTestAccess::refreshPolicy(f.radio, slice);
    check(slice->receiveFilterContextGeneration() == samePolicy,
          "reapplying the same control policy does not manufacture a filter boundary");

    Fixture reentrant(false, true);
    SliceModel* target = reentrant.slice();
    QSignalSpy requested(target, &SliceModel::receiveFilterRequested);
    QObject::connect(target, &SliceModel::filterCommandIssued, target, [&](int, int) {
        SliceDelta changed; changed.receiveFilterContextChanged = true;
        emit reentrant.backend->sliceChanged(0, changed);
    });
    target->setFilterWidth(200, 2600);
    check(requested.isEmpty() && reentrant.backend->filters.isEmpty(),
          "filter-only boundary during notification retires the outer filter intent before dispatch");

    Fixture optimistic;
    const quint64 optimisticEpoch = optimistic.slice()->receiveFilterContextGeneration();
    emit optimistic.backend->sliceChanged(0, boundary);
    check(optimistic.slice()->receiveFilterContextGeneration() == optimisticEpoch
              && optimistic.slice()->receiveObservation().filterLowHz == 100
              && optimistic.slice()->receiveObservation().filterHighHz == 2800,
          "filter-only metadata preserves established optimistic receive behavior");
}

void receivePublicationLifetime()
{
    for (int event = 0; event < 7; ++event) {
        auto slice = std::make_unique<SliceModel>(0);
        slice->applyChanges(report());
        QPointer<SliceModel> alive(slice.get());
        QObject context;
        bool notified = false;
        const auto retire = [&] { notified = true; slice.reset(); };
        SliceDelta changed;
        if (event == 0) {
            QObject::connect(slice.get(), &SliceModel::filterChanged, &context, retire);
            changed.filterHigh = 2900;
        } else if (event == 1) {
            QObject::connect(slice.get(), &SliceModel::receiveObservationChanged, &context, retire);
            changed.filterHigh = 2900;
            changed.mode = QStringLiteral("USB");
        } else if (event == 2) {
            QObject::connect(slice.get(), &SliceModel::frequencyReported, &context, retire);
            changed.frequency = 14.2;
        } else if (event == 3) {
            QObject::connect(slice.get(), &SliceModel::frequencyChanged, &context, retire);
            changed.frequency = 14.3;
        } else if (event == 4) {
            QObject::connect(slice.get(), &SliceModel::frequencyStatusReported, &context, retire);
            changed.frequency = 14.3;
            changed.mode = QStringLiteral("CW");
        } else if (event == 5) {
            QObject::connect(slice.get(), &SliceModel::rxAntennaChanged, &context, retire);
            changed.rxAntenna = QStringLiteral("RX-ANT");
            changed.locked = true;
        } else {
            QObject::connect(slice.get(), &SliceModel::txAntennaChanged, &context, retire);
            changed.txAntenna = QStringLiteral("ANT2");
            changed.locked = true;
        }
        slice->applyChanges(changed);
        check(notified && !alive && !slice,
              "receive publication may retire its standalone slice without continuing on freed state");
    }
}

void receiveResetLifetime()
{
    for (const bool deleteAtAgc : {true, false}) {
        std::unique_ptr<SliceModel> slice = std::make_unique<SliceModel>(0);
        slice->applyChanges(report());
        const QPointer<SliceModel> alive(slice.get());
        QObject context;
        bool retired = false;
        int laterNotifications = 0;
        const auto retire = [&] { retired = true; slice.reset(); };
        if (deleteAtAgc) {
            QObject::connect(slice.get(), &SliceModel::agcThresholdModeChanged, &context, retire);
            QObject::connect(slice.get(), &SliceModel::receiveObservationChanged, &context,
                [&] { ++laterNotifications; });
        } else {
            QObject::connect(slice.get(), &SliceModel::receiveObservationChanged, &context, retire);
        }
        QObject::connect(slice.get(), &SliceModel::frequencyReported, &context,
            [&] { ++laterNotifications; });
        slice->invalidateFrequencyObservation();
        check(retired && !alive && !slice && laterNotifications == 0,
              "public receive reset returns after synchronous standalone slice deletion without later publication");
    }
}

void confirmedReceiveControls()
{
    Fixture f(false, true);
    SliceModel* s = f.slice();
    QSignalSpy changed(s, &SliceModel::agcModeChanged);
    QSignalSpy nbChanged(s, &SliceModel::nbChanged);
    QSignalSpy raw(s, &SliceModel::commandReady);
    s->setAgcMode(QStringLiteral("fast"));
    s->setAgcThreshold(37);
    s->setAgcOffLevel(22);
    check(s->agcMode() == QStringLiteral("med") && s->agcThreshold() == 65
              && s->agcOffLevel() == 10 && changed.isEmpty(),
          "confirmed AGC edits do not publish requested state before adoption");
    check(f.backend->agcs.size() == 3
              && f.backend->agcs[0].mode == QStringLiteral("fast")
              && f.backend->agcs[1].threshold == 37 && f.backend->agcs[2].offLevel == 22,
          "independent AGC field intents reach backend without stale paired overwrite");
    s->setNbKind(NoiseBlankerKind::Advanced);
    s->setNbLevel(73);
    s->setApf(true);
    s->setApfLevel(61);
    s->setRit(true, 130);
    check(!s->nbOn() && !s->apfOn() && !s->ritOn() && s->ritFreq() == 0
              && nbChanged.isEmpty() && raw.isEmpty(),
          "confirmed DSP and RIT edits remain intent and emit no raw wire command");
    check(f.backend->dsps.size() == 4
              && f.backend->dsps[0].requestedBlanker() == NoiseBlankerKind::Advanced
              && f.backend->dsps[1].level == 73 && f.backend->dsps[2].enabled
              && f.backend->dsps[3].level == 61,
          "confirmed DSP requests retain selected feature and field");
    SliceDelta accepted;
    accepted.agcMode = QStringLiteral("fast"); accepted.agcThreshold = 37;
    accepted.agcOffLevel = 22; accepted.apf = true; accepted.apfLevel = 61;
    accepted.ritOn = true; accepted.ritFreq = 130;
    accepted.nbKind = NoiseBlankerKind::Advanced; accepted.nbLevel = 73;
    emit f.backend->sliceChanged(0, accepted);
    check(s->agcMode() == QStringLiteral("fast") && s->agcThreshold() == 37
              && s->agcOffLevel() == 22 && s->apfOn() && s->apfLevel() == 61
              && s->ritOn() && s->ritFreq() == 130 && changed.size() == 1
              && s->nbKind() == NoiseBlankerKind::Advanced && s->nbLevel() == 73,
          "only accepted readback advances AGC DSP and RIT state");
    // Correct back to the currently accepted value while an edit is pending.
    // Equal-to-observed is still a new intent; it must supersede the pending one.
    s->setAgcThreshold(70); s->setAgcThreshold(37);
    check(f.backend->agcs.last().threshold == 37,
          "confirmed same-value correction is dispatched to cancel pending AGC edit");

    const int priorDspRequests = f.backend->dsps.size();
    s->setNr(true); s->setNrLevel(72);
    s->setAnf(true); s->setAnfLevel(63);
    s->setMn(true); s->setMnLevel(28);
    check(!s->nrOn() && !s->anfOn() && !s->mnOn() && s->mnLevel() == 50,
          "NR ANF and manual notch remain accepted state while edits are pending");
    check(f.backend->dsps.size() == priorDspRequests + 6
              && f.backend->dsps[priorDspRequests].feature == SliceDspRequest::Feature::Nr
              && f.backend->dsps[priorDspRequests + 2].feature == SliceDspRequest::Feature::Anf
              && f.backend->dsps.last().feature == SliceDspRequest::Feature::Mn
              && f.backend->dsps.last().field == SliceDspRequest::Field::Level
              && f.backend->dsps.last().level == 28 && raw.isEmpty(),
          "NR ANF and manual notch dispatch selected fields without raw commands");
    SliceDelta acceptedFilters;
    acceptedFilters.nr = true; acceptedFilters.nrLevel = 72;
    acceptedFilters.anf = true; acceptedFilters.anfLevel = 63;
    acceptedFilters.mn = true; acceptedFilters.mnLevel = 28;
    emit f.backend->sliceChanged(0, acceptedFilters);
    check(s->nrOn() && s->nrLevel() == 72 && s->anfOn() && s->anfLevel() == 63
              && s->mnOn() && s->mnLevel() == 28,
          "adopted NR ANF and manual notch readbacks publish their actual state");
    s->setMn(false); s->setMn(true);
    check(f.backend->dsps.last().enabled,
          "same-value manual notch correction supersedes pending disable");

    RadioModelSliceLifecycleTestAccess::publishCapabilities(f.radio);
    TransmitModel& tx = f.radio.transmitModel();
    QSignalSpy pitchChanged(&tx, &TransmitModel::cwPitchChanged);
    QSignalSpy pitchRaw(&tx, &TransmitModel::commandReady);
    tx.setCwPitch(720);
    check(tx.cwPitch() == 600 && pitchChanged.isEmpty() && pitchRaw.isEmpty()
              && f.backend->pitches == QList<int>{720} && f.backend->keyCalls == 0,
          "receive pitch sends one intent without optimistic readback wire command or TX");
    TransmitDelta pitchAccepted; pitchAccepted.cwPitch = 720;
    tx.applyChanges(pitchAccepted);
    check(tx.cwPitch() == 720 && pitchChanged.size() == 1 && f.backend->pitches.size() == 1,
          "accepted receive pitch updates presentation without feeding back into backend");
    f.backend->confirmed = false;
    RadioModelSliceLifecycleTestAccess::publishCapabilities(f.radio);
    QSignalSpy pitchControls(&tx, &TransmitModel::controlRequested);
    tx.setCwPitch(800);
    check(tx.cwPitch() == 800 && pitchRaw.isEmpty() && pitchControls.size() == 1
              && qvariant_cast<TransmitControlRequest>(pitchControls.first().first()).field
                    == TransmitControlRequest::Field::CwPitch
              && std::get<int>(qvariant_cast<TransmitControlRequest>(pitchControls.first().first()).value) == 800,
          "optimistic pitch uses the typed upstream control route once without raw wire text");
}

void ritLifecycle()
{
    Fixture f(false, true);
    SliceModel* original = f.slice();
    original->setRit(true, 120);
    check(f.backend->ritEnables == QList<bool>{true} && f.backend->ritOffsets == QList<int>{120},
          "connected current receiver dispatches both RIT fields");
    f.backend->observe = [&](int) { f.backend->connected = false; };
    original->setRit(true, 230);
    check(f.backend->ritEnables.size() == 2 && f.backend->ritOffsets.size() == 1,
          "disconnect during RIT enable suppresses companion offset");
    original->setRit(true, 340);
    check(f.backend->ritEnables.size() == 2, "disconnected RIT intent never reaches backend");
    f.backend->connected = true;
    f.backend->observe = [&](int id) {
        emit f.backend->sliceRemoved(id);
        emit f.backend->sliceChanged(id, report());
    };
    original->setRit(true, 450);
    check(f.slice() != original && f.backend->ritEnables.size() == 3
              && f.backend->ritOffsets.size() == 1,
          "receiver retirement during RIT enable cannot offset replacement");
    original->setRit(true, 560);
    check(f.backend->ritEnables.size() == 3, "retired receiver cannot send another RIT intent");
    f.backend->observe = {};
    f.slice()->setRit(true, 670);
    check(f.backend->ritEnables.size() == 4 && f.backend->ritOffsets.last() == 670,
          "replacement receiver retains functional RIT controls");
}

void notchReadbackLifecycle()
{
    Fixture f(false, true);
    RadioModelSliceLifecycleTestAccess::publishCapabilities(f.radio);
    NotchDelta initial; initial.centerHz = 14'200'000; initial.widthHz = 400;
    emit f.backend->notchChanged(11, initial);
    check(f.radio.tnfModel().tnf(11) != nullptr, "backend notch observation reaches shared model");
    RadioDelta global; global.tnfEnabled = false;
    emit f.backend->radioChanged(global);
    check(!f.radio.tnfModel().globalEnabled(), "backend global notch bypass reaches shared model");

    f.radio.tnfModel().setTnfFreq(11, 14.201);
    check(f.radio.tnfModel().tnf(11)->freqMhz == 14.2,
          "capability publication selects confirmed tracking-notch policy");
    std::thread delayed([&] { emit f.backend->notchChanged(12, initial); });
    delayed.join();
    auto next = std::make_unique<Backend>();
    f.backend = next.get();
    f.radio.setBackendForTest(std::move(next), QStringLiteral("replacement"));
    QCoreApplication::processEvents();
    check(f.radio.tnfModel().tnf(12) == nullptr,
          "queued notch observation from retired backend cannot populate new session");
}

void externalReceiveHandoffOrdering()
{
    Fixture f;
    SliceModel* s = f.slice();
    QSignalSpy raw(s, &SliceModel::commandReady);
    s->setExternalReceiveAudioReplacementMute(true, false);
    check(f.backend->audio.size() == 1 && f.backend->audio.last().value == 1
              && f.backend->audio.last().origin == SliceAudioRequest::Origin::ExternalReceiveSuppression,
          "Kiwi suppression reaches the neutral backend route");
    QStringList order;
    f.backend->observe = [&](int) { order.append(QStringLiteral("restore primary mute")); };
    QObject::connect(&f.radio, &RadioModel::panBandAboutToDispatch, s, [&](const QString&) {
        // Same synchronous handoff as MainWindow_KiwiSdr. The injected backend
        // has no command plane: this proves ordering up to the band attempt,
        // not delivery of a band command to radio firmware.
        s->prepareExternalReceiveAudioReplacementBandRecall(false);
        order.append(QStringLiteral("continue band attempt"));
    });
    // Band requests address a published model pan, not the backend's own ID.
    emit f.backend->panCenterBandwidthChanged(QStringLiteral("receiver"), 14.2, 0.2);
    check(f.radio.panadapters().size() == 1, "the backend's pan is published to the model");
    f.radio.requestPanBand(f.radio.panadapters().first()->panId(), QStringLiteral("20"));
    check(order == QStringList{"restore primary mute", "continue band attempt"}
              && f.backend->audio.size() == 2 && f.backend->audio.last().value == 0
              && f.backend->audio.last().origin == SliceAudioRequest::Origin::ExternalReceiveSuppression
              && raw.isEmpty(),
          "typed Kiwi mute handoff completes before band dispatch continues, without a raw bypass");
}

void creationPaths()
{
    for (bool commandPlane : {false, true}) {
        Fixture f(commandPlane);
        SliceModel* s = f.slice();
        RadioModelSliceLifecycleTestAccess::wire(f.radio, s);
        RadioModelSliceLifecycleTestAccess::wire(f.radio, s);
        QSignalSpy raw(s, &SliceModel::commandReady);
        QStringList order;
        QObject::connect(s, &SliceModel::frequencyChanged, s, [&](double) { order << "display"; });
        QObject::connect(s, &SliceModel::frequencyCommandIssued, s, [&](double) { order << "provenance"; });
        QObject::connect(s, &SliceModel::receiveTuneRequested, s, [&](const SliceTuneRequest&) { order << "dispatch"; });
        s->setFrequency(14.234567);
        s->tuneAndRecenter(7.1);
        check(f.backend->tunes.size() == 2 && f.backend->tunes.at(0).frequencyHz == 14'234'567
                  && f.backend->tunes.at(0).panIntent == SliceTuneRequest::PanIntent::PreservePan
                  && f.backend->tunes.at(1).frequencyHz == 7'100'000
                  && f.backend->tunes.at(1).panIntent == SliceTuneRequest::PanIntent::AllowRecenter,
              "both creation paths route each tune exactly once with explicit pan intent and Hz");
        check(order == QStringList{"display", "provenance", "dispatch", "display", "provenance", "dispatch"},
              "linked-slice display then provenance order precedes backend dispatch");
        const quint64 epoch = s->userFilterEpoch();
        s->setFilterWidth(150, 2700);
        s->applyAdaptiveFilter(200, 2500);
        s->setMode(QStringLiteral("LSB"));
        check(f.backend->modes == QStringList{"LSB"} && f.backend->filters.size() == 3
                  && f.backend->filters.at(0).origin == SliceFilterRequest::Origin::Operator
                  && f.backend->filters.at(1).origin == SliceFilterRequest::Origin::Adaptive
                  && f.backend->filters.at(2).origin == SliceFilterRequest::Origin::ModeNormalization
                  && f.backend->filters.at(2).lowHz == -2500 && f.backend->filters.at(2).highHz == -200
                  && s->userFilterEpoch() == epoch + 1,
              "manual, adaptive and normalized filters preserve origin, polarity and operator epoch");
        s->setAgcMode(QStringLiteral("fast"));
        s->setAgcThreshold(42);
        s->setAgcOffLevel(31);
        check(f.backend->agcs.size() == 3 && f.backend->pairedAgcCalls == 2
                  && f.backend->agcs.at(0).field == SliceAgcRequest::Field::Mode
                  && f.backend->agcs.at(0).threshold == 65
                  && f.backend->agcs.at(1).field == SliceAgcRequest::Field::Threshold
                  && f.backend->agcs.at(1).mode == QStringLiteral("fast")
                  && f.backend->agcs.at(1).threshold == 42
                  && f.backend->agcs.at(2).field == SliceAgcRequest::Field::OffLevel
                  && f.backend->agcs.at(2).offLevel == 31,
              "AGC keeps selected field and DSP pair; default off-level remains unsupported");
        const int calls = f.backend->calls();
        emit f.backend->sliceChanged(0, report(14.3));
        check(f.backend->calls() == calls && raw.isEmpty() && f.backend->keyCalls == 0,
              "status emits no receive request, migrated setters emit no raw text, and none keys TX");
        s->setLocked(true);
        s->setFrequency(14.4);
        s->tuneAndRecenter(14.5);
        check(f.backend->calls() == calls, "locked slice refuses both tune presentations");
    }
}

void receiveControls()
{
    for (bool commandPlane : {false, true}) {
        Fixture f(commandPlane);
        SliceModel* s = f.slice();
        RadioModelSliceLifecycleTestAccess::wire(f.radio, s);
        RadioModelSliceLifecycleTestAccess::wire(f.radio, s);
        QSignalSpy raw(s, &SliceModel::commandReady);
        // Every desktop feature, not merely one representative setter.
        const std::array toggles{&SliceModel::setNb, &SliceModel::setNr, &SliceModel::setAnf,
            &SliceModel::setMn, &SliceModel::setApf, &SliceModel::setNrl, &SliceModel::setNrs,
            &SliceModel::setRnn, &SliceModel::setNrf, &SliceModel::setAnfl, &SliceModel::setAnft};
        for (std::size_t i = 0; i < toggles.size(); ++i) {
            (s->*toggles[i])(true);
            check(f.backend->dsps.size() == int(i + 1)
                      && int(f.backend->dsps.last().feature) == int(i)
                      && f.backend->dsps.last().field == SliceDspRequest::Field::Enabled
                      && f.backend->dsps.last().enabled,
                  "every DSP enable routes exactly once from both creation paths");
        }
        const std::array levels{&SliceModel::setNbLevel, &SliceModel::setNrLevel,
            &SliceModel::setAnfLevel, &SliceModel::setMnLevel, &SliceModel::setApfLevel,
            &SliceModel::setNrlLevel, &SliceModel::setNrsLevel, &SliceModel::setNrfLevel,
            &SliceModel::setAnflLevel};
        for (const auto setter : levels) {
            const int before = f.backend->dsps.size();
            (s->*setter)(123);
            check(f.backend->dsps.size() == before + 1
                      && f.backend->dsps.last().field == SliceDspRequest::Field::Level
                      && f.backend->dsps.last().level == 100 && f.backend->dsps.last().enabled,
                  "DSP levels clamp and preserve the paired enabled value");
        }
        s->setAudioGain(39.5f);
        s->setAudioMute(true);
        s->setAudioPan(81);
        check(f.backend->audio.size() == 3 && f.backend->audio[0].value == 39
                  && f.backend->audio[1].field == SliceAudioRequest::Field::Mute
                  && f.backend->audio[2].field == SliceAudioRequest::Field::Pan,
              "audio dispatch does not require a daemon audio capability record");
        s->setAudioGain(std::numeric_limits<float>::quiet_NaN());
        check(f.backend->audio.size() == 3, "non-finite gain is refused before integer conversion");
        s->setSquelch(true, 37);
        s->setSquelch(true, 37);
        check(f.backend->squelch.size() == 2 && f.backend->squelch[0].enabledChanged
                  && f.backend->squelch[0].levelChanged && !f.backend->squelch[1].enabledChanged
                  && !f.backend->squelch[1].levelChanged,
              "squelch retains field mask and repeated paired intent without duplicate bindings");
        s->setRxAntenna(QStringLiteral("ANT2"));
        s->setLocked(true);
        s->setLocked(false);
        check(f.backend->antennas == QStringList{"ANT2"} && f.backend->locks == QList<bool>{true, false},
              "antenna and slice lock are not gated on radio-wide dial-lock support");
        const int before = f.backend->receiveCalls();
        SliceDelta delta;
        delta.nb = false; delta.nbLevel = 32; delta.nr = false; delta.anf = false;
        delta.audioGain = 31; delta.audioMute = false; delta.audioPan = 22;
        delta.squelchOn = false; delta.squelchLevel = 19;
        delta.rxAntenna = QStringLiteral("ANT1"); delta.locked = true;
        emit f.backend->sliceChanged(0, delta);
        check(f.backend->receiveCalls() == before && s->audioGain() == 31 && !s->nbOn()
                  && s->rxAntenna() == QStringLiteral("ANT1") && s->isLocked(),
              "independent receive observations correct state without echoing commands");
        QSignalSpy refused(&f.radio, &RadioModel::commandDropped);
        f.backend->receiveResult = ReceiveDispatch::Unsupported;
        s->setNr(true);
        check(refused.size() == 1, "unsupported receive request reaches the existing refusal notification");
        f.backend->receiveResult = ReceiveDispatch::LocalOnly;
        s->setLocked(false);
        check(refused.size() == 1 && !s->isLocked(), "local-only lock remains useful without a false refusal");
        f.backend->receiveResult = ReceiveDispatch::Unsupported;
        const int audioBefore = f.backend->audio.size();
        s->setExternalReceiveAudioReplacementMute(true, false);
        check(f.backend->audio.size() == audioBefore + 1
                  && f.backend->audio.last().origin == SliceAudioRequest::Origin::ExternalReceiveSuppression
                  && refused.size() == 1,
              "a refused compatibility-origin mute is logged, not raised as the operator's unsupported-control notice");
        s->setExternalReceiveAudioReplacementMute(false, false);
        check(raw.isEmpty() && f.backend->keyCalls == 0, "whole migrated group sends no raw wire text or TX request");
    }
}

void receiveReentrancyAndLifecycle()
{
    Fixture f;
    SliceModel* s = f.slice();
    float displayed = 0;
    QObject::connect(s, &SliceModel::audioGainChanged, s, [&](float value) { displayed = value; });
    f.backend->observe = [&](int id) {
        SliceDelta delta; delta.audioGain = 21; delta.nb = false;
        emit f.backend->sliceChanged(id, delta);
    };
    s->setAudioGain(67);
    s->setNb(true);
    check(s->audioGain() == 21 && displayed == 21 && !s->nbOn()
              && f.backend->audio.size() == 1 && f.backend->dsps.size() == 1,
          "synchronous receive corrections win over optimistic notifications and do not echo");
    f.backend->observe = {};
    auto connection = QObject::connect(s, &SliceModel::nbChanged, s, [s](bool on) {
        if (on) { s->setNbLevel(73); }
    });
    s->setNb(true);
    check(f.backend->dsps.size() == 3 && f.backend->dsps.last().enabled
              && f.backend->dsps.last().level == 73,
          "nested companion DSP edit preserves both fields with the current pair");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::squelchChanged, s, [s](bool on, int level) {
        if (on && level == 38) { s->setSquelch(true, 61); }
    });
    s->setSquelch(true, 38);
    check(f.backend->squelch.size() == 1 && f.backend->squelch.last().enabledChanged
              && f.backend->squelch.last().levelChanged && f.backend->squelch.last().level == 61,
          "nested squelch edit retains the outer enable field in the newest paired request");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::nbChanged, s, [s](bool on) {
        if (!on) { s->setNb(true); }
    });
    s->setNb(false);
    check(f.backend->dsps.size() == 4 && f.backend->dsps.last().enabled,
          "nested same-field DSP edit supersedes the outer request");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::audioGainChanged, s, [&](float value) {
        if (value == 51) {
            f.radio.stageSessionModelsForReconnectForTest();
            emit f.backend->sliceChanged(0, report());
        }
    });
    s->setAudioGain(51);
    check(f.backend->audio.size() == 1, "reconnect during receive notification cancels prior-session audio intent");
    QObject::disconnect(connection);

    auto all = [](SliceModel* source) {
        emit source->receiveDspRequested({SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true, 50});
        emit source->receiveAudioRequested({SliceAudioRequest::Field::Mute, 1});
        emit source->receiveSquelchRequested({true, 42, true, true});
        emit source->receiveRxAntennaRequested(QStringLiteral("ANT1"));
        emit source->receiveLockRequested(true);
    };
    const int before = f.backend->receiveCalls();
    f.backend->connected = false;
    all(s);
    f.backend->connected = true;
    f.radio.stageSessionModelsForReconnectForTest();
    all(s);
    emit f.backend->sliceChanged(0, report());
    std::thread worker([&] { all(s); });
    worker.join();
    QCoreApplication::sendPostedEvents(&f.radio, QEvent::MetaCall);
    check(f.backend->receiveCalls() == before, "all receive groups refuse disconnected, staged and off-thread intents");
    all(s);
    check(f.backend->receiveCalls() == before + 5, "reclaimed slice keeps exactly one receive binding per group");
    emit f.backend->sliceRemoved(0);
    emit f.backend->sliceChanged(0, report());
    all(s);
    check(f.backend->receiveCalls() == before + 5, "retired object cannot control replacement with the same slice id");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    auto* doomed = new SliceModel(9);
    int requests = 0;
    QObject::connect(doomed, &SliceModel::receiveAudioRequested, &f.radio, [&](const SliceAudioRequest&) { ++requests; });
    QObject::connect(doomed, &SliceModel::audioGainChanged, &f.radio, [doomed](float) { delete doomed; });
    doomed->setAudioGain(43);
    check(requests == 0, "deletion during receive notification cancels dispatch safely");
}

// Radio status is the truth for the NRS level, as for every DSP level: a
// reported 50 after an operator 72 is adopted and asks for nothing (#6447).
void nrsReadbackIsPassive()
{
    SliceModel slice(0);
    QSignalSpy requests(&slice, &SliceModel::receiveDspRequested);
    slice.setNrsLevel(72);
    check(requests.size() == 1, "an operator NRS level is one request");
    SliceDelta reported; reported.nrsLevel = 50;
    slice.applyChanges(reported);
    check(requests.size() == 1 && slice.nrsLevel() == 50, "a reported NRS level of 50 is adopted, not overridden");
}

void synchronousObservations()
{
    Fixture f;
    SliceModel* s = f.slice();
    double displayedFrequency = 0;
    QString displayedMode;
    int displayedLow = 0;
    QObject::connect(s, &SliceModel::frequencyChanged, s, [&](double mhz) { displayedFrequency = mhz; });
    QObject::connect(s, &SliceModel::modeChanged, s, [&](const QString& mode) { displayedMode = mode; });
    QObject::connect(s, &SliceModel::filterChanged, s, [&](int low, int) { displayedLow = low; });
    f.backend->observe = [&](int id) { emit f.backend->sliceChanged(id, report(14.26)); };
    s->setFrequency(14.25);
    check(s->frequency() == 14.26 && s->reportedFrequency() == 14.26 && displayedFrequency == 14.26,
          "synchronous backend frequency correction is the final model and displayed value");
    s->setMode(QStringLiteral("LSB"));
    check(s->mode() == QStringLiteral("USB") && displayedMode == QStringLiteral("USB")
              && f.backend->filters.isEmpty(),
          "synchronous mode correction does not publish a stale mode or normalize the wrong passband");
    s->setFilterWidth(250, 2600);
    check(s->filterLow() == 100 && displayedLow == 100 && f.backend->filters.size() == 1,
          "synchronous filter observation wins without an echo request");
    s->setAgcMode(QStringLiteral("fast"));
    check(s->agcMode() == QStringLiteral("med") && f.backend->agcs.size() == 1,
          "synchronous AGC observation wins without an echo request");
}

void modeReentrancy()
{
    Fixture f;
    SliceModel* s = f.slice();
    QSignalSpy modes(s, &SliceModel::modeChanged);
    auto connection = QObject::connect(s, &SliceModel::modeChangeRequested, s,
        [s](const QString& mode) {
            if (mode == QStringLiteral("LSB")) {
                s->setFilterWidth(-2700, -150);
            }
        });
    s->setMode(QStringLiteral("LSB"));
    check(modes.size() == 1 && modes.last().at(0).toString() == QStringLiteral("LSB")
              && f.backend->filters.size() == 1 && s->filterLow() == -2700,
          "a synchronous filter edit cannot suppress the independent mode notification");
    QObject::disconnect(connection);

    s->setMode(QStringLiteral("USB"));
    modes.clear();
    connection = QObject::connect(s, &SliceModel::modeChangeRequested, s,
        [s](const QString& mode) {
            if (mode == QStringLiteral("LSB")) {
                s->setMode(QStringLiteral("DIGU"));
            }
        });
    s->setMode(QStringLiteral("LSB"));
    check(modes.size() == 1 && modes.last().at(0).toString() == QStringLiteral("DIGU")
              && s->mode() == QStringLiteral("DIGU"),
          "a reentrant mode edit supersedes the outer notification and normalization");
    QObject::disconnect(connection);
}

void lifetimeAndReentrancy()
{
    Fixture f;
    SliceModel* s = f.slice();
    f.backend->connected = false;
    s->setFrequency(14.21);
    s->setMode(QStringLiteral("LSB"));
    s->setFilterWidth(-2400, -100);
    s->setAgcMode(QStringLiteral("slow"));
    check(f.backend->calls() == 0, "disconnected active object cannot dispatch receive commands");
    f.backend->connected = true;
    const auto invalidation = QObject::connect(s, &SliceModel::receiveObservationChanged, s, [s] {
        s->setFrequency(14.205);
    });
    f.radio.stageSessionModelsForReconnectForTest();
    QObject::disconnect(invalidation);
    check(f.backend->calls() == 0,
          "observation invalidation during reconnect cannot send an old slice's edit to the new session");
    s->setFrequency(14.22);
    check(f.backend->calls() == 0, "staged but not reclaimed object has no command authority");
    emit f.backend->sliceChanged(0, report());
    check(f.slice() == s, "same-session reclaim preserves the slice object");
    s->setFrequency(14.23);
    check(f.backend->tunes.size() == 1, "reclaimed object retains exactly one live binding");

    emit f.backend->sliceRemoved(0); // old object lives until DeferredDelete
    emit f.backend->sliceChanged(0, report());
    check(f.slice() != s, "a new object may reuse the same numeric id");
    s->setFrequency(14.24);
    check(f.backend->tunes.size() == 1, "retired object cannot retune its replacement");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    s = f.slice();
    // A forbidden off-thread signal is refused synchronously, never queued
    // into a future session. Do not call model setters from the worker.
    std::thread worker([s] {
        emit s->receiveTuneRequested({14'500'000, SliceTuneRequest::PanIntent::PreservePan});
    });
    worker.join();
    f.radio.stageSessionModelsForReconnectForTest();
    emit f.backend->sliceChanged(0, report());
    QCoreApplication::sendPostedEvents(&f.radio, QEvent::MetaCall);
    check(f.backend->tunes.size() == 1, "off-thread intent cannot arrive after reclaim as a fresh command");

    auto connection = QObject::connect(s, &SliceModel::frequencyChanged, s, [s](double mhz) {
        if (mhz == 14.3) { s->setFrequency(14.31); }
    });
    s->setFrequency(14.3);
    check(f.backend->tunes.size() == 2 && f.backend->tunes.last().frequencyHz == 14'310'000,
          "reentrant tune supersedes the outer request rather than dispatching it last");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::frequencyChanged, s, [&](double mhz) {
        if (mhz == 14.4) {
            f.radio.stageSessionModelsForReconnectForTest();
            emit f.backend->sliceChanged(0, report());
        }
    });
    s->setFrequency(14.4);
    check(f.backend->tunes.size() == 2, "reconnect during local notification cancels the prior-session request");
    QObject::disconnect(connection);

    connection = QObject::connect(s, &SliceModel::filterChanged, s, [s](int low, int) {
        if (low == 150) { s->setFilterWidth(250, 2600); }
    });
    s->setFilterWidth(150, 2700);
    check(f.backend->filters.size() == 1 && f.backend->filters.last().lowHz == 250,
          "reentrant operator filter supersedes the older passband");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::agcModeChanged, s, [s](const QString& mode) {
        if (mode == QStringLiteral("fast")) { s->setAgcThreshold(42); }
    });
    s->setAgcMode(QStringLiteral("fast"));
    check(f.backend->agcs.size() == 2 && f.backend->agcs.last().field == SliceAgcRequest::Field::Mode
              && f.backend->agcs.last().mode == QStringLiteral("fast")
              && f.backend->agcs.last().threshold == 42,
          "a reentrant edit to another AGC field preserves both intents with the latest DSP pair");
    QObject::disconnect(connection);
    connection = QObject::connect(s, &SliceModel::agcModeChanged, s, [s](const QString& mode) {
        if (mode == QStringLiteral("slow")) { s->setAgcMode(QStringLiteral("med")); }
    });
    s->setAgcMode(QStringLiteral("slow"));
    check(f.backend->agcs.size() == 3 && f.backend->agcs.last().mode == QStringLiteral("med"),
          "a reentrant edit to the same AGC field supersedes the older request");
    QObject::disconnect(connection);
}

// A backend without a RadioConnection can still have an active WAN link.
void wanFallback()
{
    Fixture f;
    SliceModel* s = f.slice();
    WanConnection wan;   // never connectToRadio()'d: no socket is ever opened
    RadioModelSliceLifecycleTestAccess::setWanConnection(f.radio, &wan);

    // Every receive intent reaches the backend while only WAN is connected.
    f.backend->connected = false;
    WanConnectionTestAccess::setConnected(wan, true);
    s->setFrequency(14.21);
    // USB -> LSB also dispatches one ModeNormalization filter edit (see
    // creationPaths() above) in addition to the explicit setFilterWidth()
    // below — two filter dispatches is the correct count here, not a miscount.
    s->setMode(QStringLiteral("LSB"));
    s->setFilterWidth(-2400, -100);
    s->setAgcMode(QStringLiteral("slow"));
    check(f.backend->tunes.size() == 1 && f.backend->modes.size() == 1
              && f.backend->filters.size() == 2 && f.backend->agcs.size() == 1,
          "a live WAN link dispatches every receive intent even though the backend's own link is down");

    // Sanity check the other direction: neither link connected still refuses,
    // so the fix is not just "always dispatch".
    WanConnectionTestAccess::setConnected(wan, false);
    s->setFrequency(14.22);
    check(f.backend->tunes.size() == 1, "neither link connected still refuses the intent");

    RadioModelSliceLifecycleTestAccess::setWanConnection(f.radio, nullptr);
}

// Real Flex encoding with an undialed LAN link and injected WAN connectivity.
void wanOverUndialedFlexConnection()
{
    QStringList commands;
    RadioModel radio;
    check(radio.rebuildBackendForTest(QStringLiteral("flex")), "RadioModel builds a FlexBackend");
    auto* backend = qobject_cast<FlexBackend*>(radio.backend());
    check(backend != nullptr, "the WAN fixture uses the production Flex backend");
    if (!backend) {
        return;
    }
    backend->setSliceCommandSink([&commands](const QString& command) { commands.append(command); });
    check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "WAN fixture creates slice zero");
    SliceModel* slice = radio.slice(0);
    check(slice != nullptr, "WAN fixture has an active slice");
    if (!slice) {
        return;
    }
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    WanConnection wan;
    RadioModelSliceLifecycleTestAccess::setWanConnection(radio, &wan);
    WanConnectionTestAccess::setConnected(wan, true);
    check(radio.backend() && !radio.backend()->isConnected(), "the FlexBackend's own link is down");
    check(radio.isConnected(), "a live WAN link connects the model despite the undialed LAN link");
    commands.clear();
    slice->setFrequency(14.21);
    slice->setMode(QStringLiteral("LSB"));
    slice->setFilterWidth(-2400, -100);
    slice->setAgcMode(QStringLiteral("slow"));
    slice->setAgcThreshold(42);
    slice->setAgcOffLevel(31);
    check(commands == QStringList{"slice tune 0 14.210000 autopan=0",
                                  "slice set 0 mode=LSB", "filt 0 -2400 -100",
                                  "slice set 0 agc_mode=slow", "slice set 0 agc_threshold=42",
                                  "slice set 0 agc_off_level=31"},
          "WAN receive edits reach the real Flex command sink exactly once with correct values");
    WanConnectionTestAccess::setConnected(wan, false);
    commands.clear();
    warningMessages.clear();
    const QtMessageHandler previousHandler = qInstallMessageHandler(
        [](QtMsgType type, const QMessageLogContext& context, const QString& message) {
            if (type == QtWarningMsg && QString::fromUtf8(context.category) == QStringLiteral("aether.protocol")) {
                warningMessages.append(message);
            }
        });
    slice->setFrequency(14.22);
    qInstallMessageHandler(previousHandler);
    check(warningMessages == QStringList{"RadioModel: not connected, dropping slice 0 receive intent"},
          "a disconnected receive edit emits a protocol warning");
    slice->setMode(QStringLiteral("USB"));
    slice->setFilterWidth(200, 2600);
    slice->setAgcMode(QStringLiteral("fast"));
    check(!radio.isConnected() && commands.isEmpty(),
          "both links down refuses every receive intent at the real Flex sink");
    check(dropped.isEmpty(), "disconnected receive edits never emit an unsupported-control notice");
    RadioModelSliceLifecycleTestAccess::setWanConnection(radio, nullptr);
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("receive-intent-routing"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) { return 1; }
    confirmedReceiveAntennaAuthority();
    confirmedIncrementalTuneCursor();
    confirmedRecenterPreservesBackendViewport();
    confirmedReceiveControls();
    confirmedFilterContextRetirement();
    filterOnlyContextOwnership();
    receivePublicationLifetime();
    receiveResetLifetime();
    ritLifecycle();
    notchReadbackLifecycle();
    creationPaths();
    externalReceiveHandoffOrdering();
    receiveControls();
    receiveReentrancyAndLifecycle();
    nrsReadbackIsPassive();
    synchronousObservations();
    modeReentrancy();
    lifetimeAndReentrancy();
    wanFallback();
    wanOverUndialedFlexConnection();
    return failures == 0 ? 0 : 1;
}
