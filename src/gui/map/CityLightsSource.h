#pragma once

#include "CityLightsShading.h"
#include "WeatherRadarViewGeometry.h"

#include <QImage>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace AetherSDR {

// One historical NASA image provider for both projections. Bounds are always
// north-positive EPSG:3857; QGeoView reflection happens only at presentation.
class CityLightsSource final : public QObject {
    Q_OBJECT
public:
    explicit CityLightsSource(QObject* parent = nullptr,
                              QNetworkAccessManager* network = nullptr);
    ~CityLightsSource() override;
    void setEnabled(bool enabled);
    void setView(const WeatherRadarViewGeometry& view);
    void setNightOnly(bool nightOnly);
    void setFaintLights(int percent);
    void setWarmth(int percent);
    const QImage& originalImage() const { return m_original; }
    QRectF originalBounds() const { return m_loaded.bounds; }
    const QImage& image() const { return m_image; }
    QRectF bounds() const { return m_imageBounds; }
    // True while image() does not yet reflect the current parameters:
    // a render is running, or one is queued behind it.
    bool renderPending() const { return m_rendering || m_renderAgain; }

    static WeatherRadarViewGeometry boundedView(const WeatherRadarViewGeometry& view);
    static QUrl imageUrl(const WeatherRadarViewGeometry& view);
    static QImage decode(const QByteArray& bytes, const QSize& expectedSize);
    static QImage nightImage(const QImage& image, const QRectF& bounds,
                             const QDateTime& time, bool nightOnly, int faintLights = 0, int warmth = 0);

signals:
    void imageChanged();
    void statusChanged(const QString& status);

private:
    void requestImage();
    void cancelRequest();
    void renderImage();
    QNetworkAccessManager* m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_debounce;
    QTimer m_clock;
    WeatherRadarViewGeometry m_view;
    WeatherRadarViewGeometry m_requested;
    WeatherRadarViewGeometry m_loaded;
    QImage m_original;
    QImage m_image;
    QRectF m_imageBounds;
    quint64 m_generation{0};
    int m_faintLights{CityLightsShading::kDefaultFaintLights};
    int m_warmth{CityLightsShading::kDefaultWarmth};
    bool m_enabled{false};
    bool m_nightOnly{true};
    bool m_rendering{false};
    bool m_renderAgain{false};
};

} // namespace AetherSDR
