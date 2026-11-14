#pragma once

#include <QLabel>
#include <QLayout>
#include <QMainWindow>
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
    const int statusMinWidth = row->minimumSizeHint().width() + sizeGripAllowance;
    window->setMinimumSize(qBound(1024, statusMinWidth, qMax(1024, screenWidthCap)),
                           qMax(400, window->minimumHeight()));
}

} // namespace AetherSDR
