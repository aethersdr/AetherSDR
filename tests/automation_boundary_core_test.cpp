// Socket-free bridge boundary checks: JSON id rules, the MHz contract, memory
// activation, deferred connect failures, FM repeater and transmit verbs, and
// DEXP omission on the sim backend. Requests go straight to handleLine; no
// listener, socket, network peer or real radio is involved, and TX permission
// is only granted on a model with no transport behind it.
#include "TestSettingsProfile.h"
// AutomationServer's QPointer members require these complete types.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/IConnectionAutomation.h"
#include "core/RadioDiscovery.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace AetherSDR {

class AutomationServerTestAccess
{
public:
    using Wait = AutomationServer::ConnectWait;

    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    {
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact),
                                 nullptr);
    }
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
    static std::shared_ptr<Wait> addWait(AutomationServer& server)
    {
        auto wait = std::make_shared<Wait>();
        server.m_connectWaits.push_back(wait);
        return wait;
    }
    static bool waitPending(const AutomationServer& server, const std::shared_ptr<Wait>& wait)
    {
        return std::find(server.m_connectWaits.begin(), server.m_connectWaits.end(), wait)
            != server.m_connectWaits.end();
    }
    static void dropWaits(AutomationServer& server) { server.m_connectWaits.clear(); }
    static QString lastError(const AutomationServer& server) { return server.m_lastConnectError; }
    static qint64 lastErrorMs(const AutomationServer& server) { return server.m_lastConnectErrorMs; }
    static void noteFailure(AutomationServer& server, const QString& what,
                            const QString& error, bool answerPendingWaits)
    {
        server.noteConnectFailure(what, error, answerPendingWaits);
    }
    // start() is the only production writer and also opens the listener.
    static void setTxMaxPower(AutomationServer& server, int watts)
    {
        server.m_txMaxPower = watts;
    }
};

} // namespace AetherSDR

using namespace AetherSDR;
using Access = AutomationServerTestAccess;

namespace {

int failures = 0;

void check(bool ok, const QString& description, const QJsonObject& reply = {})
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(description));
    if (!ok) {
        ++failures;
        if (!reply.isEmpty()) {
            std::printf("     reply: %s\n",
                        QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
        }
    }
}

bool ok(const QJsonObject& reply) { return reply.value(QStringLiteral("ok")).toBool(); }
QString errorOf(const QJsonObject& reply) { return reply.value(QStringLiteral("error")).toString(); }

template <typename Predicate>
bool pumpUntil(Predicate done, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

QJsonObject tuneWithId(const QJsonValue& id)
{
    QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("tune")},
                        {QStringLiteral("value"), QStringLiteral("14.225")}};
    if (!id.isUndefined()) {
        request.insert(QStringLiteral("id"), id);
    }
    return request;
}

void jsonIdAndMhzContract()
{
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);
    int tuneCalls = 0;
    int tunedSlice = -99;
    server.setTuneHandler([&](double, int sliceId) {
        ++tuneCalls;
        tunedSlice = sliceId;
        return QJsonObject{{QStringLiteral("ok"), true}};
    });
    int targetCalls = 0;
    double targetMhz = 0.0;
    server.setTargetTuneHandler([&](double mhz) {
        ++targetCalls;
        targetMhz = mhz;
        return QJsonObject{{QStringLiteral("ok"), true}};
    });

    const auto accepted = [&](const QJsonValue& id, int slice, const char* what) {
        const int before = tuneCalls;
        const QJsonObject reply = Access::request(server, tuneWithId(id));
        check(ok(reply) && tuneCalls == before + 1 && tunedSlice == slice, what, reply);
    };
    const auto refused = [&](const QJsonValue& id, const QString& error, const char* what) {
        const int before = tuneCalls;
        const QJsonObject reply = Access::request(server, tuneWithId(id));
        check(!ok(reply) && errorOf(reply) == error && tuneCalls == before, what, reply);
    };
    accepted(QJsonValue::Undefined, -1, "omitted id keeps the active-slice sentinel");
    accepted(QStringLiteral("1"), 1, "string id reaches the requested slice");
    accepted(1, 1, "integer JSON id reaches the requested slice");
    const QString typeError = QStringLiteral("id must be a string or number");
    refused(true, typeError, "bool id refused");
    refused(QJsonObject{{QStringLiteral("slice"), 1}}, typeError, "object id refused");
    refused(QJsonArray{1}, typeError, "array id refused");
    refused(QJsonValue::Null, typeError, "null id refused");
    const QString intError = QStringLiteral("tune: sliceId must be a non-negative integer");
    refused(1.5, intError, "fractional id refused");
    refused(1.0000001, intError, "near-integer id is not rounded down to slice 1");
    refused(0.9999999, intError, "near-integer id is not rounded up to slice 1");

    struct Verb { QString name; int* calls; };
    const Verb verbs[] = {{QStringLiteral("tune"), &tuneCalls},
                          {QStringLiteral("targettune"), &targetCalls},
                          {QStringLiteral("pan center"), nullptr}};
    for (const Verb& verb : verbs) {
        const auto send = [&](const QString& value) {
            if (verb.name == QLatin1String("pan center")) {
                return Access::request(server, QJsonObject{
                    {QStringLiteral("cmd"), QStringLiteral("pan")},
                    {QStringLiteral("action"), QStringLiteral("center")},
                    {QStringLiteral("value"), value}});
            }
            return Access::request(server, QJsonObject{
                {QStringLiteral("cmd"), verb.name}, {QStringLiteral("value"), value}});
        };
        const auto refusedWith = [&](const QString& value, auto&& matches, const QString& what) {
            const int before = verb.calls ? *verb.calls : 0;
            const QJsonObject reply = send(value);
            check(!ok(reply) && matches(errorOf(reply))
                      && (!verb.calls || *verb.calls == before),
                  verb.name + QStringLiteral(": ") + what, reply);
        };
        const auto acceptedValue = [&](const QString& value, const QString& what) {
            const int before = verb.calls ? *verb.calls : 0;
            const QJsonObject reply = send(value);
            check(ok(reply) && (!verb.calls || *verb.calls == before + 1),
                  verb.name + QStringLiteral(": ") + what, reply);
        };
        const QString hzPrefix = verb.name + QStringLiteral(" takes MHz, not Hz");
        const auto hz = [&](const QString& e) { return e.startsWith(hzPrefix); };
        refusedWith(QStringLiteral("14200000"), hz, QStringLiteral("20 m in Hz refused"));
        refusedWith(QStringLiteral("500000"), hz, QStringLiteral("500 kHz in Hz refused"));
        const QString finite = verb.name
            + QStringLiteral(" requires a positive finite frequency in MHz");
        for (const QString& value : {QStringLiteral("nan"), QStringLiteral("NaN"),
                                     QStringLiteral("inf"), QStringLiteral("-inf")}) {
            refusedWith(value, [&](const QString& e) { return e == finite; },
                        value + QStringLiteral(" refused as non-finite"));
        }
        const QString floor = verb.name
            + QStringLiteral(" requires at least 0.001 MHz — got 1e-300");
        refusedWith(QStringLiteral("1e-300"), [&](const QString& e) { return e == floor; },
                    QStringLiteral("sub-floor value refused"));
        refusedWith(QStringLiteral("122250"),
                    [](const QString& e) {
                        return !e.contains(QStringLiteral("did you mean"))
                            && e.contains(QStringLiteral("millimetre-wave"));
                    },
                    QStringLiteral("122.25 GHz names both readings"));
        refusedWith(QStringLiteral("105000.4"),
                    [](const QString& e) { return e.contains(QStringLiteral("got 105000.4,")); },
                    QStringLiteral("over-ceiling value quoted back unrounded"));
        acceptedValue(QStringLiteral("0.001"), QStringLiteral("floor accepted"));
        acceptedValue(QStringLiteral("10368.1"), QStringLiteral("top of band table accepted"));
        acceptedValue(QStringLiteral("14200"), QStringLiteral("kHz-for-MHz passes as 14.2 GHz"));
    }
    check(std::abs(targetMhz - 14200.0) < 1e-9, "targettune hands the parsed MHz to its handler");

    // A handler-less server proves the refusal precedes the handler check.
    AutomationServer bare;
    check(errorOf(Access::request(bare, QByteArrayLiteral("targettune 14200000")))
              .startsWith(QStringLiteral("targettune takes MHz, not Hz")),
          "targettune validates before looking for its handler");
}

void memoryActivate()
{
    AutomationServer server;
    int index = -1;
    QString pan;
    server.setMemoryActivateHandler([&](int memoryIndex, const QString& panId) {
        index = memoryIndex;
        pan = panId;
        return QJsonObject{{QStringLiteral("ok"), true}};
    });
    const QJsonObject reply = Access::request(server, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("memory")},
        {QStringLiteral("action"), QStringLiteral("activate")},
        {QStringLiteral("value"), QStringLiteral("12 0x40000000")}});
    check(ok(reply) && index == 12 && pan == QStringLiteral("0x40000000"),
          "memory activate keeps index and pan", reply);
}

class RefusingConnection final : public QObject, public IConnectionAutomation
{
public:
    QList<RadioInfo> automationLocalRadios() const override { return {}; }
    bool automationConnectLocalSerial(const QString&, QString* = nullptr) override { return true; }
    bool automationConnectByIp(const QString&, const QString& = QString(),
                               QString* error = nullptr) override
    {
        ++calls;
        if (refuse && error) {
            *error = QStringLiteral("no route to radio");
        }
        return !refuse;
    }
    bool automationDisconnect(QString* = nullptr) override { return true; }
    bool automationDialogVisible() const override { return false; }
    void automationSetDialogVisible(bool) override {}
    QObject* asQObject() override { return this; }

    bool refuse = true;
    int calls = 0;
};

void deferredConnectFailures()
{
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);
    RefusingConnection conn;
    server.setConnectionAutomation(&conn);

    check(errorOf(Access::request(server, QByteArrayLiteral("connect wait 60")))
              .contains(QStringLiteral("live automation client")),
          "connect wait without a client is refused rather than parked");

    auto wait = Access::addWait(server);
    const QJsonObject accepted = Access::request(server, QByteArrayLiteral("connect ip 192.0.2.99"));
    check(ok(accepted) && accepted.value(QStringLiteral("deferred")).toBool(),
          "connect ip is accepted up front", accepted);
    check(pumpUntil([&] { return conn.calls == 1; }), "deferred connect ip runs");
    check(wait->complete && wait->error.contains(QStringLiteral("no route to radio"))
              && !Access::waitPending(server, wait),
          "a deferred connect failure answers the pending wait with its message");
    check(Access::lastError(server).contains(QStringLiteral("no route to radio"))
              && Access::lastErrorMs(server) >= 0,
          "the failure is kept as lastError with a timestamp for its age");

    conn.refuse = false;
    check(ok(Access::request(server, QByteArrayLiteral("connect ip 192.0.2.98")))
              && Access::lastError(server).isEmpty() && Access::lastErrorMs(server) < 0,
          "scheduling a fresh connect clears lastError and its age");
    pumpUntil([&] { return conn.calls == 2; });

    auto connectWait = Access::addWait(server);
    Access::noteFailure(server, QStringLiteral("disconnect"), QStringLiteral("port busy"), false);
    check(!connectWait->complete && connectWait->error.isEmpty()
              && Access::waitPending(server, connectWait),
          "a failed disconnect does not answer a connect wait");
    check(Access::lastError(server).contains(QStringLiteral("port busy")),
          "the failed disconnect is still recorded as lastError");
    Access::dropWaits(server);
}

void fmRepeaterAndTransmit()
{
    RadioModel radio; // No slices until the disconnected fixture below.
    AutomationServer server;
    server.setRadioModel(&radio);
    const auto send = [&](const QByteArray& line) { return Access::request(server, line); };
    // A boundary refusal is the verb's own error, not slice resolution's.
    const auto boundary = [&](const QByteArray& line, const QString& fragment) {
        const QJsonObject reply = send(line);
        check(!ok(reply) && errorOf(reply).contains(fragment, Qt::CaseInsensitive)
                  && !errorOf(reply).contains(QStringLiteral("no slice available")),
              QString::fromUtf8(line) + QStringLiteral(" refused at the boundary"), reply);
    };

    const QString empty = errorOf(send("slice"));
    const QString unknown = errorOf(send("slice bogusaction"));
    const QString emptyList = empty.section(QLatin1Char('('), 1).section(QLatin1Char(')'), 0, 0);
    const QString unknownList = unknown.section(QLatin1Char('('), 1).section(QLatin1Char(')'), 0, 0);
    check(!unknownList.isEmpty() && emptyList == unknownList,
          "both slice errors advertise one shared action list");
    for (const char* action : {"filter", "agc", "dsp", "tone", "offset"}) {
        check(unknownList.split(QLatin1Char('|')).contains(QLatin1String(action)),
              QStringLiteral("slice action list advertises ") + QLatin1String(action));
    }
    QStringList undispatched;
    for (const QString& action : unknownList.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        if (errorOf(send("slice " + action.toUtf8())).contains(QStringLiteral("unknown slice action"))) {
            undispatched << action;
        }
    }
    check(undispatched.isEmpty(), QStringLiteral("every advertised slice action dispatches: ")
              + undispatched.join(QLatin1Char(' ')));
    check(errorOf(send("slice dsp squelch off")).contains(QStringLiteral("no slice available")),
          "slice dsp squelch still routes to slice lookup");
    check(errorOf(send("slice tone ctcss_tx 100.0")).contains(QStringLiteral("no slice available")),
          "well-formed tone reaches slice lookup (control row)");

    boundary("slice tone", QStringLiteral("requires"));
    boundary("slice tone dcs", QStringLiteral("off/ctcss_tx"));
    boundary("slice tone ctcss_tx 9999", QStringLiteral("CTCSS"));
    boundary("slice tone ctcss_tx 0", QStringLiteral("CTCSS"));
    boundary("slice tone ctcss_tx 123.4", QStringLiteral("CTCSS"));
    boundary("slice tone ctcss_tx 67.1", QStringLiteral("CTCSS"));
    boundary("slice tone ctcss_tx 100.0 typo", QStringLiteral("unexpected"));
    boundary("slice offset", QStringLiteral("requires"));
    boundary("slice offset sideways", QStringLiteral("simplex/up/down"));
    boundary("slice offset up far", QStringLiteral("MHz"));
    boundary("slice offset up 101", QStringLiteral("0..100"));
    boundary("slice offset down -5", QStringLiteral("0..100"));
    boundary("slice offset up nan", QStringLiteral("frequency"));
    boundary("slice offset up inf", QStringLiteral("frequency"));
    boundary("slice offset down 5 typo", QStringLiteral("unexpected"));

    check(errorOf(send("transmit rfpower 40")).contains(QStringLiteral("ALLOW_TX")),
          "transmit rfpower is TX-gated");
    check(errorOf(send("transmit rfpower 9999")).contains(QStringLiteral("ALLOW_TX")),
          "the TX gate answers before value validation");
    boundary("transmit wattage 5", QStringLiteral("rfpower|tunepower"));
    boundary("transmit", QStringLiteral("requires an action"));

    // Permission on a transport-less model: nothing downstream can key.
    Access::setTxMaxPower(server, 30);
    server.setTxAllowed(true);
    auto& tx = radio.transmitModel();
    const auto power = [&] { return QStringLiteral("%1/%2").arg(tx.rfPower()).arg(tx.tunePower()); };
    QJsonObject reply = send("transmit rfpower 90");
    check(ok(reply) && reply.value(QStringLiteral("requested")).toInt() == 90
              && reply.value(QStringLiteral("clampedTo")).toInt() == 30 && tx.rfPower() == 30,
          "rfpower is clamped to the TX power ceiling and the clamp is reported", reply);
    reply = send("transmit tunepower 90");
    check(ok(reply) && reply.value(QStringLiteral("clampedTo")).toInt() == 30 && tx.tunePower() == 30,
          "tunepower is clamped by the same ceiling", reply);
    reply = send("transmit rfpower 10");
    check(ok(reply) && !reply.contains(QStringLiteral("clampedTo")) && tx.rfPower() == 10,
          "a request under the ceiling is not annotated", reply);
    boundary("transmit rfpower 101", QStringLiteral("0..100"));
    for (const QByteArray& line : {QByteArray("transmit rfpower 30 90"),
                                   QByteArray(R"({"cmd":"transmit","args":"rfpower 30 90"})"),
                                   QByteArray(R"({"cmd":"transmit","action":"rfpower","value":"30 90"})")}) {
        const QString before = power();
        reply = send(line);
        check(!ok(reply) && power() == before,
              QString::fromUtf8(line) + QStringLiteral(" refuses the trailing operand unchanged"), reply);
    }
    server.setTxAllowed(false);

    check(ok(send("slice fixture 0")), "disconnected slice fixture is available");
    const auto offset = [&](const QByteArray& line, double txOffset, const char* dir,
                            double magnitude) {
        const QJsonObject r = send(line);
        SliceModel* s = radio.slice(0);
        check(ok(r) && s && qFuzzyCompare(s->txOffsetFreq() + 1.0, txOffset + 1.0)
                  && s->repeaterOffsetDir() == QLatin1String(dir)
                  && qFuzzyCompare(s->fmRepeaterOffsetFreq() + 1.0, magnitude + 1.0)
                  && qFuzzyCompare(r.value(QStringLiteral("txOffsetFreq")).toDouble() + 1.0,
                                   txOffset + 1.0),
              QString::fromUtf8(line) + QStringLiteral(" writes direction, magnitude and TX split"), r);
    };
    offset("slice offset down 5", -5.0, "down", 5.0);
    offset("slice offset up 5", 5.0, "up", 5.0);
    offset("slice offset simplex", 0.0, "simplex", 5.0);
    offset("slice offset down 5", -5.0, "down", 5.0);
    offset("slice offset down 0.6", -0.6, "down", 0.6);
    check(!ok(send("slice offset down 5 typo")) && qFuzzyCompare(radio.slice(0)->txOffsetFreq(), -0.6),
          "a trailing offset operand leaves the slice unchanged");
    reply = send("slice tone ctcss_tx 123.0");
    check(ok(reply) && radio.slice(0)->fmToneMode() == QLatin1String("ctcss_tx")
              && radio.slice(0)->fmToneValue() == QLatin1String("123.0"),
          "a standard CTCSS tone applies to the fixture slice", reply);
    check(ok(send("slice clearfixture 0")), "slice fixture is removed again");
}

void dexpOmittedOnSim()
{
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);
    RadioInfo demo;
    demo.family = SimBackend::familyName();
    demo.serial = SimBackend::demoSerial();
    demo.model = SimBackend::demoModelName();
    radio.connectToRadio(demo);
    check(pumpUntil([&] { return radio.isConnected(); }), "sim backend connects in process");
    const QJsonObject state = Access::request(server, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("get")},
        {QStringLiteral("model"), QStringLiteral("transmit")}});
    const QJsonObject transmit = state.value(QStringLiteral("transmit")).toObject();
    check(ok(state) && transmit.contains(QStringLiteral("rfPower"))
              && !transmit.contains(QStringLiteral("dexp"))
              && !transmit.contains(QStringLiteral("dexpLevel")),
          "a backend without DEXP omits both fields", state);
    const QJsonObject narrowed = Access::request(server, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("get")},
        {QStringLiteral("model"), QStringLiteral("transmit")},
        {QStringLiteral("property"), QStringLiteral("dexp")}});
    check(!ok(narrowed) && errorOf(narrowed) == QStringLiteral("no property 'dexp' on transmit"),
          "property narrowing cannot reach unsupported DEXP state", narrowed);
    radio.disconnectFromRadio();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-automation-boundary-core"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "cannot create isolated settings profile\n");
        return 1;
    }
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    QCoreApplication app(argc, argv);
    jsonIdAndMhzContract();
    memoryActivate();
    deferredConnectFailures();
    fmRepeaterAndTransmit();
    dexpOmittedOnSim();
    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
