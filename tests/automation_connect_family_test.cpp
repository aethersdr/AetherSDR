// `connect ip` family resolution, and the family field that makes it
// checkable (#4912).
//
// The bug this pins: `connect ip <addr>` with no family argument probed with
// whatever the connect dialog's radio-type selector held — Flex on a fresh
// instance — so a Hermes-Lite 2 at a known address was probed on the Flex
// plane. The reply still said {ok:true, deferred:true}; the failure existed
// only as a qCWarning no client can read. Twenty-five minutes of a hardware
// session went into that gap.
//
// Discovery already knew the answer. RadioInfo has carried `family` since the
// aetherd Gap B backend split, and `connect list` walks that same list — it
// just never serialised the field, so a caller could not even work around the
// bug by reading the family and passing it explicitly.
//
// The tests drive AutomationServer::doConnect() directly against a fake
// IConnectionAutomation holding a fixed discovery list. No socket, no radio:
// the whole behaviour under test is "which family does the verb hand to the
// connection panel, and what does it tell the caller it did".

#include "TestSettingsProfile.h"
// AutomationServer holds QPointers to both; the complete types are needed for
// its members to instantiate, exactly as in the sibling automation tests.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/IConnectionAutomation.h"
#include "core/RadioDiscovery.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <functional>
#include <memory>

using namespace AetherSDR;

namespace AetherSDR {

// doConnect() is private; the header already befriends this name for the
// other automation tests.
class AutomationServerTestAccess
{
public:
    static QJsonObject disconnect(AutomationServer& server) { return server.doDisconnect(); }
    static QJsonObject connect(AutomationServer& server,
                               const QString& action,
                               const QString& arg)
    {
        return server.doConnect(action, arg, nullptr);
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

constexpr const char* kHl2Address = "192.0.2.184";
constexpr const char* kFlexAddress = "192.0.2.253";
constexpr const char* kUnknownAddress = "192.0.2.99";

RadioInfo makeRadio(const char* address, const QString& family, const QString& model)
{
    RadioInfo info;
    info.family = family;
    info.model = model;
    info.name = model;
    info.serial = QStringLiteral("SN-") + family;
    info.address = QHostAddress(QLatin1String(address));
    info.port = family == QLatin1String("hl2") ? 1024 : 4992;
    info.status = QStringLiteral("Available");
    return info;
}

// Records what the connect verbs actually asked for. Everything else is the
// minimum the interface demands.
class FakeConnectionAutomation : public QObject, public IConnectionAutomation
{
public:
    QList<RadioInfo> automationLocalRadios() const override { return m_radios; }

    bool automationConnectLocalSerial(const QString&, QString* = nullptr) override
    {
        return true;
    }

    bool automationConnectByIp(const QString& hostOrIp,
                               const QString& family = QString(),
                               QString* = nullptr) override
    {
        ++epoch;
        ++byIpCalls;
        lastTarget = hostOrIp;
        lastFamily = family;
        return true;
    }

    bool automationDisconnect(QString* = nullptr) override { ++disconnectCalls; ++epoch; return true; }
    quint64 automationConnectionIntentEpoch() const override { return epoch; }
    bool automationUsesDeferredNetworkLogin(const QString& = {}) const override { return deferredNetworkLogin; }
    void automationReserveIpConnect(const QString&) override {
        const std::function<void()> callback = onReserve;
        if (callback) { callback(); }
    }
    quint64 automationNativeConnectionEpoch() const override { return nativeEpoch; }
    bool automationDialogVisible() const override { return false; }
    void automationSetDialogVisible(bool) override {}
    QObject* asQObject() override { return this; }

    void setRadios(QList<RadioInfo> radios) { m_radios = std::move(radios); }

    bool deferredNetworkLogin = false;
    std::function<void()> onReserve;
    int disconnectCalls = 0;
    quint64 epoch = 0;
    quint64 nativeEpoch = 0;
    int byIpCalls = 0;
    QString lastTarget;
    QString lastFamily;

private:
    QList<RadioInfo> m_radios;
};

// The connect is scheduled with QTimer::singleShot(0, qApp, …), so the fake
// only sees it after the event loop turns once.
void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-connect-family-test"));
    check(settingsProfile.isValid(), "isolated settings profile is available");

    QCoreApplication app(argc, argv);

    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);

    FakeConnectionAutomation conn;
    conn.setRadios({
        makeRadio(kFlexAddress, QStringLiteral("flex"), QStringLiteral("FLEX-6700")),
        makeRadio(kHl2Address, QStringLiteral("hl2"), QStringLiteral("Hermes-Lite 2")),
    });
    server.setConnectionAutomation(&conn);

    // ── `connect list` publishes the discriminator ──────────────────────────
    {
        const QJsonObject reply =
            AutomationServerTestAccess::connect(server, QStringLiteral("list"),
                                                QString());
        check(reply.value(QStringLiteral("ok")).toBool(), "connect list succeeds");
        const QJsonArray radios = reply.value(QStringLiteral("radios")).toArray();
        check(radios.size() == 2, "connect list returns both discovered radios");

        QString flexFamily;
        QString hl2Family;
        for (const QJsonValue& entry : radios) {
            const QJsonObject o = entry.toObject();
            const QString address = o.value(QStringLiteral("address")).toString();
            if (address == QLatin1String(kFlexAddress)) {
                flexFamily = o.value(QStringLiteral("family")).toString();
            } else if (address == QLatin1String(kHl2Address)) {
                hl2Family = o.value(QStringLiteral("family")).toString();
            }
        }
        check(flexFamily == QLatin1String("flex"),
              "connect list reports family=flex for the Flex");
        check(hl2Family == QLatin1String("hl2"),
              "connect list reports family=hl2 for the Hermes-Lite 2");
    }

    // ── Omitted family resolves from discovery — the actual bug ─────────────
    {
        conn.byIpCalls = 0;
        conn.lastFamily = QStringLiteral("<unset>");
        const QJsonObject reply =
            AutomationServerTestAccess::connect(server, QStringLiteral("ip"),
                                                QLatin1String(kHl2Address));
        settle();
        check(reply.value(QStringLiteral("ok")).toBool(), "connect ip <hl2> succeeds");
        check(reply.value(QStringLiteral("family")).toString() == QLatin1String("hl2"),
              "reply reports the resolved family hl2, not 'dialog'");
        check(reply.value(QStringLiteral("familySource")).toString()
                  == QLatin1String("discovery"),
              "reply reports familySource=discovery");
        check(conn.byIpCalls == 1, "one connect-by-ip was scheduled");
        check(conn.lastFamily == QLatin1String("hl2"),
              "the connection panel is handed family=hl2 (pre-fix: empty → dialog selector)");
    }

    // ── Explicit family still wins, and is labelled as such ─────────────────
    {
        conn.byIpCalls = 0;
        conn.lastFamily = QStringLiteral("<unset>");
        const QJsonObject reply =
            AutomationServerTestAccess::connect(
                server, QStringLiteral("ip"),
                QString(QLatin1String(kFlexAddress)) + QStringLiteral(" flex"));
        settle();
        check(reply.value(QStringLiteral("familySource")).toString()
                  == QLatin1String("argument"),
              "an explicit family reports familySource=argument");
        check(conn.lastFamily == QLatin1String("flex"),
              "the explicit family reaches the connection panel");
    }

    // ── Explicit family OVERRIDES discovery, and the disagreement is data ───
    //
    // The argument is the caller saying "I know what is at this address", so it
    // has to win — otherwise the explicit form is weaker than the inferred one and
    // the workaround for a wrong discovery entry disappears exactly when it is
    // needed. An earlier revision refused this case; #4918's review caught it.
    {
        conn.byIpCalls = 0;
        conn.lastFamily = QStringLiteral("<unset>");
        const QJsonObject reply =
            AutomationServerTestAccess::connect(
                server, QStringLiteral("ip"),
                QString(QLatin1String(kHl2Address)) + QStringLiteral(" flex"));
        settle();
        check(reply.value(QStringLiteral("ok")).toBool(),
              "connect ip <hl2 address> flex is accepted, not refused");
        check(reply.value(QStringLiteral("family")).toString() == QLatin1String("flex"),
              "the caller's family wins over discovery");
        check(reply.value(QStringLiteral("familySource")).toString()
                  == QLatin1String("argument"),
              "and is labelled as coming from the argument");
        check(reply.value(QStringLiteral("discoveryFamily")).toString()
                  == QLatin1String("hl2"),
              "the disagreement is returned as data, so a strict caller can compare");
        check(conn.lastFamily == QLatin1String("flex"),
              "the overriding family is what reaches the connection panel");
    }

    // ── No disagreement to report when discovery agrees ─────────────────────
    {
        const QJsonObject reply =
            AutomationServerTestAccess::connect(
                server, QStringLiteral("ip"),
                QString(QLatin1String(kHl2Address)) + QStringLiteral(" hl2"));
        settle();
        check(reply.value(QStringLiteral("discoveryFamily")).toString()
                  == QLatin1String("hl2"),
              "discoveryFamily still reports what discovery believed");
        check(reply.value(QStringLiteral("familySource")).toString()
                  == QLatin1String("argument"),
              "an agreeing argument is still an argument");
    }

    // ── An undiscovered address still falls back to the dialog selector ─────
    {
        conn.byIpCalls = 0;
        conn.lastFamily = QStringLiteral("<unset>");
        const QJsonObject reply =
            AutomationServerTestAccess::connect(server, QStringLiteral("ip"),
                                                QLatin1String(kUnknownAddress));
        settle();
        check(reply.value(QStringLiteral("ok")).toBool(),
              "connect ip to an undiscovered address is still accepted");
        check(reply.value(QStringLiteral("family")).toString()
                  == QLatin1String("dialog"),
              "an undiscovered address reports family=dialog");
        check(reply.value(QStringLiteral("familySource")).toString()
                  == QLatin1String("dialog"),
              "an undiscovered address reports familySource=dialog");
        check(conn.lastFamily.isEmpty(),
              "the connection panel is handed an empty family, i.e. the selector");
    }

    {
        const int before = conn.byIpCalls;
        const QJsonObject idle = AutomationServerTestAccess::disconnect(server);
        check(!idle.value("ok").toBool() && idle.value("error").toString() == QStringLiteral("not connected to a radio"),
              "existing connection flows retain their disconnected error");
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.98 flex"));
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        ++conn.epoch;
        settle();
        check(conn.byIpCalls == before + 2,
              "existing queued connect intents preserve baseline dispatch rather than gaining deferred-login ordering");
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        ++conn.nativeEpoch;
        settle();
        check(conn.byIpCalls == before + 2,
              "entering a native authenticated session retires an older legacy queued connect without changing ordinary legacy ordering");
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        FakeConnectionAutomation nativeReplacement;
        nativeReplacement.deferredNetworkLogin = true;
        server.setConnectionAutomation(&nativeReplacement);
        settle();
        check(conn.byIpCalls == before + 2 && nativeReplacement.byIpCalls == 0,
              "a replacement native login panel cannot be displaced by an older legacy queued connect");
        server.setConnectionAutomation(&conn);
    }
    conn.deferredNetworkLogin = true;
    {
        const int connectsBefore = conn.byIpCalls;
        const int disconnectsBefore = conn.disconnectCalls;
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        AutomationServerTestAccess::disconnect(server);
        settle();
        check(conn.byIpCalls == connectsBefore && conn.disconnectCalls == disconnectsBefore + 1,
              "newer queued Disconnect retires older queued Connect");
        AutomationServerTestAccess::disconnect(server);
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        settle();
        check(conn.byIpCalls == connectsBefore + 1 && conn.disconnectCalls == disconnectsBefore + 1,
              "newer queued Connect retires older queued Disconnect");
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        AutomationServerTestAccess::connect(server, QStringLiteral("list"), QString());
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral(""));
        settle();
        check(conn.byIpCalls == connectsBefore + 2,
              "read-only and invalid requests do not retire accepted queued intent");
        AutomationServerTestAccess::connect(server, QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        ++conn.epoch;
        settle();
        check(conn.byIpCalls == connectsBefore + 2,
              "newer direct operator intent retires queued bridge Connect");
    }

    // A disconnect owns the explicit intent it observed, even between retries.
    {
        const int before = conn.disconnectCalls;
        const QJsonObject reply = AutomationServerTestAccess::disconnect(server);
        check(reply.value("ok").toBool() && reply.value("deferred").toBool(),
              "disconnected model accepts deferred cancellation");
        check(conn.disconnectCalls == before, "disconnect remains deferred");
        settle();
        check(conn.disconnectCalls == before + 1, "between-attempt cancellation reaches panel");
        AutomationServerTestAccess::disconnect(server);
        settle();
        check(conn.disconnectCalls == before + 2, "idle cancellation is idempotently dispatched");
        AutomationServerTestAccess::disconnect(server);
        ++conn.epoch; // A newer explicit Connect, unlike an automatic backend retry.
        settle();
        check(conn.disconnectCalls == before + 2, "old cancellation cannot stop newer explicit intent");
        AutomationServerTestAccess::disconnect(server);
        FakeConnectionAutomation replacement;
        server.setConnectionAutomation(&replacement);
        settle();
        check(conn.disconnectCalls == before + 2 && replacement.disconnectCalls == 0,
              "panel replacement retires queued cancellation");
        server.setConnectionAutomation(&conn);
        AutomationServerTestAccess::disconnect(server);
        RadioModel replacementModel;
        server.setRadioModel(&replacementModel);
        settle();
        check(conn.disconnectCalls == before + 2, "model replacement retires queued cancellation");
        server.setRadioModel(&radio);
        auto temporary = std::make_unique<FakeConnectionAutomation>();
        server.setConnectionAutomation(temporary.get());
        AutomationServerTestAccess::disconnect(server);
        temporary.reset();
        settle();
        check(conn.disconnectCalls == before + 2, "destroyed panel retires queued cancellation");
        server.setConnectionAutomation(&conn);
        auto transientModel = std::make_unique<RadioModel>();
        server.setRadioModel(transientModel.get());
        AutomationServerTestAccess::disconnect(server);
        transientModel.reset();
        settle();
        check(conn.disconnectCalls == before + 2, "destroyed model retires queued cancellation");
        server.setRadioModel(&radio);
        auto transientServer = std::make_unique<AutomationServer>();
        transientServer->setConnectionAutomation(&conn);
        transientServer->setRadioModel(&radio);
        AutomationServerTestAccess::disconnect(*transientServer);
        transientServer.reset();
        settle();
        check(conn.disconnectCalls == before + 2, "destroyed bridge retires queued cancellation");
    }

    {
        auto transientServer = std::make_unique<AutomationServer>();
        transientServer->setConnectionAutomation(&conn);
        const int before = conn.byIpCalls;
        conn.onReserve = [&] { transientServer.reset(); };
        const QJsonObject reply = AutomationServerTestAccess::connect(*transientServer,
            QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        conn.onReserve = {};
        settle();
        check(!reply.value("ok").toBool() && conn.byIpCalls == before,
            "synchronous native preparation cannot use a destroyed bridge");
    }
    {
        auto transientPanel = std::make_unique<FakeConnectionAutomation>();
        server.setConnectionAutomation(transientPanel.get());
        transientPanel->onReserve = [&] { transientPanel.reset(); };
        const QJsonObject reply = AutomationServerTestAccess::connect(server,
            QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        settle();
        check(!reply.value("ok").toBool(), "synchronous native preparation cannot use a destroyed panel");
        server.setConnectionAutomation(&conn);
    }
    {
        FakeConnectionAutomation replacement;
        conn.onReserve = [&] { server.setConnectionAutomation(&replacement); };
        const int before = conn.byIpCalls;
        const QJsonObject reply = AutomationServerTestAccess::connect(server,
            QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        conn.onReserve = {};
        settle();
        check(!reply.value("ok").toBool() && conn.byIpCalls == before && replacement.byIpCalls == 0,
            "synchronous native preparation cannot dispatch through a replaced panel");
        server.setConnectionAutomation(&conn);
    }
    {
        const int connectsBefore = conn.byIpCalls;
        const int disconnectsBefore = conn.disconnectCalls;
        conn.onReserve = [&] { AutomationServerTestAccess::disconnect(server); };
        const QJsonObject reply = AutomationServerTestAccess::connect(server,
            QStringLiteral("ip"), QStringLiteral("192.0.2.99 flex"));
        conn.onReserve = {};
        settle();
        check(!reply.value("ok").toBool() && conn.byIpCalls == connectsBefore
            && conn.disconnectCalls == disconnectsBefore + 1,
            "a newer synchronous intent supersedes native preparation on the same owner");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
