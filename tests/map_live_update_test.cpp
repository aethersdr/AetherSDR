#include "gui/map/MapHoverPathSelection.h"
#include "gui/map/MapMarkerBatchItem.h"
#include "gui/map/MapPathBatchItem.h"
#include "gui/map/MapTerminatorItem.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVLayer.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVProjection.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cmath>
#include <functional>
#include <iostream>

namespace AetherSDR {

class MapBatchItemTestAccess {
public:
    static quint64 cacheKey(const MapMarkerBatchItem& item)
    {
        return item.m_cache.cacheKey();
    }

    static double cacheScale(const MapMarkerBatchItem& item)
    {
        return item.m_cacheScale;
    }

    static bool overviewRefreshScheduled(const MapMarkerBatchItem& item)
    {
        return item.m_overviewRefreshTimer.isActive();
    }

    static std::shared_ptr<std::atomic_bool> cacheCancellation(
        const MapMarkerBatchItem& item)
    {
        return item.m_cacheCancelled;
    }

    static void rebuildDetailCache(MapMarkerBatchItem& item)
    {
        item.rebuildCache();
    }

    static quint64 cacheKey(const MapPathBatchItem& item)
    {
        return item.m_cache.cacheKey();
    }

    static double cacheScale(const MapPathBatchItem& item)
    {
        return item.m_cacheScale;
    }

    static bool overviewRefreshScheduled(const MapPathBatchItem& item)
    {
        return item.m_overviewRefreshTimer.isActive();
    }

    static int projectedPathCount(const MapPathBatchItem& item)
    {
        return item.m_paths.size();
    }

    static std::shared_ptr<std::atomic_bool> cacheCancellation(
        const MapPathBatchItem& item)
    {
        return item.m_cacheCancelled;
    }

    static void rebuildDetailCache(MapPathBatchItem& item)
    {
        item.rebuildCache();
    }

    static quint64 imageKey(const MapTerminatorItem& item)
    {
        return item.m_image.cacheKey();
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool waitUntil(QApplication& app, const std::function<bool()>& predicate,
               int timeoutMs = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        app.processEvents();
        QThread::msleep(10);
    }
    app.processEvents();
    return predicate();
}

MapView::Marker marker(double lat, double lon, const QColor& color)
{
    MapView::Marker value;
    value.lat = lat;
    value.lon = lon;
    value.color = color;
    value.pathEnabled = true;
    value.hasPathOrigin = true;
    value.pathFromLat = 35.0;
    value.pathFromLon = -78.0;
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;

    QGVMap map;
    map.resize(900, 600);
    map.show();
    app.processEvents();
    const QRectF world = map.getProjection()->boundaryProjRect();
    map.cameraTo(QGVCameraActions(&map).scaleTo(world), false);

    auto* layer = new QGVLayer;
    map.addItem(layer);
    QVector<MapView::Marker> markers{
        marker(51.5, -0.1, Qt::red),
        marker(-33.9, 151.2, Qt::cyan),
    };
    auto* markerBatch = new MapMarkerBatchItem(
        markers, QColor(Qt::white), QColor(Qt::black));
    auto* pathBatch = new MapPathBatchItem(markers, true, 35.0, -78.0);
    auto* terminator = new MapTerminatorItem;
    layer->addItem(markerBatch);
    layer->addItem(pathBatch);
    layer->addItem(terminator);

    ok &= expect(waitUntil(app, [&] {
        return MapBatchItemTestAccess::cacheKey(*markerBatch) != 0
            && MapBatchItemTestAccess::cacheKey(*pathBatch) != 0;
    }), "initial detail caches complete");
    ok &= expect(waitUntil(app, [&] {
        return MapBatchItemTestAccess::imageKey(*terminator) != 0;
    }), "initial terminator image completes asynchronously");

    const quint64 markerCacheBefore =
        MapBatchItemTestAccess::cacheKey(*markerBatch);
    const quint64 pathCacheBefore =
        MapBatchItemTestAccess::cacheKey(*pathBatch);
    const double markerScaleBefore =
        MapBatchItemTestAccess::cacheScale(*markerBatch);
    const double pathScaleBefore =
        MapBatchItemTestAccess::cacheScale(*pathBatch);
    const int itemCountBefore = layer->countItems();
    const int pathCountBefore =
        MapBatchItemTestAccess::projectedPathCount(*pathBatch);
    const quint64 terminatorImageBefore =
        MapBatchItemTestAccess::imageKey(*terminator);

    markers.append(marker(40.7, -74.0, Qt::yellow));
    markerBatch->setMarkers(markers);
    pathBatch->setMarkers(markers, true, 35.0, -78.0);

    ok &= expect(layer->countItems() == itemCountBefore,
                 "live update reuses the existing scene batches");
    ok &= expect(MapBatchItemTestAccess::cacheKey(*markerBatch)
                     == markerCacheBefore,
                 "marker detail cache remains visible during refresh");
    ok &= expect(MapBatchItemTestAccess::cacheKey(*pathBatch)
                     == pathCacheBefore,
                 "path detail cache remains visible during refresh");
    ok &= expect(MapBatchItemTestAccess::projectedPathCount(*pathBatch)
                     == pathCountBefore,
                 "path geometry remains visible while replacement projects");
    ok &= expect(waitUntil(app, [&] {
        return MapBatchItemTestAccess::overviewRefreshScheduled(*markerBatch)
            && MapBatchItemTestAccess::overviewRefreshScheduled(*pathBatch);
    }), "slow overview refresh is scheduled without replacing detail");

    terminator->setDateTime(QDateTime::currentDateTimeUtc().addSecs(60));
    ok &= expect(MapBatchItemTestAccess::imageKey(*terminator)
                     == terminatorImageBefore,
                 "terminator image remains visible while replacement renders");
    ok &= expect(waitUntil(app, [&] {
        return MapBatchItemTestAccess::imageKey(*terminator)
            != terminatorImageBefore;
    }), "replacement terminator image completes asynchronously");

    ok &= expect(waitUntil(app, [&] {
        return MapBatchItemTestAccess::cacheKey(*markerBatch)
                   != markerCacheBefore
            && MapBatchItemTestAccess::cacheKey(*pathBatch)
                   != pathCacheBefore;
    }), "replacement detail caches complete");
    ok &= expect(std::abs(MapBatchItemTestAccess::cacheScale(*markerBatch)
                          - markerScaleBefore) < 1e-9,
                 "marker replacement preserves effective pixel scale");
    ok &= expect(std::abs(MapBatchItemTestAccess::cacheScale(*pathBatch)
                          - pathScaleBefore) < 1e-9,
                 "path replacement preserves effective pixel scale");
    ok &= expect(MapBatchItemTestAccess::projectedPathCount(*pathBatch)
                     == markers.size(),
                 "replacement path geometry contains every marker");

    QVector<MapView::Marker> groupedMarkers;
    MapView::Marker firstReceiver = marker(51.5, -0.1, Qt::red);
    firstReceiver.pathGroup = QStringLiteral("VE2AO");
    groupedMarkers.append(firstReceiver);
    MapView::Marker secondReceiver = marker(40.7, -74.0, Qt::yellow);
    secondReceiver.pathGroup = QStringLiteral("VE2AO");
    groupedMarkers.append(secondReceiver);
    MapView::Marker unrelated = marker(-33.9, 151.2, Qt::cyan);
    unrelated.pathGroup = QStringLiteral("OTHER");
    groupedMarkers.append(unrelated);
    MapView::Marker selectedStation;
    selectedStation.pathEnabled = false;
    selectedStation.pathGroup = QStringLiteral("VE2AO");
    selectedStation.hoverShowsPathGroup = true;
    groupedMarkers.append(selectedStation);
    ok &= expect(MapHoverPathSelection::pathsForMarker(
                     groupedMarkers, groupedMarkers.size() - 1).size() == 2,
                 "selected callsign hover includes every grouped path");
    ok &= expect(MapHoverPathSelection::pathsForMarker(
                     groupedMarkers, 0).size() == 1,
                 "receiver hover includes only its own path");
    MapView::Marker noHoverPath;
    noHoverPath.pathEnabled = false;
    groupedMarkers.append(noHoverPath);
    ok &= expect(MapHoverPathSelection::pathsForMarker(
                     groupedMarkers, groupedMarkers.size() - 1).isEmpty(),
                 "marker without a path does not create a hover batch");

    MapBatchItemTestAccess::rebuildDetailCache(*markerBatch);
    const std::shared_ptr<std::atomic_bool> supersededMarkerRender =
        MapBatchItemTestAccess::cacheCancellation(*markerBatch);
    MapBatchItemTestAccess::rebuildDetailCache(*markerBatch);
    ok &= expect(supersededMarkerRender != nullptr
                     && supersededMarkerRender->load(std::memory_order_relaxed),
                 "starting a marker detail render cancels its predecessor");

    MapBatchItemTestAccess::rebuildDetailCache(*pathBatch);
    const std::shared_ptr<std::atomic_bool> supersededPathRender =
        MapBatchItemTestAccess::cacheCancellation(*pathBatch);
    MapBatchItemTestAccess::rebuildDetailCache(*pathBatch);
    ok &= expect(supersededPathRender != nullptr
                     && supersededPathRender->load(std::memory_order_relaxed),
                 "starting a path detail render cancels its predecessor");

    return ok ? 0 : 1;
}
