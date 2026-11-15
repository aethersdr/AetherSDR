#pragma once

#include <QString>

class QWidget;

namespace AetherSDR {

// Drop-in replacement for QFileDialog::getOpenFileName() for picking a
// background image (#4717). Adds a thumbnail/grid view toggle, an image
// preview pane, and widens the filter to every format QImageReader can
// decode. Returns an empty string if the user cancels.
QString getBackgroundImagePath(QWidget* parent, const QString& caption);

} // namespace AetherSDR
