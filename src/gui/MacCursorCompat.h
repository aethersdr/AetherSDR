#pragma once

#include <Qt>

namespace AetherSDR {

// Qt 6.11's Cocoa plugin has no native NSCursor for SizeAllCursor and
// bitmap-renders it instead. On macOS 26 that fallback can trap in
// QImage::toCGImage(), so substitute the native open-hand cursor.
inline Qt::CursorShape macSafeCursorShape(Qt::CursorShape shape)
{
#ifdef Q_OS_MAC
    if (shape == Qt::SizeAllCursor) {
        return Qt::OpenHandCursor;
    }
#endif
    return shape;
}

} // namespace AetherSDR
