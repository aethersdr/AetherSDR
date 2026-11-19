// The app serving aetherd's local control endpoint (AppControlHost,
// docs/aetherd-app-control-socket.md): over a real local socket, a client tunes
// the app's radio, but can neither connect, disconnect nor key it, and the host
// going away leaves the app's connection alone.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/control/AppControlHost.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QUuid>

#include <cstdio>
#include <functional>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

// Records intents; no socket, discovery or DSP. connectRadio/disconnectRadio
// count calls, which the host must never make.
class Backend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int connects{0};
    int disconnects{0};
    int tunes{0};
    int keys{0};
    double lastHz{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { ++connects; connected = true; }
    void disconnectRadio() override { ++disconnects; connected = false; }
    void setSliceFrequency(int, double hz) override { ++tunes; lastHz = hz; }
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override { ++keys; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// One client session on the endpoint, line-framed JSON as g2paneld speaks it.
class Client {
public:
    explicit Client(const QString& endpoint)
    {
        m_socket.connectToServer(endpoint);
        waitUntil([this] { return m_socket.state() == QLocalSocket::ConnectedState; });
    }
    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    QJsonObject ask(const QString& method, const QJsonObject& params = {})
    {
        const QString id = QStringLiteral("r%1").arg(++m_next);
        QJsonObject request{{"v", 1}, {"id", id}, {"method", method}, {"params", params}};
        if (!m_session.isEmpty()) {
            request.insert("sessionId", m_session);
        }
        m_socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        m_socket.flush();
        QJsonObject reply;
        waitUntil([&] {
            while (m_socket.canReadLine()) {
                const QJsonObject message = QJsonDocument::fromJson(m_socket.readLine()).object();
                if (message.value("id").toString() == id) {
                    reply = message;
                    return true;
                }
            }
            return false;
        });
        if (method == QStringLiteral("hello")) {
            m_session = reply.value("result").toObject().value("sessionId").toString();
        }
        return reply;
    }

private:
    QLocalSocket m_socket;
    QString m_session;
    int m_next{0};
};

QString errorOf(const QJsonObject& reply)
{
    return reply.value("error").toObject().value("code").toString();
}

QJsonObject sliceAddress()
{
    return {{"type", "slice"}, {"radioSession", "radio-1"}, {"id", "0"}};
}

qint64 sliceRevision(Client& client)
{
    return client.ask("resource.get", {{"resource", sliceAddress()}})
        .value("result").toObject().value("revision").toInteger();
}

QJsonObject tune(Client& client, qint64 hz)
{
    return client.ask("slice.setFrequency", {{"radioSession", "radio-1"}, {"slice", "0"},
                                             {"expectedRevision", sliceRevision(client)}, {"hz", hz}});
}

void run()
{
    RadioModel radio;
    auto owned = std::make_unique<Backend>();
    Backend* backend = owned.get();
    backend->caps.canTransmit = false;
    backend->caps.sliceFrequencyControl = {SliceFrequencyControl::Authority::Radio, 100'000, 60'000'000};
    radio.setBackendForTest(std::move(owned), QStringLiteral("app-control-host-test"));
    check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "model slice fixture installed");
    {
        SliceDelta delta;
        delta.frequency = 14.225;
        radio.slice(0)->applyChanges(delta);
    }

    const QString name = QStringLiteral("app-ctl-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    QString error;
    std::unique_ptr<AppControlHost> host = AppControlHost::start(&radio, name, &error);
    check(host != nullptr, "the host serves the named endpoint");
    if (!host) {
        std::fprintf(stderr, "  start failed: %s\n", qPrintable(error));
        return;
    }
    check(!host->endpoint().isEmpty(), "the host reports its endpoint");

    // A second server on the same name, as aetherd still running would be, is refused.
    QString secondError;
    check(AppControlHost::start(&radio, name, &secondError) == nullptr && !secondError.isEmpty(),
          "a second host on a name in use is refused and says why");

    Client client(host->endpoint());
    check(client.connected(), "a current-user client connects");
    const QJsonObject welcome = client.ask("hello", {{"client", QJsonObject{{"name", "test"}, {"version", "1"}}},
                                                     {"versions", QJsonArray{1}}});
    check(welcome.contains("result"), "hello is answered");

    // g2paneld subscribes to the catalogue too; the app has none, which must not fail the subscribe.
    const QJsonObject subscribed = client.ask("resource.subscribe", {{"resources", QJsonArray{
        QJsonObject{{"type", "radioCatalogue"}},
        QJsonObject{{"type", "radioSession"}, {"id", "radio-1"}},
        QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}},
        QJsonObject{{"type", "panadapter"}, {"radioSession", "radio-1"}}}}});
    check(subscribed.contains("result"), "a subscribe naming the absent catalogue still succeeds");

    // Not connected yet: the receive verbs wait for the app's own connection.
    check(errorOf(tune(client, 14'230'000)) == QStringLiteral("request.conflict") && backend->tunes == 0,
          "tuning before the app is connected is refused as not ready");

    // The app connects through its own path; the host only observes it.
    backend->connected = true;
    emit radio.connectionStateChanged(true);
    const QJsonObject accepted = tune(client, 14'230'000);
    check(accepted.contains("result") && backend->tunes == 1 && qAbs(backend->lastHz - 14'230'000.0) < 0.5,
          "with the app connected, a client tunes the app's slice");

    // Connection and transmit belong to the app, not to its clients.
    // What a session is offered comes in its welcome; a second client sees the connected state.
    Client second(host->endpoint());
    const QJsonArray offered = second.ask("hello", {{"versions", QJsonArray{1}}}).value("result").toObject()
        .value("capabilities").toArray();
    check(offered.contains(QStringLiteral("slice.setFrequency")), "frequency control is offered");
    check(!offered.contains(QStringLiteral("radio.connect")) && !offered.contains(QStringLiteral("radio.disconnect")),
          "connect and disconnect are not offered");
    for (const QJsonValue& method : offered) {
        check(!method.toString().startsWith(QStringLiteral("transmit.")), "no transmit verb is offered");
    }
    check(!errorOf(client.ask("radio.disconnect", {{"radioSession", "radio-1"}})).isEmpty()
              && backend->connected && backend->disconnects == 0,
          "radio.disconnect is refused and the app's radio stays connected");
    check(!errorOf(client.ask("radio.connect", {{"radioSession", "radio-1"}, {"radioId", "x"},
                                                {"catalogueRevision", 1}})).isEmpty()
              && backend->connects == 0,
          "radio.connect is refused");

    // The app loses its radio: the verbs stop, and nothing reconnects it behind the app.
    // The client still holds the last revision it saw, as a real client would.
    const qint64 lastSeen = sliceRevision(client);
    backend->connected = false;
    emit radio.connectionStateChanged(false);
    const QJsonObject afterDrop = client.ask("slice.setFrequency", {{"radioSession", "radio-1"}, {"slice", "0"},
                                                                  {"expectedRevision", lastSeen}, {"hz", 14'240'000}});
    check(!errorOf(afterDrop).isEmpty() && backend->tunes == 1
              && backend->connects == 0,
          "after the app disconnects, tuning is refused and no reconnect is made");

    // The host going away leaves the app's connection as it is.
    backend->connected = true;
    emit radio.connectionStateChanged(true);
    host.reset();
    check(backend->connected && backend->disconnects == 0 && backend->keys == 0,
          "destroying the host neither disconnects nor keys the radio");
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("app_control_host_test"));
    QCoreApplication app(argc, argv);
    run();
    if (failures == 0) {
        std::printf("app_control_host_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
