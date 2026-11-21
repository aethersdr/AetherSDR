#pragma once

#include <QLabel>
#include <QLayout>
#include <QMainWindow>
#include <QMargins>
#include <QStatusBar>

namespace AetherSDR {

inline bool updateStationLabelMinimumWidth(QLabel* label)
{
    label->ensurePolished();
    const int minimumWidth = label->sizeHint().width() + 2;
    if (label->minimumWidth() == minimumWidth) {
        return false;
    }
    label->setMinimumWidth(minimumWidth);
    return true;
}

// Messages do not hide the permanent row, so its width is valid during a notice.
inline void updateStatusBarWindowMinimum(QMainWindow* window, QWidget* row,
                                         QWidget* sizeGrip, bool minimalMode, int screenWidthCap)
{
    QStatusBar* bar = window->statusBar();
    if (minimalMode || !row || bar->isHidden()) {
        return;
    }
    if (QLayout* layout = row->layout()) {
        layout->activate();
    }
    row->updateGeometry();
    bar->updateGeometry();
    const int sizeGripAllowance = sizeGrip && sizeGrip->isVisible() ? sizeGrip->width() + 4 : 4;
    // The contents margins (the Windows 11 non-client band) come off the row's
    // width, and the grip sits inside them. 0 on every other platform.
    const QMargins margins = window->contentsMargins();
    const int statusMinWidth = row->minimumSizeHint().width() + sizeGripAllowance
        + margins.left() + margins.right();
    window->setMinimumSize(qBound(1024, statusMinWidth, qMax(1024, screenWidthCap)),
                           qMax(400, window->minimumHeight()));
}

} // namespace AetherSDR
