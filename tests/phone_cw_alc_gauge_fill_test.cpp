// The Phone and CW ALC bars end under the tick of the value they show (#6228).
// The gauges are the applet's own, painted offscreen and read back by pixel:
// HGauge::filledFraction() is the same for a bar anchored at either end, so
// only the painted geometry can tell which way the bar grows.

#include "TestSettingsProfile.h"
#include "gui/HGauge.h"
#include "gui/PhoneCwApplet.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const QString& description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]",
                qPrintable(description));
    if (!condition) {
        ++failures;
    }
}

constexpr int kGaugeW = 400;
constexpr int kGaugeH = 24;
// Two rows above the bar's bottom border, inside the fill at this height.
constexpr int kProbeRow = kGaugeH - 6;
// The centred "ALC" label paints over the bar; no probe column falls in it.
constexpr int kLabelHalfW = 40;
constexpr int kEdgeTolerancePx = 2;

QImage renderOnce(HGauge* gauge)
{
    gauge->resize(kGaugeW, kGaugeH);
    QImage image(kGaugeW, kGaugeH, QImage::Format_ARGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    gauge->render(&painter);
    return image;
}

// render() delivers a pending layout request before it paints, and that puts
// the gauge back at the layout's width. A second pass paints at the probe
// size; the size check fails the test if it still does not hold.
QImage paint(HGauge* gauge, const QString& what)
{
    QImage image = renderOnce(gauge);
    if (gauge->size() != QSize(kGaugeW, kGaugeH)) {
        image = renderOnce(gauge);
    }
    check(gauge->size() == QSize(kGaugeW, kGaugeH),
          what + QStringLiteral(": painted at the probe size"));
    return image;
}

// Any fill zone colour is brighter than the empty track and its border.
bool filledAt(const QImage& image, int x)
{
    const QColor c = image.pixelColor(x, kProbeRow);
    return qMax(c.red(), qMax(c.green(), c.blue())) > 0x60;
}

bool inLabel(int x)
{
    return x > kGaugeW / 2 - kLabelHalfW && x < kGaugeW / 2 + kLabelHalfW;
}

struct FillSpan {
    int first{-1};
    int last{-1};
    int count{0};
};

FillSpan fillSpan(const QImage& image)
{
    FillSpan span;
    for (int x = 0; x < kGaugeW; ++x) {
        if (inLabel(x) || !filledAt(image, x)) {
            continue;
        }
        if (span.first < 0) {
            span.first = x;
        }
        span.last = x;
        ++span.count;
    }
    return span;
}

// The bar starts at the scale's left end and stops at `fraction` of the width.
void checkBarEndsAt(HGauge* gauge, float fraction, const QString& what)
{
    const FillSpan span = fillSpan(paint(gauge, what));
    const int expected = qRound(fraction * kGaugeW);
    std::printf("       %s: fill spans x=%d..%d of %d, expected end %d\n",
                qPrintable(what), span.first, span.last, kGaugeW, expected);
    check(span.first >= 0 && span.first <= kEdgeTolerancePx,
          what + QStringLiteral(": the bar starts at the scale's left end"));
    check(qAbs(span.last - expected) <= kEdgeTolerancePx,
          what + QStringLiteral(": the bar ends under the value's tick"));
}

void checkEmpty(HGauge* gauge, const QString& what)
{
    const FillSpan span = fillSpan(paint(gauge, what));
    std::printf("       %s: %d filled columns\n", qPrintable(what), span.count);
    check(span.count == 0,
          what + QStringLiteral(": the floor paints no bar"));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-cw-alc-gauge-fill-test"));
    QApplication app(argc, argv);
    PhoneCwApplet applet;

    QList<HGauge*> gauges;
    QStringList names;
    for (QWidget* widget : applet.findChildren<QWidget*>()) {
        if (widget->accessibleName() == QLatin1String("ALC gauge (Phone)")
            || widget->accessibleName() == QLatin1String("ALC gauge (CW)")) {
            gauges.append(static_cast<HGauge*>(widget));
            names.append(widget->accessibleName());
        }
    }
    check(gauges.size() == 2, QStringLiteral("both ALC gauges are present"));
    if (gauges.size() != 2) {
        return 1;
    }

    // updateAlc() is the applet's path; setValueImmediate() then settles the
    // ballistics so the painted bar is the reading, not a frame on the way.
    const auto drive = [&](float value) {
        applet.updateAlc(value);
        for (HGauge* gauge : gauges) {
            gauge->setValueImmediate(value);
        }
    };

    for (int i = 0; i < gauges.size(); ++i) {
        checkEmpty(gauges[i], names[i] + QStringLiteral(" dBFS at -20"));
    }
    drive(-5.0f);
    for (int i = 0; i < gauges.size(); ++i) {
        check(gauges[i]->filledFraction() == 0.75f,
              names[i] + QStringLiteral(": -5 dBFS is 0.75 of the range"));
        checkBarEndsAt(gauges[i], 0.75f, names[i] + QStringLiteral(" dBFS at -5"));
    }
    drive(-15.0f);
    for (int i = 0; i < gauges.size(); ++i) {
        checkBarEndsAt(gauges[i], 0.25f, names[i] + QStringLiteral(" dBFS at -15"));
    }

    applet.setAlcMeterUnit(QStringLiteral("Percent"));
    for (int i = 0; i < gauges.size(); ++i) {
        checkEmpty(gauges[i], names[i] + QStringLiteral(" percent at 0"));
    }
    drive(75.0f);
    for (int i = 0; i < gauges.size(); ++i) {
        check(gauges[i]->filledFraction() == 0.75f,
              names[i] + QStringLiteral(": 75 % is 0.75 of the range"));
        checkBarEndsAt(gauges[i], 0.75f, names[i] + QStringLiteral(" percent at 75"));
    }

    applet.setAlcMeterUnit(QStringLiteral("dBFS"));
    drive(-5.0f);
    for (int i = 0; i < gauges.size(); ++i) {
        checkBarEndsAt(gauges[i], 0.75f,
                       names[i] + QStringLiteral(" dBFS at -5 after percent"));
    }

    return failures == 0 ? 0 : 1;
}
