#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"
#include "core/backends/flex/FlexStatusIngress.h"
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
    static quint64 displayEpoch(const RadioModel& radio) { return radio.m_displayEpoch; }
    static quint64 beginSession(RadioModel& radio) { return radio.beginStatusSession(); }
    static void endSession(RadioModel& radio)
    { QMetaObject::invokeMethod(&radio, "onDisconnected", Qt::DirectConnection); }
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

// The ingress stamps each display report with the session it arrived in and
// refuses a line from a session that has since been replaced.
void ingressLifetime()
{
    RadioConnection connection;
    FlexStatusIngress ingress;
    ingress.bind(&connection);
    std::optional<DisplayObservation> report;
    QObject::connect(&ingress, &FlexStatusIngress::displayObserved, &ingress,
                     [&](const DisplayObservation& value) { report = value; });
    emit connection.lineReceived(CommandParser::parseLine(
        QStringLiteral("S0|display pan 0xC0000001 client_handle=0x71 center=7.15")), ingress.epoch());
    const quint64 next = ingress.beginSession();
    QCoreApplication::sendPostedEvents();
    check(!report, "a display line queued by a replaced session is refused");
    emit connection.lineReceived(CommandParser::parseLine(
        QStringLiteral("S0|display pan 0xC0000001 client_handle=0x71 center=14.2")), next);
    QCoreApplication::sendPostedEvents();
    check(report && report->statusEpoch == next && report->values.centerMhz == 14.2,
          "the current session publishes typed fields stamped with its epoch");
}

// A report deferred until its pan is claimed never applies in a later session.
void deferredReportDiesWithItsSession()
{
    PanControlFixture f;
    f.status(panObject, {{"center", "7.15"}});
    check(!f.radio.panadapter(panId) && FlexDisplayStatusTestAccess::deferred(f.radio) == 1,
          "an unclaimed pan's fields are deferred");
    FlexDisplayStatusTestAccess::endSession(f.radio);
    FlexDisplayStatusTestAccess::beginSession(f.radio);
    f.status(panObject, {{"client_handle", "0x71"}});
    check(f.radio.panadapter(panId) && !f.radio.panadapter(panId)->centerKnown(),
          "a claim in the next session does not apply the ended session's deferred fields");
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
    auto stale = decodeFlexDisplayStatus(panObject, {{"center", "28.7"}});
    stale->statusEpoch = FlexDisplayStatusTestAccess::displayEpoch(f.radio) + 1;   // another session's
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
        [&](int) { FlexDisplayStatusTestAccess::endSession(f.radio); });
    f.status(QStringLiteral("display pan ") + secondId, {{"fps", "13"}, {"average", "73"}});
    check(second->fps() == 13 && second->average() == beforeAverage,
        "a disconnect during a model signal stops the remaining fields");

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
    ingressLifetime();
    deferredReportDiesWithItsSession();
    lifecycle();
    reparentAndReentrantRetirement();
    reentrancyAndBounds();
    return failures == 0 ? 0 : 1;
}
