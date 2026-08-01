// Unit tests for the pure half of the #4328 frameless-window restore fix.
//
// The Windows custom frame removes the caption through WM_NCCALCSIZE, but
// QWidget::restoreGeometry() clamps as though the caption were still there.
// WindowGeometryRestore.{h,cpp} re-runs that clamp without the caption term.
// Both halves are platform-independent, so they are pinned here on every
// platform even though only Windows calls them.
//
// Case 4 is the load-bearing one: it drives a REAL QWidget through
// saveGeometry()/restoreGeometry() and asserts (a) that our parser agrees with
// what Qt wrote, and (b) that Qt's clamp really does move and shrink the
// window the way the fix assumes.  If a future Qt reorders the blob or drops
// the caption reservation, this fails rather than silently mis-placing the
// main window.

#include "gui/WindowGeometryRestore.h"

#include <QApplication>
#include <QDataStream>
#include <QIODevice>
#include <QRect>
#include <QScreen>
#include <QStyle>
#include <QWidget>

#include <cstdio>

using AetherSDR::SavedWindowGeometry;
using AetherSDR::clampFrameToWorkArea;
using AetherSDR::parseSavedWindowGeometry;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

// A QWidget::saveGeometry() blob, built field by field so the layout this
// build depends on is written down in exactly one place.
QByteArray makeBlob(const QRect& frame,
                    const QRect& normal,
                    const QRect& geometry,
                    bool maximized = false,
                    bool fullScreen = false,
                    quint16 majorVersion = 3,
                    int screenWidth = 1920)
{
    QByteArray blob;
    QDataStream s(&blob, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_4_0);
    s << quint32(0x1D9D0CB) << majorVersion << quint16(0)
      << frame << normal << qint32(0)
      << quint8(maximized ? 1 : 0) << quint8(fullScreen ? 1 : 0);
    if (majorVersion > 1) {
        s << qint32(screenWidth);
    }
    if (majorVersion > 2) {
        s << geometry;
    }
    return blob;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // ── 1. Parser: the happy path and the field it must pick ────────────────
    {
        // v3 blobs carry both normalGeometry and geometry(); Qt applies
        // geometry() to a non-maximized window, so we must read the same one.
        const QRect frame(100, 200, 800, 600);
        const QRect normal(111, 222, 800, 600);
        const QRect geometry(123, 234, 800, 600);
        SavedWindowGeometry saved;
        const bool ok = parseSavedWindowGeometry(
            makeBlob(frame, normal, geometry), &saved);
        report("v3 blob parses", ok);
        report("v3 blob yields geometry(), not normalGeometry",
               saved.normalRect == geometry);
        report("v3 blob reports normal state",
               !saved.maximized && !saved.fullScreen);

        // v2 has no geometry() field — fall back the way Qt does.
        SavedWindowGeometry legacy;
        const bool legacyOk = parseSavedWindowGeometry(
            makeBlob(frame, normal, geometry, false, false, /*majorVersion=*/2),
            &legacy);
        report("v2 blob parses", legacyOk);
        report("v2 blob falls back to normalGeometry",
               legacy.normalRect == normal);
    }

    // ── 2. Parser: every reject path ────────────────────────────────────────
    {
        const QRect r(0, 0, 800, 600);
        SavedWindowGeometry saved;

        report("empty blob rejected",
               !parseSavedWindowGeometry(QByteArray(), &saved));

        QByteArray badMagic = makeBlob(r, r, r);
        badMagic[0] = char(0xFF);
        report("wrong magic rejected", !parseSavedWindowGeometry(badMagic, &saved));

        report("future major version rejected",
               !parseSavedWindowGeometry(
                   makeBlob(r, r, r, false, false, /*majorVersion=*/4), &saved));

        QByteArray truncated = makeBlob(r, r, r);
        truncated.truncate(truncated.size() - 6);
        report("truncated blob rejected",
               !parseSavedWindowGeometry(truncated, &saved));

        report("invalid saved rect rejected",
               !parseSavedWindowGeometry(
                   makeBlob(r, r, QRect(10, 10, 0, 0)), &saved));

        report("null out pointer rejected",
               !parseSavedWindowGeometry(makeBlob(r, r, r), nullptr));

        // A rejected parse must not have scribbled on the caller's struct.
        SavedWindowGeometry untouched;
        untouched.normalRect = QRect(5, 6, 7, 8);
        parseSavedWindowGeometry(QByteArray("junk"), &untouched);
        report("rejected parse leaves the out param alone",
               untouched.normalRect == QRect(5, 6, 7, 8));

        // Maximized and fullscreen flags survive the round trip — the caller
        // uses them to leave those states entirely to Qt.
        SavedWindowGeometry maxed;
        parseSavedWindowGeometry(makeBlob(r, r, r, /*maximized=*/true), &maxed);
        report("maximized flag round-trips", maxed.maximized && !maxed.fullScreen);
        SavedWindowGeometry full;
        parseSavedWindowGeometry(makeBlob(r, r, r, false, /*fullScreen=*/true), &full);
        report("fullScreen flag round-trips", full.fullScreen && !full.maximized);
    }

    // ── 3. Clamp: no caption reservation, size preserved ────────────────────
    {
        const QRect work(0, 0, 1920, 1040);

        // The #4328 symptom: flush against the top edge must stay flush.
        report("top-edge window keeps its origin",
               clampFrameToWorkArea(QRect(288, 0, 1443, 839), work)
                   == QRect(288, 0, 1443, 839));

        // A window sized to the work area keeps every pixel of it.  Qt's clamp
        // would shave 2 + PM_TitleBarHeight here and leave a gap at the bottom.
        report("work-area-sized window keeps its size", clampFrameToWorkArea(work, work) == work);

        // An origin above the work area — a taskbar moved to the top, or the
        // monitors were rearranged — must come back down, or the custom title
        // bar (the only mouse drag handle) lands under the taskbar.
        report("origin above the work area is pulled down",
               clampFrameToWorkArea(QRect(288, -40, 1443, 839), work)
                   == QRect(288, 0, 1443, 839));
        report("work area with a top-docked taskbar is respected",
               clampFrameToWorkArea(QRect(288, 0, 1443, 839), QRect(0, 48, 1920, 992))
                   == QRect(288, 48, 1443, 839));

        // Overhanging edges are pulled in without resizing.
        report("right overhang slides left",
               clampFrameToWorkArea(QRect(1800, 100, 400, 300), work)
                   == QRect(1520, 100, 400, 300));
        report("bottom overhang slides up",
               clampFrameToWorkArea(QRect(100, 1000, 400, 300), work)
                   == QRect(100, 740, 400, 300));
        report("left overhang slides right",
               clampFrameToWorkArea(QRect(-200, 100, 400, 300), work)
                   == QRect(0, 100, 400, 300));

        // Too big for the screen: shrink to fit exactly, top-left flush.
        report("oversized window shrinks to the work area",
               clampFrameToWorkArea(QRect(-100, -100, 4000, 3000), work) == work);

        // Degenerate inputs pass through rather than producing a garbage rect.
        report("invalid saved rect passes through",
               clampFrameToWorkArea(QRect(), work) == QRect());
        report("invalid work area passes through",
               clampFrameToWorkArea(QRect(1, 2, 3, 4), QRect()) == QRect(1, 2, 3, 4));
    }

    // ── 4. Against the real Qt, not our idea of it ──────────────────────────
    {
        QScreen* sc = QGuiApplication::primaryScreen();
        const QRect work = sc->availableGeometry();
        const int frameHeight =
            QApplication::style()->pixelMetric(QStyle::PM_TitleBarHeight);

        // (a) Our parser must agree with what Qt actually wrote.
        QWidget w;
        w.setGeometry(work.left() + 40, work.top() + 60, work.width() / 2, work.height() / 2);
        w.show();
        const QRect before = w.geometry();
        SavedWindowGeometry saved;
        report("parser accepts a real saveGeometry() blob",
               parseSavedWindowGeometry(w.saveGeometry(), &saved));
        report("parser recovers the widget's own geometry", saved.normalRect == before);

        // (b) Qt's clamp really does reserve the caption.  A window flush with
        //     the top of the work area comes back pushed down by
        //     1 + PM_TitleBarHeight — the bug this fix exists to undo.
        QWidget top;
        top.setGeometry(work.left() + 100, work.top(), 400, 300);
        top.show();
        const QByteArray topBlob = top.saveGeometry();
        top.move(work.left() + 500, work.top() + 500);
        report("restoreGeometry accepts the blob", top.restoreGeometry(topBlob));
        report("Qt pushes a top-edge window down by 1 + PM_TitleBarHeight",
               top.geometry().top() == work.top() + 1 + frameHeight);

        // (c) …and our clamp puts it back where the user left it.
        SavedWindowGeometry topSaved;
        parseSavedWindowGeometry(topBlob, &topSaved);
        report("clamp restores the top edge Qt moved",
               clampFrameToWorkArea(topSaved.normalRect, work).top() == work.top());

        // (d) Qt also SHRINKS a work-area-sized window; the clamp must not.
        QWidget filled;
        filled.setGeometry(work);
        filled.show();
        const QByteArray filledBlob = filled.saveGeometry();
        filled.setGeometry(work.left() + 10, work.top() + 10, 200, 200);
        filled.restoreGeometry(filledBlob);
        report("Qt shrinks a work-area-sized window", filled.geometry().size() != work.size());
        SavedWindowGeometry filledSaved;
        parseSavedWindowGeometry(filledBlob, &filledSaved);
        report("clamp keeps a work-area-sized window whole",
               clampFrameToWorkArea(filledSaved.normalRect, work).size() == work.size());
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
