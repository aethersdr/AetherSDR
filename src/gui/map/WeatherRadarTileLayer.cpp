#include "WeatherRadarTileLayer.h"
#include "WeatherRadarStyle.h"

#include <algorithm>

namespace AetherSDR {

WeatherRadarTileLayer::WeatherRadarTileLayer()
{
    setName(QStringLiteral("NOAA weather radar"));
    setTilesMarginWithZoomChange(1);
    setTilesMarginNoZoomChange(1);
    // Retain bounded fallback coverage in BOTH zoom directions. Transparent
    // tiles clip covered parent pixels, so rainfall opacity is applied once.
    setVisibleZoomLayersBelowCurrent(3);
    setVisibleZoomLayersAboveCurrent(3);
    setTransparentFallbackEnabled(true);
    setHorizontalWrapEnabled(true);
    setOpacity(kWeatherRadarOpacity);
    m_readinessTimer.setInterval(25);
    connect(&m_readinessTimer, &QTimer::timeout, this, [this] {
        const int pending = pendingRequestCount();
        if (pending == 0
            && failedTileRequestCount() > m_failureBaseline) {
            if (m_readinessElapsed.elapsed() < (m_loadFailed ? 5000 : 1000)) {
                return;
            }
            if (m_retryCount >= 3 && !m_loadFailed) {
                m_loadFailed = true;
                emit frameLoadFailed(m_source.frameTime());
            }
            m_retryCount = std::min(3, m_retryCount + 1);
            m_failureBaseline = failedTileRequestCount();
            m_readinessElapsed.restart();
            // Retain completed tiles and retry real null placeholders even
            // without camera movement. After reporting failure, keep retrying
            // quietly at a slower cadence until disabled or superseded.
            retryUnfinishedTiles();
            return;
        }
        // Camera processing is coalesced by QGeoView. Give it a short window
        // to enqueue requests; an all-cache hit legitimately stays at zero.
        if (pending == 0 && currentTilesComplete()
            && m_readinessElapsed.elapsed() >= 300) {
            m_readinessTimer.stop();
            m_loadFailed = false;
            m_readyFrameId = m_source.frameId();
            emit frameReady(m_source.frameTime());
        }
    });
}

void WeatherRadarTileLayer::setSource(const WeatherRadarSource& source)
{
    if (m_source.frameId() == source.frameId()) {
        return;
    }
    m_source = source;
    m_readyFrameId.clear();
    onClean();
    if (isVisible()) {
        beginReadinessCheck();
    }
    update();
}

void WeatherRadarTileLayer::setEnabled(bool enabled)
{
    if (isVisible() == enabled) {
        return;
    }
    if (!enabled) {
        onClean();
        m_readyFrameId.clear();
    }
    setVisible(enabled);
    if (enabled) {
        beginReadinessCheck();
        update();
    } else {
        m_readinessTimer.stop();
    }
}

void WeatherRadarTileLayer::beginReadinessCheck()
{
    m_loadFailed = false;
    if (m_readyFrameId == m_source.frameId()) {
        QTimer::singleShot(0, this, [this] {
            emit frameReady(m_source.frameTime());
        });
        return;
    }
    m_failureBaseline = failedTileRequestCount();
    m_retryCount = 0;
    m_readinessElapsed.restart();
    m_readinessTimer.start();
}

void WeatherRadarTileLayer::onCamera(
    const QGVCameraState& oldState, const QGVCameraState& newState)
{
    QGVLayerTilesOnline::onCamera(oldState, newState);
    if (isVisible() && oldState != newState) {
        // A frame that was complete at the previous zoom is not proof that
        // the newly visible high-resolution coverage succeeded.
        m_readyFrameId.clear();
        // A small pan can keep exactly the same completed tile set. Preserve
        // failure accounting too: a pan must not forgive an unfinished tile.
        m_readinessElapsed.restart();
        m_readinessTimer.start();
    }
}

int WeatherRadarTileLayer::minZoomlevel() const
{
    return m_source.minimumZoom();
}

int WeatherRadarTileLayer::maxZoomlevel() const
{
    return m_source.maximumZoom();
}

int WeatherRadarTileLayer::scaleToZoom(double scale) const
{
    // QGV rejects cameras outside a layer's zoom range instead of requesting
    // a capped level. Clamp the radar tile selection, NOT the map camera, so
    // enabling/refreshing radar at street zoom still fetches valid coverage.
    return std::clamp(QGVLayerTilesOnline::scaleToZoom(scale),
                      minZoomlevel(), maxZoomlevel());
}

QString WeatherRadarTileLayer::tilePosToUrl(
    const QGV::GeoTilePos& tilePos) const
{
    return m_source.tileUrl(tilePos.zoom(), tilePos.pos().x(),
                            tilePos.pos().y()).toString();
}

} // namespace AetherSDR
