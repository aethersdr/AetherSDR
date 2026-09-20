// Socket-free: invoke the production bridge dispatcher and backend extension
// client against an injected transport. Never create a firmware peer.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/DroopCalibration.h"
#include "core/backends/anan/AnanBackend.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject call(AutomationServer& server, const QString& action)
    {
        return server.handleLine(QJsonDocument(QJsonObject{
            {"cmd", "droopcal"}, {"action", action}}).toJson(QJsonDocument::Compact), nullptr);
    }
};
}
using namespace AetherSDR;

class ExtensionProbe : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool online = true;
    int calls = 0;
    int bandwidthWrites = 0;
    bool fail = false;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return online; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setPanBandwidth(const QString&, double) override { ++bandwidthWrites; }
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override {}
    void invokeExtension(const QString& ns, const QString& verb, quint64 id, const QVariant&) override
    {
        ++calls;
        if (ns != "anan" || !verb.startsWith("droop.") || fail) {
            emit extensionError(id, "refused");
            return;
        }
        // An unrelated response must not satisfy this request.
        emit extensionError(id + 1, "unrelated");
        emit extensionResult(id, QVariantMap{{"running", verb == "droop.start"},
                                           {"hasResult", false}, {"rateIndex", 0}});
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("droop-seam"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&failures](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    check(settings.isValid(), "isolated settings");
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("icom"),
                                 QStringLiteral("rtl"), QStringLiteral("hl2"), QStringLiteral("sim")}) {
        for (bool advertised : {false, true}) {
            auto owned = std::make_unique<ExtensionProbe>();
            auto* probe = owned.get();
            probe->caps.family = family;
            probe->caps.hostDroopCalibration = advertised;
            radio.setBackendForTest(std::move(owned), family);
            for (const QString& action : {QStringLiteral("status"), QStringLiteral("start"),
                                         QStringLiteral("stop"), QStringLiteral("apply"),
                                         QStringLiteral("discard")}) {
                check(!requestDroopCalibration(probe, action).value("ok").toBool(),
                      "dialog request rejects a non-ANAN family even with advertised capability");
                check(!AutomationServerTestAccess::call(server, action).value("ok").toBool(),
                      "bridge rejects a non-ANAN family");
            }
            check(probe->calls == 0 && probe->bandwidthWrites == 0,
                  "refusal emits no extension or generic bandwidth write");
        }
    }
    auto owned = std::make_unique<ExtensionProbe>();
    auto* probeAnan = owned.get();
    probeAnan->caps.family = "anan";
    radio.setBackendForTest(std::move(owned), "anan");
    check(!requestDroopCalibration(probeAnan, "start").value("ok").toBool(),
          "ANAN without capability is refused");
    probeAnan->caps.hostDroopCalibration = true;
    probeAnan->online = false;
    check(!requestDroopCalibration(probeAnan, "start").value("ok").toBool(),
          "disconnected ANAN is refused");
    check(probeAnan->calls == 0, "unavailable ANAN gets no request");
    probeAnan->online = true;
    const QJsonObject started = AutomationServerTestAccess::call(server, "start");
    check(started.value("ok").toBool() && started.value("running").toBool(),
          "capable ANAN receives the request and reports backend state");
    check(probeAnan->calls == 1 && probeAnan->bandwidthWrites == 0,
          "clients use only the extension, never generic pan commands");
    probeAnan->fail = true;
    check(!requestDroopCalibration(probeAnan, "apply").value("ok").toBool(),
          "backend refusal is propagated");
    // Actual ANAN backend is inert until connectRadio, which is never called.
    anan::AnanBackend backend;
    bool refused = false;
    QObject::connect(&backend, &IRadioBackend::extensionError, &app,
                     [&refused](quint64, const QString&) { refused = true; });
    backend.invokeExtension("anan", "droop.start", 10, {});
    check(refused, "backend itself refuses a disconnected start");
    return failures == 0 ? 0 : 1;
}
