#pragma once

#include <QGeoView/QGVDrawItem.h>

#include <QDateTime>
#include <QImage>
#include <QRectF>

#include <memory>

class QPainter;

namespace AetherSDR {

// Flat-map original-observation playback with bounded GPU uploads and retained
// texture slots. Pixels and their geographic bounds are replaced together.
class WeatherRadarPlaybackItem final : public QGVDrawItem {
    Q_OBJECT

public:
    WeatherRadarPlaybackItem();
    ~WeatherRadarPlaybackItem() override;

    bool setFrame(const QImage& image, const QDateTime& frameTime,
                  const QRectF& bounds);
    void preloadFrame(const QImage& image, const QDateTime& frameTime,
                      const QRectF& bounds);
    void acknowledgeFrame(quint64 presentationSequence);
    void clear();
    bool ready() const;

signals:
    void presented(quint64 presentationSequence);
    void framePreloaded(const QDateTime& frameTime);

protected:
    void onProjection(QGVMap* geoMap) override;
    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;

private:
    struct OpenGlResources;

    bool paintOpenGlComposite(QPainter* painter);
    void paintRasterFallback(QPainter* painter);
    void releaseOpenGlResources();
    void applyFrame(const QImage& image, const QDateTime& frameTime,
                    const QRectF& bounds);
    QImage m_current;
    QImage m_preloadImage;
    QDateTime m_currentFrameTime;
    QDateTime m_preloadFrameTime;
    QRectF m_bounds;
    QRectF m_preloadBounds;
    QRectF m_cameraBounds;
    quint64 m_pendingPresentationSequence{0};
    std::unique_ptr<OpenGlResources> m_openGlResources;
};

} // namespace AetherSDR
