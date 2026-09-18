#include "gui/FilterPassbandWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

static void checkEdges(const FilterPassbandWidget& widget, int low, int high,
                       const char* message)
{
    if (widget.filterLo() != low || widget.filterHi() != high) {
        std::fprintf(stderr, "FAIL: %s: expected %d..%d, got %d..%d\n",
                     message, low, high, widget.filterLo(), widget.filterHi());
        ++g_failures;
    }
}

// Drive the production event handler without a socket or a radio peer.
static void drag(FilterPassbandWidget& widget, int x, int dx, int dy)
{
    const QPointF start(x, 30);
    const QPointF end(x + dx, 30 + dy);
    QMouseEvent press(QEvent::MouseButtonPress, start, start,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent move(QEvent::MouseMove, end, end,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    FilterPassbandWidget widget;
    widget.resize(200, 80);
    widget.setMode(QStringLiteral("AM"));

    widget.setFilter(-5000, 5000);
    drag(widget, 100, 5, 0);
    checkEdges(widget, -4850, 5150,
               "empty capabilities preserve a 10 kHz width and legacy rounding");

    widget.setFilter(-1000, 1000);
    drag(widget, 100, 0, -10);
    checkEdges(widget, -1250, 1250, "legacy vertical scale remains 4 kHz");

    widget.setFilter(-5000, 5000);
    drag(widget, 190, 100, 0);
    checkEdges(widget, -5000, 8550, "legacy high-edge drag has no 6 kHz ceiling");

    widget.setFilter(-1000, 1000);
    drag(widget, 10, 200, 0);
    checkEdges(widget, 950, 1000, "legacy minimum width still applies");

    widget.setWidthRange(200, 10000, 200);
    widget.setFilter(-4000, 4000);
    drag(widget, 190, 100, 0);
    checkEdges(widget, -4000, 6000, "advertised AM maximum still applies");

    widget.setWidthRange(50, 3600, 50);
    widget.setMode(QStringLiteral("USB"));
    widget.setFilter(300, 2700);
    drag(widget, 190, 100, 0);
    checkEdges(widget, 300, 3900, "mode change adopts the advertised USB maximum");

    widget.setWidthRange(0, 0, 0);
    widget.setMode(QStringLiteral("AM"));
    widget.setFilter(-5000, 5000);
    drag(widget, 100, 5, 0);
    checkEdges(widget, -4850, 5150, "disconnect restores unrestricted legacy width");

    if (g_failures == 0) {
        std::puts("filter_passband_widget_test: all checks passed");
    }
    return g_failures == 0 ? 0 : 1;
}
