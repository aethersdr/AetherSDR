// Unit tests for the normalized canvas geometry of RFC #4887 phase 1.
//
// The load-bearing cases are 2 and 5.  Case 2 pins the edge-rounding rule:
// two items sharing a normalized edge must land on the same pixel column at
// EVERY canvas size, or a tiled arrangement grows hairline seams that move as
// the window is resized.  Case 5 pins the property the whole RFC rests on —
// a layout authored on a big display restores in proportion on a small one,
// which is the thing pixel geometry cannot do.
//
// Pure logic, no widgets: this runs headless on all three platforms.

#include "gui/workspace/WorkspaceGeometry.h"

#include <QRect>
#include <QSize>

#include <cmath>
#include <cstdio>
#include <limits>

using AetherSDR::NormRect;
using AetherSDR::clampToCanvas;
using AetherSDR::fromPixels;
using AetherSDR::minimumNormSize;
using AetherSDR::toPixels;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

bool nearly(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main()
{
    // ── Case 1: the basic mapping ────────────────────────────────────────
    {
        const QSize canvas(800, 600);

        report("full-canvas rect maps to the whole canvas",
               toPixels(NormRect{0.0, 0.0, 1.0, 1.0}, canvas) == QRect(0, 0, 800, 600));

        report("quarter rect maps to the expected pixels",
               toPixels(NormRect{0.5, 0.5, 0.5, 0.5}, canvas) == QRect(400, 300, 400, 300));

        report("fromPixels inverts toPixels",
               fromPixels(QRect(400, 300, 400, 300), canvas)
                   == NormRect{0.5, 0.5, 0.5, 0.5});
    }

    // ── Case 2: shared edges land on one pixel (the anti-seam rule) ──────
    //
    // Odd canvas sizes are where rounding origin+size instead of the edges
    // goes wrong, so they are the ones tested.
    {
        const NormRect left {0.0, 0.0, 0.5, 1.0};
        const NormRect right{0.5, 0.0, 0.5, 1.0};

        bool adjacentEverywhere = true;
        bool coversEverywhere   = true;
        for (int w : {1001, 1023, 777, 1920, 3841}) {
            const QSize canvas(w, 500);
            const QRect a = toPixels(left,  canvas);
            const QRect b = toPixels(right, canvas);

            // No gap and no overlap: b starts exactly where a ends.
            if (a.x() + a.width() != b.x()) {
                adjacentEverywhere = false;
            }
            // And between them they cover the canvas exactly.
            if (a.width() + b.width() != w) {
                coversEverywhere = false;
            }
        }
        report("split items stay pixel-adjacent at any canvas width", adjacentEverywhere);
        report("split items cover the canvas exactly at any width", coversEverywhere);

        // Three-way split on a width divisible by nothing convenient.
        const QSize canvas(1000, 1000);
        const QRect a = toPixels(NormRect{0.0,       0.0, 1.0 / 3.0, 1.0}, canvas);
        const QRect b = toPixels(NormRect{1.0 / 3.0, 0.0, 1.0 / 3.0, 1.0}, canvas);
        const QRect c = toPixels(NormRect{2.0 / 3.0, 0.0, 1.0 / 3.0, 1.0}, canvas);
        report("three-way split leaves no seam",
               a.x() + a.width() == b.x() && b.x() + b.width() == c.x()
                   && a.width() + b.width() + c.width() == 1000);
    }

    // ── Case 3: round-trip stability ─────────────────────────────────────
    {
        const QSize canvas(1920, 1080);
        const NormRect original{0.137, 0.271, 0.463, 0.219};
        const NormRect back = fromPixels(toPixels(original, canvas), canvas);

        // A round trip through integer pixels cannot be exact; it must be
        // within one pixel, which is the resolution the operator can see.
        report("round trip stays within one pixel",
               nearly(back.x, original.x, 1.0 / 1920.0)
                   && nearly(back.y, original.y, 1.0 / 1080.0)
                   && nearly(back.w, original.w, 1.0 / 1920.0)
                   && nearly(back.h, original.h, 1.0 / 1080.0));

        // And a second trip must not drift further — otherwise repeated
        // resizes would walk a layout across the screen.
        const NormRect twice = fromPixels(toPixels(back, canvas), canvas);
        report("a second round trip does not drift", twice == back);
    }

    // ── Case 4: degenerate inputs are refused, not mangled ───────────────
    {
        report("zero canvas yields a null rect",
               toPixels(NormRect{0.0, 0.0, 1.0, 1.0}, QSize(0, 0)).isNull());
        report("zero canvas inverts to a default rect",
               fromPixels(QRect(0, 0, 10, 10), QSize(0, 0)) == NormRect{});

        // A canvas that has not been shown yet must not rewrite stored
        // placement — losing real layout to a transient size is worse than
        // doing nothing.
        const NormRect stored{0.25, 0.25, 0.5, 0.5};
        report("clamp leaves placement alone on a zero canvas",
               clampToCanvas(stored, QSize(160, 90), QSize(0, 0)) == stored);

        const double nan = std::numeric_limits<double>::quiet_NaN();
        report("clamp rejects a non-finite rect",
               clampToCanvas(NormRect{nan, 0.0, 0.5, 0.5}, QSize(0, 0), QSize(800, 600))
                   == NormRect{});

        report("zero-area rect is not valid", !NormRect{0.1, 0.1, 0.0, 0.5}.isValid());
        report("a normal rect is valid", NormRect{0.1, 0.1, 0.5, 0.5}.isValid());
    }

    // ── Case 5: a layout survives a change of display ────────────────────
    {
        // Authored on a 3840x1600 ultrawide...
        const QSize big(3840, 1600);
        const NormRect pan{0.0, 0.0, 0.72, 0.55};
        const QRect onBig = toPixels(pan, big);
        report("authored rect is correct on the big canvas",
               onBig == QRect(0, 0, 2765, 880));

        // ...restored on a 1920x1080 laptop: same proportions, different pitch.
        const QSize small(1920, 1080);
        const QRect onSmall = toPixels(pan, small);
        report("same rect keeps its proportions on a smaller canvas",
               nearly(onSmall.width()  / static_cast<double>(small.width()),
                      onBig.width()    / static_cast<double>(big.width()),   1e-3)
                   && nearly(onSmall.height() / static_cast<double>(small.height()),
                             onBig.height()   / static_cast<double>(big.height()), 1e-3));
        report("restored rect stays inside the smaller canvas",
               onSmall.right() < small.width() && onSmall.bottom() < small.height());
    }

    // ── Case 6: clamping keeps items reachable ───────────────────────────
    {
        const QSize canvas(1000, 1000);
        const QSize minPx(0, 0);

        // Overshooting the right edge slides the item back; it does not
        // resize under the cursor.
        const NormRect out = clampToCanvas(NormRect{0.9, 0.0, 0.3, 0.3}, minPx, canvas);
        report("an item pushed past the edge slides back, keeping its size",
               nearly(out.x, 0.7) && nearly(out.w, 0.3));

        const NormRect neg = clampToCanvas(NormRect{-0.5, -0.5, 0.4, 0.4}, minPx, canvas);
        report("an item pushed past the origin slides back",
               nearly(neg.x, 0.0) && nearly(neg.y, 0.0)
                   && nearly(neg.w, 0.4) && nearly(neg.h, 0.4));
    }

    // ── Case 7: minimum size ─────────────────────────────────────────────
    {
        const QSize canvas(1000, 1000);

        report("minimum maps into fractions",
               nearly(minimumNormSize(QSize(200, 100), canvas).width(),  0.2)
                   && nearly(minimumNormSize(QSize(200, 100), canvas).height(), 0.1));

        const NormRect grown =
            clampToCanvas(NormRect{0.0, 0.0, 0.05, 0.05}, QSize(200, 200), canvas);
        report("a too-small item is grown to the minimum",
               nearly(grown.w, 0.2) && nearly(grown.h, 0.2));

        // A canvas smaller than the minimum: the cap wins and the item fills
        // the surface.  An item that overflows a tiny window is recoverable;
        // one rounded to nothing is not.
        const NormRect capped =
            clampToCanvas(NormRect{0.3, 0.3, 0.1, 0.1}, QSize(200, 200), QSize(100, 100));
        report("a canvas smaller than the minimum caps at full size",
               nearly(capped.w, 1.0) && nearly(capped.h, 1.0)
                   && nearly(capped.x, 0.0) && nearly(capped.y, 0.0));
    }

    // ── Case 8: half-open containment ────────────────────────────────────
    {
        const NormRect r{0.25, 0.25, 0.5, 0.5};
        report("contains its top-left corner",        r.contains(0.25, 0.25));
        report("contains an interior point",          r.contains(0.5, 0.5));
        report("excludes its far edge",              !r.contains(0.75, 0.5));
        report("excludes a point outside",           !r.contains(0.1, 0.5));

        // Two items sharing an edge must not both claim a point on it, or a
        // click there would depend on iteration order.
        const NormRect left {0.0, 0.0, 0.5, 1.0};
        const NormRect right{0.5, 0.0, 0.5, 1.0};
        report("a point on a shared edge belongs to exactly one item",
               left.contains(0.5, 0.5) != right.contains(0.5, 0.5));
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
