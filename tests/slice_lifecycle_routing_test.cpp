// Socket-free model, backend encoding and protocol routing. Observations and
// replies are injected independently; no peer, radio, TX or listening server.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/RigctlProtocol.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMap>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStringList>
#include <cstddef>
#include <cstdio>
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
};
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
        f.radio.handleSliceStatusForTest(id, {}, true);
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
        if (alreadyForeign) { f.radio.handleSliceStatusForTest(2, foreign); }
        QObject owner;
        const quint64 ticket = f.create(&owner, {});
        f.radio.cancelSliceCreation(ticket);
        f.radio.handleSliceStatusForTest(2, foreign);
        f.radio.handleSliceStatusForTest(2, {{QStringLiteral("client_handle"), QStringLiteral("99")},
                                           {QStringLiteral("in_use"), QStringLiteral("0")}}, true);
        f.radio.handleSliceStatusForTest(2, {{QStringLiteral("client_handle"), QStringLiteral("1")},
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
    completedCreationReleasesState();
    answeredSelectionAndInhibit();
    duplexAndRefresh();
    rigctlCorrelation();
    tciCancellation();
    return failures == 0 ? 0 : 1;
}
