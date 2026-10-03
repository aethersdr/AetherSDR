#pragma once

// Undo Qt's caption-reserving restore clamp for a captionless window.
// QWidgetPrivate::checkRestoredGeometry() assumes a native title bar: it reserves
// PM_TitleBarHeight above the top and shrinks a work-area-filling window by
// 2 + PM_TitleBarHeight. The Windows custom frame removes the caption via
// WM_NCCALCSIZE, so both leave gaps (#4328). Pure (blob layout + clamp math) so
// tests/window_geometry_restore_test.cpp runs everywhere; only Windows calls it.

#include <QByteArray>
#include <QRect>

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
// invalid rect.  The guards mirror restoreGeometry()'s own, so we never act on
// a blob Qt itself would have refused.
bool parseSavedWindowGeometry(const QByteArray& geometryBlob,
                              SavedWindowGeometry* out);

// Re-runs the "keep the window on screen" clamp with a zero top margin: the
// frame is kept inside `availableGeometry` without reserving room for a
// caption that does not exist, and its size is preserved unless the work area
// genuinely cannot hold it.
QRect clampFrameToWorkArea(const QRect& savedFrame, const QRect& availableGeometry);

}  // namespace AetherSDR
