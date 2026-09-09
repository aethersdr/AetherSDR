#pragma once

#include <QGeoView/QGVDrawItem.h>
#include <QImage>

namespace AetherSDR {

// Static, non-interactive image overlay. Uses the radar PR's canonical world
// wrapping, but has no playback state or timers of its own.
class CityLightsItem final : public QGVDrawItem {
public:
    CityLightsItem();
    void setImage(const QImage& image, const QRectF& canonicalBounds);

protected:
    void onProjection(QGVMap* map) override;
    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;

private:
    QImage m_image;
    QRectF m_bounds;
    QRectF m_camera;
};

} // namespace AetherSDR
