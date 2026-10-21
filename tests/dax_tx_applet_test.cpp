// Socket-free UI/bridge checks: production models and dispatcher, no radio,
// listener, audio device or peer. Model ownership/ACK lifecycle tests are separate.
#include "TestSettingsProfile.h"
#include "TestEventLoop.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
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
#include <vector>

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
class TxOperationIntegrationTestAccess {
public:
    static void identity(RadioConnection& connection)
    {
        connection.m_handle.store(0x12345678);
        connection.m_state.store(ConnectionState::Connected);
    }
};
class DaxTxOwnershipTestAccess {
public:
    static void timeout(RadioModel& model)
    {
        if (model.m_daxTxOwnershipIntent) {
            model.expireDaxTxOwnership(model.m_daxTxOwnershipIntent->ticket);
        }
    }
    static void disconnect(RadioModel& model)
    { model.m_intentionalDisconnect = true; model.onDisconnected(); }
    static void blockAudio(RadioModel& model)
    {
        model.m_daxTxAudioPermit.revoke();
        emit model.daxTxOwnershipChanged();
    }
    static RadioModel::ResponseCallback takeCreateReply(RadioModel& model)
    {
        if (model.m_pendingCallbacks.isEmpty()) { return {}; }
        return model.m_pendingCallbacks.take(model.m_pendingCallbacks.cbegin().key());
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
    QPushButton* toggle = applet.findChild<QPushButton*>(QStringLiteral("daxTxToggle"));
    QLabel* status = applet.findChild<QLabel*>(QStringLiteral("daxTxOwnershipStatus"));
    check(toggle && status && toggle->isCheckable(), "one TX stream toggle exists on every platform");
    if (!toggle || !status) { return; }
    check(!applet.findChild<QPushButton*>(QStringLiteral("daxTxTake"))
        && !applet.findChild<QPushButton*>(QStringLiteral("daxTxRelease")),
        "the two old ownership actions are replaced");
    check(toggle->isVisible() && !toggle->isEnabled() && !toggle->isChecked()
        && !toggle->accessibleDescription().isEmpty(),
        "unbound selection stays visible, unavailable and explained");
    check(!toggle->property("aetherTxKeying").toBool(), "stream selection carries no keying authority");
#ifdef Q_OS_WIN
    check(applet.findChild<QLabel*>(QStringLiteral("daxWindowsNote")) != nullptr,
        "Windows retains its external audio guidance");
#endif
    auto first = std::make_unique<RadioModel>();
    UnsupportedBackend* firstSource = installUnsupported(*first);
    applet.setRadioModel(first.get());
    const auto registryCount = [&applet] {
        return applet.findChildren<ControlAvailabilityRegistry*>().size();
    };
    const qsizetype boundRegistries = registryCount();
    check(boundRegistries > 0 && !toggle->isEnabled()
        && toggle->toolTip() == first->daxTxOwnershipUnavailableReason(true)
        && toggle->accessibleDescription() == toggle->toolTip(),
        "unsupported selection uses the model's reason in both accessible channels");
    applet.setRadioModel(first.get());
    check(registryCount() == boundRegistries && firstSource->ownershipCalls == 0,
        "rebinding replaces registrations without requesting selection");
    auto second = std::make_unique<RadioModel>();
    UnsupportedBackend* secondSource = installUnsupported(*second);
    applet.setRadioModel(second.get());
    const QString currentStatus = status->text();
    emit first->capabilitiesChanged(true, firstSource->capabilities());
    emit first->daxTxOwnershipChanged();
    first.reset();
    check(registryCount() == boundRegistries && status->text() == currentStatus && !toggle->isEnabled(),
        "old model events and destruction cannot change the new model's UI");
    check(secondSource->ownershipCalls == 0 && secondSource->keyingCalls == 0,
        "binding and incoming state dispatch neither selection nor keying");
    second.reset();
    check(!toggle->isEnabled() && toggle->text() == QLatin1String("Unavailable")
        && !toggle->accessibleDescription().isEmpty(), "model destruction makes selection unavailable");
    applet.setRadioModel(nullptr);
    check(registryCount() == 0, "clearing a model retires all model-scoped registries");
}

struct SelectionWrite {
    QString command;
    FlexBackend::CommandReply reply;
};
struct SelectionFixture {
    std::vector<SelectionWrite> writes;
    RadioModel model;
    DaxApplet applet;
    QPushButton* toggle{nullptr};
    QPushButton* bridge{nullptr};
    QLabel* status{nullptr};
    SelectionFixture()
    {
        model.rebuildBackendForTest(QStringLiteral("flex"));
        auto* backend = static_cast<FlexBackend*>(model.backend());
        RadioConnection* connection = backend->connection();
        QMetaObject::invokeMethod(connection, [connection] {
            TxOperationIntegrationTestAccess::identity(*connection);
        }, Qt::BlockingQueuedConnection);
        backend->setDaxTxCommandSink([this](const QString& command, FlexBackend::CommandReply reply,
                                           const DaxTxRequestGuard&) {
            writes.push_back({command, std::move(reply)});
            return true;
        });
        model.handleSliceStatusForTest(0, {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
            {QStringLiteral("tx"), QStringLiteral("1")}, {QStringLiteral("mode"), QStringLiteral("DIGU")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.074000")}});
        applet.setRadioModel(&model);
        applet.show();
        toggle = applet.findChild<QPushButton*>(QStringLiteral("daxTxToggle"));
        bridge = applet.findChild<QPushButton*>(QStringLiteral("daxEnable"));
        status = applet.findChild<QLabel*>(QStringLiteral("daxTxOwnershipStatus"));
    }
    void stream(bool selected, quint32 id = 0x4a000001, const QString& owner = QStringLiteral("0x12345678"))
    {
        model.handleStatusForTest(QStringLiteral("stream 0x%1").arg(id, 0, 16), {
            {QStringLiteral("type"), QStringLiteral("dax_tx")},
            {QStringLiteral("client_handle"), owner},
            {QStringLiteral("tx"), selected ? QStringLiteral("1") : QStringLiteral("0")}});
    }
    void reply(size_t index, int code = 0)
    {
        const FlexBackend::CommandReply callback = writes.at(index).reply;
        callback(code, code ? QStringLiteral("refused") : QString());
    }
};

void selectionOrdering(bool statusFirst)
{
    SelectionFixture f;
    int bridgeChanges = 0;
    QObject::connect(&f.applet, &DaxApplet::daxToggled, &f.applet, [&bridgeChanges](bool) { ++bridgeChanges; });
    const QVariant saved = AppSettings::instance().value("AutoStartDAX", "False");
    f.stream(false);
    f.applet.setDaxEnabled(true);
    check(f.toggle->isEnabled() && !f.toggle->isChecked() && f.toggle->text() == QLatin1String("Not selected"),
        "receive bridge and deselected TX stream can coexist");
    AutomationServer automation;
    automation.setRadioModel(&f.model);
    automation.setTxAllowed(false);
    const QJsonObject invoked = AutomationServerTestAccess::request(automation, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("invoke")},
        {QStringLiteral("target"), QStringLiteral("daxTxToggle")},
        {QStringLiteral("action"), statusFirst ? QStringLiteral("toggle") : QStringLiteral("click")}});
    check(invoked.value("ok").toBool(), "production automation click/toggle can request selection with TX automation off");
    check(AetherTest::waitFor([&f] { return !f.writes.empty() && !f.toggle->isEnabled(); }, 2000),
        "deferred automation selection reaches its pending state before the deadline");
    check(f.writes.size() == 1 && f.writes.front().command == QLatin1String("stream set 0x4a000001 tx=1")
        && !f.toggle->isChecked() && !f.toggle->isEnabled() && f.toggle->text() == QStringLiteral("Selecting…"),
        "one select click requests only our stream and never optimistically checks the toggle");
    f.toggle->click();
    check(f.writes.size() == 1, "clicks while pending cannot dispatch another selection");
    if (statusFirst) { f.stream(true); } else { f.reply(0); }
    check(!f.toggle->isEnabled() && f.toggle->text() == QStringLiteral("Selecting…"),
        "one half of reply/status cannot finish the selection UI");
    if (statusFirst) { f.reply(0); } else { f.stream(true); }
    check(f.toggle->isEnabled() && f.toggle->isChecked() && f.toggle->text() == QLatin1String("Selected"),
        "both reply/status orderings confirm selection");
    f.applet.setDaxEnabled(false);
    check(f.toggle->isChecked() && f.toggle->isEnabled(), "TX selection remains usable with the local bridge disabled");
    DaxTxOwnershipTestAccess::blockAudio(f.model);
    check(f.toggle->isChecked() && f.status->text().contains(QStringLiteral("Audio blocked")),
        "selected stream and blocked audio remain distinct");
    f.stream(true);
    if (statusFirst) {
        const QJsonObject set = AutomationServerTestAccess::request(automation, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("invoke")},
            {QStringLiteral("target"), QStringLiteral("daxTxToggle")},
            {QStringLiteral("action"), QStringLiteral("setChecked")},
            {QStringLiteral("value"), QStringLiteral("false")}});
        check(set.value("ok").toBool(), "automation setChecked drives deselection through the same handler");
    } else {
        f.toggle->click();
    }
    check(AetherTest::waitFor([&f] { return f.writes.size() >= 2 && !f.toggle->isEnabled(); }, 2000),
        "deferred automation deselection reaches its pending state before the deadline");
    check(f.writes.size() == 2 && f.writes.back().command == QLatin1String("stream set 0x4a000001 tx=0")
        && f.toggle->text() == QStringLiteral("Deselecting…") && !f.toggle->isEnabled(),
        "deselect dispatches only one release without stopping the bridge");
    if (statusFirst) { f.stream(false); } else { f.reply(1); }
    check(!f.toggle->isEnabled() && f.toggle->text() == QStringLiteral("Deselecting…"),
        "deselection also waits for both reply and status");
    if (statusFirst) { f.reply(1); } else { f.stream(false); }
    check(f.toggle->isEnabled() && !f.toggle->isChecked() && f.toggle->text() == QLatin1String("Not selected"),
        "deselection confirmation clears the toggle");
    check(f.writes.size() == 2 && bridgeChanges == 0
        && AppSettings::instance().value("AutoStartDAX", "False") == saved && !f.model.isRadioTransmitting(),
        "selection and status updates neither change bridge/autostart nor key the transmitter");
}

void selectionFailuresAndCreation()
{
    SelectionFixture f;
    f.stream(true);
    f.toggle->click();
    DaxTxOwnershipTestAccess::timeout(f.model);
    const QJsonObject stale = f.model.daxTxOwnershipSnapshot();
    check(stale.value("owned").toBool() && stale.value("uncertain").toBool()
        && !stale.value("ready").toBool() && f.toggle->text() == QLatin1String("Unknown")
        && !f.toggle->isChecked() && !f.toggle->isEnabled()
        && f.status->text().contains(QStringLiteral("timed out")),
        "release timeout overrides stale owned=true with Unknown and an accessible failure");
    f.reply(0);
    check(f.toggle->text() == QLatin1String("Unknown") && f.writes.size() == 1,
        "a late reply cannot confirm or retry an uncertain outcome");
    f.stream(false);
    check(f.toggle->isEnabled() && f.toggle->text() == QLatin1String("Not selected"),
        "fresh radio status restores a usable toggle");
    f.toggle->click();
    f.reply(1, 1);
    check(!f.toggle->isChecked() && f.toggle->isEnabled()
        && f.status->text().contains(QStringLiteral("Request refused")),
        "a refused selection retains the confirmed state and displays its reason");
    f.stream(true, 0x4a000002, QStringLiteral("0x87654321"));
    check(!f.toggle->isChecked() && f.writes.size() == 2, "foreign stream status cannot select this client's toggle");
    f.model.handleStatusForTest(QStringLiteral("stream 0x4a000001"), {{QStringLiteral("tx"), QStringLiteral("2")}});
    check(f.toggle->text() == QLatin1String("Unknown") && !f.toggle->isEnabled(),
        "malformed selection status cannot be presented as Not selected");

    SelectionFixture create;
    check(create.toggle->isEnabled() && create.toggle->text() == QLatin1String("Not selected"),
        "a missing stream can be selected explicitly");
    create.toggle->click();
    check(create.toggle->text() == QStringLiteral("Selecting…") && !create.toggle->isEnabled(),
        "stream creation uses the pending selection presentation");
    const auto callback = DaxTxOwnershipTestAccess::takeCreateReply(create.model);
    check(static_cast<bool>(callback), "creation uses the production model create path");
    if (callback) {
        create.stream(false);
        callback(0, QStringLiteral("0x4a000001"));
        check(create.writes.size() == 1, "created stream enters the same guarded selection path");
        if (!create.writes.empty()) { create.reply(0); }
        create.stream(true);
        check(create.toggle->isChecked() && create.toggle->text() == QLatin1String("Selected"),
            "explicit creation and selection converge without persisting intent");
    }
    DaxTxOwnershipTestAccess::disconnect(create.model);
}

void bridgeSettingBehavior()
{
    auto& settings = AppSettings::instance();
    settings.setValue("AutoStartDAX", "True");
    settings.save();
    DaxApplet applet;
    QPushButton* bridge = applet.findChild<QPushButton*>(QStringLiteral("daxEnable"));
    check(bridge != nullptr, "Audio bridge row exists on all platforms");
    if (!bridge) { return; }
    int changes = 0;
    QObject::connect(&applet, &DaxApplet::daxToggled, &applet, [&changes](bool) { ++changes; });
#if defined(Q_OS_MACOS) || defined(HAVE_PIPEWIRE)
    check(bridge->isEnabled() && bridge->isChecked() && bridge->text() == QLatin1String("Enabled"),
        "supported bridge retains the saved autostart behavior");
    applet.setDaxEnabled(false);
    check(changes == 0 && settings.value("AutoStartDAX").toString() == QLatin1String("True"),
        "runtime bridge synchronization never changes autostart");
    bridge->click();
    bridge->click();
    check(changes == 2 && settings.value("AutoStartDAX").toString() == QLatin1String("False"),
        "bridge user clicks still notify routing and save autostart");
#else
    applet.setDaxEnabled(true);
    bridge->click();
    check(!bridge->isEnabled() && !bridge->isChecked() && bridge->text() == QLatin1String("Unavailable")
        && !bridge->accessibleDescription().isEmpty() && changes == 0
        && settings.value("AutoStartDAX").toString() == QLatin1String("True"),
        "unavailable bridge ignores stored autostart, synchronization and clicks without writing settings");
#endif
    settings.load();
#if defined(Q_OS_MACOS) || defined(HAVE_PIPEWIRE)
    check(settings.value("AutoStartDAX").toString() == QLatin1String("False"),
        "bridge autostart changes survive a settings reload");
#else
    check(settings.value("AutoStartDAX").toString() == QLatin1String("True"),
        "unavailable bridge preserves the stored preference across reload");
#endif
    settings.setValue("AutoStartDAX", "False");
    settings.save();
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
        QStringLiteral("clientHandle"), QStringLiteral("txKnown"), QStringLiteral("uncertain"), QStringLiteral("owned"),
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
    AppSettings::instance().load();
    appletLifetimeAndReasons();
    bridgeSettingBehavior();
    selectionOrdering(false);
    selectionOrdering(true);
    selectionFailuresAndCreation();
    bridgeSnapshotAndBoundaries();
    std::printf("dax_tx_applet_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
