// TCI 2.1 §HW_SENSORS_ENABLE / §HW_SENSORS: `hw_sensors:volts,amps,celsius;`
// from the vitals the backend declares. A field the radio does not measure is
// NaN (the fields are positional and §4.7 forbids empty arguments); a radio that
// measures none sends nothing. Nothing is sent until a client enables it.
//
// Socket-free: TciClient is a model-thread handle with a text sink, attached
// the way onClientOpened() does it. Nothing is bound, opened or keyed.

#include "TestSettingsProfile.h"
#include "core/TciClient.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/backends/MeterDef.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/sim/SimBackend.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failed;
    }
}

class LinkedHl2Backend final : public hl2::Hl2Backend
{
public:
    bool isConnected() const override { return true; }
};

void defineVital(MeterModel& meters, int index, const char* name, const char* unit)
{
    MeterDef def;
    def.index = index;
    def.source = QStringLiteral("RAD");
    def.sourceIndex = 0;
    def.name = QString::fromLatin1(name);
    def.unit = QString::fromLatin1(unit);
    meters.defineMeter(def);
}

void feedTemperature(MeterModel& meters, float celsius)
{
    defineVital(meters, 5, "PATEMP", "degC");
    meters.updateValueByName(QStringLiteral("RAD"), QStringLiteral("PATEMP"), celsius);
}

void feedSupply(MeterModel& meters, float volts)
{
    defineVital(meters, 6, "+13.8A", "Volts");
    meters.updateValueByName(QStringLiteral("RAD"), QStringLiteral("+13.8A"), volts);
}

std::optional<QString> messageFor(const RadioCapabilities& caps, const MeterModel& meters)
{
    return TciProtocol::hwSensorsMessage(TciProtocol::hwSensorReadings(caps, meters));
}

} // namespace

class TciHwSensorsTestAccess {
public:
    static void attach(TciServer& s, TciClient* c, RadioModel& model)
    {
        TciServer::ClientState cs;
        cs.socket = c;
        cs.protocol = new TciProtocol(&model, &s.m_routingState, &s.m_trxMap);
        s.m_clients.append(cs);
        QObject::connect(c, &TciClient::textMessageReceived, &s, &TciServer::onTextMessage);
    }
    // start() would bind a listener; the scheduler only re-arms while running.
    static void markRunning(TciServer& s) { s.m_running.store(true); }
    static int intervalMs(TciServer& s, int client) { return s.m_clients.at(client).hwSensorsIntervalMs; }
    static bool timerActive(TciServer& s) { return s.m_hwSensorsTimer->isActive(); }
};

namespace {

void testEnableArguments()
{
    using P = TciProtocol;
    const auto a = P::parseHwSensorsEnable(QStringLiteral("true,500"));
    check(a && a->enabled && a->intervalMs == 500, "enable: true,500 enables at 500 ms");
    const auto b = P::parseHwSensorsEnable(QStringLiteral("1"));
    check(b && b->enabled && b->intervalMs == P::kHwSensorsDefaultIntervalMs,
          "enable: 1 is true and the interval is optional");
    const auto c = P::parseHwSensorsEnable(QStringLiteral("false"));
    check(c && !c->enabled, "enable: false disables");
    const auto d = P::parseHwSensorsEnable(QStringLiteral("yes"));
    check(d && !d->enabled, "enable: anything but true/1 is false (§4.7 item 3)");
    const auto e = P::parseHwSensorsEnable(QStringLiteral("true,5"));
    check(e && e->intervalMs == 30, "enable: an interval below 30 ms is held at 30");
    const auto f = P::parseHwSensorsEnable(QStringLiteral("true,5000"));
    check(f && f->intervalMs == 1000, "enable: an interval above 1000 ms is held at 1000");
    const auto g = P::parseHwSensorsEnable(QStringLiteral("true,abc"));
    check(g && g->enabled && g->intervalMs == P::kHwSensorsDefaultIntervalMs,
          "enable: an unparsable interval is missing, not zero (§4.7 item 2)");
    const auto h = P::parseHwSensorsEnable(QStringLiteral(",true"));
    check(h && h->enabled, "enable: empty arguments are discarded (§4.7 item 1)");
    check(!P::parseHwSensorsEnable(QString()), "enable: no argument is not a request");
}

void testHl2SendsTemperatureOnly()
{
    LinkedHl2Backend hl2;
    const RadioCapabilities caps = hl2.capabilities();
    MeterModel meters;
    check(!messageFor(caps, meters), "HL2: nothing before a temperature sample");
    defineVital(meters, 5, "PATEMP", "degC");
    check(!messageFor(caps, meters), "HL2: a declared but unfed PATEMP is not a reading");
    meters.updateValueByName(QStringLiteral("RAD"), QStringLiteral("PATEMP"), 42.04f);
    const auto msg = messageFor(caps, meters);
    check(msg && *msg == QStringLiteral("hw_sensors:NaN,NaN,42.0;"),
          "HL2: temperature in arg3, voltage and current NaN");
    // A rail sample the HL2 does not declare stays out of arg1.
    feedSupply(meters, 13.8f);
    const auto gated = messageFor(caps, meters);
    check(gated && gated->startsWith(QStringLiteral("hw_sensors:NaN,")),
          "HL2: an undeclared supply meter is not reported");
}

void testDeclaredSupplyAndTemperature()
{
    RadioCapabilities caps;
    caps.hasSupplyVoltageTelemetry = true;
    caps.hasPaTemperatureTelemetry = true;
    caps.hasPaCurrentTelemetry = true;
    MeterModel meters;
    feedSupply(meters, 13.8f);
    feedTemperature(meters, 45.0f);
    defineVital(meters, 7, "PACURRENT", "Amps");
    meters.updateValueByName(QStringLiteral("RAD"), QStringLiteral("PACURRENT"), 4.5f);
    const auto msg = messageFor(caps, meters);
    check(msg && *msg == QStringLiteral("hw_sensors:13.8,NaN,45.0;"),
          "declared rail and temperature fill arg1 and arg3; PA drain current is not arg2");
}

void testRadioWithoutSensorsSendsNothing()
{
    SimBackend sim;
    MeterModel meters;
    feedSupply(meters, 13.8f);
    feedTemperature(meters, 42.0f);
    check(!messageFor(sim.capabilities(), meters),
          "a backend declaring no vitals sends no hw_sensors, whatever the meters hold");
}

void testNanFieldsStayPositional()
{
    TciProtocol::HwSensorReadings r;
    r.temperatureC = 42.0;
    const QString msg = TciProtocol::hwSensorsMessage(r).value_or(QString());
    const QStringList args = msg.section(QLatin1Char(':'), 1).chopped(1).split(QLatin1Char(','));
    check(args.size() == 3 && !args.contains(QString()), "three non-empty arguments");
    bool ok = false;
    const double v = args.value(0).toDouble(&ok);
    check(ok && std::isnan(v), "a float parser reads NaN as no value, not as 0 V");
    check(args.value(2).toDouble() == 42.0, "arg3 is the temperature");
    TciProtocol::HwSensorReadings bad;
    bad.temperatureC = std::nan("");
    check(!TciProtocol::hwSensorsMessage(bad), "a non-finite reading is not a measurement");
}

bool pumpUntil(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

void testServerSendsOnlyAfterEnable()
{
    RadioModel model;
    auto owned = std::make_unique<LinkedHl2Backend>();
    model.setBackendForTest(std::move(owned), QStringLiteral("tci-hw-sensors-test"));
    feedTemperature(model.meterModel(), 41.0f);
    TciServer server(&model);
    TciHwSensorsTestAccess::markRunning(server);

    TciClient enabled;
    TciClient silent;
    QStringList toEnabled;
    QStringList toSilent;
    enabled.textSink = [&](const QString& m) { toEnabled << m; return qint64(m.size()); };
    silent.textSink = [&](const QString& m) { toSilent << m; return qint64(m.size()); };
    TciHwSensorsTestAccess::attach(server, &enabled, model);
    TciHwSensorsTestAccess::attach(server, &silent, model);
    const auto hwCount = [](const QStringList& l) {
        return int(l.filter(QStringLiteral("hw_sensors:")).size());
    };

    QCoreApplication::processEvents();
    check(hwCount(toEnabled) == 0 && !TciHwSensorsTestAccess::timerActive(server),
          "server: no hw_sensors and no timer before enable");

    emit enabled.textMessageReceived(QStringLiteral("HW_SENSORS_ENABLE:true,5;"));
    check(toEnabled.filter(QStringLiteral("hw_sensors_enable")).isEmpty(),
          "server: the unidirectional enable is not answered");
    check(TciHwSensorsTestAccess::intervalMs(server, 0) == 30, "server: interval held at 30 ms");
    check(pumpUntil([&] { return hwCount(toEnabled) >= 1; }, 5000),
          "server: the enabling client receives hw_sensors");
    check(toEnabled.filter(QStringLiteral("hw_sensors:")).value(0)
              == QStringLiteral("hw_sensors:NaN,NaN,41.0;"),
          "server: the HL2 message carries the model's temperature");
    check(TciHwSensorsTestAccess::timerActive(server), "server: the next send is scheduled");
    check(hwCount(toSilent) == 0, "server: a client that did not enable receives nothing");

    emit enabled.textMessageReceived(QStringLiteral("hw_sensors_enable:false;"));
    check(pumpUntil([&] { return !TciHwSensorsTestAccess::timerActive(server); }, 5000),
          "server: disabling the last client stops the timer");
    const int after = hwCount(toEnabled);
    QCoreApplication::processEvents();
    check(hwCount(toEnabled) == after, "server: nothing is sent after disable");
}

void testStaleReadingIsNotSent()
{
    LinkedHl2Backend hl2;
    MeterModel meters;
    feedTemperature(meters, 42.0f);
    check(messageFor(hl2.capabilities(), meters).has_value(), "stale: a fresh temperature is sent");
    // MeterModel has no clock hook: wait out the vitals window for real.
    QThread::msleep(static_cast<unsigned long>(MeterModel::kVitalsFreshMs + 100));
    check(!messageFor(hl2.capabilities(), meters),
          "stale: a temperature older than the vitals window is not a reading");
}

void testServerKeepsTheInterval()
{
    RadioModel model;
    model.setBackendForTest(std::make_unique<LinkedHl2Backend>(),
                            QStringLiteral("tci-hw-sensors-cadence"));
    feedTemperature(model.meterModel(), 41.0f);
    TciServer server(&model);
    TciHwSensorsTestAccess::markRunning(server);
    TciClient client;
    QStringList got;
    client.textSink = [&](const QString& m) { got << m; return qint64(m.size()); };
    TciHwSensorsTestAccess::attach(server, &client, model);
    emit client.textMessageReceived(QStringLiteral("hw_sensors_enable:true,100;"));
    QElapsedTimer t;
    t.start();
    pumpUntil([&] { return t.elapsed() >= 450; }, 1000);
    const int sent = int(got.filter(QStringLiteral("hw_sensors:")).size());
    check(sent >= 3 && sent <= 6,
          "server: a 100 ms interval sends about every 100 ms, not on every event-loop pass");
    emit client.textMessageReceived(QStringLiteral("hw_sensors_enable:false;"));
    pumpUntil([&] { return !TciHwSensorsTestAccess::timerActive(server); }, 1000);
}

} // namespace
} // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tci-hw-sensors-test"));
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    testEnableArguments();
    testHl2SendsTemperatureOnly();
    testDeclaredSupplyAndTemperature();
    testRadioWithoutSensorsSendsNothing();
    testNanFieldsStayPositional();
    testServerSendsOnlyAfterEnable();
    testStaleReadingIsNotSent();
    testServerKeepsTheInterval();
    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
