// The opt-in AetherSDR TCI extension set: registration of command groups,
// the enable and its capability list, the per-client gate, and its end on
// disable and disconnect. Socket-free: the transport is injected.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <memory>

namespace AetherSDR {

class TciExtensionSetTest {
public:
    static void attach(TciServer& server, TciClient* client, RadioModel& model)
    {
        TciServer::ClientState state;
        state.socket = client;
        state.protocol = new TciProtocol(&model, &server.m_routingState, &server.m_trxMap);
        server.m_clients.append(state);
        QObject::connect(client, &TciClient::textMessageReceived,
                         &server, &TciServer::onTextMessage);
        QObject::connect(client, &TciClient::disconnected,
                         &server, &TciServer::onClientDisconnected);
    }
    static bool enabled(TciServer& server, TciClient* client)
    {
        const TciServer::ClientState* state = server.clientStateFor(client);
        return state && state->extensionsEnabled;
    }
    static bool attached(TciServer& server, TciClient* client)
    {
        return server.clientStateFor(client) != nullptr;
    }
    static QStringList offeredTokens(TciServer& server)
    {
        return server.m_extensions.offeredTokens();
    }
    static void broadcastExtension(TciServer& server, const QString& msg)
    {
        server.broadcastExtension(msg);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// A stand-in command group with a gate the test can flip.
struct Probe {
    bool offered{true};
    int handled{0};
    QStringList lastArgs;

    TciExtensionSet::Group group(const QString& token = QStringLiteral("probe"),
                                 const QString& name = QStringLiteral("aethersdr_probe"))
    {
        TciExtensionSet::Group g;
        g.token = token;
        g.commands = {name};
        g.offered = [this] { return offered; };
        g.stateLines = [] { return QStringList{QStringLiteral("aethersdr_probe:7;")}; };
        g.handle = [this](const QString&, const QStringList& args) {
            ++handled;
            lastArgs = args;
            TciExtensionSet::Result result;
            if (args.isEmpty()) {
                result.reply = QStringLiteral("aethersdr_probe:7;");
            } else {
                result.broadcast = QStringLiteral("aethersdr_probe:%1;").arg(args.first());
            }
            return result;
        };
        return g;
    }
};

struct Wire {
    QPointer<TciClient> client{new TciClient};
    QStringList rx;
    Wire()
    {
        client->textSink = [this](const QString& text) {
            rx << text;
            return static_cast<qint64>(text.size());
        };
    }
    ~Wire() { delete client.data(); }
    void send(const char* text) { emit client->textMessageReceived(QString::fromLatin1(text)); }
    QStringList take()
    {
        QStringList out = rx;
        rx.clear();
        return out;
    }
};

void registrationRules()
{
    Probe probe;
    TciExtensionSet set;
    check(set.advertisement() == QStringLiteral("aethersdr_ext:1;"),
          "an empty set advertises version 1 and no items");

    TciExtensionSet::Group noGate = probe.group();
    noGate.offered = nullptr;
    TciExtensionSet::Group noHandler = probe.group();
    noHandler.handle = nullptr;
    check(!set.registerGroup(noGate) && !set.registerGroup(noHandler),
          "a group without a gate or a handler is refused");
    check(!set.registerGroup(probe.group(QString()))
              && !set.registerGroup(probe.group(QStringLiteral("a,b")))
              && !set.registerGroup(probe.group(QStringLiteral("Probe"))),
          "an empty, reserved-character or upper-case token is refused");
    check(!set.registerGroup(probe.group(QStringLiteral("p"), QStringLiteral("probe")))
              && !set.registerGroup(probe.group(QStringLiteral("p"),
                                                QStringLiteral("aethersdr_ext_enable")))
              && !set.registerGroup(probe.group(QStringLiteral("p"),
                                                QStringLiteral("aethersdr_ext"))),
          "a name without the prefix, or the mechanism's own names, is refused");
    check(set.registerGroup(probe.group()), "a well-formed group registers");
    check(!set.registerGroup(probe.group(QStringLiteral("probe"), QStringLiteral("aethersdr_other")))
              && !set.registerGroup(probe.group(QStringLiteral("other"))),
          "a token or a command another group holds is refused");

    check(set.advertisement() == QStringLiteral("aethersdr_ext:1,probe;")
              && set.offeredGroupFor(QStringLiteral("aethersdr_probe")) != nullptr,
          "an offered group is listed and answers its command");
    probe.offered = false;
    check(set.advertisement() == QStringLiteral("aethersdr_ext:1;")
              && set.offeredGroupFor(QStringLiteral("aethersdr_probe")) == nullptr
              && set.stateLines().isEmpty(),
          "a group whose gate fails is neither listed, answered nor reported");

    check(TciExtensionSet::parseBool(QStringLiteral("true")) == true
              && TciExtensionSet::parseBool(QStringLiteral("1")) == true
              && TciExtensionSet::parseBool(QStringLiteral("FALSE")) == false
              && TciExtensionSet::parseBool(QStringLiteral("0")) == false
              && !TciExtensionSet::parseBool(QStringLiteral("yes"))
              && !TciExtensionSet::parseBool(QString()),
          "the enable takes true/1 and false/0, nothing else");
}

void productionSetIsEmpty()
{
    RadioModel model;
    TciServer server(&model);
    Wire a;
    TciExtensionSetTest::attach(server, a.client, model);
    check(TciExtensionSetTest::offeredTokens(server).isEmpty(),
          "no command group is offered yet");
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == QStringList{QStringLiteral("aethersdr_ext:1;")},
          "enabling answers aethersdr_ext:1; and nothing else");
    check(TciExtensionSetTest::enabled(server, a.client), "the enable is recorded");
}

void enableAndGate()
{
    RadioModel model;
    TciServer server(&model);
    Probe probe;
    check(server.registerExtensionGroup(probe.group()), "the server takes a group");
    Wire a;
    Wire b;
    TciExtensionSetTest::attach(server, a.client, model);
    TciExtensionSetTest::attach(server, b.client, model);

    // (3) Before the enable, an extension command is ignored without a reply.
    a.send("aethersdr_probe;aethersdr_probe:5;aethersdr_unknown:1;");
    check(a.take().isEmpty() && probe.handled == 0,
          "a client that has not enabled the set gets no reply and no handler runs");
    a.send("aethersdr_ext_enable;aethersdr_ext_enable:maybe;");
    check(a.take().isEmpty() && !TciExtensionSetTest::enabled(server, a.client),
          "a bare or malformed enable changes nothing and gets no reply");

    // (2) The enable answers the capability list, then each offered item's state.
    a.send("AETHERSDR_EXT_ENABLE:TRUE;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1,probe;"),
                                   QStringLiteral("aethersdr_probe:7;")}),
          "enabling answers the capability list, then the state lines");
    check(b.take().isEmpty() && !TciExtensionSetTest::enabled(server, b.client),
          "another client's enable reaches no one else");

    a.send("aethersdr_probe;");
    check(a.take() == QStringList{QStringLiteral("aethersdr_probe:7;")} && b.take().isEmpty(),
          "a read is answered to the requester only");
    a.send("aethersdr_probe:Ant 1;");
    check(probe.lastArgs == QStringList{QStringLiteral("Ant 1")},
          "arguments reach the handler trimmed, case kept");
    check(a.take() == QStringList{QStringLiteral("aethersdr_probe:Ant 1;")} && b.take().isEmpty(),
          "(4) a set's broadcast reaches enabled clients only");
    const int handledBefore = probe.handled;
    b.send("aethersdr_probe:9;");
    check(b.take().isEmpty() && a.take().isEmpty() && probe.handled == handledBefore,
          "(4) the enabled peer does not open the gate for another client");

    // The list is re-sent when it changes, and only then.
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    check(a.take().isEmpty() && b.take().isEmpty(), "an unchanged list is not re-sent");
    probe.offered = false;
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    check(a.take() == QStringList{QStringLiteral("aethersdr_ext:1;")} && b.take().isEmpty(),
          "a changed list goes to enabled clients only");
    a.send("aethersdr_probe;");
    check(a.take().isEmpty() && probe.handled == handledBefore,
          "a command that is no longer offered is ignored and writes nothing");
    probe.offered = true;
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1,probe;"),
                                   QStringLiteral("aethersdr_probe:7;")}),
          "an item offered again is listed with its state");

    // (5) Disable ends the traffic at once.
    a.send("aethersdr_ext_enable:false;");
    check(a.take().isEmpty() && !TciExtensionSetTest::enabled(server, a.client),
          "disabling gets no reply and clears the state");
    probe.offered = false;
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    a.send("aethersdr_probe;");
    TciExtensionSetTest::broadcastExtension(server, QStringLiteral("aethersdr_probe:3;"));
    check(a.take().isEmpty() && b.take().isEmpty() && probe.handled == handledBefore,
          "after disabling, no list, no reply and no broadcast arrive");
    probe.offered = true;
    a.send("aethersdr_ext_enable:1;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1,probe;"),
                                   QStringLiteral("aethersdr_probe:7;")}),
          "enabling again answers the list again");

    // (5) Disconnect drops the client's state with it.
    emit a.client->disconnected();
    QCoreApplication::processEvents();
    check(!TciExtensionSetTest::attached(server, a.client), "a disconnected client is gone");
    TciExtensionSetTest::broadcastExtension(server, QStringLiteral("aethersdr_probe:4;"));
    check(b.take().isEmpty(), "a broadcast after the disconnect reaches no one");
    Wire c;
    TciExtensionSetTest::attach(server, c.client, model);
    c.send("aethersdr_probe;");
    check(c.take().isEmpty() && !TciExtensionSetTest::enabled(server, c.client),
          "a new client starts with the set disabled");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-extension-set-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    registrationRules();
    productionSetIsEmpty();
    enableAndGate();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
