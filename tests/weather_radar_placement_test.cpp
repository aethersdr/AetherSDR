#include "gui/map/WeatherRadarPainterGeometry.h"

#include <QImage>
#include <QPainter>

#include <array>
#include <cmath>
#include <iostream>

using namespace AetherSDR;

int main()
{
    // Real QPainter pixels are the oracle, not a second copy of the radar
    // transform. No GUI, network, radio, or GPU is needed for this regression.
    bool ok = true;
    for (const double dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        for (const double zoom : {0.5, 1.0, 2.0}) {
            QImage image(1200, 900, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.translate(41.0, 29.0); // Pan, deliberately nonzero.
            painter.scale(zoom, zoom);
            const QPointF center(80.0, 55.0);
            painter.fillRect(QRectF(center - QPointF(3, 3), QSizeF(6, 6)), Qt::white);
            // Include a nonzero viewport origin to catch offset regressions.
            const std::array<QRectF, 2> viewports{
                QRectF(0, 0, 1200, 900), QRectF(13, 19, 1000, 800)};
            std::array<QPointF, 2> clips;
            for (size_t index = 0; index < viewports.size(); ++index) {
                clips[index] = weatherRadarPainterClipPosition(painter, center, viewports[index]);
            }
            painter.end();

            // Locate the marker Qt actually drew, independent of its matrix.
            double xTotal = 0.0;
            double yTotal = 0.0;
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(image.pixel(x, y)) != 0) {
                        xTotal += x + 0.5;
                        yTotal += y + 0.5;
                        ++count;
                    }
                }
            }
            if (count == 0) {
                std::cerr << "FAIL: QPainter reference marker is missing\n";
                return 1;
            }
            for (size_t index = 0; index < viewports.size(); ++index) {
                const QRectF& viewport = viewports[index];
                const QPointF& clip = clips[index];
                // GPU viewport mapping back to physical top-left pixels.
                const QPointF radarPixel(
                    viewport.left() + (clip.x() + 1.0) * 0.5 * viewport.width(),
                    viewport.top() + (1.0 - clip.y()) * 0.5 * viewport.height());
                if (std::abs(radarPixel.x() - xTotal / count) > 0.75
                    || std::abs(radarPixel.y() - yTotal / count) > 0.75) {
                    std::cerr << "FAIL: radar/Qt pixel alignment at DPR=" << dpr
                              << " zoom=" << zoom << '\n';
                    ok = false;
                }
            }
        }
    }
    return ok ? 0 : 1;
}
