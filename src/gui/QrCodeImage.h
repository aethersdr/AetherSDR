#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace AetherSDR {

// Encodes text as a QR code image for a phone camera: dark modules on a
// light ground with the four-module quiet zone the standard requires, each
// module modulePixels square, at medium error correction. Returns a null
// image when the text doesn't fit in a QR code.
QImage qrCodeImage(const QString& text, int modulePixels, const QColor& dark, const QColor& light);

}  // namespace AetherSDR
