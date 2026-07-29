#pragma once

#include <Qt>

namespace AetherSDR {

// Qt 6.11's Cocoa plugin bitmap-renders cursor shapes without native NSCursor
// equivalents. On macOS 26 that fallback can trap in QImage::toCGImage()
// while realizing the cursor, so substitute shapes backed by native cursors.
inline Qt::CursorShape macSafeCursorShape(Qt::CursorShape shape)
{
#ifdef Q_OS_MAC
    switch (shape) {
    case Qt::SplitVCursor:
        return Qt::SizeVerCursor;
    case Qt::SplitHCursor:
        return Qt::SizeHorCursor;
    case Qt::SizeAllCursor:
        return Qt::OpenHandCursor;
    case Qt::SizeBDiagCursor:
    case Qt::SizeFDiagCursor:
        return Qt::SizeHorCursor;
    default:
        break;
    }
#endif
    return shape;
}

} // namespace AetherSDR
