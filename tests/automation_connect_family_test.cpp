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

using namespace AetherSDR;

namespace AetherSDR {

// doConnect() is private; the header already befriends this name for the
// other automation tests.
class AutomationServerTestAccess
{
public:
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
        ++byIpCalls;
        lastTarget = hostOrIp;
        lastFamily = family;
        return true;
    }

    bool automationDisconnect(QString* = nullptr) override { return true; }
    bool automationDialogVisible() const override { return false; }
    void automationSetDialogVisible(bool) override {}
    QObject* asQObject() override { return this; }

    void setRadios(QList<RadioInfo> radios) { m_radios = std::move(radios); }

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

    if (failures > 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
