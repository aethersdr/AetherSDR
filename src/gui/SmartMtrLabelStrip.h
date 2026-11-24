#pragma once

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QRectF>

#include <cstring>

namespace AetherSDR {

// The SmartMTR value-label strip of the GPU static overlay. A meter value
// change restores the pixels saved under the labels, repaints the labels and
// everything stacked above them clipped to the strip, and uploads only the
// strip. SpectrumWidget::renderGpuFrame() and its unit test share this code.
namespace SmartMtrLabelStrip {

// The logical label band in device pixels, rounded outwards to whole pixels
// and kept inside the overlay image.
inline QRect devicePixels(const QRect& logical, qreal dpr, const QRect& imageRect)
{
    if (logical.isEmpty())
        return {};
    return QRectF(logical.x() * dpr, logical.y() * dpr,
                  logical.width() * dpr, logical.height() * dpr)
        .toAlignedRect().intersected(imageRect);
}

// The device-pixel band back in logical painter units. Clipping to the
// original logical rect instead drops a border row at 1.5x DPR when its top
// lands halfway through a pixel.
inline QRectF logicalClip(const QRect& px, qreal dpr)
{
    return QRectF(px.x() / dpr, px.y() / dpr, px.width() / dpr, px.height() / dpr);
}

// Copy the saved strip `base` (a tightly packed QImage::copy() of `px`) back
// into `overlay` byte for byte, so old glyph and antialiasing pixels cannot
// accumulate. Returns false and changes nothing when `base` no longer matches
// the band or the overlay.
inline bool restore(QImage& overlay, const QImage& base, const QRect& px)
{
    if (px.isEmpty() || base.size() != px.size()
        || base.format() != overlay.format() || overlay.depth() != 32
        || !overlay.rect().contains(px)) {
        return false;
    }
    const int bytesPerLine = px.width() * 4;
    for (int row = 0; row < px.height(); ++row) {
        std::memcpy(overlay.scanLine(row + px.top()) + px.left() * 4,
                    base.constScanLine(row),
                    static_cast<size_t>(bytesPerLine));
    }
    return true;
}

// restore(), then run `paint` (the labels and the foreground above them)
// clipped to the band. Returns false and changes nothing when restore() does.
template <typename Paint>
bool repaint(QImage& overlay, const QImage& base, const QRect& px, qreal dpr,
             Paint&& paint)
{
    if (!restore(overlay, base, px))
        return false;
    QPainter painter(&overlay);
    painter.setClipRect(logicalClip(px, dpr));
    paint(painter);
    return true;
}

} // namespace SmartMtrLabelStrip
} // namespace AetherSDR
