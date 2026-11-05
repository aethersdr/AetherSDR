#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"
#include "core/backends/flex/WanConnection.h"
#include "core/control/RadioResourceAdapter.h"
#include "core/control/ControlResourceStore.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
class FlexDisplayStatusTestAccess {
public:
    static void line(RadioConnection& connection, const QString& line) { connection.processLine(line); }
    static void line(WanConnection& connection, const QString& line) { connection.processLine(line); }
    static void buffered(RadioConnection& connection, const QByteArray& bytes) {
        if (!connection.m_socket) { connection.init(); }
        connection.m_readBuffer.append(bytes);
        connection.onReadyRead();
    }
    static void buffered(WanConnection& connection, const QByteArray& bytes) {
        connection.m_readBuffer.append(bytes);
        connection.onReadyRead();
    }
    static void reset(RadioConnection& connection) { connection.resetSessionState(); }
    static void reset(WanConnection& connection) { connection.resetSessionState(); }
    static void renew(RadioConnection& connection) { connection.m_displayStatusSource.reset(); }
    static void renew(WanConnection& connection) { connection.m_displayStatusSource.reset(); }
    static int deferred(const RadioModel& radio) { return radio.m_pendingDisplayObservations.size(); }
    static void age(RadioModel& radio) {
        for (auto& entry : radio.m_pendingDisplayObservations) { entry.insertedMs -= 31000; }
    }
    static int inventory(const RadioModel& radio) { return radio.m_radioDisplayPans.size(); }
    static QString parent(const RadioModel& radio, const QString& id) { return radio.m_radioDisplayWaterfalls.value(id).parentPanId; }
    static void stage(RadioModel& radio) {
        for (auto* pan : radio.m_panadapters) { pan->resetCenterKnownForReconnect(); }
        radio.m_stalePanadapters = radio.m_panadapters;
        radio.m_panadapters.clear();
        radio.m_radioDisplayPans.clear();
        radio.m_radioDisplayWaterfalls.clear();
        radio.m_pendingDisplayObservations.clear();
    }
};
}
namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
const QString panId = QStringLiteral("0x40000000");
const QString wfId = QStringLiteral("0x42000000");
const QString panObject = QStringLiteral("display pan 0x40000000");
const QString wfObject = QStringLiteral("display waterfall 0x42000000");

template<typename Transport> void transportLifetime()
{
    Transport connection;
    std::optional<DisplayObservation> report;
    QObject owner;
    QObject::connect(&connection, &Transport::displayObserved, &owner,
                     [&](const DisplayObservation& value) { report = value; }, Qt::QueuedConnection);
    FlexDisplayStatusTestAccess::line(connection, "S0|display pan 0xC0000001 client_handle=0x71 center=7.15");
    FlexDisplayStatusTestAccess::reset(connection);
    QCoreApplication::processEvents();
    check(report && !report->current(), "queued report keeps its retired original transport epoch");
    FlexDisplayStatusTestAccess::renew(connection);
    FlexDisplayStatusTestAccess::line(connection, "S0|display pan 0xC0000001 client_handle=0x71 center=14.2");
    QCoreApplication::processEvents();
    check(report && report->current() && report->values.centerMhz == 14.2, "new transport lifetime publishes typed fields");
    report.reset();
    QObject::connect(&connection, &Transport::messageReceived, &owner,
                     [&](const ParsedMessage&) { FlexDisplayStatusTestAccess::reset(connection); });
    FlexDisplayStatusTestAccess::line(connection, "S0|display pan 0xC0000001 center=28.2");
    QCoreApplication::processEvents();
    check(!report, "reentrant reset before status delivery cannot re-tag old input as current");
}

template<typename Transport> void retiredDisplayAcknowledgment()
{
    Transport connection;
    connection.retireDisplayObservations();
    QSignalSpy messages(&connection, &Transport::messageReceived);
    QSignalSpy displays(&connection, &Transport::displayObserved);
    FlexDisplayStatusTestAccess::buffered(connection,
        "R77|0|closed\nS0|display pan 0x40000000 client_handle=0x71 center=7.2\n");
    check(messages.count() == 2 && displays.count() == 0,
          "display revocation still parses teardown acknowledgments while suppressing display publication");
}

void lifecycle()
{
    PanControlFixture f;
    control::ControlResourceStore store;
    control::RadioResourceAdapter adapter(&f.radio, &store, QStringLiteral("display-session"));
    f.status(wfObject, {{"client_handle", "0x71"}, {"panadapter", panId}, {"line_duration", "65"}});
    f.status(panObject, {{"center", "7.15"}, {"fps", "25"}});
    f.status(panObject, {{"bandwidth", "0.1"}, {"average", "42"}});
    check(!f.radio.panadapter(panId), "unconfirmed resources are deferred without claiming a parent from waterfall association");
    bool initialized = false;
    QObject::connect(&f.radio, &RadioModel::panadapterAdded, &f.radio, [&](PanadapterModel* pan) {
        initialized = pan->centerMhz() == 7.15 && pan->bandwidthMhz() == 0.1
            && pan->average() == 42 && pan->fps() == 25 && pan->waterfallLineDuration() == 65
            && pan->lifecyclePublished();
    });
    f.status(panObject, {{"client_handle", "0x71"}, {"waterfall", wfId}});
    QPointer<PanadapterModel> original = f.radio.panadapter(panId);
    check(original && initialized, "publication includes merged partial fields and previously confirmed waterfall state");
    if (!original) { return; }
    const auto published = store.get({QStringLiteral("panadapter"), QStringLiteral("display-session"), panId});
    check(published && published->value.value(QStringLiteral("centerHz")).toInteger() == 7'150'000,
          "daemon resource publication sees initialized geometry");
    const auto staleEpoch = std::make_shared<std::atomic_bool>(true);
    const auto stale = decodeFlexDisplayStatus(panObject, {{"center", "28.7"}}, staleEpoch);
    staleEpoch->store(false);
    emit f.backend->displayObserved(*stale);
    check(original->centerMhz() == 7.15, "a retired observation cannot mutate an existing resource");
    f.clear();
    f.status(wfObject, {{"client_handle", "0x72"}, {"line_duration", "99"}});
    check(original->waterfallLineDuration() == 65 && original->waterfallId().isEmpty(),
          "explicit foreign waterfall overrides an existing association");
    f.status(panObject, {{"client_handle", "0x72"}, {"center", "28.5"}});
    f.status(panObject, {{"center", "28.6"}});
    check(f.radio.panadapter(panId) == original && original->centerMhz() == 7.15
        && !original->ownedByClient(0x71), "ownership loss keeps the display quiet and rejects handle-less foreign updates");
    check(f.radio.requestPanDisplay(original, {PanDisplayField::FrameRate, 15}) == ReceiveDispatch::Unsupported,
        "a held foreign display cannot issue commands");
    f.status(panObject, {{"client_handle", "0"}});
    check(!original->ownedByClient(0x71), "explicit zero ownership is not unknown-owner permission");
    f.status(panObject, {{"client_handle", "0x71"}, {"center", "14.2"}, {"waterfall", "0x42000001"}});
    check(original->ownedByClient(0x71) && original->centerMhz() == 14.2
        && original->waterfallId() == QLatin1String("0x42000001"), "confirmed reclaim restores current values and new pairing");
    f.status(wfObject, {{"client_handle", "0x71"}, {"panadapter", panId}, {"line_duration", "90"}});
    check(original->waterfallId() == QLatin1String("0x42000001") && original->waterfallLineDuration() == 65,
          "late old waterfall status cannot overwrite the replacement");
    f.status("display waterfall 0x42000001", {{"client_handle", "0x71"}, {"panadapter", panId}, {"line_duration", "45"}});
    check(original->waterfallLineDuration() == 45, "replacement waterfall receives its own state");
    f.status("display waterfall 0x42000001 removed", {});
    check(original->waterfallId().isEmpty(), "bare waterfall removal clears the association");
    f.status("display waterfall 0x42000001", {{"client_handle", "0x71"}, {"panadapter", panId}, {"line_duration", "99"}});
    check(original->waterfallId().isEmpty() && original->waterfallLineDuration() == 45,
          "retired waterfall cannot resurrect itself from a late parent report");
    f.status(wfObject, {{"removed", "1"}});
    f.remove(panId);
    f.remove(panId);
    check(!f.radio.panadapter(panId), "duplicate pan retirement is harmless");
    auto* replacement = f.publish(panId);
    check(replacement && replacement != original, "retired numeric identity creates a new object");
}

void reparentAndReentrantRetirement()
{
    PanControlFixture f;
    PanadapterModel* first = f.publish(panId, wfId);
    const QString secondId = QStringLiteral("0x40000001");
    PanadapterModel* second = f.publish(secondId);
    f.status(wfObject, {{"client_handle", "0x71"}, {"panadapter", panId}, {"line_duration", "55"}});
    f.status(wfObject, {{"panadapter", secondId}, {"line_duration", "85"}});
    check(first->waterfallId().isEmpty() && first->waterfallLineDuration() == 55
        && second->waterfallId().isEmpty(), "reparenting detaches the old pan and waits for a confirmed new child link");
    f.status(QStringLiteral("display pan ") + secondId, {{"waterfall", wfId}});
    check(second->waterfallId() == wfId && second->waterfallLineDuration() == 85,
        "confirmed new pairing consumes only its own deferred waterfall state");
    const int beforeAverage = second->average();
    QObject::connect(second, &PanadapterModel::fpsChanged, &f.radio,
        [&](int) { FlexDisplayStatusTestAccess::reset(f.identity); });
    f.status(QStringLiteral("display pan ") + secondId, {{"fps", "13"}, {"average", "73"}});
    check(second->fps() == 13 && second->average() == beforeAverage,
        "transport retirement during a model signal stops the remaining fields");

    auto model = std::make_unique<PanadapterModel>(panId);
    QObject receiver;
    QObject::connect(model.get(), &PanadapterModel::fpsChanged, &receiver,
        [&](int) { model.reset(); });
    DisplayValues fields;
    fields.fps = 17;
    fields.average = 66;
    check(!model->applyDisplayValues(fields, {}) && !model,
        "model deletion during notification stops application without touching freed state");
}

void reentrancyAndBounds()
{
    PanControlFixture f;
    PanadapterModel* first = f.publish(panId, wfId);
    bool recreated = false;
    QObject::connect(&f.radio, &RadioModel::panadapterRemoved, &f.radio, [&](const QString& id) {
        if (!recreated) { recreated = true; f.publish(id); }
    });
    f.remove(panId);
    check(recreated && f.radio.panadapter(panId) && f.radio.panadapter(panId) != first,
          "reentrant same-id creation runs after old removal notifications");
    FlexDisplayStatusTestAccess::stage(f.radio);
    f.status(panObject, {{"center", "7.2"}});
    check(!f.radio.panadapter(panId), "staged previous-session models require ownership confirmation");
    f.status(panObject, {{"client_handle", "0x71"}});
    check(f.radio.panadapter(panId) && f.radio.panadapter(panId)->centerMhz() == 7.2,
          "reclaimed model applies current deferred fields before publication");
    for (int i = 1; i < 200; ++i) {
        f.status(QStringLiteral("display pan 0x%1").arg(0x40000000U + i, 8, 16, QLatin1Char('0')), {{"center", "14.2"}});
    }
    check(FlexDisplayStatusTestAccess::deferred(f.radio) <= 128, "unknown display cache is bounded");
    FlexDisplayStatusTestAccess::age(f.radio);
    f.status("display pan 0x40000001", {{"client_handle", "0x71"}});
    check(f.radio.panadapter("0x40000001") && !f.radio.panadapter("0x40000001")->centerKnown(),
          "expired deferred fields do not become current on a late claim");
    for (int i = 200; i < 400; ++i) {
        f.status(QStringLiteral("display pan 0x%1").arg(0x40000000U + i, 8, 16, QLatin1Char('0')), {{"client_handle", "0x72"}});
    }
    check(FlexDisplayStatusTestAccess::inventory(f.radio) <= 256, "radio inventory has a hard bound");
    check(!f.radio.receiveControlPanId(panId), "incomplete inventory also holds daemon receive controls");
    check(f.radio.requestPanDisplay(f.radio.panadapter(panId), {PanDisplayField::FrameRate, 20}) == ReceiveDispatch::Unsupported,
          "incomplete inventory fails closed instead of granting writes from missing evidence");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("flex-display-observation"));
    QCoreApplication app(argc, argv);
    qRegisterMetaType<DisplayObservation>();
    transportLifetime<RadioConnection>();
    transportLifetime<WanConnection>();
    retiredDisplayAcknowledgment<RadioConnection>();
    retiredDisplayAcknowledgment<WanConnection>();
    lifecycle();
    reparentAndReentrantRetirement();
    reentrancyAndBounds();
    return failures == 0 ? 0 : 1;
}
