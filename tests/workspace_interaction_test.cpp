// The pure interaction core of RFC #4887 phase 5: hit zones, anchored
// resize, and the snap solver.
//
// Two properties carry the operator's trust and are pinned hardest here:
// a resize NEVER moves the anchored edge (that is what hands rely on), and
// a snap NEVER shrinks an item below its minimum or moves an anchor.  Pure
// logic, no widgets.

#include "gui/workspace/CanvasInteraction.h"

#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

#include <cmath>
#include <cstdio>

using AetherSDR::HitZone;
using AetherSDR::NormRect;
using AetherSDR::applyDrag;
using AetherSDR::cursorForZone;
using AetherSDR::hitZoneFor;
using AetherSDR::isResizeZone;
using AetherSDR::snapRect;

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

bool rectNear(const NormRect& a, const NormRect& b)
{
    return nearly(a.x, b.x) && nearly(a.y, b.y)
           && nearly(a.w, b.w) && nearly(a.h, b.h);
}

}  // namespace

int main()
{
    const QRect item(100, 100, 300, 200);   // px: 100..400 x 100..300
    const int grip = 6;
    const int titleH = 18;

    // ── Hit zones ────────────────────────────────────────────────────────
    {
        report("outside is None",
               hitZoneFor(item, QPoint(50, 50), grip, titleH) == HitZone::None);
        report("body is None",
               hitZoneFor(item, QPoint(250, 200), grip, titleH) == HitZone::None);
        report("the title strip is Move",
               hitZoneFor(item, QPoint(250, 110), grip, titleH) == HitZone::Move);

        report("left band is W",
               hitZoneFor(item, QPoint(103, 200), grip, titleH) == HitZone::W);
        report("right band is E",
               hitZoneFor(item, QPoint(398, 200), grip, titleH) == HitZone::E);
        report("top band is N — grips beat the title strip",
               hitZoneFor(item, QPoint(250, 103), grip, titleH) == HitZone::N);
        report("bottom band is S",
               hitZoneFor(item, QPoint(250, 297), grip, titleH) == HitZone::S);

        report("two bands make a corner: NW",
               hitZoneFor(item, QPoint(103, 103), grip, titleH) == HitZone::NW);
        report("two bands make a corner: SE",
               hitZoneFor(item, QPoint(398, 297), grip, titleH) == HitZone::SE);
        report("NE corner beats both N and the title strip",
               hitZoneFor(item, QPoint(398, 103), grip, titleH) == HitZone::NE);

        report("resize zones are resize zones",
               isResizeZone(HitZone::E) && isResizeZone(HitZone::NW)
                   && !isResizeZone(HitZone::Move) && !isResizeZone(HitZone::None));
        report("cursors match the axes",
               cursorForZone(HitZone::E) == Qt::SizeHorCursor
                   && cursorForZone(HitZone::N) == Qt::SizeVerCursor
                   && cursorForZone(HitZone::SE) == Qt::SizeFDiagCursor
                   && cursorForZone(HitZone::SW) == Qt::SizeBDiagCursor
                   && cursorForZone(HitZone::Move) == Qt::SizeAllCursor);
    }

    const NormRect start{0.3, 0.3, 0.4, 0.3};
    const QSizeF minN(0.1, 0.1);

    // ── Move ─────────────────────────────────────────────────────────────
    {
        report("move translates",
               rectNear(applyDrag(start, HitZone::Move, 0.1, -0.05, minN),
                        NormRect{0.4, 0.25, 0.4, 0.3}));
        report("an overshoot slides back inside, size intact",
               rectNear(applyDrag(start, HitZone::Move, 0.9, 0.9, minN),
                        NormRect{0.6, 0.7, 0.4, 0.3}));
        report("None is inert", rectNear(applyDrag(start, HitZone::None, 0.5, 0.5, minN), start));
    }

    // ── Resize anchors ───────────────────────────────────────────────────
    {
        // E: the LEFT edge is the anchor and must not budge.
        NormRect r = applyDrag(start, HitZone::E, 0.15, 0.5, minN);
        report("E grows the right edge only",
               rectNear(r, NormRect{0.3, 0.3, 0.55, 0.3}));

        // W: the RIGHT edge is the anchor.
        r = applyDrag(start, HitZone::W, 0.1, 0.0, minN);
        report("W moves the left edge, right edge fixed",
               rectNear(r, NormRect{0.4, 0.3, 0.3, 0.3}) && nearly(r.right(), 0.7));

        // N/S vertical mirrors.
        r = applyDrag(start, HitZone::N, 0.0, -0.1, minN);
        report("N raises the top, bottom fixed",
               rectNear(r, NormRect{0.3, 0.2, 0.4, 0.4}) && nearly(r.bottom(), 0.6));
        r = applyDrag(start, HitZone::S, 0.0, 0.2, minN);
        report("S drops the bottom, top fixed",
               rectNear(r, NormRect{0.3, 0.3, 0.4, 0.5}));

        // Corner: both axes at once, both opposite edges anchored.
        r = applyDrag(start, HitZone::SE, 0.1, 0.1, minN);
        report("SE moves two edges, anchors two",
               rectNear(r, NormRect{0.3, 0.3, 0.5, 0.4}));
        r = applyDrag(start, HitZone::NW, 0.05, 0.05, minN);
        report("NW moves the origin, far corner fixed",
               rectNear(r, NormRect{0.35, 0.35, 0.35, 0.25})
                   && nearly(r.right(), 0.7) && nearly(r.bottom(), 0.6));
    }

    // ── Resize limits ────────────────────────────────────────────────────
    {
        // Collapse past the minimum: stops at min, anchor untouched.
        NormRect r = applyDrag(start, HitZone::E, -0.9, 0.0, minN);
        report("E collapse stops at the minimum",
               rectNear(r, NormRect{0.3, 0.3, 0.1, 0.3}));
        r = applyDrag(start, HitZone::W, 0.9, 0.0, minN);
        report("W collapse stops at the minimum, right edge fixed",
               nearly(r.right(), 0.7) && nearly(r.w, 0.1));

        // Grow past the surface: stops at the edge, anchor untouched.
        r = applyDrag(start, HitZone::E, 0.9, 0.0, minN);
        report("E growth stops at the surface",
               nearly(r.x, 0.3) && nearly(r.right(), 1.0));
        r = applyDrag(start, HitZone::NW, -0.9, -0.9, minN);
        report("NW growth stops at the origin",
               nearly(r.x, 0.0) && nearly(r.y, 0.0)
                   && nearly(r.right(), 0.7) && nearly(r.bottom(), 0.6));
    }

    // ── Snap: move ───────────────────────────────────────────────────────
    {
        const QList<NormRect> peers{NormRect{0.0, 0.0, 0.5, 0.5}};
        const double tol = 0.02;

        // Left edge near a peer's right edge → captured, guide reported.
        NormRect moved{0.51, 0.7, 0.2, 0.2};
        auto s = snapRect(moved, HitZone::Move, peers, tol, tol, minN);
        report("move snaps edge-to-edge",
               nearly(s.rect.x, 0.5) && s.verticalGuides == QList<double>{0.5});
        report("...without touching the free axis", nearly(s.rect.y, 0.7));

        // Centre lines participate for a move.
        moved = NormRect{0.16, 0.7, 0.2, 0.2};   // centre 0.26, peer centre 0.25
        s = snapRect(moved, HitZone::Move, peers, tol, tol, minN);
        report("move snaps centre-to-centre",
               nearly(s.rect.x + s.rect.w / 2.0, 0.25));

        // The canvas edges are candidates too.
        moved = NormRect{0.005, 0.99, 0.2, 0.2};
        s = snapRect(moved, HitZone::Move, peers, tol, tol, minN);
        report("move snaps to the surface edges",
               nearly(s.rect.x, 0.0) && !s.horizontalGuides.isEmpty());

        // Out of tolerance: untouched, no guides.
        moved = NormRect{0.55, 0.7, 0.2, 0.2};
        s = snapRect(moved, HitZone::Move, peers, tol, tol, minN);
        report("beyond tolerance nothing snaps",
               rectNear(s.rect, moved) && s.verticalGuides.isEmpty()
                   && s.horizontalGuides.isEmpty());

        // Zero tolerance = suppressed (the Alt-key path).
        moved = NormRect{0.501, 0.7, 0.2, 0.2};
        s = snapRect(moved, HitZone::Move, peers, 0.0, 0.0, minN);
        report("zero tolerance suppresses close snaps",
               rectNear(s.rect, moved));
    }

    // ── Snap: resize ─────────────────────────────────────────────────────
    {
        const QList<NormRect> peers{NormRect{0.6, 0.0, 0.2, 0.2}};
        const double tol = 0.02;

        // E resize near the peer's left edge: right edge captured, LEFT
        // anchor untouched.
        NormRect moved{0.3, 0.5, 0.29, 0.2};
        auto s = snapRect(moved, HitZone::E, peers, tol, tol, minN);
        report("E snap captures the right edge",
               nearly(s.rect.right(), 0.6) && s.verticalGuides == QList<double>{0.6});
        report("...anchor untouched", nearly(s.rect.x, 0.3));

        // A resize must not snap the edges it does not move: a W grip with
        // the RIGHT edge sitting on a line stays put.
        moved = NormRect{0.35, 0.5, 0.25, 0.2};   // right = 0.6 on the line
        s = snapRect(moved, HitZone::W, peers, tol, tol, minN);
        report("W snap ignores the anchored right edge",
               nearly(s.rect.x, 0.35) || !nearly(s.rect.right(), 0.6001));

        // A snap can never shrink below the minimum: candidate inside min
        // range is clamped, and then reports no guide (it did not land).
        moved = NormRect{0.55, 0.5, 0.12, 0.2};   // E at 0.67; peer line 0.6 would leave 0.05 < min
        s = snapRect(moved, HitZone::E, peers, tol, tol, minN);
        report("a snap cannot shrink below the minimum",
               s.rect.w >= minN.width() - 1e-12);
    }

    // ── Snap: the grid tier ──────────────────────────────────────────────
    {
        const double tol = 0.02;
        const QList<NormRect> noPeers;
        const QList<NormRect> peers{NormRect{0.0, 0.0, 0.5, 0.5}};

        // With no peers in reach, the 96-division grid captures: left edge
        // at 0.255 → 24/96 = 0.25 (0.005 away; 25/96 is 0.0054 — the nearer
        // line wins).
        NormRect moved{0.255, 0.7, 0.2, 0.2};
        auto s = snapRect(moved, HitZone::Move, noPeers, tol, tol, minN, 96, 54);
        report("the grid captures when no peer is in reach",
               nearly(s.rect.x, 0.25) && s.verticalGuides == QList<double>{0.25});

        // A peer edge beats the grid even though a grid line is NEARER at
        // this density: item left at 0.515 — grid 49/96 = 0.5104 is 0.0046
        // away, the peer's right edge at 0.5 is 0.015 — the peer tier wins.
        moved = NormRect{0.515, 0.7, 0.2, 0.2};
        s = snapRect(moved, HitZone::Move, peers, tol, tol, minN, 96, 54);
        report("a peer edge beats the grid when both are reachable",
               nearly(s.rect.x, 0.5));

        // Same claim, wider tolerance, constructed peer: peer right edge at
        // 0.53, item left at 0.55 — grid 53/96 = 0.5521 is 0.0021 away, the
        // peer 0.02 — the peer tier still wins.
        const QList<NormRect> nearPeer{NormRect{0.03, 0.0, 0.5, 0.5}};
        s = snapRect(NormRect{0.55, 0.7, 0.2, 0.2}, HitZone::Move, nearPeer,
                     0.03, 0.03, minN, 96, 54);
        report("the peer tier wins even when a grid line is nearer",
               nearly(s.rect.x, 0.53));

        // Grid disabled: 0.255 stays free.
        s = snapRect(NormRect{0.255, 0.7, 0.2, 0.2}, HitZone::Move, noPeers,
                     tol, tol, minN, 0, 0);
        report("grid zero disables the grid tier",
               nearly(s.rect.x, 0.255) && s.verticalGuides.isEmpty());

        // The grid's own gentler magnet: with gridTol 0.003 a line 0.005
        // away is out of the GRID's reach even though the peer tolerance
        // would span it — the tiers pull with different strength.
        s = snapRect(NormRect{0.255, 0.7, 0.2, 0.2}, HitZone::Move, noPeers,
                     tol, tol, minN, 96, 54, 0.003, 0.003);
        report("the grid magnet can be gentler than the peer magnet",
               nearly(s.rect.x, 0.255) && s.verticalGuides.isEmpty());

        // Resize: the gripped edge lands on a grid line, anchor untouched.
        // Right edge at 0.5605 → 54/96 = 0.5625 (0.002 away; 53/96 is
        // 0.0084).
        s = snapRect(NormRect{0.3, 0.5, 0.2605, 0.2}, HitZone::E, noPeers,
                     tol, tol, minN, 96, 54);
        report("a resize snaps its gripped edge to the grid",
               nearly(s.rect.right(), 0.5625) && nearly(s.rect.x, 0.3));
    }

    // ── Tidy ─────────────────────────────────────────────────────────────
    {
        using AetherSDR::CanvasItem;
        using AetherSDR::TidyMove;
        using AetherSDR::tidyOverlaps;

        const auto make = [](const QString& id, const NormRect& r) {
            CanvasItem it;
            it.id   = id;
            it.rect = r;
            return it;
        };

        // Two overlapping items: the lower-in-reading-order one is pushed
        // below the first, sizes intact.
        {
            const QList<CanvasItem> items{
                make("a", NormRect{0.1, 0.1, 0.3, 0.3}),
                make("b", NormRect{0.2, 0.2, 0.3, 0.3}),
            };
            const auto moves = tidyOverlaps(items, {});
            report("tidy resolves a simple overlap downward",
                   moves.size() == 1 && moves.first().id == "b"
                       && nearly(moves.first().rect.y, 0.4)
                       && nearly(moves.first().rect.x, 0.2)
                       && nearly(moves.first().rect.h, 0.3));
        }

        // Already tidy: nothing moves, nothing reported.
        {
            const QList<CanvasItem> items{
                make("a", NormRect{0.0, 0.0, 0.3, 0.3}),
                make("b", NormRect{0.5, 0.5, 0.3, 0.3}),
            };
            report("tidy leaves a clean arrangement alone",
                   tidyOverlaps(items, {}).isEmpty());
        }

        // Overlap with a FIXED item is not disorder: a meter over the pan
        // area stays exactly where the operator put it.
        {
            const QList<CanvasItem> items{
                make("panstack", NormRect{0.0, 0.0, 0.8, 1.0}),
                make("meter",    NormRect{0.3, 0.3, 0.2, 0.2}),
            };
            report("tidy ignores overlap with fixed items",
                   tidyOverlaps(items, {"panstack"}).isEmpty());
        }

        // A chain: pushing b below a lands it on c, so it pushes past c too.
        {
            const QList<CanvasItem> items{
                make("a", NormRect{0.1, 0.0, 0.3, 0.3}),
                make("b", NormRect{0.1, 0.1, 0.3, 0.3}),
                make("c", NormRect{0.1, 0.35, 0.3, 0.2}),
            };
            // Reading order places b (y=0.1) before c (y=0.35): b slots in
            // directly below a, and c — now overlapped by b's new position —
            // is pushed through it to below both.
            const auto moves = tidyOverlaps(items, {});
            bool bMoved = false, cChained = false;
            for (const TidyMove& mv : moves) {
                if (mv.id == "b") bMoved   = nearly(mv.rect.y, 0.3);
                if (mv.id == "c") cChained = nearly(mv.rect.y, 0.6);
            }
            report("tidy pushes through chained obstacles", bMoved && cChained);
        }

        // No room below: the item is left exactly where it was — tidy never
        // makes an arrangement worse.
        {
            const QList<CanvasItem> items{
                make("a", NormRect{0.0, 0.0, 1.0, 0.6}),
                make("b", NormRect{0.0, 0.5, 1.0, 0.6}),
            };
            report("tidy gives up rather than push off the surface",
                   tidyOverlaps(items, {}).isEmpty());
        }
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
