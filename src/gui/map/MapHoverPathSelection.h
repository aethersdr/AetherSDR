#pragma once

#include "MapView.h"

namespace AetherSDR::MapHoverPathSelection {

inline QVector<MapView::Marker> pathsForMarker(
    const QVector<MapView::Marker>& markers, int markerIndex)
{
    if (markerIndex < 0 || markerIndex >= markers.size()) {
        return {};
    }
    const MapView::Marker& marker = markers.at(markerIndex);
    if (!marker.hoverShowsPathGroup || marker.pathGroup.isEmpty()) {
        return marker.pathEnabled ? QVector<MapView::Marker>{marker}
                                  : QVector<MapView::Marker>{};
    }

    QVector<MapView::Marker> paths;
    for (const MapView::Marker& candidate : markers) {
        if (candidate.pathEnabled
            && candidate.pathGroup == marker.pathGroup) {
            paths.append(candidate);
        }
    }
    return paths;
}

} // namespace AetherSDR::MapHoverPathSelection
