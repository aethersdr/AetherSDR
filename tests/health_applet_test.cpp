#include "gui/HealthApplet.h"
#include "models/MeterModel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QThread>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-58s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

void processEventsFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

qint16 rawMeterValue(float value)
{
    return static_cast<qint16>(std::lround(value * 128.0f));
}

MeterDef txMeter(int index, const QString& name, const QString& unit)
{
    MeterDef def;
    def.index = index;
    def.source = QStringLiteral("TX-");
    def.sourceIndex = 8;
    def.name = name;
    def.unit = unit;
    return def;
}

void defineTxMeters(MeterModel& model)
{
    model.defineMeter(txMeter(1, QStringLiteral("FWDPWR"), QStringLiteral("dBm")));
    model.defineMeter(txMeter(2, QStringLiteral("SWR"), QStringLiteral("SWR")));
}

void sendMeters(MeterModel& model, float powerWatts, float swr)
{
    const float powerDbm = 10.0f * std::log10(std::max(powerWatts, 0.000001f) * 1000.0f);
    model.updateValues({1, 2}, {rawMeterValue(powerDbm), rawMeterValue(swr)});
}

QLabel* healthStatusLabel(HealthApplet& applet)
{
    const QList<QLabel*> labels = applet.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->toolTip().startsWith(QStringLiteral("Overall antenna health state"))) {
            return label;
        }
    }
    return nullptr;
}

QString statusText(HealthApplet& applet)
{
    QLabel* label = healthStatusLabel(applet);
    return label ? label->text() : QStringLiteral("<missing>");
}

void testUnkeyTransientDoesNotLatchWarning()
{
    MeterModel model;
    defineTxMeters(model);
    HealthApplet applet;
    applet.setMeterModel(&model);
    applet.show();

    sendMeters(model, 25.0f, 1.10f);
    processEventsFor(350);
    report("stable dummy-load sample is healthy",
           statusText(applet) == QStringLiteral("OK"), statusText(applet));

    // MeterModel deliberately retains a slowly-decaying display power, while
    // directionalPowerMetersChanged carries the near-zero instantaneous power.
    // The SWR value is not physically meaningful during this unkey packet.
    sendMeters(model, 0.001f, 3.50f);
    processEventsFor(320);  // headroom over the 4-frame settle for loaded runners
    report("unkey SWR transient does not latch HLTH warning",
           statusText(applet) == QStringLiteral("OK"), statusText(applet));
}

void testKeyTransientDoesNotPoisonBaseline()
{
    MeterModel model;
    defineTxMeters(model);
    HealthApplet applet;
    applet.setMeterModel(&model);
    applet.show();

    sendMeters(model, 1.0f, 3.50f);
    processEventsFor(60);
    sendMeters(model, 25.0f, 1.10f);
    processEventsFor(420);

    report("key-up SWR transient does not poison baseline",
           statusText(applet) == QStringLiteral("OK"), statusText(applet));
}

void testIntermittentSwrOutliersDoNotLatchWarning()
{
    MeterModel model;
    defineTxMeters(model);
    HealthApplet applet;
    applet.setMeterModel(&model);
    applet.show();

    const float noisyDummyLoadSamples[] = {
        1.00f, 1.79f, 1.00f, 1.59f, 1.00f, 1.00f, 1.31f, 1.00f,
    };
    for (float swr : noisyDummyLoadSamples) {
        sendMeters(model, 1.4f, swr);
        processEventsFor(70);
    }

    report("intermittent low-power SWR outliers do not latch warning",
           statusText(applet) == QStringLiteral("OK"), statusText(applet));
}

void testSustainedHighSwrStillWarnsAtLowPower()
{
    MeterModel model;
    defineTxMeters(model);
    HealthApplet applet;
    applet.setMeterModel(&model);
    applet.show();

    for (int i = 0; i < 6; ++i) {
        sendMeters(model, 1.0f, 3.00f);
        processEventsFor(70);
    }
    processEventsFor(350);

    report("sustained high SWR still warns at one watt",
           statusText(applet) == QStringLiteral("GROUND?"), statusText(applet));
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    testUnkeyTransientDoesNotLatchWarning();
    testKeyTransientDoesNotPoisonBaseline();
    testIntermittentSwrOutliersDoNotLatchWarning();
    testSustainedHighSwrStillWarnsAtLowPower();

    std::printf("\n%d health applet test(s) failed\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
