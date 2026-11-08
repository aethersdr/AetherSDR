#include "QrCodeImage.h"

#include <qrcodegen.hpp>

#include <QPainter>

#include <stdexcept>

namespace AetherSDR {

QImage qrCodeImage(const QString& text, int modulePixels, const QColor& dark, const QColor& light)
{
    constexpr int kQuietZone = 4;
    if (modulePixels < 1) {
        return {};
    }
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize() + 2 * kQuietZone;
        QImage image(modules * modulePixels, modules * modulePixels, QImage::Format_RGB32);
        image.fill(light);
        QPainter p(&image);
        for (int y = 0; y < qr.getSize(); ++y) {
            for (int x = 0; x < qr.getSize(); ++x) {
                if (qr.getModule(x, y)) {
                    p.fillRect((x + kQuietZone) * modulePixels, (y + kQuietZone) * modulePixels,
                               modulePixels, modulePixels, dark);
                }
            }
        }
        return image;
    } catch (const qrcodegen::data_too_long&) {
        return {};
    }
}

}  // namespace AetherSDR
