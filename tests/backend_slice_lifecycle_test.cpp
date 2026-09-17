// RFC #5468 P01: ordinary RX requests use the backend seam and model lifetime
// follows normalized state. Socket-free: injected backend state and a command /
// reply sink; no synthetic firmware peer, USB device, DSP channel or wire.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/RadioConnection.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess
{
public:
    static void useCommandPlane(RadioModel& radio, RadioConnection* connection)
    {
        radio.m_connection = connection;
    }
    static void setSessionSerials(RadioModel& radio, const QString& connected,
                                 const QString& nextTarget)
    {
        radio.m_connectedSessionSerial = connected;
        radio.m_lastInfo.serial = nextTarget;
    }
    static void completeDisconnect(RadioModel& radio)
    {
        // Inject the receipt edge into the real handler. setBackendForTest
        // installs receiver bindings, not setupBackend's connection wiring.
        radio.onDisconnected();
    }
};
class AutomationServerTestAccess
{
public:
    static QJsonObject slice(AutomationServer& server, const QString& action,
                             const QString& arg = {})
    {
        const QJsonObject request{{"cmd", "slice"}, {"action", action}, {"value", arg}};
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* description)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

class FixedBackend : public IRadioBackend
{
public:
    RadioCapabilities caps;
    int frequencyCalls = 0;
    int modeCalls = 0;
    bool connected = true;
    int disconnectCalls = 0;
    // Uninitialized RadioConnection has no socket, timer, thread or peer. Its
    // presence models the exact ownership boundary used by Flex and hybrid Sim.
    std::unique_ptr<RadioConnection> commandPlane;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; ++disconnectCalls; }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override { ++frequencyCalls; }
    void setSliceMode(int, const QString&) override { ++modeCalls; }
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override {} // no transport can transmit
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

class LifecycleBackend : public FixedBackend
{
public:
    LifecycleBackend()
    {
        caps.maxSlices = 4;
        caps.canCreateSlices = true;
    }
    bool acceptCreate = true;
    bool acceptRemove = true;
    bool removeSynchronously = false;
    int createCalls = 0;
    int removeCalls = 0;
    QString requestedPan;
    double requestedHz = 0.0;
    int requestedRemoval = -1;
    bool createSlice(const QString& panId, double frequencyHz) override
    {
        ++createCalls;
        requestedPan = panId;
        requestedHz = frequencyHz;
        return acceptCreate;
    }
    bool removeSlice(int sliceId) override
    {
        ++removeCalls;
        requestedRemoval = sliceId;
        if (acceptRemove && removeSynchronously) {
            emit sliceRemoved(sliceId);
        }
        return acceptRemove;
    }
};

SliceDelta fullDelta(const QString& pan, double frequency = 14.2)
{
    SliceDelta delta;
    delta.panId = pan;
    delta.frequency = frequency;
    delta.mode = QStringLiteral("USB");
    delta.filterLow = 150;
    delta.filterHigh = 2700;
    delta.inUse = true;
    delta.audioPan = 37;
    return delta;
}

LifecycleBackend* install(RadioModel& radio, const QString& family = QStringLiteral("rtl"))
{
    auto backend = std::make_unique<LifecycleBackend>();
    LifecycleBackend* pointer = backend.get();
    radio.setBackendForTest(std::move(backend), family);
    return pointer;
}

QString publishPan(RadioModel& radio, IRadioBackend& backend, const QString& opaquePan)
{
    emit backend.panCenterBandwidthChanged(opaquePan, 14.2, 0.1);
    check(radio.panadapters().size() == 1, "production geometry binding creates one pan");
    return radio.panId();
}

void testNeutralLifecycle()
{
    RadioModel radio;
    check(!radio.addSlice() && !radio.addSliceOnPan({}, 14.2) && !radio.removeSlice(0),
          "absent backend/pan refuses ordinary lifecycle");
    LifecycleBackend* backend = install(radio);
    const QString opaquePan = QStringLiteral("usb:capture/one");
    const QString pan = publishPan(radio, *backend, opaquePan);
    check(!pan.isEmpty() && pan != opaquePan, "fixture exercises opaque backend-to-model pan mapping");
    QSignalSpy added(&radio, &RadioModel::sliceAdded);
    QSignalSpy removed(&radio, &RadioModel::sliceRemoved);
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    QSignalSpy failed(&radio, &RadioModel::sliceLifecycleFailed);
    int lifecycleCommands = 0;
    radio.setSliceLifecycleCommandSinkForTest([&](const QString&, std::function<void(int, const QString&)>) {
        ++lifecycleCommands;
        return true;
    });
    bool completeAtNotification = false;
    QObject::connect(&radio, &RadioModel::sliceAdded, &radio, [&](SliceModel* slice) {
        completeAtNotification = slice->panId() == pan && slice->frequency() == 14.234567
            && slice->mode() == QLatin1String("USB") && slice->filterLow() == 150
            && slice->filterHigh() == 2700 && slice->audioPan() == 37;
    });

    check(!radio.addSliceOnPan(QStringLiteral("unknown"), 14.2), "unknown pan refuses without dispatch");
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::max()}) {
        check(!radio.addSliceOnPan(pan, invalid), "invalid/overflowing RF frequency refuses");
    }
    check(backend->createCalls == 0 && radio.slices().isEmpty(), "invalid requests never reach backend or create state");
    check(radio.addSliceOnPan(pan, 14.234567), "backend accepts explicit slice creation");
    check(backend->createCalls == 1 && backend->requestedPan == opaquePan
              && std::abs(backend->requestedHz - 14234567.0) < 0.001,
          "exactly one request uses opaque pan identity and Hz once");
    check(radio.slices().isEmpty() && added.isEmpty(), "accepted request has no optimistic model");
    emit backend->sliceChanged(0, fullDelta(opaquePan, 14.234567));
    SliceModel* original = radio.slice(0);
    check(original && added.size() == 1 && completeAtNotification,
          "first normalized delta publishes one fully populated slice");
    SliceDelta update;
    update.frequency = 14.25;
    emit backend->sliceChanged(0, update);
    check(radio.slice(0) == original && original->frequency() == 14.25 && added.size() == 1,
          "later delta updates existing object without duplicate ownership");
    check(backend->frequencyCalls == 0 && backend->modeCalls == 0,
          "normalized status never echoes tuning or mode intents");
    original->setFrequency(14.26);
    original->setMode(QStringLiteral("LSB"));
    check(backend->frequencyCalls == 1 && backend->modeCalls == 1,
          "repeated status does not duplicate per-slice intent bindings");
    check(!radio.removeSlice(0) && backend->removeCalls == 0, "last receiver cannot be closed");

    backend->acceptCreate = false;
    check(!radio.addSliceOnPan(pan, 14.22), "backend creation refusal propagates");
    check(failed.size() == 1 && failed.at(0).at(0).toString() == QLatin1String("create")
              && failed.at(0).at(1).toInt() == -1 && !failed.at(0).at(2).toString().isEmpty(),
          "backend creation refusal tells the operator through the lifecycle failure signal");
    backend->caps.canCreateSlices = false;
    const int callsBeforeUnsupported = backend->createCalls;
    check(!radio.addSliceOnPan(pan, 14.23) && backend->createCalls == callsBeforeUnsupported,
          "capacity alone cannot grant independent slice creation");
    check(failed.size() == 2 && failed.at(1).at(0).toString() == QLatin1String("create")
              && failed.at(1).at(1).toInt() == -1 && !failed.at(1).at(2).toString().isEmpty(),
          "unsupported creation also tells the operator without calling the backend hook");
    check(radio.slices().size() == 1 && radio.slice(0) == original
              && original->frequency() == 14.26 && original->mode() == QLatin1String("LSB")
              && radio.panadapters().size() == 1 && original->panId() == pan
              && added.size() == 1 && removed.isEmpty(),
          "creation refusals preserve receiver state and ownership");
    check(lifecycleCommands == 0 && dropped.isEmpty(),
          "creation refusal diagnostic does not fall back to a Flex command");
    failed.clear();
    backend->caps.canCreateSlices = true;
    backend->acceptCreate = true;
    check(radio.addSliceOnPan(pan, 14.24), "pending creation is accepted before a later failure");
    emit backend->sliceLifecycleFailed(QStringLiteral("create"), -1, QStringLiteral("receiver preparation failed"));
    check(failed.size() == 1 && failed.at(0).at(2).toString() == QLatin1String("receiver preparation failed")
              && radio.slices().size() == 1,
          "asynchronous failure is observable without inventing state");

    emit backend->sliceChanged(1, fullDelta(opaquePan, 14.3));
    emit backend->sliceChanged(3, fullDelta(opaquePan, 14.35));
    SliceModel* sibling = radio.slice(3);
    backend->acceptRemove = false;
    check(!radio.removeSlice(1), "backend removal refusal propagates");
    check(radio.slice(1) && radio.slice(3) == sibling && removed.isEmpty()
              && radio.panadapters().size() == 1,
          "refusal preserves slices and pan without authoritative removal notification");
    check(failed.size() == 2 && failed.at(1).at(0).toString() == QLatin1String("remove")
              && failed.at(1).at(1).toInt() == 1 && !failed.at(1).at(2).toString().isEmpty(),
          "backend removal refusal tells the operator through the lifecycle failure signal");
    check(!radio.removeSlice(2) && backend->removeCalls == 1, "unknown sparse ID is not a vector position");
    backend->acceptRemove = true;
    check(radio.removeSlice(1) && backend->requestedRemoval == 1 && radio.slice(1),
          "pending removal dispatches requested stable ID without removing it");
    emit backend->sliceRemoved(1);
    check(!radio.slice(1) && radio.slice(0) == original && radio.slice(3) == sibling
              && sibling->panId() == pan && sibling->audioPan() == 37 && removed.size() == 1,
          "confirmed middle removal preserves sibling identity, pan and audio destination");
    emit backend->sliceChanged(1, fullDelta(opaquePan, 14.32));
    check(radio.slice(0) == original && radio.slice(3) == sibling, "reusing a free slot does not renumber survivors");
    backend->removeSynchronously = true;
    check(radio.removeSlice(1) && !radio.slice(1), "synchronous confirmation is safe for the request caller");
    emit backend->sliceRemoved(1);
    check(removed.size() == 2, "duplicate removal emits no duplicate model removal");
}

void testReplacement()
{
    RadioModel radio;
    LifecycleBackend* old = install(radio);
    publishPan(radio, *old, QStringLiteral("old-pan"));
    emit old->sliceChanged(0, fullDelta(QStringLiteral("old-pan")));
    // AutoConnection queues from the emitting thread. Queue real production
    // callbacks, then destroy their sender before pumping the receiving thread.
    std::thread producer([old] {
        emit old->sliceChanged(2, fullDelta(QStringLiteral("old-pan")));
        emit old->sliceRemoved(0);
        emit old->panCenterBandwidthChanged(QStringLiteral("old-pan"), 999.0, 10.0);
        emit old->sliceLifecycleFailed(QStringLiteral("remove"), 0, QStringLiteral("old failure"));
    });
    producer.join();
    LifecycleBackend* current = install(radio, QStringLiteral("hl2"));
    const QString pan = publishPan(radio, *current, QStringLiteral("new-pan"));
    emit current->sliceChanged(0, fullDelta(QStringLiteral("new-pan"), 14.4));
    SliceModel* survivor = radio.slice(0);
    QSignalSpy added(&radio, &RadioModel::sliceAdded);
    QSignalSpy removed(&radio, &RadioModel::sliceRemoved);
    QSignalSpy failed(&radio, &RadioModel::sliceLifecycleFailed);
    QCoreApplication::processEvents();
    check(radio.slice(0) == survivor && !radio.slice(2) && added.isEmpty()
              && removed.isEmpty() && failed.isEmpty(),
          "already queued retired-backend events cannot create, remove or fail the new session");
    check(radio.panadapters().size() == 1 && radio.panadapter(pan)->centerMhz() == 14.2,
          "retired geometry cannot recreate an old pane or change new geometry");
    emit current->sliceChanged(1, fullDelta(QStringLiteral("new-pan")));
    check(added.size() == 1, "replacement installs the production binding exactly once");
    // Replacing the same family is still a new backend identity.
    current = install(radio, QStringLiteral("hl2"));
    check(radio.slices().isEmpty() && radio.panadapters().isEmpty(), "same-family replacement drops old model ownership");
    current->caps.canCreateSlices = false;
    const QString thirdPan = publishPan(radio, *current, QStringLiteral("third-pan"));
    check(!radio.addSliceOnPan(thirdPan, 14.2) && current->createCalls == 0,
          "replacement cannot inherit the previous backend's creation capability");
    emit current->sliceChanged(0, fullDelta(QStringLiteral("third-pan")));
    check(radio.slices().size() == 1, "same-family replacement can publish fresh state");
    // Complete RTL -> another family -> RTL through the same production teardown
    // and receiver bindings. Reusing the first RTL pan ID must not retain state.
    current = install(radio, QStringLiteral("rtl"));
    check(radio.slices().isEmpty() && radio.panadapters().isEmpty(),
          "returning to RTL starts without the intervening family's models");
    const QString rtlPan = publishPan(radio, *current, QStringLiteral("old-pan"));
    emit current->sliceChanged(0, fullDelta(QStringLiteral("old-pan"), 14.5));
    check(radio.addSliceOnPan(rtlPan, 14.6) && current->createCalls == 1
              && current->requestedPan == QLatin1String("old-pan")
              && std::abs(current->requestedHz - 14600000.0) < 0.001,
          "returning RTL uses its own creation capability and fresh pan mapping");
    SliceModel* returned = radio.slice(0);
    returned->setFrequency(14.55);
    check(current->frequencyCalls == 1 && radio.slices().size() == 1
              && returned->panId() == rtlPan,
          "returning RTL has one intent binding and accepted creation remains pending");
}

void testDisconnectAndReclaim()
{
    for (const bool sameRadio : {true, false}) {
        RadioModel radio;
        LifecycleBackend* backend = install(radio);
        const QString opaquePan = QStringLiteral("disconnect-pan");
        const QString pan = publishPan(radio, *backend, opaquePan);
        emit backend->sliceChanged(0, fullDelta(opaquePan, 14.2));
        emit backend->sliceChanged(1, fullDelta(opaquePan, 14.3));
        SliceModel* original = radio.slice(0);
        SliceModel* sibling = radio.slice(1);
        PanadapterModel* originalPan = radio.panadapter(pan);
        QSignalSpy added(&radio, &RadioModel::sliceAdded);
        QSignalSpy removed(&radio, &RadioModel::sliceRemoved);
        QSignalSpy failed(&radio, &RadioModel::sliceLifecycleFailed);
        QSignalSpy connectionState(&radio, &RadioModel::connectionStateChanged);
        QSignalSpy occupancy(&radio, &RadioModel::slotOccupancyChanged);
        check(radio.addSliceOnPan(pan, 14.4) && radio.removeSlice(1)
                  && radio.slices().size() == 2,
              "disconnect fixture starts with accepted, unconfirmed create and removal");

        // Same-family radio swaps can replace the target before the old backend
        // reports disconnect. The connected serial, not that target, owns the
        // surviving models. Both serials are inputs to the production handler.
        RadioModelSliceLifecycleTestAccess::setSessionSerials(
            radio, QStringLiteral("rtl-device-A"),
            sameRadio ? QStringLiteral("rtl-device-A") : QStringLiteral("rtl-device-B"));
        radio.disconnectFromRadio();
        check(backend->disconnectCalls == 1 && !radio.isConnected(),
              "ordinary disconnect dispatches once through the neutral backend");
        RadioModelSliceLifecycleTestAccess::completeDisconnect(radio);
        check(connectionState.size() == 1 && !connectionState.at(0).at(0).toBool(),
              "production disconnect handler publishes the disconnected state");
        check(radio.slice(0) == original && radio.slice(1) == sibling
                  && radio.panadapter(pan) == originalPan && removed.isEmpty()
                  && added.isEmpty() && failed.isEmpty(),
              "disconnect preserves UI objects without completing pending slice operations");

        // Inject only the next connected state and invoke the existing production
        // staging seam. No synthetic transport or duplicate lifecycle wiring.
        backend->connected = true;
        radio.stageSessionModelsForReconnectForTest();
        check(radio.slices().isEmpty() && radio.panadapters().isEmpty(),
              "reconnect staging withdraws prior-session models from live ownership");
        if (sameRadio) {
            check(removed.isEmpty() && !originalPan->centerKnown(),
                  "same-radio staging preserves reclaim candidates but resets center authority");
            emit backend->panCenterBandwidthChanged(opaquePan, 14.25, 0.1);
            emit backend->sliceChanged(0, fullDelta(opaquePan, 14.25));
            emit backend->sliceChanged(1, fullDelta(opaquePan, 14.35));
            check(radio.slice(0) == original && radio.slice(1) == sibling
                      && radio.panadapter(pan) == originalPan && added.isEmpty()
                      && removed.isEmpty() && occupancy.size() == 2,
                  "same-radio normalized state reclaims objects without duplicate slice ownership");
            check(original->frequency() == 14.25 && sibling->frequency() == 14.35
                      && originalPan->centerKnown() && originalPan->centerMhz() == 14.25,
                  "reclaim refreshes slice and pan state from the new session");
            original->setFrequency(14.26);
            check(backend->frequencyCalls == 1 && backend->createCalls == 1
                      && backend->removeCalls == 1,
                  "reclaim retains one intent binding and never retries pending lifecycle requests");
        } else {
            check(removed.size() == 2 && occupancy.size() == 2,
                  "changed radio serial prunes old ownership after production disconnect capture");
            emit backend->panCenterBandwidthChanged(opaquePan, 14.25, 0.1);
            emit backend->sliceChanged(0, fullDelta(opaquePan, 14.25));
            check(radio.slice(0) != original && radio.panadapter(pan) != originalPan
                      && added.size() == 1 && radio.slices().size() == 1,
                  "same-family different-radio state cannot reclaim the old device's objects");
        }
    }
}

void testCommandAdapter()
{
    using Reply = std::function<void(int, const QString&)>;
    for (const QString family : {QStringLiteral("flex"), QStringLiteral("sim")}) {
        RadioModel radio;
        LifecycleBackend* backend = install(radio, family);
        backend->caps.canCreateSlices = family == QLatin1String("flex");
        const QString pan = publishPan(radio, *backend, QStringLiteral("adapter-pan"));
        emit backend->sliceChanged(0, fullDelta(QStringLiteral("adapter-pan"), 14.2));
        backend->commandPlane = std::make_unique<RadioConnection>();
        RadioModelSliceLifecycleTestAccess::useCommandPlane(radio, backend->commandPlane.get());
        check(radio.hasCommandPlane(), "fixture supplies command-plane ownership independently of the sink");
        QStringList commands;
        std::vector<Reply> replies;
        bool accept = true;
        radio.setSliceLifecycleCommandSinkForTest([&](const QString& command, Reply reply) {
            commands.append(command);
            if (reply) {
                replies.push_back(std::move(reply));
            }
            return accept;
        });
        QSignalSpy failuresSpy(&radio, &RadioModel::sliceCreateFailed);
        check(radio.addSlice(), "command-plane adapter accepts default ordinary creation");
        check(commands.value(commands.size() - 1) == QStringLiteral("slice create pan=%1 freq=14.220000").arg(pan),
              "command adapter preserves center offset and six-decimal MHz formatting");
        check(radio.addSliceOnPan(pan, 14.2345678)
                  && commands.value(commands.size() - 1) == QStringLiteral("slice create pan=%1 freq=14.234568").arg(pan),
              "explicit creation preserves existing command encoding");
        check(!replies.empty(), "accepted command dispatch retains its response callback");
        if (!replies.empty()) {
            replies.back()(0, QStringLiteral("1"));
            check(radio.slices().size() == 1 && failuresSpy.isEmpty(), "successful reply alone cannot invent slice ownership");
            replies.back()(1, QStringLiteral("capacity"));
            check(failuresSpy.size() == 1, "existing Flex create failure callback remains observable");
        }
        emit backend->sliceChanged(1, fullDelta(QStringLiteral("adapter-pan"), 14.3));
        check(radio.removeSlice(1) && commands.value(commands.size() - 1) == QLatin1String("slice remove 1") && radio.slice(1),
              "command adapter preserves removal encoding and status authority");
        accept = false;
        check(!radio.addSliceOnPan(pan, 14.27) && !radio.removeSlice(1), "command transport refusal propagates");
        check(backend->createCalls == 0 && backend->removeCalls == 0,
              "Flex/Sim adapter never also dispatches neutral lifecycle verbs");
        install(radio, QStringLiteral("rtl"));
        if (!replies.empty()) {
            replies.front()(1, QStringLiteral("late reply"));
            check(failuresSpy.size() == 1, "retired command callback cannot report failure into a new session");
        }
    }
}

void testBridgeAndDefaults()
{
    FixedBackend fixed;
    check(!fixed.capabilities().canCreateSlices && !fixed.createSlice(QStringLiteral("pan"), 14200000.0)
              && !fixed.removeSlice(1), "default backend lifecycle refuses without opting fixed topologies in");
    RadioModel radio;
    LifecycleBackend* backend = install(radio);
    publishPan(radio, *backend, QStringLiteral("bridge-pan"));
    emit backend->sliceChanged(0, fullDelta(QStringLiteral("bridge-pan")));
    AutomationServer server;
    server.setRadioModel(&radio);
    for (const QString& invalid : {QStringLiteral("invalid"), QStringLiteral("0"),
                                   QStringLiteral("-1"), QStringLiteral("nan"),
                                   QStringLiteral("inf")}) {
        const QJsonObject invalidReply = AutomationServerTestAccess::slice(
            server, QStringLiteral("add"), invalid);
        check(!invalidReply.value("ok").toBool(), "bridge refuses explicit invalid frequency instead of defaulting");
    }
    check(backend->createCalls == 0, "malformed bridge requests never create at a default frequency");
    backend->acceptCreate = false;
    QJsonObject reply = AutomationServerTestAccess::slice(server, QStringLiteral("add"), QStringLiteral("14.25"));
    check(!reply.value("ok").toBool() && !reply.value("requested").toBool(), "bridge refuses rejected creation truthfully");
    backend->acceptCreate = true;
    reply = AutomationServerTestAccess::slice(server, QStringLiteral("add"), QStringLiteral("14.25"));
    check(reply.value("ok").toBool() && reply.value("requested").toBool()
              && reply.value("sliceCount").toInt() == 1
              && backend->requestedPan == QLatin1String("bridge-pan")
              && std::abs(backend->requestedHz - 14250000.0) < 0.001,
          "bridge distinguishes accepted explicit-frequency request from confirmed count");
    emit backend->sliceChanged(2, fullDelta(QStringLiteral("bridge-pan"), 14.25));
    backend->acceptRemove = false;
    reply = AutomationServerTestAccess::slice(server, QStringLiteral("remove"), QStringLiteral("2"));
    check(!reply.value("ok").toBool() && radio.slice(2), "bridge refuses rejected removal without changing state");
    backend->acceptRemove = true;
    backend->removeSynchronously = true;
    reply = AutomationServerTestAccess::slice(server, QStringLiteral("remove"), QStringLiteral("2"));
    check(reply.value("ok").toBool() && !radio.slice(2), "bridge supports confirmed neutral backend removal");
    reply = AutomationServerTestAccess::slice(server, QStringLiteral("remove"), QStringLiteral("0"));
    check(!reply.value("ok").toBool() && radio.slice(0), "bridge retains last-slice refusal");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-backend-slice-lifecycle-test"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    testNeutralLifecycle();
    testReplacement();
    testDisconnectAndReclaim();
    testCommandAdapter();
    testBridgeAndDefaults();
    return failures == 0 ? 0 : 1;
}
