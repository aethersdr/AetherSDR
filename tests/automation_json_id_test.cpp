#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/RadioDiscovery.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

class BridgeClient
{
public:
    bool connectToServer(const QString& serverName)
    {
        m_socket.connectToServer(serverName);
        return m_socket.waitForConnected(1000);
    }

    QJsonObject request(const QJsonObject& request)
    {
        QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
        payload.append('\n');

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&m_socket, &QLocalSocket::readyRead,
                         &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout,
                         &loop, &QEventLoop::quit);

        m_socket.write(payload);
        m_socket.flush();
        timeout.start(1000);
        if (!m_socket.canReadLine()) {
            loop.exec();
        }

        if (!m_socket.canReadLine()) {
            return {};
        }
        return QJsonDocument::fromJson(m_socket.readLine()).object();
    }

private:
    QLocalSocket m_socket;
};

QJsonObject tuneRequest(const QJsonValue& id = QJsonValue::Undefined)
{
    QJsonObject request{
        {QStringLiteral("cmd"), QStringLiteral("tune")},
        {QStringLiteral("value"), QStringLiteral("14.225")},
    };
    if (!id.isUndefined()) {
        request.insert(QStringLiteral("id"), id);
    }
    return request;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-json-id-test"));
    check(settingsProfile.isValid(), "isolated settings profile is available");

    QCoreApplication app(argc, argv);
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);

    int handlerCalls = 0;
    int capturedSliceId = -99;
    server.setTuneHandler([&](double, int sliceId) {
        ++handlerCalls;
        capturedSliceId = sliceId;
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("sliceId"), sliceId},
        };
    });

    // targettune HARD-REQUIRES its handler — doTargetTune() errors out without
    // one — so installing it is what makes a refusal distinguishable from
    // "handler unavailable". As with tune, the app installs this unconditionally
    // at session setup (MainWindow_Session.cpp:1939).
    int targetTuneCalls = 0;
    server.setTargetTuneHandler([&](double mhz) {
        ++targetTuneCalls;
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("targetTune"), mhz},
        };
    });

    const QString serverName = QStringLiteral("aether-json-id-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    check(server.start(serverName), "automation server starts");

    BridgeClient client;
    check(client.connectToServer(serverName), "client connects to automation server");

    auto expectAccepted = [&](const QJsonObject& request, int expectedSliceId,
                              const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(request);
        check(response.value(QStringLiteral("ok")).toBool()
                  && handlerCalls == callsBefore + 1
                  && capturedSliceId == expectedSliceId,
              description);
    };
    auto expectRejected = [&](const QJsonObject& request, const QString& expectedError,
                              const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(request);
        check(!response.value(QStringLiteral("ok")).toBool()
                  && response.value(QStringLiteral("error")).toString() == expectedError
                  && handlerCalls == callsBefore,
              description);
    };

    expectAccepted(tuneRequest(), -1, "omitted id keeps active-slice sentinel");
    expectAccepted(tuneRequest(QStringLiteral("1")), 1,
                   "string id reaches the requested slice");
    expectAccepted(tuneRequest(1), 1,
                   "integer JSON id reaches the requested slice");

    const QString typeError = QStringLiteral("id must be a string or number");
    expectRejected(tuneRequest(true), typeError,
                   "boolean id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonObject{{QStringLiteral("slice"), 1}}), typeError,
                   "object id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonArray{1}), typeError,
                   "array id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonValue::Null), typeError,
                   "null id is rejected instead of selecting the active slice");

    const QString integerError =
        QStringLiteral("tune: sliceId must be a non-negative integer");
    expectRejected(tuneRequest(1.5), integerError,
                   "fractional numeric id is rejected");
    expectRejected(tuneRequest(1.0000001), integerError,
                   "near-integer numeric id is not rounded up to a slice");
    expectRejected(tuneRequest(0.9999999), integerError,
                   "near-integer numeric id is not rounded down to a slice");

    // ---- #4550: the MHz-value contract, shared by every verb taking MHz ----
    //
    // `tune`, `targettune` and `pan center` route their value through one
    // helper (AutomationServer.cpp, refuseUntunableMhz), so the contract is
    // asserted once against a verb parameter rather than once per verb. That is
    // the point of the shared helper and the point of this loop: if a verb ever
    // stops using it, or the thresholds drift apart again, it shows up as one
    // verb failing a case its siblings pass.
    //
    // Handlers for `tune` and `targettune` ARE installed above, exactly as
    // MainWindow_Session.cpp installs them in the shipping app. That matters —
    // the guard is only worth anything if it runs BEFORE the dispatch — so
    // every rejection case also asserts the handler counter did not move.
    auto verbRequest = [](const QString& verb, const QString& value) {
        // `pan center` is one verb spelled as two fields. Hiding that here
        // keeps every case below identical across all three verbs.
        if (verb == QLatin1String("pan center")) {
            return QJsonObject{{QStringLiteral("cmd"), QStringLiteral("pan")},
                               {QStringLiteral("action"), QStringLiteral("center")},
                               {QStringLiteral("value"), value}};
        }
        return QJsonObject{{QStringLiteral("cmd"), verb},
                           {QStringLiteral("value"), value}};
    };
    // `matches` is a predicate on the error text, so one helper serves the cases
    // that pin an exact message, a prefix, or a fragment. `handlerCounter` is
    // null for a verb with no handler indirection (`pan center`).
    auto expectVerbRejected = [&](const QString& verb, const QString& value,
                                  auto&& matches, int* handlerCounter,
                                  const QString& description) {
        const int before = handlerCounter ? *handlerCounter : 0;
        const QJsonObject response = client.request(verbRequest(verb, value));
        const QString error = response.value(QStringLiteral("error")).toString();
        const QByteArray desc = description.toUtf8();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && matches(error)
                  && (!handlerCounter || *handlerCounter == before),
              desc.constData());
    };
    auto expectVerbAccepted = [&](const QString& verb, const QString& value,
                                  int* handlerCounter,
                                  const QString& description) {
        const int before = handlerCounter ? *handlerCounter : 0;
        const QJsonObject response = client.request(verbRequest(verb, value));
        const QByteArray desc = description.toUtf8();
        check(response.value(QStringLiteral("ok")).toBool()
                  && (!handlerCounter || *handlerCounter == before + 1),
              desc.constData());
    };
    auto exactly = [](const QString& text) {
        return [text](const QString& error) { return error == text; };
    };
    auto startingWith = [](const QString& prefix) {
        return [prefix](const QString& error) { return error.startsWith(prefix); };
    };

    struct MhzVerb { const char* name; int* calls; };
    const MhzVerb kMhzVerbs[] = {
        {"tune", &handlerCalls},
        {"targettune", &targetTuneCalls},
        {"pan center", nullptr},  // no handler indirection
    };

    for (const MhzVerb& v : kMhzVerbs) {
        const QString verb = QString::fromLatin1(v.name);
        const QString at = verb + QStringLiteral(": ");

        // The original report: an Hz value is far outside any band, the radio
        // ignores it, and the verb used to answer ok:true with the request
        // echoed back.
        expectVerbRejected(verb, QStringLiteral("14200000"),
                           startingWith(verb + QStringLiteral(" takes MHz, not Hz")),
                           v.calls, at + QStringLiteral("20m in Hz is refused, not applied"));
        // The case a 1 THz threshold let through: 500000 is under 1e6, so it
        // fell past the looser guard and became a 500000 MHz request — the
        // silent no-op this refusal exists to prevent.
        expectVerbRejected(verb, QStringLiteral("500000"),
                           startingWith(verb + QStringLiteral(" takes MHz, not Hz")),
                           v.calls, at + QStringLiteral("500 kHz in Hz is refused too"));

        // Non-finite. QString::toDouble() happily parses "nan" and "inf", and
        // NaN fails EVERY comparison (NaN <= 0, NaN < floor and NaN > ceiling
        // are all false), so without an explicit isfinite check it sails
        // through every guard and reaches the radio. Refused by name, with the
        // message about the value itself rather than a unit mistake it isn't.
        const auto finiteError = exactly(
            verb + QStringLiteral(" requires a positive finite frequency in MHz"));
        for (const QString& nonFinite : {QStringLiteral("nan"), QStringLiteral("NaN"),
                                         QStringLiteral("inf"), QStringLiteral("-inf")}) {
            expectVerbRejected(verb, nonFinite, finiteError, v.calls,
                               at + nonFinite + QStringLiteral(" is refused by the finite check"));
        }

        // Floor. Nothing below what any supported radio tunes should reach the
        // radio; 0.001 matches the floor typed frequency entry applies to the
        // same value (VfoWidget / RxApplet), so the two paths agree.
        expectVerbRejected(verb, QStringLiteral("1e-300"),
                           exactly(verb + QStringLiteral(" requires at least 0.001 MHz — got 1e-300")),
                           v.calls, at + QStringLiteral("a sub-floor frequency is refused"));

        // The ambiguous window (ceiling..300 GHz): could be an Hz mistake OR a
        // real millimetre-wave MHz value (the 122.25 GHz allocation). Refused,
        // but the message must present both readings instead of confidently
        // asserting the wrong one ("did you mean 0.122250?" to a 122 GHz user).
        expectVerbRejected(verb, QStringLiteral("122250"),
                           [](const QString& error) {
                               return !error.contains(QStringLiteral("did you mean"))
                                      && error.contains(QStringLiteral("millimetre-wave"));
                           },
                           v.calls,
                           at + QStringLiteral("122.25 GHz is refused without misdiagnosing it as Hz"));

        // A rejected value must be quoted back as sent. Rounding it to whole
        // MHz made everything in (105000, 105000.5) report as "got 105000,
        // above the 105000 MHz ceiling" — a message that contradicts itself, in
        // the branch that exists to stop this guard asserting things that
        // aren't true.
        expectVerbRejected(verb, QStringLiteral("105000.4"),
                           [](const QString& error) {
                               return error.contains(QStringLiteral("got 105000.4,"));
                           },
                           v.calls,
                           at + QStringLiteral("a value just over the ceiling is quoted back unrounded"));

        // ...and neither bound may refuse anything real. 0.001 MHz is 1 kHz,
        // below every amateur allocation including the VLF experiments at
        // ~8.3 kHz; 10368.1 is the 3cm calling frequency, the top of the band
        // table. Both have to keep working.
        expectVerbAccepted(verb, QStringLiteral("0.001"), v.calls,
                           at + QStringLiteral("the floor value itself is accepted"));
        expectVerbAccepted(verb, QStringLiteral("10368.1"), v.calls,
                           at + QStringLiteral("the top of the band table is accepted"));
        // kHz-for-MHz (14200 = 20m in kHz, or 14.2 GHz in MHz) deliberately
        // passes: the value is indistinguishable from a legitimate transverter
        // request in the headroom the ceiling protects. Pinned as a DECISION,
        // not an omission — if this starts rejecting, the transverter range
        // broke.
        expectVerbAccepted(verb, QStringLiteral("14200"), v.calls,
                           at + QStringLiteral("kHz-for-MHz passes (indistinguishable from 14.2 GHz)"));
    }

    // DEXP is part of the automation surface only when the active backend can
    // carry that intent and report authoritative state. This pins Pat's "UI
    // and automation capability surface" requirement end to end through the
    // real local-socket `get transmit` path on a connected unsupported backend.
    RadioInfo demo;
    demo.family = SimBackend::familyName();
    demo.serial = SimBackend::demoSerial();
    demo.model = SimBackend::demoModelName();
    radio.connectToRadio(demo);
    QEventLoop connectLoop;
    QTimer::singleShot(100, &connectLoop, &QEventLoop::quit);
    connectLoop.exec();

    const QJsonObject simResponse = client.request(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("get")},
                    {QStringLiteral("model"), QStringLiteral("transmit")}});
    const QJsonObject simTransmit =
        simResponse.value(QStringLiteral("transmit")).toObject();
    check(simResponse.value(QStringLiteral("ok")).toBool()
              && !simTransmit.contains(QStringLiteral("dexp"))
              && !simTransmit.contains(QStringLiteral("dexpLevel")),
          "a backend without DEXP omits both fields from automation state");

    const QJsonObject unsupportedDexp = client.request(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("get")},
                    {QStringLiteral("model"), QStringLiteral("transmit")},
                    {QStringLiteral("property"), QStringLiteral("dexp")}});
    check(!unsupportedDexp.value(QStringLiteral("ok")).toBool()
              && unsupportedDexp.value(QStringLiteral("error")).toString()
                     == QStringLiteral("no property 'dexp' on transmit"),
          "property narrowing cannot reach unsupported DEXP state");

    server.stop();
    return failures == 0 ? 0 : 1;
}
