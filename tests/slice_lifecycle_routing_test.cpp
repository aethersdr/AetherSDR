#include "FlexSliceStatusTestSupport.h"
// Socket-free model, backend encoding and protocol routing. Observations and
// replies are injected independently; no peer, radio, TX or listening server.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/RigctlProtocol.h"
#include "core/WsprAgentClient.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QMap>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStringList>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void expire(RadioModel& radio, quint64 ticket) { radio.expireSliceCreation(ticket); }
    static void endSession(RadioModel& radio)
    {
        ++radio.m_sessionGeneration;
        radio.reconcileSliceCreates();
    }
    static void foreign(RadioModel& radio, int id) { radio.m_foreignSliceOwners.insert(id, 99); }
    static void gate(RadioModel& radio, std::function<void()> continuation, bool sessionCurrent = true)
    { radio.runRegistrationGateThen(std::move(continuation), [sessionCurrent] { return sessionCurrent; }); }
    static qsizetype retirementCount(const RadioModel& radio) { return radio.m_sliceRetirements.size(); }
    static auto pendingReference(const RadioModel& radio, quint64 ticket)
    { return std::weak_ptr<RadioModel::PendingSliceCreate>(radio.m_pendingSliceCreates.value(ticket)); }
    static void quarantine(RadioModel& radio, int id)
    {
        SliceModel* source = radio.slice(id);
        radio.m_slices.removeOne(source);
        radio.m_staleSlices.insert(id, source);
        emit radio.sliceRemoved(id);
    }
};
class TxOperationIntegrationTestAccess {
public:
    static void bindIdentity(RadioModel& radio, RadioConnection* connection)
    {
        radio.m_connection = connection;
        if (connection) { connection->m_handle.store(1); }
    }
};
#ifdef HAVE_WEBSOCKETS
class TciSliceLifecycleTestAccess {
public:
    static void create(TciServer& server, TciClient& client, SliceModel* rx)
    {
        if (!server.clientStateFor(&client)) {
            TciServer::ClientState state;
            state.socket = &client;
            server.m_clients.append(state);
        }
        server.createTxSliceForVfoB(&client, {0, 1, 14234000}, rx, {}, false);
    }
    static bool pending(const TciServer& server) { return server.m_pendingVfoBCreate.has_value(); }
    static bool ownsRoute(const TciServer& server) { return server.m_routingState.ownsRoute(); }
    static qsizetype createdCount(const TciServer& server) { return server.m_createdVfoBSlices.size(); }
    static void disableSplit(TciServer& server, TciClient& client, const CreatedSliceReceipt& receipt)
    {
        server.m_createdVfoBSlices.insert(receipt.sliceId(), receipt);
        server.m_trxMap.acquire(0);
        server.m_routingState.bindCreatedRoute(0, receipt.sliceId());
        server.m_routingState.setSplitRequested(true);
        server.handleSplitRequest(&client, {0, false});
    }
    static void replaceRoute(TciServer& server)
    {
        ++server.m_routeTransitionGeneration;
        server.m_routeTransitionInFlight = false;
        server.m_routingState.bindCreatedRoute(0, 2);
    }
    static int freeDax(const RadioModel& radio) { return TciServer::freeDaxChannelForTci(radio); }
    static qsizetype createdPanCount(const TciServer& server) { return server.m_createdVfoBPans.size(); }
    static void reap(TciServer& server, int sliceId) { server.reapCreatedVfoBSlice(sliceId); }
    // #7589 rows: socket-free entry points to TCI's own route teardown.
    static void addClient(TciServer& server, TciClient& client)
    {
        if (!server.clientStateFor(&client)) {
            TciServer::ClientState state;
            state.socket = &client;
            server.m_clients.append(state);
        }
        server.m_trxMap.acquire(0);
    }
    static void teardown(TciServer& server) { server.teardownTciRoute(); }
    static void setSplit(TciServer& server, bool on) { server.m_routingState.setSplitRequested(on); }
    static bool splitRequested(const TciServer& server) { return server.m_routingState.splitRequested(); }
    static void split(TciServer& server, TciClient& client, bool on) { server.handleSplitRequest(&client, {0, on}); }
    static void vfoB(TciServer& server, TciClient& client, long long hz) { server.handleVfoRequest(&client, {0, 1, hz}); }
    static int routeRx(const TciServer& server) { return server.m_routingState.rxSliceId(); }
    static int routeTx(const TciServer& server) { return server.m_routingState.txSliceId(); }};
#endif
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool success, const char* message)
{
    std::printf("[%s] %s\n", success ? "PASS" : "FAIL", message);
    failures += !success;
}
void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

class LifecycleProbe final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool live{true};
    bool accepts{true};
    std::vector<SliceCreateRequest> creates;
    std::vector<SliceCreateCompletion> createReplies;
    std::vector<int> removes;
    std::vector<std::pair<int, bool>> selections;
    std::vector<RadioControlCompletion> selectionReplies;
    std::vector<bool> duplex;
    std::vector<RadioControlCompletion> duplexReplies;
    int refreshes{0};
    bool closesPans{false};   // opt-in: answer a pan close as done
    int panRemoves{0};
    bool removesVfoBSlice{false};   // opt-in: a closed capture/two takes slice 1 with it
    bool removePanadapter(const QString& panId) override
    {
        ++panRemoves;
        if (closesPans && removesVfoBSlice && panId == QLatin1String("capture/two")) {
            emit sliceRemoved(1);
        }
        return closesPans || IRadioBackend::removePanadapter(panId);
    }
    LifecycleProbe()
    {
        caps.maxSlices = 4;
        caps.maxPanadapters = 1;
        caps.canCreateSlices = true;
        caps.canTransmit = true;
        caps.hasFullDuplex = true;
    }
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { live = false; }
    bool isConnected() const override { return live; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    ReceiveDispatch requestSliceCreate(const SliceCreateRequest& request,
        const SliceCreateCompletion& completion, const RadioControlFence& current) override
    {
        if (!accepts || !current()) { return ReceiveDispatch::Unsupported; }
        creates.push_back(request);
        createReplies.push_back(completion);
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestSliceRemove(int id, const RadioControlCompletion&,
                                      const RadioControlFence& current) override
    {
        if (!accepts || !current()) { return ReceiveDispatch::Unsupported; }
        removes.push_back(id);
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestTxSlice(int id, bool enabled, const RadioControlCompletion& completion,
                                   const RadioControlFence& current) override
    {
        if (!accepts || !current()) { return ReceiveDispatch::Unsupported; }
        selections.emplace_back(id, enabled);
        selectionReplies.push_back(completion);
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestFullDuplex(bool enabled, const RadioControlCompletion& completion,
                                      const RadioControlFence& current) override
    {
        if (!accepts || !current()) { return ReceiveDispatch::Unsupported; }
        duplex.push_back(enabled);
        duplexReplies.push_back(completion);
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestSliceInventoryRefresh(const RadioControlCompletion&,
                                                 const RadioControlFence& current) override
    {
        if (!accepts || !current()) { return ReceiveDispatch::Unsupported; }
        ++refreshes;
        return ReceiveDispatch::Dispatched;
    }
    // A paired receiver/pan backend's native open: its pan and slice are
    // published before this returns. Reached only with panLifecycleControl set.
    int panCreates{0};
    int panSliceId{1};   // the receiver id the next native open publishes
    bool createPanadapter() override
    {
        ++panCreates;
        emit panCenterBandwidthChanged(QStringLiteral("capture/two"), 14.25, 0.1);
        SliceDelta delta;
        delta.panId = QStringLiteral("capture/two");
        delta.frequency = 14.25;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 150;
        delta.filterHigh = 2700;
        delta.inUse = true;
        emit sliceChanged(panSliceId, delta);
        return true;
    }
};

struct Fixture {
    RadioModel radio;
    LifecycleProbe* backend;
    QString pan;
    Fixture()
    {
        auto owned = std::make_unique<LifecycleProbe>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));
        emit backend->panCenterBandwidthChanged(QStringLiteral("capture/one"), 14.2, 0.1);
        pan = radio.panId();
        publish(0, true);
    }
    void publish(int id, bool tx = false)
    {
        SliceDelta delta;
        delta.panId = QStringLiteral("capture/one");
        delta.frequency = 14.2 + id * 0.001;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 150;
        delta.filterHigh = 2700;
        delta.inUse = true;
        delta.txSlice = tx;
        emit backend->sliceChanged(id, delta);
    }
    quint64 create(QObject* owner, const SliceCreationCompletion& completed, bool transient = true)
    { return radio.createSlice({pan, 14234000.0, {}, {}}, owner, completed, transient); }
    void acknowledge(std::size_t index, int id)
    { backend->createReplies.at(index)({{RadioControlReply::Kind::Accepted, 0, {}}, id}); }
};

void encoding()
{
    RadioSetupTestBackend backend;
    std::optional<SliceCreateReply> answer;
    check(backend.requestSliceCreate({"0x40000000", 14234567.89, QStringLiteral("USB"), QStringLiteral("ANT1")},
        [&](const SliceCreateReply& result) { answer = result; }) == ReceiveDispatch::Dispatched,
        "Flex accepts typed create with mode and RX antenna");
    check(backend.commands == QStringList{"slice create pan=0x40000000 freq=14.234568 mode=USB rxant=ANT1"},
          "FlexLib create fields and MHz rounding are backend-owned");
    backend.replyLast({RadioControlReply::Kind::Accepted, 0, QStringLiteral("7|debug")});
    check(answer && answer->sliceId == 7, "FlexLib reply parser extracts ID before debug information");
    backend.requestSliceCreate({"0x40000000", 14200000.0, {}, {}},
        [&](const SliceCreateReply& result) { answer = result; });
    backend.replyLast({RadioControlReply::Kind::Accepted, 0, QStringLiteral("7 bad")});
    check(answer && answer->result.kind == RadioControlReply::Kind::Failed && !answer->sliceId,
          "malformed create reply never grants an ID");
    const auto before = backend.commands.size();
    check(backend.requestSliceCreate({"0x40000000\nradio reboot", 14200000.0, {}, {}})
              == ReceiveDispatch::Unsupported && backend.commands.size() == before,
          "pan token injection is rejected before dispatch");
    check(backend.requestSliceCreate({"0x40000000", std::numeric_limits<double>::infinity(), {}, {}})
              == ReceiveDispatch::Unsupported, "non-finite frequency refuses");
    check(backend.requestSliceCreate({"0x40000000", 14200000.0, QStringLiteral("USB tx=1"), {}})
              == ReceiveDispatch::Unsupported, "mode token injection refuses");
    backend.commands.clear();
    backend.requestSliceRemove(3);
    backend.requestTxSlice(2, true);
    backend.requestTxSlice(2, false);
    backend.requestFullDuplex(true);
    backend.requestSliceInventoryRefresh();
    check(backend.commands == QStringList{"slice remove 3", "slice set 2 tx=1", "slice set 2 tx=0",
              "radio set full_duplex_enabled=1", "sub slice all"}, "remaining lifecycle encoders stay below seam");
    const auto count = backend.commands.size();
    check(backend.requestFullDuplex(false, {}, [] { return false; }) == ReceiveDispatch::Unsupported
              && backend.requestSliceRemove(3, {}, [] { return false; }) == ReceiveDispatch::Unsupported
              && backend.commands.size() == count, "stale dispatch fence emits nothing");
}

void correlatedCreation()
{
    Fixture f;
    QObject owner;
    int countA = 0;
    int countB = 0;
    CreatedSliceReceipt a;
    CreatedSliceReceipt b;
    const quint64 first = f.create(&owner, [&](const SliceCreationResult& result) { ++countA; a = result.created; });
    const quint64 second = f.create(&owner, [&](const SliceCreationResult& result) { ++countB; b = result.created; });
    check(first && second && first != second && f.backend->creates.size() == 2,
          "competing creators have independent requests");
    check(f.backend->creates.front().panId == QLatin1String("capture/one")
              && f.backend->creates.front().frequencyHz == 14234000.0,
          "opaque backend pan identity and Hz cross the seam exactly once");
    check(f.radio.slices().size() == 1, "dispatch does not invent observed receivers");
    f.acknowledge(1, 2);
    check(countB == 0, "create ACK alone is not owned model publication");
    f.publish(1);
    settle();
    check(countA == 0 && countB == 0, "unrelated or unacknowledged status cannot complete either creator");
    f.acknowledge(0, 1);
    check(countA == 1 && a.sliceId() == 1 && countB == 0, "status-before-reply joins the correct creator");
    f.publish(2);
    settle();
    check(countB == 1 && b.sliceId() == 2, "reply-before-status joins the correct creator");
    f.acknowledge(0, 3);
    check(countA == 1, "duplicate reply cannot change the created identity");
    check(f.radio.removeCreatedSlice(a) && f.backend->removes == std::vector<int>{1},
          "receipt removes only its own current slice");
    check(f.radio.removeCreatedSlice(a) && f.backend->removes.size() == 1,
          "repeated cleanup does not dispatch a duplicate removal");
    emit f.backend->sliceRemoved(1);
    f.publish(1);
    check(!f.radio.resolveCreatedSlice(a) && !f.radio.removeCreatedSlice(a),
          "retired receipt cannot act on a reused numeric ID before deferred deletion");
    check(f.radio.removeSlice(1) && f.radio.removeCreatedSlice(b) && !f.radio.removeSlice(0),
          "concurrent removals preserve one receiver, counting in-flight requests");
}

void cancelledCreation()
{
    for (int scenario = 0; scenario != 3; ++scenario) {
        Fixture f;
        auto owner = std::make_unique<QObject>();
        int notifications = 0;
        const quint64 ticket = f.create(owner.get(), [&](const SliceCreationResult&) { ++notifications; });
        if (scenario == 0) { f.radio.cancelSliceCreation(ticket); }
        if (scenario == 1) { owner.reset(); }
        if (scenario == 2) { RadioModelSliceLifecycleTestAccess::expire(f.radio, ticket); }
        const int notified = notifications;
        f.acknowledge(0, 1);
        f.publish(1);
        settle();
        check(f.backend->removes == std::vector<int>{1},
              "cancel, destroyed requester and timeout retain original-session late cleanup");
        check(notifications == notified, "late completion never notifies a cancelled caller twice");
    }
    Fixture f;
    QObject owner;
    int notifications = 0;
    f.create(&owner, [&](const SliceCreationResult&) { ++notifications; });
    RadioModelSliceLifecycleTestAccess::endSession(f.radio);
    f.acknowledge(0, 1);
    f.publish(1);
    settle();
    check(notifications == 1 && f.backend->removes.empty(),
          "late prior-session ACK cannot clean up a replacement session's slice");
}

void retiredAndForeign()
{
    Fixture f;
    QObject owner;
    CreatedSliceReceipt created;
    f.create(&owner, [&](const SliceCreationResult& result) { created = result.created; });
    f.publish(1);
    emit f.backend->sliceRemoved(1);
    f.publish(1);
    f.acknowledge(0, 1);
    settle();
    check(!created.valid() && f.backend->removes.empty(), "slot retirement before ACK closes the ABA hole");
    Fixture unpublished;
    unpublished.create(&owner, [&](const SliceCreationResult& result) { created = result.created; });
    emit unpublished.backend->sliceRemoved(2);
    unpublished.publish(2);
    unpublished.acknowledge(0, 2);
    settle();
    check(!created.valid() && unpublished.backend->removes.empty(),
          "unpublished ID removal still invalidates a delayed create ACK");
    Fixture foreign;
    foreign.create(&owner, [&](const SliceCreationResult& result) { created = result.created; });
    foreign.publish(2);
    RadioModelSliceLifecycleTestAccess::foreign(foreign.radio, 2);
    foreign.acknowledge(0, 2);
    settle();
    check(!created.valid() && !foreign.radio.removeSlice(2) && foreign.backend->removes.empty(),
          "foreign status cannot grant a cleanup receipt or an ordinary close");
    Fixture capacity;
    capacity.backend->caps.maxSlices = 2;
    capacity.create(&owner, {});
    capacity.acknowledge(0, 1);
    check(capacity.create(&owner, {}) == 0 && capacity.backend->creates.size() == 1,
          "acknowledged but unpublished creation still reserves capacity");
}

void boundedRetirementHistory()
{
    Fixture f;
    QObject owner;
    CreatedSliceReceipt retained;
    f.create(&owner, [&](const SliceCreationResult& result) { retained = result.created; });
    f.acknowledge(0, 1);
    f.publish(1);
    settle();
    RadioControlReply::Kind pendingResult = RadioControlReply::Kind::Accepted;
    CreatedSliceReceipt uncertain;
    f.create(&owner, [&](const SliceCreationResult& result) {
        pendingResult = result.result.kind;
        uncertain = result.created;
    });
    for (int id = 1000; id != 2024; ++id) {
        applyFlexSliceStatus(f.radio, id, {}, true);
    }
    settle();
    check(RadioModelSliceLifecycleTestAccess::retirementCount(f.radio) <= 64,
          "arbitrary unknown removal status cannot grow retirement history beyond its fixed bound");
    check(f.radio.resolveCreatedSlice(retained) == f.radio.slice(1),
          "history compaction preserves a live slice's cleanup identity");
    f.acknowledge(1, 2);
    f.publish(2);
    settle();
    check(pendingResult != RadioControlReply::Kind::Accepted && !uncertain.valid(),
          "a pending creation with discarded retirement history cannot acquire cleanup authority");
    check(f.radio.removeCreatedSlice(retained) && f.backend->removes == std::vector<int>{1},
          "bounded history still permits cleanup of the original live receiver");
    Fixture stale;
    stale.create(&owner, [&](const SliceCreationResult& result) { retained = result.created; });
    stale.acknowledge(0, 1);
    stale.publish(1);
    settle();
    SliceModel* identity = stale.radio.slice(1);
    RadioModelSliceLifecycleTestAccess::quarantine(stale.radio, 1);
    for (int id = 3000; id != 4024; ++id) { emit stale.backend->sliceRemoved(id); }
    stale.publish(1);
    check(stale.radio.slice(1) == identity && !stale.radio.resolveCreatedSlice(retained)
              && RadioModelSliceLifecycleTestAccess::retirementCount(stale.radio) <= 64,
          "compaction retains tombstones for quarantined objects reclaimed in the same session");
}

void foreignRetirementBeforePublication()
{
    for (const bool alreadyForeign : {false, true}) {
        Fixture f;
        RadioConnection identity; // No init(): no socket or transport is constructed.
        TxOperationIntegrationTestAccess::bindIdentity(f.radio, &identity);
        const auto detach = qScopeGuard([&] {
            TxOperationIntegrationTestAccess::bindIdentity(f.radio, nullptr);
        });
        const QMap<QString, QString> foreign{{QStringLiteral("client_handle"), QStringLiteral("99")},
                                            {QStringLiteral("in_use"), QStringLiteral("1")}};
        if (alreadyForeign) { applyFlexSliceStatus(f.radio, 2, foreign); }
        QObject owner;
        const quint64 ticket = f.create(&owner, {});
        f.radio.cancelSliceCreation(ticket);
        applyFlexSliceStatus(f.radio, 2, foreign);
        applyFlexSliceStatus(f.radio, 2, {{QStringLiteral("client_handle"), QStringLiteral("99")},
                                           {QStringLiteral("in_use"), QStringLiteral("0")}}, true);
        applyFlexSliceStatus(f.radio, 2, {{QStringLiteral("client_handle"), QStringLiteral("1")},
                                           {QStringLiteral("in_use"), QStringLiteral("1")}});
        f.publish(2);
        SliceDelta pan;
        pan.panId = f.pan;
        emit f.backend->sliceChanged(2, pan);
        check(f.radio.slice(2)->panId() == f.pan && f.radio.sliceOwnedByUs(2),
              "foreign-churn fixture publishes an owned replacement on the original pan");
        f.acknowledge(0, 2);
        settle();
        check(f.radio.slice(2) && f.backend->removes.empty(),
              "foreign occupancy and removal before publication prevent cleanup of a later owned replacement");
    }
}

// A program on the radio (the WSPR receiver) holds a slice the radio reports
// as ours (#7139). Once reserved, no status makes it ours; a SliceModel made
// before the reservation is dropped; the radio removing it ends the
// reservation, so the index is ours to use again.
void reservedSliceForOnRadioProgram()
{
    Fixture f;
    RadioConnection identity; // No init(): no socket or transport is constructed.
    TxOperationIntegrationTestAccess::bindIdentity(f.radio, &identity);
    const auto detach = qScopeGuard([&] {
        TxOperationIntegrationTestAccess::bindIdentity(f.radio, nullptr);
    });
    auto ours = [](const char* freq) {
        return QMap<QString, QString>{{QStringLiteral("client_handle"), QStringLiteral("1")},
                                      {QStringLiteral("in_use"), QStringLiteral("1")},
                                      {QStringLiteral("RF_frequency"), QString::fromLatin1(freq)},
                                      {QStringLiteral("mode"), QStringLiteral("DIGU")},
                                      {QStringLiteral("pan"), QStringLiteral("0x40000000")},
                                      {QStringLiteral("dax"), QStringLiteral("4")},
                                      {QStringLiteral("tx"), QStringLiteral("0")}};
    };
    QList<int> removed;
    QObject::connect(&f.radio, &RadioModel::sliceRemoved, &f.radio, [&](int id) { removed.append(id); });

    // Reserved before its status arrives: never adopted.
    f.radio.reserveSlice(3, QStringLiteral("WSPR receiver"));
    applyFlexSliceStatus(f.radio, 3, ours("14.095600"));
    applyFlexSliceStatus(f.radio, 3, {{QStringLiteral("client_handle"), QStringLiteral("1")},
                                         {QStringLiteral("dax"), QStringLiteral("2")}});
    check(!f.radio.slice(3) && f.radio.isSlotForeign(3) && !f.radio.sliceMayBelongToUs(3)
              && f.radio.foreignSliceOwnerStation(3) == QStringLiteral("WSPR receiver"),
          "a reserved slice reported as ours is another program's slot, never a SliceModel");
    check(f.radio.daxChannelReserved(2) && !f.radio.daxChannelReserved(4) && !f.radio.daxChannelReserved(0),
          "a reserved slot's DAX channel follows the radio's statuses for it");

    // Adopted first, reserved after: the SliceModel goes.
    applyFlexSliceStatus(f.radio, 2, ours("18.104600"));
    SliceDelta onDax;   // the backend's typed status carries the DAX channel
    onDax.daxChannel = 4;
    emit f.backend->sliceChanged(2, onDax);
    const bool adopted = f.radio.slice(2) != nullptr && f.radio.slice(2)->daxChannel() == 4;
    f.radio.reserveSlice(2, QStringLiteral("WSPR receiver"));
    check(adopted && !f.radio.slice(2) && removed.contains(2) && f.radio.isSlotForeign(2),
          "reserving a slice already adopted drops its SliceModel");
    check(f.radio.daxChannelReserved(4), "a slice adopted before its reservation keeps its DAX channel reserved");

    // The radio removes it: the reservation ends and the index is free.
    applyFlexSliceStatus(f.radio, 3, {{QStringLiteral("in_use"), QStringLiteral("0")}}, true);
    check(!f.radio.isSliceReserved(3) && !f.radio.isSlotForeign(3),
          "removing a reserved slice ends the reservation");
    applyFlexSliceStatus(f.radio, 3, ours("14.200000"));
    check(f.radio.slice(3) != nullptr, "a later slice at a released index is ours again");

    f.radio.releaseReservedSlice(2);
    check(!f.radio.isSliceReserved(2) && !f.radio.isSlotForeign(2), "releasing a reservation frees the slot");
    check(!f.radio.daxChannelReserved(2) && !f.radio.daxChannelReserved(4),
          "no reservation, no reserved DAX channel");
    f.radio.reserveSlice(-1, QStringLiteral("WSPR receiver"));
    check(!f.radio.isSliceReserved(-1), "no slice, no reservation");

#ifdef HAVE_WEBSOCKETS
    // TCI's DAX auto-assign skips a reserved slot's channel as well as ours.
    f.radio.reserveSlice(5, QStringLiteral("WSPR receiver"));
    applyFlexSliceStatus(f.radio, 5, {{QStringLiteral("client_handle"), QStringLiteral("1")},
                                         {QStringLiteral("dax"), QStringLiteral("1")}});
    SliceDelta ourDax;
    ourDax.daxChannel = 2;
    emit f.backend->sliceChanged(3, ourDax);
    const bool ours2 = f.radio.slice(3) != nullptr && f.radio.slice(3)->daxChannel() == 2;
    check(ours2 && f.radio.daxChannelReserved(1)
              && TciSliceLifecycleTestAccess::freeDax(f.radio) == 3,
          "TCI's DAX auto-assign never takes a reserved slot's channel");
#endif
}

// The spot feed's status reads reserve the receiver's slice; the feed is never
// started, so nothing is requested.
void receiverSliceFromFeed()
{
    Fixture f;
    f.backend->live = true;
    f.publish(2);
    WsprSpotFeed feed;
    wspr::reserveReceiverSlice(&feed, &f.radio, QStringLiteral("WSPR receiver"));
    WsprStatus status;
    status.sliceIndex = -1;
    emit feed.statusReceived(status);
    status.sliceIndex = 2;
    const bool adopted = f.radio.slice(2) != nullptr;
    emit feed.statusReceived(status);
    check(adopted && !f.radio.slice(2) && f.radio.isSliceReserved(2)
              && f.radio.foreignSliceOwnerStation(2) == QStringLiteral("WSPR receiver")
              && !f.radio.isSliceReserved(-1),
          "a feed status naming an adopted slice reserves it and drops its SliceModel");
    f.backend->live = false;
    status.sliceIndex = 3;
    emit feed.statusReceived(status);
    check(!f.radio.isSliceReserved(3), "a feed status while disconnected reserves nothing");
}

// Registration waits for the gate, runs once, and never waits forever.
void registrationGate()
{
    Fixture f;
    int ran = 0;
    RadioModelSliceLifecycleTestAccess::gate(f.radio, [&] { ++ran; });
    check(ran == 1, "with no gate, registration goes ahead at once");

    std::function<void()> held;
    f.radio.setRegistrationGate([&](const QHostAddress&, std::function<void()> proceed) { held = proceed; });
    ran = 0;
    RadioModelSliceLifecycleTestAccess::gate(f.radio, [&] { ++ran; });
    check(ran == 0 && held, "registration waits for the gate");
    held();
    held();
    check(ran == 1, "the gate proceeds once");

    ran = 0;
    RadioModelSliceLifecycleTestAccess::gate(f.radio, [&] { ++ran; }, false);
    held();
    check(ran == 0, "a gate that proceeds after a reconnect doesn't register into the new session");

    f.radio.setRegistrationGate([](const QHostAddress&, std::function<void()>) {});
    ran = 0;
    RadioModelSliceLifecycleTestAccess::gate(f.radio, [&] { ++ran; });
    QElapsedTimer t;
    t.start();
    while (!ran && t.elapsed() < RadioModel::kRegistrationGateCapMs + 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    // A coarse timer may fire up to 5% early.
    check(ran == 1 && t.elapsed() >= RadioModel::kRegistrationGateCapMs * 9 / 10,
          "a gate that never proceeds holds registration only until the cap");
}

void completedCreationReleasesState()
{
    for (const bool accepted : {false, true}) {
        Fixture f;
        QObject owner;
        const quint64 ticket = f.create(&owner, {});
        const auto pending = RadioModelSliceLifecycleTestAccess::pendingReference(f.radio, ticket);
        if (accepted) {
            f.acknowledge(0, 1);
            f.publish(1);
            settle();
        } else {
            f.backend->createReplies.front()({{RadioControlReply::Kind::Failed, 1, "refused"}, {}});
        }
        f.backend->createReplies.clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(pending.expired(), "terminal creation releases its request snapshots and timers immediately");
    }
}

void answeredSelectionAndInhibit()
{
    Fixture f;
    f.publish(1);
    int answers = 0;
    RadioControlReply::Kind answer = RadioControlReply::Kind::Failed;
    check(f.radio.requestTxSlice(f.radio.slice(1), true, &f.radio,
        [&](const RadioControlReply& reply) { ++answers; answer = reply.kind; }) == ReceiveDispatch::Dispatched
              && answers == 0, "answered TX selection preserves continuation ordering");
    f.backend->selectionReplies.back()({RadioControlReply::Kind::Accepted, 0, {}});
    f.backend->selectionReplies.back()({RadioControlReply::Kind::Failed, 1, "duplicate"});
    check(answers == 1 && answer == RadioControlReply::Kind::Accepted, "selection continuation completes once");
    f.radio.setPanTransmitInhibited(f.pan, true);
    check(!f.backend->selections.empty() && !f.backend->selections.back().second,
          "RX-only-pan inhibit uses typed TX assignment disable");
    const auto count = f.backend->selections.size();
    check(f.radio.requestTxSlice(f.radio.slice(1)) == ReceiveDispatch::Unsupported
              && f.backend->selections.size() == count, "TX assignment cannot bypass a pan inhibit");
    emit f.backend->sliceRemoved(0);
    f.publish(0);
    f.radio.setPanTransmitInhibited(f.pan, false);
    check(f.backend->selections.size() == count,
          "inhibit removal never restores TX to a reused slice ID");
    Fixture stale;
    stale.publish(1);
    stale.radio.requestTxSlice(stale.radio.slice(1), true, &stale.radio,
        [&](const RadioControlReply& result) { answer = result.kind; });
    const auto reply = stale.backend->selectionReplies.back();
    RadioModelSliceLifecycleTestAccess::endSession(stale.radio);
    reply({RadioControlReply::Kind::Accepted, 0, {}});
    check(answer == RadioControlReply::Kind::Cancelled, "old TX-selection reply cannot admit a new session");

    Fixture superseded;
    superseded.publish(1);
    superseded.publish(2);
    superseded.radio.requestTxSlice(superseded.radio.slice(1), true, &superseded.radio,
        [&](const RadioControlReply& result) { answer = result.kind; });
    superseded.radio.requestTxSlice(superseded.radio.slice(2));
    superseded.backend->selectionReplies.front()({RadioControlReply::Kind::Accepted, 0, {}});
    check(answer == RadioControlReply::Kind::Cancelled,
          "superseded TX-selection ACK cannot admit a stale continuation");
    superseded.radio.requestTxSlice(superseded.radio.slice(1), true, &superseded.radio,
        [&](const RadioControlReply& result) { answer = result.kind; });
    superseded.radio.requestTxSlice(nullptr);
    superseded.backend->selectionReplies.back()({RadioControlReply::Kind::Accepted, 0, {}});
    check(answer == RadioControlReply::Kind::Accepted, "invalid target does not supersede valid TX intent");

    Fixture retired;
    retired.publish(1);
    retired.radio.requestTxSlice(retired.radio.slice(1), true, &retired.radio,
        [&](const RadioControlReply& result) { answer = result.kind; });
    const auto retiredReply = retired.backend->selectionReplies.back();
    RadioModelSliceLifecycleTestAccess::quarantine(retired.radio, 1);
    retired.publish(1);
    retiredReply({RadioControlReply::Kind::Accepted, 0, {}});
    check(answer == RadioControlReply::Kind::Cancelled,
          "TX-selection reply cannot authorize a retired and reclaimed slice object");
}

void duplexAndRefresh()
{
    Fixture f;
    f.radio.requestFullDuplex(true);
    f.radio.requestFullDuplex(false);
    f.backend->duplexReplies[1]({RadioControlReply::Kind::Accepted, 0, {}});
    f.backend->duplexReplies[0]({RadioControlReply::Kind::Accepted, 0, {}});
    check(!f.radio.fullDuplexEnabled(), "older FDX ACK cannot overwrite a newer operator request");
    f.radio.requestFullDuplex(true);
    RadioDelta observed;
    observed.fullDuplex = false;
    emit f.backend->radioChanged(observed);
    f.backend->duplexReplies.back()({RadioControlReply::Kind::Accepted, 0, {}});
    check(!f.radio.fullDuplexEnabled(), "radio FDX observation outranks delayed optimistic fallback");
    f.radio.requestFullDuplex(true);
    f.backend->duplexReplies.back()({RadioControlReply::Kind::Failed, 1, "not available"});
    check(!f.radio.fullDuplexEnabled(), "rejected FDX request never advances model state");
    const RadioControlSession old = f.radio.controlSession();
    SliceModel* original = f.radio.slice(0);
    check(f.radio.requestSliceInventoryRefresh() == ReceiveDispatch::Dispatched
              && f.backend->refreshes == 1 && f.radio.slice(0) == original,
          "runtime inventory refresh preserves all live models");
    RadioModelSliceLifecycleTestAccess::endSession(f.radio);
    check(f.radio.requestSliceInventoryRefresh(old) == ReceiveDispatch::Unsupported
              && f.backend->refreshes == 1, "post-operation refresh cannot borrow a replacement session");
    f.backend->live = false;
    check(f.radio.requestFullDuplex(true) == ReceiveDispatch::Unsupported
              && f.radio.requestSliceInventoryRefresh() == ReceiveDispatch::Unsupported,
          "disconnected configuration and refresh refuse");
}

void rigctlCorrelation()
{
    Fixture f;
    {
        RigctlProtocol protocol(&f.radio);
        protocol.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
        settle();
        check(f.backend->creates.size() == 1, "rigctl uses typed create for its on-demand split slice");
        f.publish(1);
        protocol.handleLine(QStringLiteral("\\get_split_freq"));
        settle();
        check(f.backend->selections.empty(), "rigctl does not adopt a competing creator's arriving slice");
        f.acknowledge(0, 2);
        f.publish(2);
        settle();
        protocol.handleLine(QStringLiteral("\\get_split_freq"));
        settle();
        check(!f.backend->selections.empty() && f.backend->selections.back().first == 2,
              "rigctl promotes only the slice from its create receipt");
    }
    settle();
    check(f.backend->removes == std::vector<int>{2}, "rigctl destruction reaps only its own created slice");

    Fixture cancelled;
    RigctlProtocol protocol(&cancelled.radio);
    protocol.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    protocol.handleLine(QStringLiteral("\\set_split_vfo 0 VFOA"));
    settle();
    check(cancelled.backend->creates.empty(), "same-pass CAT disable cancels before queued creation");

    Fixture reconnected;
    RigctlProtocol surviving(&reconnected.radio);
    surviving.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    settle();
    reconnected.acknowledge(0, 1);
    reconnected.publish(1);
    settle();
    SliceModel* original = reconnected.radio.slice(1);
    RadioModelSliceLifecycleTestAccess::endSession(reconnected.radio);
    RadioModelSliceLifecycleTestAccess::quarantine(reconnected.radio, 1);
    reconnected.publish(1);
    check(reconnected.radio.slice(1) == original,
          "CAT reconnect fixture preserves the original QObject without preserving its receipt");
    surviving.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    settle();
    check(reconnected.backend->selections.size() == 1
              && reconnected.backend->selections.back().first == 1,
          "CAT retries split after an unpromoted create receipt loses its original session");

    Fixture replaced;
    RigctlProtocol queued(&replaced.radio);
    queued.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    emit replaced.backend->sliceRemoved(0);
    replaced.publish(0, true);
    settle();
    check(replaced.backend->creates.empty(), "CAT queued create cannot follow a replacement RX object");
    queued.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    settle();
    check(replaced.backend->creates.size() == 1,
          "an explicit CAT retry may create split for the replacement receiver");

    Fixture inFlight;
    RigctlProtocol pending(&inFlight.radio);
    pending.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    settle();
    emit inFlight.backend->sliceRemoved(0);
    inFlight.publish(0, true);
    inFlight.acknowledge(0, 1);
    inFlight.publish(1);
    settle();
    check(inFlight.backend->removes == std::vector<int>{1}
              && inFlight.backend->selections.empty(),
          "CAT reaps its late creation instead of binding split to a replacement RX");

    Fixture promotion;
    RigctlProtocol promoting(&promotion.radio);
    promoting.handleLine(QStringLiteral("\\set_split_vfo 1 VFOB"));
    settle();
    promotion.acknowledge(0, 1);
    promotion.publish(1);
    settle();
    promoting.handleLine(QStringLiteral("\\get_split_freq"));
    emit promotion.backend->sliceRemoved(0);
    promotion.publish(0, true);
    settle();
    check(promotion.backend->selections.empty() && promotion.backend->removes == std::vector<int>{1},
          "CAT queued promotion cannot follow a replacement RX object");
}

void tciCancellation()
{
#ifdef HAVE_WEBSOCKETS
    for (int scenario = 0; scenario != 4; ++scenario) {
        Fixture f;
        TciServer server(&f.radio);
        TciClient client; // Value endpoint only: server.start() is never called.
        TciSliceLifecycleTestAccess::create(server, client, f.radio.slice(0));
        check(f.backend->creates.size() == 1 && TciSliceLifecycleTestAccess::pending(server),
              "TCI on-demand VFO-B uses the correlated model create");
        if (scenario == 0) { server.stop(); }
        if (scenario == 1 || scenario == 3) {
            emit f.backend->sliceRemoved(0);
            if (scenario == 1) { f.publish(0, true); }
        }
        f.acknowledge(0, 1);
        f.publish(1);
        settle();
        if (scenario == 2) {
            check(!f.backend->selectionReplies.empty(), "TCI waits for answered TX selection");
            server.stop();
            f.backend->selectionReplies.front()({RadioControlReply::Kind::Accepted, 0, {}});
        }
        check(!TciSliceLifecycleTestAccess::pending(server)
                  && !TciSliceLifecycleTestAccess::ownsRoute(server)
                  && f.backend->removes == std::vector<int>{1},
              "TCI stop, replaced RX and late selection cannot resurrect a route or reap another slice");
    }
    Fixture f;
    QObject owner;
    CreatedSliceReceipt receipt;
    f.create(&owner, [&](const SliceCreationResult& result) { receipt = result.created; });
    f.acknowledge(0, 1);
    f.publish(1, true);
    f.publish(0, false);
    settle();
    TciServer server(&f.radio);
    TciClient client;
    TciSliceLifecycleTestAccess::disableSplit(server, client, receipt);
    check(f.backend->selectionReplies.size() == 1, "TCI split disable waits for original RX selection");
    TciSliceLifecycleTestAccess::replaceRoute(server);
    if (!f.backend->selectionReplies.empty()) {
        f.backend->selectionReplies.front()({RadioControlReply::Kind::Accepted, 0, {}});
    }
    check(TciSliceLifecycleTestAccess::ownsRoute(server) && f.backend->removes.empty(),
          "late split-disable selection cannot clear a replacement route or start stale cleanup");

    for (int scenario = 0; scenario != 3; ++scenario) {
        Fixture retiredRx;
        TciServer retiredServer(&retiredRx.radio);
        TciClient retiredClient;
        TciSliceLifecycleTestAccess::create(retiredServer, retiredClient, retiredRx.radio.slice(0));
        retiredRx.acknowledge(0, 1);
        retiredRx.publish(1);
        settle();
        check(retiredRx.backend->selectionReplies.size() == 1,
              "TCI waits for TX selection after correlated creation");
        if (scenario == 2) {
            RadioModelSliceLifecycleTestAccess::quarantine(retiredRx.radio, 0);
        } else {
            emit retiredRx.backend->sliceRemoved(0);
        }
        if (scenario != 0) { retiredRx.publish(0, true); }
        retiredRx.backend->selectionReplies.front()({RadioControlReply::Kind::Accepted, 0, {}});
        settle();
        check(retiredRx.backend->removes == std::vector<int>{1},
              "TCI reaps created VFO B when original RX retires before selection ACK");
    }

    Fixture churn;
    TciServer churnServer(&churn.radio);
    TciClient churnClient;
    for (int id = 10; id != 90; ++id) {
        TciSliceLifecycleTestAccess::create(churnServer, churnClient, churn.radio.slice(0));
        churn.acknowledge(static_cast<std::size_t>(id - 10), id);
        churn.publish(id);
        settle();
        check(churn.backend->selectionReplies.size() == static_cast<std::size_t>(id - 9),
              "retired history does not consume capacity for a fresh TCI create and TX selection");
        churn.backend->selectionReplies.back()({RadioControlReply::Kind::Accepted, 0, {}});
        emit churn.backend->sliceRemoved(id);
    }
    check(TciSliceLifecycleTestAccess::createdCount(churnServer) == 0,
          "TCI discards cleanup receipts when their receiver is authoritatively removed");
#endif
}

// VFO B on a backend whose receivers open through the pan lifecycle. Its TX
// selection is the route's point, so a backend that cannot transmit opens no
// receiver for it; one that can keeps the route it had.
void tciVfoBOnPairedReceivers()
{
#ifdef HAVE_WEBSOCKETS
    for (const bool canTransmit : {false, true}) {
        Fixture f;
        f.backend->caps.canCreateSlices = false;
        f.backend->caps.maxSlices = 2;
        f.backend->caps.maxPanadapters = 2;
        f.backend->caps.canTransmit = canTransmit;
        f.backend->caps.panLifecycleControl = PanLifecycleControl{
            PanLifecycleControl::Creation::Native,
            {PanLifecycleAction::Create, PanLifecycleAction::Remove}};
        TciServer server(&f.radio);
        TciClient client;
        TciSliceLifecycleTestAccess::create(server, client, f.radio.slice(0));
        settle();
        if (!canTransmit) {
            check(f.backend->panCreates == 0 && f.radio.slices().size() == 1
                      && f.backend->selections.empty(),
                  "VFO B on a receive-only paired backend opens no receiver: the layout stays");
            check(!TciSliceLifecycleTestAccess::pending(server)
                      && !TciSliceLifecycleTestAccess::ownsRoute(server),
                  "and leaves no route or create behind");
        } else {
            check(f.backend->panCreates == 1 && f.radio.slices().size() == 2
                      && f.backend->selections.size() == 1
                      && f.backend->selections.front() == std::make_pair(1, true),
                  "a paired backend that can transmit still opens VFO B's receiver and selects it for TX");
        }
    }
#endif
}
}

// #7553: TCI reaps the pan it opened for VFO B. When that pan hosts the TX
// slice, the radio is keyed and the backend declares the keyed TX-pan close
// Refused, the pan stays open for the operator, TCI drops its entry and no
// operator Notice is raised. Unkeyed, the same reap closes the pan.
void tciKeyedTxPanReap()
{
#ifdef HAVE_WEBSOCKETS
    for (const bool keyed : {true, false}) {
        Fixture f;
        f.backend->caps.canCreateSlices = false;
        f.backend->caps.maxSlices = 2;
        f.backend->caps.maxPanadapters = 2;
        f.backend->caps.panLifecycleControl = PanLifecycleControl{
            PanLifecycleControl::Creation::Native,
            {PanLifecycleAction::Create, PanLifecycleAction::Remove}};
        f.backend->caps.panLifecycleControl->txPanCloseWhileKeyed =
            PanLifecycleControl::TxPanCloseWhileKeyed::Refused;
        f.backend->closesPans = true;
        TciServer server(&f.radio);
        TciClient client;
        TciSliceLifecycleTestAccess::create(server, client, f.radio.slice(0));
        settle();
        f.publish(0, false);
        SliceDelta tx;
        tx.panId = QStringLiteral("capture/two");
        tx.txSlice = true;
        emit f.backend->sliceChanged(1, tx);
        const SliceModel* vfoB = f.radio.slice(1);
        const QString txPan = vfoB ? vfoB->panId() : QString();
        const bool setUp = vfoB && vfoB->isTxSlice() && !txPan.isEmpty()
            && TciSliceLifecycleTestAccess::createdPanCount(server) == 1;
        TransmitDelta transmit;
        transmit.mox = keyed;
        f.radio.applyBackendTransmitDeltaForTest(transmit);
        int aboutToRemove = 0;
        int notices = 0;
        QObject::connect(&f.radio, &RadioModel::panAboutToRemove, &f.radio, [&](const QString&) { ++aboutToRemove; });
        QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested, &f.radio,
                         [&](const QString&, const QString&, const QString&) { ++notices; });
        TciSliceLifecycleTestAccess::reap(server, 1);
        if (keyed) {
            check(setUp && f.radio.panadapter(txPan) && aboutToRemove == 0 && f.backend->panRemoves == 0
                      && notices == 0 && TciSliceLifecycleTestAccess::createdPanCount(server) == 0,
                  "a keyed reap of TCI's VFO-B TX pan leaves it open, drops TCI's entry and raises no Notice");
        } else {
            check(setUp && aboutToRemove == 1 && f.backend->panRemoves == 1
                      && TciSliceLifecycleTestAccess::createdPanCount(server) == 0,
                  "an unkeyed reap of TCI's VFO-B TX pan closes it and drops the entry");
        }
        if (!setUp) {
            std::printf("  setup: vfoB=%d tx=%d pan=%s created=%lld\n", vfoB != nullptr,
                        vfoB && vfoB->isTxSlice(), qPrintable(txPan),
                        static_cast<long long>(TciSliceLifecycleTestAccess::createdPanCount(server)));
        }
    }
#endif
}

// #7589: TCI's own route teardown while the HL2 is keyed. Moving TX back to
// VFO A is refused while keyed, so the teardown is owed and finished after
// unkey: TX back to VFO A, then the created VFO-B pan closes. Nothing here
// keys; the rows drive the model's transmit edge and answer the backend's
// TX-selection requests by hand.
enum class KeyedTeardownCase { Owed, OtherRefusal, ReusedDuringOver, DeferredSplitOn,
                               ModelUnkeyedFirst, LostConfirmation };
void tciKeyedTeardown(KeyedTeardownCase c)
{
#ifdef HAVE_WEBSOCKETS
    using Access = TciSliceLifecycleTestAccess;
    Fixture f;
    f.backend->caps.canCreateSlices = false;
    f.backend->caps.maxSlices = 2;
    f.backend->caps.maxPanadapters = 2;
    f.backend->caps.panLifecycleControl = PanLifecycleControl{
        PanLifecycleControl::Creation::Native,
        {PanLifecycleAction::Create, PanLifecycleAction::Remove}};
    f.backend->caps.panLifecycleControl->txPanCloseWhileKeyed =
        PanLifecycleControl::TxPanCloseWhileKeyed::Refused;
    // The HL2 declares its keyed TX-move refusal, so TCI can ask first.
    f.backend->caps.txSliceChangeRefusedWhileKeyed = c != KeyedTeardownCase::OtherRefusal;
    f.backend->closesPans = true;
    f.backend->removesVfoBSlice = true;
    TciServer server(&f.radio);
    TciClient client;
    Access::addClient(server, client);
    QStringList sent;
    QObject::connect(&server, &TciServer::tciMessage, &server,
                     [&](const QString& dir, const QString& msg) { if (dir == QLatin1String("tx")) sent << msg; });
    int notices = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested, &f.radio,
                     [&](const QString&, const QString&, const QString&) { ++notices; });
    const auto moveTx = [&f](int to) {   // the radio's answer to a TX selection
        f.publish(0, to == 0);
        SliceDelta delta;
        delta.panId = QStringLiteral("capture/two");
        delta.txSlice = to == 1;
        emit f.backend->sliceChanged(1, delta);
    };
    const auto transmit = [&f](bool on) {
        TransmitDelta delta;
        delta.mox = on;
        f.radio.applyBackendTransmitDeltaForTest(delta);
    };
    const auto answer = [&f](const RadioControlReply& reply) {
        if (!f.backend->selectionReplies.empty()) { f.backend->selectionReplies.back()(reply); }
        settle();
    };
    const RadioControlReply accepted{RadioControlReply::Kind::Accepted, 0, {}};
    const RadioControlReply keyedRefusal{RadioControlReply::Kind::Refused, RadioControlReply::kRefusedWhileKeyed,
                                         QStringLiteral("TX slice can't change while transmitting.")};
    const RadioControlReply otherRefusal{RadioControlReply::Kind::Refused, -1,
                                         QStringLiteral("TX-slice selection refused")};
    const auto lastSelection = [&f](int id) {
        return !f.backend->selections.empty() && f.backend->selections.back() == std::make_pair(id, true);
    };
    const auto waitFor = [](const std::function<bool()>& done, int ms) {
        QElapsedTimer t;
        t.start();
        while (!done() && t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
        return done();
    };

    // VFO B routed: the created slice 1 on capture/two holds TX.
    Access::create(server, client, f.radio.slice(0));
    settle();
    bool routed = lastSelection(1);
    if (routed) { answer(accepted); moveTx(1); settle(); }
    routed = routed && Access::ownsRoute(server) && f.radio.slice(1) && f.radio.slice(1)->isTxSlice()
        && Access::createdPanCount(server) == 1;
    if (!routed) { std::printf("  setup: route to VFO B was not established\n"); }
    std::size_t before = f.backend->selections.size();

    switch (c) {
    case KeyedTeardownCase::Owed: {
        transmit(true);
        Access::teardown(server);
        settle();
        check(routed && f.backend->selections.size() == before && notices == 0,
              "#7589 N6: a keyed TCI teardown asks the HL2 for no TX move it would refuse (no Notice)");
        check(routed && f.radio.slice(1) && f.radio.slice(1)->isTxSlice() && f.backend->panRemoves == 0
                  && Access::createdPanCount(server) == 1,
              "#7589 R1: a teardown while keyed is owed: TX stays on VFO B, its pan open, TCI keeps the entry");
        check(routed && Access::ownsRoute(server) && Access::routeRx(server) == 0 && Access::routeTx(server) == 1,
              "#7589 N2: while the finish is owed the TCI route stays bound to VFO A / VFO B");
        before = f.backend->selections.size();
        transmit(false);
        settle();
        const bool asked = f.backend->selections.size() == before + 1 && lastSelection(0);
        if (asked) { answer(accepted); moveTx(0); settle(); }
        check(routed && asked && f.radio.slice(0) && f.radio.slice(0)->isTxSlice() && f.backend->panRemoves == 1
                  && Access::createdPanCount(server) == 0 && !Access::ownsRoute(server),
              "#7589 R2: on unkey TX goes back to VFO A, then the created pan closes and the route ends");
        break;
    }
    case KeyedTeardownCase::OtherRefusal: {
        transmit(true);
        Access::teardown(server);
        settle();
        const bool asked = f.backend->selections.size() == before + 1 && lastSelection(0);
        answer(otherRefusal);
        before = f.backend->selections.size();
        transmit(false);
        settle();
        settle();
        check(routed && asked && f.backend->selections.size() == before && f.backend->panRemoves == 0
                  && !Access::ownsRoute(server),
              "#7589 R4: a keyed teardown refused for another reason is not owed: no retry on unkey");
        break;
    }
    case KeyedTeardownCase::ReusedDuringOver: {
        transmit(true);
        Access::teardown(server);
        settle();
        TciClient other;   // a new client uses VFO B during the over
        Access::addClient(server, other);
        Access::vfoB(server, other, 14250000);
        settle();
        before = f.backend->selections.size();
        transmit(false);
        settle();
        settle();
        check(routed && f.backend->selections.size() == before && f.backend->panRemoves == 0
                  && Access::ownsRoute(server) && Access::routeTx(server) == 1,
              "#7589 R3: a slice a new route uses during the over is not closed, and stays TCI-created");
        Access::teardown(server);   // that client leaves, unkeyed
        settle();
        const bool asked = f.backend->selections.size() == before + 1 && lastSelection(0);
        if (asked) { answer(accepted); moveTx(0); settle(); }
        check(routed && asked && f.backend->panRemoves == 1 && Access::createdPanCount(server) == 0,
              "#7589 N7: when the route that reused the owed slice ends, its pan is still reaped");
        break;
    }
    case KeyedTeardownCase::DeferredSplitOn: {
        transmit(true);
        Access::teardown(server);
        settle();
        TciClient other;   // split on during the over: deferred by TCI's routing gate
        Access::addClient(server, other);
        Access::split(server, other, true);
        settle();
        before = f.backend->selections.size();
        const int pansBefore = f.backend->panCreates;
        // The radio's close of slice 1 lands after the replay may run, so the
        // replayed open gets its own receiver id.
        f.backend->caps.maxSlices = 3;
        f.backend->caps.maxPanadapters = 3;
        f.backend->panSliceId = 2;
        transmit(false);
        settle();
        const bool firstIsBack = f.backend->selections.size() == before + 1 && lastSelection(0)
            && f.backend->panCreates == pansBefore;
        check(routed && firstIsBack,
              "#7589 N1: at unkey the owed finish runs before the deferred split:true is replayed");
        // The radio moves TX first; its txSliceChanged is what answers the request.
        if (firstIsBack) { moveTx(0); settle(); }
        const bool replayed = f.backend->panRemoves == 1 && f.backend->panCreates == pansBefore + 1
            && f.backend->selections.size() == before + 2 && lastSelection(2);
        if (replayed) {
            answer(accepted);
            f.publish(0, false);
            SliceDelta delta;
            delta.panId = QStringLiteral("capture/two");
            delta.txSlice = true;
            emit f.backend->sliceChanged(2, delta);
            settle();
        }
        settle();
        if (!replayed) {
            std::printf("  replay: removes=%d creates=%d/%d selections=%zu/%zu last=(%d,%d) owns=%d slices=%lld\n",
                        f.backend->panRemoves, f.backend->panCreates, pansBefore, f.backend->selections.size(),
                        before, f.backend->selections.empty() ? -1 : f.backend->selections.back().first,
                        f.backend->selections.empty() ? -1 : int(f.backend->selections.back().second),
                        Access::ownsRoute(server), static_cast<long long>(f.radio.slices().size()));        }
        check(routed && replayed && f.backend->selections.size() == before + 2 && Access::ownsRoute(server)
                  && Access::routeTx(server) == 2 && f.radio.slice(2) && f.radio.slice(2)->isTxSlice(),
              "#7589 N1: the replayed split:true then owns TX on a new VFO B, and nothing moves it back");
        break;
    }
    case KeyedTeardownCase::ModelUnkeyedFirst: {
        // The model has already unkeyed (abortTciPtt before the teardown), the
        // backend has not: its refusal alone says the move is owed.
        Access::teardown(server);
        settle();
        const bool asked = f.backend->selections.size() == before + 1 && lastSelection(0);
        answer(keyedRefusal);
        check(routed && asked && Access::ownsRoute(server) && f.backend->panRemoves == 0,
              "#7589 N4: a keyed refusal owes the finish from the reply itself, with the model already unkeyed");
        const bool retried = waitFor([&] { return f.backend->selections.size() == before + 2; }, 3000)
            && lastSelection(0);
        if (retried) { answer(accepted); moveTx(0); settle(); }
        check(routed && retried && f.backend->panRemoves == 1 && !Access::ownsRoute(server),
              "#7589 N3: owed with no unkey edge still to come, the finish is retried and completes");
        break;
    }
    case KeyedTeardownCase::LostConfirmation: {
        Access::setSplit(server, true);
        Access::split(server, client, false);   // model unkeyed: not deferred
        settle();
        const bool asked = f.backend->selections.size() == before + 1 && lastSelection(0);
        sent.clear();
        answer(keyedRefusal);
        const bool retried = waitFor([&] { return f.backend->selections.size() == before + 2; }, 3000);
        answer(otherRefusal);
        settle();
        check(routed && asked && retried && sent.contains(QStringLiteral("split_enable:0,true;"))
                  && Access::splitRequested(server) && Access::ownsRoute(server),
              "#7589 N5: a held split:false whose retry is refused for another reason sends the corrected state");
        break;
    }
    }
#else
    (void)c;
#endif
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("slice-lifecycle-routing"));
    if (!settings.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    encoding();
    correlatedCreation();
    cancelledCreation();
    retiredAndForeign();
    boundedRetirementHistory();
    foreignRetirementBeforePublication();
    reservedSliceForOnRadioProgram();
    receiverSliceFromFeed();
    registrationGate();
    completedCreationReleasesState();
    answeredSelectionAndInhibit();
    duplexAndRefresh();
    rigctlCorrelation();
    tciCancellation();
    tciVfoBOnPairedReceivers();
    tciKeyedTxPanReap();
    tciKeyedTeardown(KeyedTeardownCase::Owed);
    tciKeyedTeardown(KeyedTeardownCase::OtherRefusal);
    tciKeyedTeardown(KeyedTeardownCase::ReusedDuringOver);
    tciKeyedTeardown(KeyedTeardownCase::DeferredSplitOn);
    tciKeyedTeardown(KeyedTeardownCase::ModelUnkeyedFirst);
    tciKeyedTeardown(KeyedTeardownCase::LostConfirmation);
    return failures == 0 ? 0 : 1;
}
