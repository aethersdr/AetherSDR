// PR #5824: real ANAN state publication and applied NB readback. Socket-free:
// inject connection receipts and call the bridge dispatcher directly. DSP work
// runs on its production I/O thread; P2Client is never started.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AutomationServer.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <functional>
#include <memory>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject get(AutomationServer& server, const QString& property = {})
    {
        const QJsonObject request{{"cmd", "get"}, {"model", "hostnb"},
                                  {"property", property}};
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
};
class RadioModelSliceLifecycleTestAccess {
public:
    static void disconnectForNext(RadioModel& radio, const QString& next)
    {
        radio.m_connectedSessionSerial = QStringLiteral("ANAN-A");
        radio.m_lastInfo.serial = next;
        radio.m_intentionalDisconnect = true;
        radio.onDisconnected();
    }
};
namespace anan {
class AnanNoiseBlankerTestAccess {
public:
    static bool onDsp(AnanBackend& backend, const std::function<void(AnanRxDsp&)>& action)
    {
        return QMetaObject::invokeMethod(backend.m_dsp, [&] {
            action(*backend.m_dsp);
        }, Qt::BlockingQueuedConnection);
    }
};
} // namespace anan
} // namespace AetherSDR

using namespace AetherSDR;
using namespace AetherSDR::anan;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

class ConnectedAnan : public AnanBackend {
public:
    bool connected = true;
    bool isConnected() const override { return connected; }
};

QJsonObject receiver(AutomationServer& server)
{
    const QJsonObject reply = AutomationServerTestAccess::get(server);
    check(reply.value("ok").toBool(), "hostnb dispatch succeeds through the ANAN namespace");
    const QJsonArray receivers = reply.value("hostnb").toObject().value("receivers").toArray();
    check(receivers.size() == 1, "ANAN reports exactly one receiver");
    return receivers.isEmpty() ? QJsonObject{} : receivers.first().toObject();
}

void testIdentity()
{
    for (const bool sameRadio : {true, false}) {
        RadioModel radio;
        auto owned = std::make_unique<ConnectedAnan>();
        ConnectedAnan* backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("anan"));
        test::SeamThreadAffinityProbe probe(backend);
        test::attachAllSeamSignals(probe);
        backend->setSliceFilter(0, 100, 2900);
        SliceModel* original = radio.slice(0);
        check(original != nullptr, "ANAN publication creates the original slice");
        if (!original) {
            continue;
        }
        original->setNbLevel(80);
        original->setNb(true);
        backend->connected = false;
        RadioModelSliceLifecycleTestAccess::disconnectForNext(
            radio, sameRadio ? QStringLiteral("ANAN-A") : QStringLiteral("ANAN-B"));
        backend->connected = true;
        radio.stageSessionModelsForReconnectForTest();
        // This calls the production emitSliceState(), including the NB fields.
        backend->setSliceFilter(0, 100, 2900);
        SliceModel* current = radio.slice(0);
        check(current != nullptr, "ANAN publication supplies the next session's slice");
        if (!current) {
            continue;
        }
        check((current == original) == sameRadio, "only the same serial reclaims the previous slice");
        check(current->nbOn() && current->nbLevel() == 80
                  && backend->noiseBlankerOnForTest() && backend->noiseBlankerLevelForTest() == 80,
              "same and different serials display the retained backend NB pair");
        current->setNb(false);
        current->setNbLevel(25);
        backend->setSliceFilter(0, 100, 2900);
        check(!current->nbOn() && current->nbLevel() == 25
                  && !backend->noiseBlankerOnForTest() && backend->noiseBlankerLevelForTest() == 25,
              "the replacement slice still drives NB and accepts its subsequent publication");
        check(probe.violations().isEmpty() && probe.count(QStringLiteral("sliceChanged")) >= 3,
              "real backend slice publications retain seam thread affinity");
    }
}

void testReadback()
{
    RadioModel radio;
    auto owned = std::make_unique<AnanBackend>();
    AnanBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("anan"));
    test::SeamThreadAffinityProbe probe(backend);
    test::attachAllSeamSignals(probe);
    AutomationServer server;
    server.setRadioModel(&radio);
    QJsonObject state = receiver(server);
    check(!state.value("hasChain").toBool() && !state.value("on").toBool(),
          "a cold DSP object does not claim an installed blanker");
    backend->setSliceNoiseBlanker(0, true, 80);
    check(AnanNoiseBlankerTestAccess::onDsp(*backend, [](AnanRxDsp&) {}), "drain the queued request");
    state = receiver(server);
    check(state.value("requestedOn").toBool() && state.value("requestedLevel").toInt() == 80
              && !state.value("hasChain").toBool() && !state.value("on").toBool(),
          "a request without a channel is not certified as applied");

    AnanRxDsp::Config config;
    config.noiseBlankerEnabled = true;
    config.noiseBlankerLevel = 80;
    config.blockForOutput = true;
    bool configured = false;
    check(AnanNoiseBlankerTestAccess::onDsp(*backend, [&](AnanRxDsp& dsp) {
        configured = dsp.configure(config);
    }), "configure on the production DSP thread without starting transport");
    check(configured, "the real WDSP channel configures");
    state = receiver(server);
    check(state.value("hasChain").toBool() && state.value("on").toBool()
              && state.value("level").toInt() == 80,
          "readback observes installed WDSP state");
    const QJsonObject narrowed = AutomationServerTestAccess::get(server, QStringLiteral("receivers"));
    const QJsonArray narrowedReceivers = narrowed.value("value").toArray();
    check(narrowed.value("ok").toBool()
              && narrowedReceivers.size() == 1 && narrowedReceivers.first().toObject() == state,
          "property narrowing returns the same applied receiver snapshot");
    check(!AutomationServerTestAccess::get(server, QStringLiteral("missing")).value("ok").toBool(),
          "unknown hostnb properties refuse");

    check(AnanNoiseBlankerTestAccess::onDsp(*backend, [](AnanRxDsp& dsp) { dsp.beginRebuild(); }),
          "mark a background rebuild in flight");
    backend->setSliceNoiseBlanker(0, false, 20);
    check(AnanNoiseBlankerTestAccess::onDsp(*backend, [](AnanRxDsp&) {}), "drain the deferred request");
    state = receiver(server);
    check(state.value("hasChain").toBool() && state.value("on").toBool()
              && state.value("level").toInt() == 80
              && !state.value("requestedOn").toBool() && state.value("requestedLevel").toInt() == 20,
          "in-flight readback preserves applied state separately from the deferred request");
    bool installed = false;
    check(AnanNoiseBlankerTestAccess::onDsp(*backend, [&](AnanRxDsp& dsp) {
        installed = dsp.installRebuiltChannel(AnanRxDsp::buildChannel(config));
    }), "install a channel built from the stale NB snapshot");
    state = receiver(server);
    check(installed && !state.value("on").toBool() && state.value("level").toInt() == 20,
          "readback follows current NB at the channel swap, not the stale build");
    for (const int requested : {-5, 120}) {
        backend->setSliceNoiseBlanker(0, true, requested);
        check(AnanNoiseBlankerTestAccess::onDsp(*backend, [](AnanRxDsp&) {}), "drain the live request");
        state = receiver(server);
        const int expected = requested < 0 ? 0 : 100;
        check(state.value("on").toBool() && state.value("level").toInt() == expected
                  && state.value("requestedLevel").toInt() == expected,
              "live applied readback follows the clamped request at both bounds");
        check(state.value("threshold").toDouble()
                  == WdspChannel::noiseBlankerThresholdForLevel(expected),
              "threshold describes the applied level");
    }
    check(probe.violations().isEmpty() && probe.count(QStringLiteral("extensionResult")) > 0,
          "readback emits results on the backend's owning thread");
}

void testOtherBackend()
{
    RadioModel radio;
    radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
    AutomationServer server;
    server.setRadioModel(&radio);
    check(AutomationServerTestAccess::get(server).value("ok").toBool(),
          "existing HL2 namespace still answers hostnb without a radio connection");
    radio.setBackendForTest({}, QStringLiteral("none"));
    check(!AutomationServerTestAccess::get(server).value("ok").toBool(),
          "an unsupported backend cannot return an empty success");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-nb-readback"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    testIdentity();
    testReadback();
    testOtherBackend();
    return failures == 0 ? 0 : 1;
}
