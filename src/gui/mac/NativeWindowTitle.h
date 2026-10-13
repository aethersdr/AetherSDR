#pragma once

#include <QRectF>

class QWidget;

namespace AetherSDR::mac {

void updateNativeTitleVisibility(QWidget* window);
QRectF nativeCaptionBounds(const QWidget* window);

}
