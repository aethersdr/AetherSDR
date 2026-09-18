#include "gui/map/WeatherRadarPlaybackItem.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QApplication>
#include <QGraphicsScene>
#include <QPainter>

#include <cmath>
#include <iostream>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    // Real radar item and QGeoView scene, with NO tile provider or sockets.
    // The offscreen platform exercises the production raster fallback; the
    // native app/bridge verifies the same world offsets on the GL path.
    QGVMap map;
    map.resize(1200, 600);
    map.show();
    app.processEvents();
    map.geoView()->setHorizontalWrapEnabled(true);
    const QRectF world = map.getProjection()->boundaryProjRect();
    const double scale = 600.0 / world.height();
    map.geoView()->setScaleLimits(scale, 16.0);
    auto* radar = new WeatherRadarPlaybackItem(); // Map owns the item.
    map.addItem(radar);
    bool ok = true;
    for (const int centerCopy : {-5, 0, 5}) {
        // QGeoView deliberately skips camera notifications for hidden items.
        // Play / a first globe-to-flat switch must refresh that camera before
        // deriving the radar's scene bounds, without needing another zoom.
        radar->setVisible(false);
        map.cameraTo(QGVCameraActions(&map).scaleTo(scale)
            .moveTo(QPointF(centerCopy * world.width(), 0)), false);
        app.processEvents();
        radar->setVisible(true);
        for (int frame = 0; frame < 2; ++frame) {
            QImage source(256, 256, QImage::Format_ARGB32_Premultiplied);
            source.fill(Qt::transparent);
            QPainter marker(&source);
            marker.fillRect(QRect(58, 58, 12, 12), frame == 0 ? Qt::red : Qt::blue);
            marker.end();
            const QDateTime time = QDateTime::fromSecsSinceEpoch(1700000000 + frame * 300);
            ok &= radar->setFrame(source, time, world);
            radar->acknowledgeFrame(0);
            app.processEvents();
            for (const int dpr : {1, 2}) {
                QImage pixels(1200 * dpr, 600 * dpr, QImage::Format_ARGB32_Premultiplied);
                pixels.setDevicePixelRatio(dpr);
                pixels.fill(Qt::transparent);
                QPainter painter(&pixels);
                const QRectF view = map.getCamera().projRect();
                map.geoView()->scene()->render(&painter, QRectF(0, 0, 1200, 600),
                                               view, Qt::IgnoreAspectRatio);
                painter.end();
                int visibleMarkers = 0;
                for (int copy = centerCopy - 3; copy <= centerCopy + 3; ++copy) {
                    // Independent geographic oracle: source marker center is
                    // one quarter across/down the world; copies are 360deg apart.
                    const double x = world.left() + world.width() * (.25 + copy);
                    const double y = world.top() + world.height() * .25;
                    const int px = std::lround((x - view.left()) / view.width() * pixels.width());
                    const int py = std::lround((y - view.top()) / view.height() * pixels.height());
                    if (px < 0 || px >= pixels.width() || py < 0 || py >= pixels.height()) {
                        continue;
                    }
                    ++visibleMarkers;
                    const QColor actual = pixels.pixelColor(px, py);
                    if (actual.alpha() < 180 || actual.alpha() > 210
                        || (frame == 0 ? actual.red() : actual.blue()) < 240) {
                        std::cerr << "FAIL: radar copy=" << copy << " frame=" << frame
                                  << " DPR=" << dpr << " missing/stale/overdrawn\n";
                        ok = false;
                    }
                }
                if (visibleMarkers < 2) {
                    std::cerr << "FAIL: fixture did not expose multiple copies\n";
                    ok = false;
                }
            }
        }
    }
    return ok ? 0 : 1;
}
