// #5904: the production model bindings, not a second implementation of them.
// Injected state/dispatch only: no sockets, firmware peer, hardware or keying.
#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <cstdio>
#include <functional>
#include <thread>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void wire(RadioModel& radio, SliceModel* slice)
    {
        radio.wireSliceReceiveIntentsToBackend(slice);
    }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class Backend final : public IRadioBackend {
public:
    bool connected = false;
    QList<SliceTuneRequest> tunes;
    QList<SliceFilterRequest> filters;
    QList<SliceAgcRequest> agcs;
    QStringList modes;
    int pairedAgcCalls = 0;
    int keyCalls = 0;
    std::function<void(int)> observe;
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
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
    explicit Fixture(bool commandPlaneCreation = false)
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
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
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("receive-intent-routing"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) { return 1; }
    creationPaths();
    synchronousObservations();
    lifetimeAndReentrancy();
    return failures == 0 ? 0 : 1;
}
