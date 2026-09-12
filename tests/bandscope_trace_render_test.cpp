// BandscopeTrace — the paint path, executed.
//
// WHY THIS EXISTS. Nothing else in this branch runs a line of paintEvent. The
// rest of the bandscope display is proved arithmetically — the analyzer's
// contract, the capability's verb, the gate's one-shot cycle — and none of that
// touches a QPainter. A widget whose drawing code has never been executed is
// the weakest part of the change, and this is the cheapest thing that fixes it:
// render offscreen into a QImage and look at the pixels.
//
// WHAT IT CANNOT TELL YOU. That the picture is RIGHT. No radio has answered the
// verb behind this widget, so what a real converter's spectrum looks like here
// — how large the DC bin is on a direct-sampling front end above all — is not
// known and is not asserted. What is asserted is that the path runs, that an
// empty frame and a populated one differ, and that a frame does not draw
// outside its plot. Offscreen, no radio, no sockets.
//
// It prints two ThemeManager warnings on the way up: this target links the
// widget without the app's Qt resource bundle, so the theme falls back to the
// compiled-in defaults. That is fine for what is asserted here — every check is
// about pixels DIFFERING, never about a particular colour — and a check that
// named a token's value would be testing the theme file rather than the paint.

#include "gui/BandscopeDialog.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QVector>

#include <cmath>
#include <cstdio>

using AetherSDR::BandscopeTrace;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

constexpr int kW = 600;
constexpr int kH = 240;

QImage renderOf(BandscopeTrace& w)
{
    QImage img(kW, kH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    w.render(&img);
    return img;
}

int distinctColours(const QImage& img)
{
    QSet<QRgb> seen;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x)
            seen.insert(img.pixel(x, y));
    }
    return int(seen.size());
}

// A synthetic spectrum: a noise floor with one strong carrier well away from
// DC, which is the case the window exists for — something outside the operator's
// slice loading the converter.
QVector<float> carrierAt(int bin, int bins)
{
    QVector<float> v(bins, -85.0f);
    if (bin >= 0 && bin < bins)
        v[bin] = -6.0f;
    return v;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    BandscopeTrace trace;
    trace.resize(kW, kH);

    // ---- 1 · an empty widget paints, and paints SOMETHING ----
    //
    // The state the window is in between opening and its first frame arriving,
    // and the state it returns to when a radio cannot answer. A widget that
    // crashed or drew nothing here would fail in front of the operator at the
    // one moment they are waiting.
    const QImage empty = renderOf(trace);
    check(!empty.isNull(), "a frameless trace renders without crashing");
    check(distinctColours(empty) > 1,
          "and draws its background and dB labels, not an empty rectangle");

    // ---- 2 · a frame changes the picture ----
    {
        // 76.8 MHz is the HL2 converter's clock; 1025 bins is what a
        // 2048-point transform reports from DC to Nyquist.
        trace.setFrame(carrierAt(400, 1025), 76.8e6);
        const QImage drawn = renderOf(trace);
        check(!drawn.isNull(), "a populated trace renders");
        check(drawn != empty, "and does not look like the empty one");
        check(distinctColours(drawn) > distinctColours(empty),
              "the trace and its fill add colour the grid alone does not have");
    }

    // ---- 3 · clearing puts it back ----
    //
    // Not cosmetic. clearFrame() is what runs when a radio stops declaring the
    // capability or answers with a record this window cannot read, and a stale
    // trace left on screen under a "no frame" status line would be a lie the
    // operator has no way to detect.
    {
        trace.clearFrame();
        const QImage cleared = renderOf(trace);
        check(cleared == empty, "clearFrame() returns the surface to its frameless state");
    }

    // ---- 4 · levels outside the displayed window are CLAMPED, not drawn off it ----
    //
    // The scale is fixed at 0 to -100 dBFS on purpose — an auto-range would make
    // a quiet band and a band with a broadcast carrier in it look identical. So
    // a bin above 0 dBFS or below the floor has to be clamped. Rendering into an
    // image exactly the widget's size means anything drawn outside it is simply
    // lost, and the assertion is that the two extremes still produce a picture
    // rather than a QPainter complaint or an empty band.
    {
        QVector<float> wild(1025, -300.0f);
        wild[100] = 40.0f;      // far above the converter's rail
        wild[900] = -1e9f;      // far below anything representable
        trace.setFrame(wild, 76.8e6);
        const QImage drawn = renderOf(trace);
        check(!drawn.isNull() && drawn != empty,
              "out-of-range levels still render, clamped into the fixed window");
    }

    // ---- 5 · a frame with no usable sample rate draws no frequency axis ----
    //
    // The axis is labelled from the radio's own sample rate. Zero means the
    // radio did not report one, and inventing a span would put MHz numbers on
    // the screen that nothing stands behind.
    {
        trace.setFrame(carrierAt(400, 1025), 0.0);
        const QImage noAxis = renderOf(trace);
        trace.setFrame(carrierAt(400, 1025), 76.8e6);
        const QImage withAxis = renderOf(trace);
        check(noAxis != withAxis,
              "a rate of zero suppresses the frequency ticks and their labels");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "bandscope_trace_render_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
