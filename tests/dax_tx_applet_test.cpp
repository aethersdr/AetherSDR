// Socket-free UI/bridge checks: production models and dispatcher, no radio,
// listener, audio device or peer. Model ownership/ACK lifecycle tests are separate.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "gui/DaxApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    {
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
    static QJsonObject request(AutomationServer& server, const QByteArray& request)
    {
        return server.handleLine(request, nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool passed, const char* description)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) {
        ++failures;
    }
}

class UnsupportedBackend final : public IRadioBackend {
public:
    int ownershipCalls{0};
    int keyingCalls{0};
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    {
        ++keyingCalls;
    }
    ReceiveDispatch requestDaxTxOwnership(const DaxTxOwnershipRequest&,
                                         DaxTxOwnershipCompletion) override
    {
        ++ownershipCalls;
        return ReceiveDispatch::Unsupported;
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

UnsupportedBackend* installUnsupported(RadioModel& model)
{
    auto backend = std::make_unique<UnsupportedBackend>();
    UnsupportedBackend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("test"));
    return source;
}

void appletLifetimeAndReasons()
{
    DaxApplet applet;
    applet.show();
    QPushButton* take = applet.findChild<QPushButton*>(QStringLiteral("daxTxTake"));
    QPushButton* release = applet.findChild<QPushButton*>(QStringLiteral("daxTxRelease"));
    QLabel* status = applet.findChild<QLabel*>(QStringLiteral("daxTxOwnershipStatus"));
    check(take && release && status, "ownership controls exist on every platform");
    if (!take || !release || !status) {
        return;
    }
    check(take->isVisible() && release->isVisible() && !take->isEnabled() && !release->isEnabled()
        && !take->accessibleDescription().isEmpty() && !release->accessibleDescription().isEmpty(),
        "unbound controls remain visible and disabled with accessible reasons");
    check(!take->property("aetherTxKeying").toBool() && !release->property("aetherTxKeying").toBool(),
        "ownership controls carry no transmit-keying authority");
#ifdef Q_OS_WIN
    check(applet.findChild<QLabel*>(QStringLiteral("daxWindowsNote")) != nullptr,
        "Windows retains its hosted-driver guidance beside ownership controls");
#endif

    auto first = std::make_unique<RadioModel>();
    UnsupportedBackend* firstSource = installUnsupported(*first);
    applet.setRadioModel(first.get());
    const auto registryCount = [&applet] {
        return applet.findChildren<ControlAvailabilityRegistry*>().size();
    };
    const qsizetype boundRegistries = registryCount();
    check(boundRegistries > 0 && !take->isEnabled() && !release->isEnabled()
        && take->toolTip() == first->daxTxOwnershipUnavailableReason(true)
        && take->accessibleDescription() == take->toolTip()
        && release->toolTip() == first->daxTxOwnershipUnavailableReason(false)
        && release->accessibleDescription() == release->toolTip(),
        "unsupported actions use the model's current reason on both accessible channels");
    applet.setRadioModel(first.get());
    check(registryCount() == boundRegistries && firstSource->ownershipCalls == 0,
        "rebinding the same model replaces registrations and never requests ownership");

    auto second = std::make_unique<RadioModel>();
    UnsupportedBackend* secondSource = installUnsupported(*second);
    applet.setRadioModel(second.get());
    const QString currentStatus = status->text();
    emit first->capabilitiesChanged(true, firstSource->capabilities());
    emit first->daxTxOwnershipChanged();
    first.reset();
    check(registryCount() == boundRegistries && status->text() == currentStatus
        && !take->isEnabled() && !release->isEnabled(),
        "replaced model events and destruction cannot retain old registrations or change the new status");
    check(secondSource->ownershipCalls == 0 && secondSource->keyingCalls == 0,
        "binding and status changes request neither stream selection nor keying");
    second.reset();
    check(!take->isEnabled() && !release->isEnabled()
        && status->text().contains(QStringLiteral("Unavailable"))
        && !take->accessibleDescription().isEmpty(),
        "destroying the bound model immediately disables ownership controls safely");
    applet.setRadioModel(nullptr);
    check(registryCount() == 0, "clearing the model retires every model-scoped registry");
}

void bridgeSnapshotAndBoundaries()
{
    RadioModel model;
    UnsupportedBackend* source = installUnsupported(model);
    AutomationServer server;
    server.setRadioModel(&model);
    server.setTxAllowed(false);
    const auto request = [&server](const QJsonObject& input) {
        return AutomationServerTestAccess::request(server, input);
    };
    const QJsonObject full = request({{QStringLiteral("cmd"), QStringLiteral("get")},
                                     {QStringLiteral("model"), QStringLiteral("daxtx")}});
    const QJsonObject snapshot = full.value(QStringLiteral("daxtx")).toObject();
    check(full.value(QStringLiteral("ok")).toBool() && snapshot == model.daxTxOwnershipSnapshot(),
        "get daxtx returns the real ownership snapshot");
    const QStringList fields{QStringLiteral("available"), QStringLiteral("streamId"),
        QStringLiteral("clientHandle"), QStringLiteral("txKnown"), QStringLiteral("owned"),
        QStringLiteral("ready"), QStringLiteral("pending"), QStringLiteral("state"),
        QStringLiteral("error"), QStringLiteral("canTake"), QStringLiteral("canRelease"),
        QStringLiteral("takeReason"), QStringLiteral("releaseReason")};
    for (const QString& field : fields) {
        check(snapshot.contains(field), qPrintable(QStringLiteral("snapshot publishes %1").arg(field)));
    }
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        const QJsonObject narrowed = request({{QStringLiteral("cmd"), QStringLiteral("get")},
            {QStringLiteral("model"), QStringLiteral("daxtx")}, {QStringLiteral("property"), it.key()}});
        const QJsonObject bare = AutomationServerTestAccess::request(
            server, QByteArray("get daxtx ") + it.key().toUtf8());
        check(narrowed.value(QStringLiteral("ok")).toBool()
            && narrowed.value(QStringLiteral("value")) == it.value()
            && bare.value(QStringLiteral("ok")).toBool()
            && bare.value(QStringLiteral("value")) == it.value(),
            qPrintable(QStringLiteral("JSON and bare property narrowing preserve %1").arg(it.key())));
    }
    check(!request({{QStringLiteral("cmd"), QStringLiteral("get")},
        {QStringLiteral("model"), QStringLiteral("daxtx")},
        {QStringLiteral("selector"), QStringLiteral("0x40000001")}}).value(QStringLiteral("ok")).toBool(),
        "get daxtx refuses a stream selector");
    check(!request({{QStringLiteral("cmd"), QStringLiteral("daxTx")},
        {QStringLiteral("action"), QStringLiteral("take")},
        {QStringLiteral("streamId"), QStringLiteral("0x40000001")}}).value(QStringLiteral("ok")).toBool(),
        "daxTx refuses arbitrary stream identities");
    check(!AutomationServerTestAccess::request(server, QByteArray("daxTx take 0x40000001"))
        .value(QStringLiteral("ok")).toBool(), "daxTx refuses a trailing operand");

    server.setReadOnly(true);
    const QJsonObject status = request({{QStringLiteral("cmd"), QStringLiteral("daxTx")},
        {QStringLiteral("action"), QStringLiteral("status")}});
    check(status.value(QStringLiteral("ok")).toBool()
        && status.value(QStringLiteral("daxtx")).toObject() == snapshot,
        "ownership status remains available without TX permission and in observe-only mode");
    check(!request({{QStringLiteral("cmd"), QStringLiteral("daxTx")},
        {QStringLiteral("action"), QStringLiteral("take")}}).value(QStringLiteral("ok")).toBool(),
        "observe-only mode refuses an ownership mutation");
    server.setReadOnly(false);
    const QJsonObject refused = request({{QStringLiteral("cmd"), QStringLiteral("daxTx")},
        {QStringLiteral("action"), QStringLiteral("take")}});
    check(!refused.value(QStringLiteral("ok")).toBool()
        && !refused.value(QStringLiteral("error")).toString().isEmpty()
        && refused.value(QStringLiteral("daxtx")).toObject() == model.daxTxOwnershipSnapshot(),
        "a model refusal carries its actual reason and the current snapshot");
    check(source->ownershipCalls == 0 && source->keyingCalls == 0,
        "reads and unsupported mutations dispatch neither ownership nor keying");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dax-tx-applet-test"));
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    QApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    appletLifetimeAndReasons();
    bridgeSnapshotAndBoundaries();
    std::printf("dax_tx_applet_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
