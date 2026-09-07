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

    void setSource(const WeatherRadarSource& source);
    void setEnabled(bool enabled);
    const WeatherRadarSource& source() const { return m_source; }
    bool loadFailed() const { return isVisible() && m_loadFailed; }

signals:
    void frameReady(const QDateTime& frameTime);
    void frameLoadFailed(const QDateTime& frameTime);

protected:
    void onCamera(const QGVCameraState& oldState,
                  const QGVCameraState& newState) override;
    int minZoomlevel() const override;
    int maxZoomlevel() const override;
    int scaleToZoom(double scale) const override;
    QString tilePosToUrl(const QGV::GeoTilePos& tilePos) const override;

private:
    void beginReadinessCheck();

    WeatherRadarSource m_source;
    QTimer m_readinessTimer;
    QElapsedTimer m_readinessElapsed;
    QString m_readyFrameId;
    quint64 m_failureBaseline{0};
    int m_retryCount{0};
    bool m_loadFailed{false};
};

} // namespace AetherSDR
