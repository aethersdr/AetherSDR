// The `gauge` verb: a meter's value, peak and painted fraction, without
// dragging the whole widget tree across the socket.
//
// It exists because monitoring a meter means sampling it, and dumpTree is
// hundreds of kilobytes per sample. Two things here are easy to get wrong and
// silent when wrong:
//
//   * gaugeValue and gaugeFraction disagree for the whole length of a
//     ballistics animation. A monitor reading only the former reports a
//     settled meter while the bar is still travelling (#3845), so the verb
//     must carry both.
//   * a gauge is addressed by accessible name, and accessible names are
//     phrases ("Forward power"). The bridge splits its command line on
//     whitespace, so a single-token parse truncates that to "Forward" and
//     reports the widget missing.

#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "gui/HGauge.h"

#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}

namespace {
int failures = 0;
void check(bool ok, const char* description)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) ++failures;
}

void settle(int ms)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("automation-gauge-verb"));
    if (!profile.isValid()) return 1;
    // The gauge only publishes its properties when automation is on -- the
    // same gate the bridge itself runs behind.
    qputenv("AETHER_AUTOMATION", "1");
    QApplication app(argc, argv);

    AetherSDR::AutomationServer server(nullptr);
    const auto request = [&server](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };

    AetherSDR::HGauge gauge(0.0f, 200.0f, 125.0f, "PWR", "W",
                            {{0, "0"}, {100, "100"}, {200, "200"}},
                            nullptr, 80.0f);
    gauge.setAccessibleName(QStringLiteral("Forward power"));
    gauge.show();
    QApplication::processEvents();

    // A phrase target must survive the command-line split.
    QJsonObject r = request("gauge Forward power");
    check(r.value("ok").toBool(), "multi-word accessible name resolves");
    check(r.value("target").toString() == QLatin1String("Forward power"),
          "the whole name is echoed back, not the first word");

    gauge.setValue(150.0f);
    QApplication::processEvents();
    r = request("gauge Forward power");
    check(qFuzzyCompare(r.value("value").toDouble(), 150.0), "value is reported");
    check(!r.value("peakHeld").toBool(),
          "an ordinary gauge does not grow a peak marker by default");

    // The derived state. Immediately after a step the bar has not travelled
    // yet, so fraction must NOT already equal the new value's fraction --
    // that is the whole reason it is published separately.
    check(r.contains("fraction"), "painted fraction is carried");
    check(r.value("fraction").toDouble() < 0.75,
          "fraction lags the value while the ballistics run");

    gauge.setPeakValue(180.0f);
    settle(80); // Cross several animation ticks: none may overwrite MICPEAK.
    r = request("gauge Forward power");
    check(qFuzzyCompare(r.value("peak").toDouble(), 180.0),
          "a manual external reading survives animation ticks");
    check(r.value("peakHeld").toBool(), "a held peak says so");

    const QJsonObject range = r.value("range").toObject();
    check(qFuzzyCompare(range.value("max").toDouble(), 200.0)
              && qFuzzyCompare(range.value("yellowStart").toDouble(), 80.0),
          "range including the yellow zone is carried");

    gauge.clearPeak();
    QApplication::processEvents();
    r = request("gauge Forward power");
    check(!r.value("peakHeld").toBool(), "a cleared peak says so");

    // Every gauge, for a monitor that does not know the names.
    r = request("gauge");
    check(r.value("ok").toBool() && r.value("count").toInt() >= 1,
          "bare verb lists every gauge");
    bool found = false;
    for (const auto v : r.value("gauges").toArray()) {
        if (v.toObject().value("target").toString() == QLatin1String("Forward power"))
            found = true;
    }
    check(found, "the listing names gauges by accessible name");

    // Errors: a widget that is not a meter, and one that does not exist.
    QPushButton button;
    button.setObjectName(QStringLiteral("notAGauge"));
    button.show();
    QApplication::processEvents();
    r = request("gauge notAGauge");
    check(!r.value("ok").toBool()
              && r.value("error").toString().contains(QLatin1String("not a gauge")),
          "a non-gauge widget is refused as such, not reported empty");
    r = request("gauge NoSuchWidget");
    check(!r.value("ok").toBool()
              && r.value("error").toString().contains(QLatin1String("not found")),
          "a missing widget is refused");

    return failures == 0 ? 0 : 1;
}
