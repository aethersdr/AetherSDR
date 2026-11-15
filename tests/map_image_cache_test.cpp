#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/Raster/QGVImage.h>

#include <QApplication>
#include <QGraphicsScene>
#include <QOpenGLWidget>
#include <QPainterPath>

#include <iostream>

namespace {

class VectorItem final : public QGVDrawItem {
public:
    QPainterPath projShape() const override
    {
        QPainterPath shape;
        shape.addRect(QRectF(0, 0, 10, 10));
        return shape;
    }
    void projPaint(QPainter*) override {}
};

QGraphicsItem* graphicsItem(QGVMap& map, QGVDrawItem* object)
{
    for (QGraphicsItem* item : map.geoView()->scene()->items()) {
        if (QGVMapQGItem::geoObjectFromQGItem(item) == object) {
            return item;
        }
    }
    return nullptr;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;
    // Real QGeoView item attachment, no tile provider, socket, visible window
    // or GL context. Cache policy is selected before the first paint.
    QGVMap rasterMap;
    auto* rasterImage = new QGVImage(); // QGVMap takes ownership.
    rasterMap.addItem(rasterImage);
    QGraphicsItem* rasterItem = graphicsItem(rasterMap, rasterImage);
    ok &= expect(rasterItem != nullptr
                     && rasterItem->cacheMode() == QGraphicsItem::DeviceCoordinateCache,
                 "raster image keeps the upstream device cache");

    QGVMap glMap;
    glMap.geoView()->setViewport(new QOpenGLWidget()); // View takes ownership.
    auto* glImage = new QGVImage();
    glMap.addItem(glImage);
    QGraphicsItem* glItem = graphicsItem(glMap, glImage);
    ok &= expect(glItem != nullptr
                     && glItem->cacheMode() == QGraphicsItem::NoCache,
                 "GL image must bypass the CPU device cache");

    QImage source(256, 256, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    const qint64 identity = source.cacheKey();
    glImage->loadImage(source);
    glImage->setGeometry(QRectF(0, 0, 1024, 1024));
    glImage->refresh();
    ok &= expect(glItem->cacheMode() == QGraphicsItem::NoCache,
                 "loading and refreshing a GL image preserve direct rendering");
    ok &= expect(glImage->getImage().cacheKey() == identity
                     && glImage->getImage().size() == source.size(),
                 "source image identity and resolution stay intact");

    auto* vector = new VectorItem();
    glMap.addItem(vector);
    QGraphicsItem* vectorItem = graphicsItem(glMap, vector);
    ok &= expect(vectorItem != nullptr
                     && vectorItem->cacheMode() == QGraphicsItem::DeviceCoordinateCache,
                 "non-image GL items keep the upstream device cache");
    return ok ? 0 : 1;
}
