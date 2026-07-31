#pragma once

// Undo Qt's caption-reserving restore clamp for a window that has no caption.
//
// QWidget::saveGeometry() writes a versioned blob; QWidget::restoreGeometry()
// runs it back through QWidgetPrivate::checkRestoredGeometry()
// (qtbase src/widgets/kernel/qwidget.cpp), which assumes a native title bar
// sits above the client rect.  It therefore
//   * reserves PM_TitleBarHeight above the top edge, and
//   * shrinks a window that would otherwise fill the work area by
//     2 + PM_TitleBarHeight.
// AetherSDR's Windows custom frame takes the caption away through
// WM_NCCALCSIZE (MainWindow::nativeEvent), so neither adjustment pays for
// anything real: the first shows up as a title-bar-sized gap above the window
// (#4328), the second as one below it.
//
// These two functions are the pure half of the correction — no widget, no
// platform, no Qt GUI — so the blob layout and the clamp arithmetic are unit
// tested on every platform even though only Windows calls them.
// See tests/window_geometry_restore_test.cpp.

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
