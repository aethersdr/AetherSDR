#include "gui/workspace/CanvasInteraction.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

double clampD(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

HitZone hitZoneFor(const QRect& itemPx, const QPoint& posPx,
                   int gripPx, int titleHPx)
{
    if (!itemPx.contains(posPx)) {
        return HitZone::None;
    }

    const bool nearL = posPx.x() - itemPx.left()  < gripPx;
    const bool nearR = itemPx.right() - posPx.x() < gripPx;
    const bool nearT = posPx.y() - itemPx.top()   < gripPx;
    const bool nearB = itemPx.bottom() - posPx.y() < gripPx;

    // Corners first: within two bands at once.  Without this priority the
    // top corners would be swallowed by the title strip and short items
    // would have no reachable corner grips at all.
    if (nearT && nearL) return HitZone::NW;
    if (nearT && nearR) return HitZone::NE;
    if (nearB && nearL) return HitZone::SW;
    if (nearB && nearR) return HitZone::SE;
    if (nearT) return HitZone::N;
    if (nearB) return HitZone::S;
    if (nearL) return HitZone::W;
    if (nearR) return HitZone::E;

    // The move strip sits below the top grip band.
    if (posPx.y() - itemPx.top() < titleHPx) {
        return HitZone::Move;
    }
    return HitZone::None;
}

Qt::CursorShape cursorForZone(HitZone zone)
{
    switch (zone) {
    case HitZone::Move:              return Qt::SizeAllCursor;
    case HitZone::N: case HitZone::S: return Qt::SizeVerCursor;
    case HitZone::E: case HitZone::W: return Qt::SizeHorCursor;
    case HitZone::NW: case HitZone::SE: return Qt::SizeFDiagCursor;
    case HitZone::NE: case HitZone::SW: return Qt::SizeBDiagCursor;
    case HitZone::None:              break;
    }
    return Qt::ArrowCursor;
}

bool isResizeZone(HitZone zone)
{
    return zone != HitZone::None && zone != HitZone::Move;
}

NormRect applyDrag(const NormRect& start, HitZone zone,
                   double dxNorm, double dyNorm, const QSizeF& minNorm)
{
    if (zone == HitZone::None) {
        return start;
    }

    if (zone == HitZone::Move) {
        NormRect out = start;
        out.x += dxNorm;
        out.y += dyNorm;
        return clampToBounds(out);
    }

    const bool west  = zone == HitZone::W || zone == HitZone::NW || zone == HitZone::SW;
    const bool east  = zone == HitZone::E || zone == HitZone::NE || zone == HitZone::SE;
    const bool north = zone == HitZone::N || zone == HitZone::NW || zone == HitZone::NE;
    const bool south = zone == HitZone::S || zone == HitZone::SW || zone == HitZone::SE;

    const double minW = clampD(minNorm.width(),  0.0, 1.0);
    const double minH = clampD(minNorm.height(), 0.0, 1.0);

    NormRect out = start;

    // Horizontal.  The anchored edge is a constant; only the gripped edge
    // moves, bounded by the surface on one side and the minimum on the other.
    if (east) {
        const double left = start.x;                       // anchor
        double right = start.right() + dxNorm;
        right = clampD(right, left + minW, 1.0);
        out.w = right - left;
    } else if (west) {
        const double right = start.right();                // anchor
        double left = start.x + dxNorm;
        left = clampD(left, 0.0, right - minW);
        out.x = left;
        out.w = right - left;
    }

    // Vertical, same shape.
    if (south) {
        const double top = start.y;                        // anchor
        double bottom = start.bottom() + dyNorm;
        bottom = clampD(bottom, top + minH, 1.0);
        out.h = bottom - top;
    } else if (north) {
        const double bottom = start.bottom();              // anchor
        double top = start.y + dyNorm;
        top = clampD(top, 0.0, bottom - minH);
        out.y = top;
        out.h = bottom - top;
    }

    return out;
}

SnapResult snapRect(const NormRect& moved, HitZone zone,
                    const QList<NormRect>& peers,
                    double tolNormX, double tolNormY,
                    const QSizeF& minNorm,
                    int gridX, int gridY,
                    double gridTolNormX, double gridTolNormY)
{
    SnapResult result;
    result.rect = moved;

    if (zone == HitZone::None || tolNormX < 0 || tolNormY < 0) {
        return result;
    }

    if (gridTolNormX < 0) gridTolNormX = tolNormX;
    if (gridTolNormY < 0) gridTolNormY = tolNormY;

    const bool move  = zone == HitZone::Move;
    const bool west  = zone == HitZone::W || zone == HitZone::NW || zone == HitZone::SW;
    const bool east  = zone == HitZone::E || zone == HitZone::NE || zone == HitZone::SE;
    const bool north = zone == HitZone::N || zone == HitZone::NW || zone == HitZone::NE;
    const bool south = zone == HitZone::S || zone == HitZone::SW || zone == HitZone::SE;

    // Candidate lines in two tiers.  Tier one: the surface's own edges and
    // every peer edge + centre.  Tier two: the grid.  The grid is consulted
    // only when tier one finds nothing — a neighbour's edge within reach
    // always beats a grid line.
    QList<double> xLines{0.0, 1.0};
    QList<double> yLines{0.0, 1.0};
    for (const NormRect& p : peers) {
        xLines << p.x << p.right() << p.x + p.w / 2.0;
        yLines << p.y << p.bottom() << p.y + p.h / 2.0;
    }
    QList<double> xGrid;
    QList<double> yGrid;
    for (int i = 1; i < gridX; ++i) {
        xGrid << i / double(gridX);
    }
    for (int i = 1; i < gridY; ++i) {
        yGrid << i / double(gridY);
    }

    // One axis at a time: find the nearest (candidate line, own edge) pair
    // within tolerance and take it.  A Move offers left/right/centre; a
    // resize offers only the gripped edge — the anchor must never move.
    struct Best {
        double distance;
        double target;
        double ownOffset;   // where the matched own-line sits relative to x/y
        bool   found = false;
    };

    const auto pickFrom = [](const QList<double>& lines,
                             const QList<double>& ownOffsets, double base,
                             double tolerance) {
        Best best{tolerance, 0.0, 0.0, false};
        for (double own : ownOffsets) {
            const double at = base + own;
            for (double line : lines) {
                const double d = std::fabs(line - at);
                if (d <= best.distance) {
                    best = Best{d, line, own, true};
                }
            }
        }
        return best;
    };
    // The grid tier deliberately offers FEWER own-lines than the peer tier:
    // only the origin (or the gripped edge).  With left, right AND centre
    // all courting the grid pitch at +/-8 px, nearly every position
    // captured somewhere and free placement needed Alt held permanently —
    // aligning edges and centres is what PEERS are for; the grid quantizes
    // where the item STARTS.
    const auto pick = [&pickFrom](const QList<double>& tierOne,
                                  const QList<double>& tierOneOffsets,
                                  const QList<double>& grid,
                                  const QList<double>& gridOffsets, double base,
                                  double tolerance, double gridTolerance) {
        const Best edges = pickFrom(tierOne, tierOneOffsets, base, tolerance);
        if (edges.found || grid.isEmpty()) {
            return edges;
        }
        return pickFrom(grid, gridOffsets, base, gridTolerance);
    };

    NormRect& r = result.rect;

    if (move) {
        const Best bx = pick(xLines, {0.0, r.w, r.w / 2.0}, xGrid, {0.0}, r.x, tolNormX, gridTolNormX);
        if (bx.found) {
            r.x = clampD(bx.target - bx.ownOffset, 0.0, 1.0 - r.w);
            result.verticalGuides << bx.target;
        }
        const Best by = pick(yLines, {0.0, r.h, r.h / 2.0}, yGrid, {0.0}, r.y, tolNormY, gridTolNormY);
        if (by.found) {
            r.y = clampD(by.target - by.ownOffset, 0.0, 1.0 - r.h);
            result.horizontalGuides << by.target;
        }
        return result;
    }

    const double minW = clampD(minNorm.width(),  0.0, 1.0);
    const double minH = clampD(minNorm.height(), 0.0, 1.0);

    if (east) {
        const Best b = pick(xLines, {0.0}, xGrid, {0.0}, r.right(), tolNormX, gridTolNormX);
        if (b.found) {
            const double right = clampD(b.target, r.x + minW, 1.0);
            if (std::fabs(right - b.target) < 1e-12) {
                result.verticalGuides << b.target;
            }
            r.w = right - r.x;
        }
    } else if (west) {
        const Best b = pick(xLines, {0.0}, xGrid, {0.0}, r.x, tolNormX, gridTolNormX);
        if (b.found) {
            const double right = r.right();
            const double left  = clampD(b.target, 0.0, right - minW);
            if (std::fabs(left - b.target) < 1e-12) {
                result.verticalGuides << b.target;
            }
            r.x = left;
            r.w = right - left;
        }
    }

    if (south) {
        const Best b = pick(yLines, {0.0}, yGrid, {0.0}, r.bottom(), tolNormY, gridTolNormY);
        if (b.found) {
            const double bottom = clampD(b.target, r.y + minH, 1.0);
            if (std::fabs(bottom - b.target) < 1e-12) {
                result.horizontalGuides << b.target;
            }
            r.h = bottom - r.y;
        }
    } else if (north) {
        const Best b = pick(yLines, {0.0}, yGrid, {0.0}, r.y, tolNormY, gridTolNormY);
        if (b.found) {
            const double bottom = r.bottom();
            const double top    = clampD(b.target, 0.0, bottom - minH);
            if (std::fabs(top - b.target) < 1e-12) {
                result.horizontalGuides << b.target;
            }
            r.y = top;
            r.h = bottom - top;
        }
    }

    return result;
}

QList<TidyMove> tidyOverlaps(const QList<CanvasItem>& items,
                             const QStringList& fixedIds)
{
    const auto overlaps = [](const NormRect& a, const NormRect& b) {
        return a.x < b.right() && b.x < a.right()
               && a.y < b.bottom() && b.y < a.bottom();
    };

    // Movable items in reading order (top-to-bottom, then left-to-right):
    // the order the eye scans is the order tidy preserves.
    QList<CanvasItem> movable;
    for (const CanvasItem& it : items) {
        if (!fixedIds.contains(it.id)) {
            movable.append(it);
        }
    }
    std::stable_sort(movable.begin(), movable.end(),
                     [](const CanvasItem& a, const CanvasItem& b) {
                         if (!qFuzzyCompare(a.rect.y + 1.0, b.rect.y + 1.0)) {
                             return a.rect.y < b.rect.y;
                         }
                         return a.rect.x < b.rect.x;
                     });

    QList<TidyMove> moves;
    QList<NormRect> placed;

    for (const CanvasItem& it : movable) {
        NormRect r = it.rect;

        // Push down past every already-placed rect it overlaps, repeating
        // until clear — a push can land it on a later obstacle.
        bool pushed = true;
        int guard = 0;
        while (pushed && ++guard < 64) {
            pushed = false;
            for (const NormRect& p : placed) {
                if (overlaps(r, p)) {
                    r.y    = p.bottom();
                    pushed = true;
                }
            }
        }

        // Off the bottom, or still colliding: leave the ORIGINAL placement.
        // Tidy never trades one overlap for a worse one.
        bool ok = r.bottom() <= 1.0 + 1e-9;
        if (ok) {
            for (const NormRect& p : placed) {
                if (overlaps(r, p)) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            placed.append(it.rect);
            continue;
        }

        placed.append(r);
        if (!(r == it.rect)) {
            moves.append(TidyMove{it.id, r});
        }
    }
    return moves;
}

}  // namespace AetherSDR
