#pragma once

#include "WeatherRadarSource.h"

#include <QGeoView/QGVLayerTilesOnline.h>
#include <QElapsedTimer>
#include <QTimer>

namespace AetherSDR {

class WeatherRadarTileLayer final : public QGVLayerTilesOnline {
    Q_OBJECT

public:
    WeatherRadarTileLayer();
    ~WeatherRadarTileLayer() override;

    void setSource(const WeatherRadarSource& source);
    void setEnabled(bool enabled);
    const WeatherRadarSource& source() const { return m_source; }
    int displayedProviders() const;
    bool loadFailed() const { return isVisible() && m_loadFailed; }

signals:
    void providersChanged(int providers);
    void frameReady(const QDateTime& frameTime);
    void frameLoadFailed(const QDateTime& frameTime);

protected:
    QGVImage* createTileImage(const QGV::GeoTilePos& pos, const QImage& image) override;
    void onCamera(const QGVCameraState& oldState,
                  const QGVCameraState& newState) override;
    int minZoomlevel() const override;
    int maxZoomlevel() const override;
    int scaleToZoom(double scale) const override;
    QString tilePosToUrl(const QGV::GeoTilePos& tilePos) const override;

private:
    friend class WeatherRadarLoadingTest;
    void checkReadiness(qint64 elapsedMs, qint64 retryElapsedMs);
    void beginReadinessCheck();

    QHash<QObject*, int> m_imageProviders;
    WeatherRadarSource m_source;
    QTimer m_readinessTimer;
    QElapsedTimer m_readinessElapsed;
    QElapsedTimer m_retryElapsed;
    QString m_readyFrameId;
    quint64 m_failureBaseline{0};
    int m_retryCount{0};
    bool m_loadFailed{false};
};

} // namespace AetherSDR
