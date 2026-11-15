#pragma once

// Undo Qt's caption-reserving restore clamp for a captionless window.
// QWidgetPrivate::checkRestoredGeometry() assumes a native title bar: it reserves
// PM_TitleBarHeight above the top and shrinks a work-area-filling window by
// 2 + PM_TitleBarHeight. The Windows custom frame removes the caption via
// WM_NCCALCSIZE, so both leave gaps (#4328). Platform-independent, so
// tests/window_geometry_restore_test.cpp runs everywhere. The startup fallback
// also uses it on every platform after Qt rejects a changed-monitor blob (#7076).

#include <QByteArray>
#include <QMargins>
#include <QRect>
#include <QSize>

class QWidget;

namespace AetherSDR {

// What QWidget::saveGeometry() recorded, reduced to the parts a re-anchor
// needs.  `normalRect` is the rect Qt itself applies to a non-maximized
// window: the v3 `geometry()` field when the blob has one, else
// `normalGeometry`.
struct SavedWindowGeometry
{
    QRect normalRect;
    bool  maximized  = false;
    bool  fullScreen = false;
};

// Parses a QWidget::saveGeometry() blob.  Returns false — leaving `out`
// untouched — for anything this build cannot trust: wrong magic, a major
// version newer than the one Qt writes today, a truncated stream, or an
// invalid rect. These are format guards; Qt can still reject a valid blob
// because its saved screen width differs from today's monitor (#7076).
bool parseSavedWindowGeometry(const QByteArray& geometryBlob,
                              SavedWindowGeometry* out);

// Re-runs the "keep the window on screen" clamp with a zero top margin: the
// frame is kept inside `availableGeometry` without reserving room for a
// caption that does not exist, and its size is preserved unless the work area
// genuinely cannot hold it.
QRect clampFrameToWorkArea(const QRect& savedFrame, const QRect& availableGeometry);

// QWidget::restoreGeometry() refuses an otherwise well-formed blob if the
// saved screen width differs greatly from the current screen. In that case
// center on the primary work area, keeping the saved window size when it fits,
// instead of trusting an arbitrary position left by construction (#7076).
// A malformed blob falls back to the window's current size. `frameMargins` is
// the window's non-client frame (title bar and borders): the returned client
// rect leaves room for it, so a native title bar stays inside the work area.
QRect fallbackGeometryForRejectedRestore(const QByteArray& geometryBlob,
                                         const QRect& primaryAvailableGeometry,
                                         const QSize& currentSize,
                                         const QMargins& frameMargins = {});

// Startup recovery for a blob that `window->restoreGeometry()` just refused:
// places `window` with fallbackGeometryForRejectedRestore() on
// `primaryAvailableGeometry` and re-applies the blob's maximized or full-screen
// state. Returns false, leaving the window alone, when no valid placement
// exists (no usable work area).
bool placeWindowAfterRejectedRestore(QWidget* window,
                                     const QByteArray& geometryBlob,
                                     const QRect& primaryAvailableGeometry);

}  // namespace AetherSDR
