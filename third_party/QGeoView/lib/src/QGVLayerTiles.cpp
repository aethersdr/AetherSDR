/***************************************************************************
 * QGeoView is a Qt / C ++ widget for visualizing geographic data.
 * Copyright (C) 2018-2025 Andrey Yaroshenko.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see https://www.gnu.org/licenses.
 ****************************************************************************/

#include "QGVLayerTiles.h"
#include "QGVDrawItem.h"

#include <QtMath>

QGVLayerTiles::QGVLayerTiles()
{
    mCurZoom = -1;
    mCameraUpdateTimer.setSingleShot(true);
    mCameraUpdateTimer.setInterval(100);
    connect(&mCameraUpdateTimer, &QTimer::timeout,
            this, &QGVLayerTiles::processCamera);
    sendToBack();
}

void QGVLayerTiles::setTilesMarginWithZoomChange(size_t value)
{
    mPerfomanceProfile.TilesMarginWithZoomChange = value;
    qgvDebug() << "TilesMarginWithZoomChange changed to" << value;
}

void QGVLayerTiles::setTilesMarginNoZoomChange(size_t value)
{
    mPerfomanceProfile.TilesMarginNoZoomChange = value;
    qgvDebug() << "TilesMarginNoZoomChange changed to" << value;
}

void QGVLayerTiles::setAnimationUpdateDelayMs(size_t value)
{
    mPerfomanceProfile.AnimationUpdateDelayMs = value;
    qgvDebug() << "AnimationUpdateDelayMs changed to" << value;
}

void QGVLayerTiles::setVisibleZoomLayersBelowCurrent(size_t value)
{
    mPerfomanceProfile.VisibleZoomLayersBelowCurrent = value;
    qgvDebug() << "VisibleZoomLayersBelowCurrent changed to" << value;
}

void QGVLayerTiles::setVisibleZoomLayersAboveCurrent(size_t value)
{
    mPerfomanceProfile.VisibleZoomLayersAboveCurrent = value;
    qgvDebug() << "VisibleZoomLayersAboveCurrent changed to" << value;
}

void QGVLayerTiles::setCameraUpdatesDuringAnimation(bool value)
{
    mPerfomanceProfile.CameraUpdatesDuringAnimation = value;
    qgvDebug() << "CameraUpdatesDuringAnimation changed to" << value;
}

void QGVLayerTiles::setHorizontalWrapEnabled(bool enabled)
{
    if (mHorizontalWrapEnabled == enabled) {
        return;
    }
    mHorizontalWrapEnabled = enabled;
    processCamera();
}

void QGVLayerTiles::onProjection(QGVMap* geoMap)
{
    QGVLayer::onProjection(geoMap);
}

void QGVLayerTiles::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    QGVLayer::onCamera(oldState, newState);

    if (oldState == newState) {
        return;
    }

    bool needUpdate = true;

    if (newState.animation()) {
        if (!mPerfomanceProfile.CameraUpdatesDuringAnimation) {
            needUpdate = false;
        } else if (!mLastAnimation.isValid()) {
            mLastAnimation.start();
        } else if (mLastAnimation.elapsed() < static_cast<qint64>(mPerfomanceProfile.AnimationUpdateDelayMs)) {
            needUpdate = false;
        } else {
            mLastAnimation.restart();
        }
    } else {
        mLastAnimation.invalidate();
    }

    if (needUpdate) {
        // Existing tile graphics follow the QGraphicsView transform
        // immediately. Rebuilding the active tile set for every fractional
        // trackpad or drag event only churns scene items on the GUI thread.
        // Coalesce that bookkeeping until input pauses; the final camera is
        // still processed, while interaction remains responsive.
        mCameraUpdateTimer.start();
    }
}

void QGVLayerTiles::onUpdate()
{
    QGVLayer::onUpdate();
    mCameraUpdateTimer.stop();
    processCamera();
}

void QGVLayerTiles::onClean()
{
    QGVLayer::onClean();
    mCameraUpdateTimer.stop();
    mCurZoom = -1;
    mCurRect = {};
    mIndex.clear();
    deleteItems();
}

void QGVLayerTiles::onTile(const QGV::GeoTilePos& tilePos, QGVDrawItem* tileObj)
{
    if (tilePos.zoom() != mCurZoom || !mCurRect.contains(tilePos.pos())) {
        delete tileObj;
        return;
    }
    addTile(tilePos, tileObj);

    removeAllAbove(tilePos);

    const int fromZoom = minZoomlevel();
    const int toZoom = tilePos.zoom() - 1;
    for (int zoom = fromZoom; zoom <= toZoom; ++zoom) {
        for (const QGV::GeoTilePos& below : existingTiles(zoom)) {
            if (below.contains(tilePos)) {
                removeWhenCovered(below);
            }
        }
    }
    if (mTransparentFallbackEnabled) {
        for (const auto& level : std::as_const(mIndex)) {
            for (QGVDrawItem* item : level) {
                if (item != nullptr) {
                    item->repaint();
                }
            }
        }
    }
}

QPainterPath QGVLayerTiles::tileUncoveredPath(const QGV::GeoTilePos& tilePos) const
{
    QPainterPath path;
    // Keep both lookups const: painting must not copy/refcount a level map
    // or detach the shared tile index.
    const auto levelIt = mIndex.constFind(tilePos.zoom());
    if (levelIt == mIndex.cend()) {
        return path;
    }
    const QGVDrawItem* tile = levelIt.value().value(tilePos, nullptr);
    if (tile == nullptr) {
        return path;
    }
    path = tile->projShape();
    // Transparent replacement pixels mean "no rain", NOT "show old rain
    // underneath". Remove the entire completed child footprint from parents.
    // Pending/failed children have no footprint and keep their parent visible.
    for (auto level = mIndex.upperBound(tilePos.zoom()); level != mIndex.cend(); ++level) {
        for (auto item = level->cbegin(); item != level->cend(); ++item) {
            if (item.value() != nullptr && tilePos.contains(item.key())) {
                path = path.subtracted(item.value()->projShape());
            }
        }
    }
    return path;
}

bool QGVLayerTiles::currentTilesComplete() const
{
    // A queued camera update still describes the previous viewport. Do not
    // publish that coverage until the coalesced tile selection has settled.
    if (mCameraUpdateTimer.isActive() || mCurZoom < 0 || mCurRect.isEmpty()) {
        return false;
    }
    for (int x = mCurRect.left(); x < mCurRect.right(); ++x) {
        for (int y = mCurRect.top(); y < mCurRect.bottom(); ++y) {
            if (!isTileFinished(QGV::GeoTilePos(mCurZoom, QPoint(x, y)))) {
                return false;
            }
        }
    }
    return true;
}

void QGVLayerTiles::retryUnfinishedTiles()
{
    if (getMap() == nullptr || !isVisible()) {
        return;
    }
    // Failed requests leave null placeholders in mIndex. They are not absent
    // tiles, and processCamera() is a no-op at the same zoom/rect. Retry only
    // these current-view placeholders; the online layer coalesces requests
    // still in flight and repeated world copies. Snapshot keys before calling
    // request(), since a decoded-cache hit can synchronously change mIndex.
    for (const QGV::GeoTilePos& tile : existingTiles(mCurZoom)) {
        if (mCurRect.contains(tile.pos()) && !isTileFinished(tile)) {
            request(tile);
        }
    }
}

int QGVLayerTiles::scaleToZoom(double scale) const
{
    const double scaleChange = 1 / scale;
    const int newZoom = qRound((17.0 - qLn(scaleChange) * M_LOG2E));
    return newZoom;
}

void QGVLayerTiles::processCamera()
{
    if (getMap() == nullptr || !isVisible()) {
        return;
    }
    const QGVProjection* projection = getMap()->getProjection();
    const QGVCameraState camera = getMap()->getCamera();
    QRectF areaProjRect = camera.projRect();
    const QRectF world = projection->boundaryProjRect();
    if (mHorizontalWrapEnabled) {
        areaProjRect.setTop(qMax(areaProjRect.top(), world.top()));
        areaProjRect.setBottom(qMin(areaProjRect.bottom(), world.bottom()));
    } else {
        areaProjRect = areaProjRect.intersected(world);
    }
    const QGV::GeoRect areaGeoRect = projection->projToGeo(areaProjRect);

    int originZoom = scaleToZoom(camera.scale());
    int newZoom = qMin(maxZoomlevel(), qMax(minZoomlevel(), originZoom));
    if (newZoom != originZoom) {
        return;
    }

    const bool zoomChanged = (mCurZoom != newZoom);
    mCurZoom = newZoom;

    const int margin = (zoomChanged) ? static_cast<int>(mPerfomanceProfile.TilesMarginWithZoomChange)
                                     : static_cast<int>(mPerfomanceProfile.TilesMarginNoZoomChange);
    const int sizePerZoom = static_cast<int>(qPow(2, mCurZoom));
    const QRect maxRect = QRect(QPoint(0, 0), QPoint(sizePerZoom, sizePerZoom));
    QPoint topLeft = QGV::GeoTilePos::geoToTilePos(mCurZoom, areaGeoRect.topLeft()).pos();
    QPoint bottomRight = QGV::GeoTilePos::geoToTilePos(mCurZoom, areaGeoRect.bottomRight()).pos();
    if (mHorizontalWrapEnabled) {
        const double tilesPerProjectionUnit = sizePerZoom / world.width();
        topLeft.setX(static_cast<int>(qFloor(
            (areaProjRect.left() - world.left()) * tilesPerProjectionUnit)));
        bottomRight.setX(static_cast<int>(qFloor(
            (areaProjRect.right() - world.left()) * tilesPerProjectionUnit)));
    }
    QRect activeRect = QRect(topLeft, bottomRight);
    activeRect = activeRect.adjusted(-margin, -margin, margin, margin);
    if (mHorizontalWrapEnabled) {
        activeRect.setTop(qMax(activeRect.top(), maxRect.top()));
        activeRect.setBottom(qMin(activeRect.bottom(), maxRect.bottom()));
    } else {
        activeRect = activeRect.intersected(maxRect);
    }
    const bool rectChanged = (!zoomChanged && (mCurRect != activeRect));
    mCurRect = activeRect;

    if (!zoomChanged && !rectChanged) {
        return;
    }

    if (zoomChanged) {
        qgvDebug() << "new active zoom" << mCurZoom;
        const int fromZoom = minZoomlevel();
        const int toZoom = maxZoomlevel();
        for (int zoom = fromZoom; zoom <= toZoom; ++zoom) {
            if (zoom == mCurZoom) {
                for (const QGV::GeoTilePos& current : existingTiles(zoom)) {
                    removeAllAbove(current);
                }
            } else {
                for (const QGV::GeoTilePos& nonCurrent : existingTiles(zoom)) {
                    if (!isTileFinished(nonCurrent)) {
                        qgvDebug() << "cancel non-finished" << nonCurrent;
                        removeTile(nonCurrent);
                    } else if (zoom < mCurZoom) {
                        removeWhenCovered(nonCurrent);
                    }
                    removeForPerfomance(nonCurrent);
                }
            }
        }
    }

    if (rectChanged) {
        qgvDebug() << "new active rect" << mCurRect.topLeft() << mCurRect.bottomRight();
        for (const QGV::GeoTilePos& tilePos : existingTiles(mCurZoom)) {
            if (!mCurRect.contains(tilePos.pos())) {
                qgvDebug() << "delete out of boundary view" << tilePos;
                removeTile(tilePos);
            }
        }
    }

    // AetherSDR patch: the cull above only touches mCurZoom. Tiles at the other
    // retained zoom levels are evicted solely by removeWhenCovered(),
    // removeAllAbove() and removeForPerfomance(), all of which key on coverage
    // or on zoom distance and never on position. Upstream that was safe because
    // tile x was clamped to [0, 2^zoom], so the set was finite by construction.
    // Horizontal wrap removes that clamp, so every world copy the camera
    // crosses leaves behind another band of fallback tiles that nothing ever
    // reclaims — unbounded growth on exactly the pan-forever path wrap adds.
    // Cull those positionally too, against the same rect.
    if (mHorizontalWrapEnabled && (zoomChanged || rectChanged)) {
        const int fromZoom = minZoomlevel();
        const int toZoom = maxZoomlevel();
        for (int zoom = fromZoom; zoom <= toZoom; ++zoom) {
            if (zoom == mCurZoom) {
                continue;
            }
            for (const QGV::GeoTilePos& tilePos : existingTiles(zoom)) {
                if (!overlapsActiveRect(tilePos)) {
                    qgvDebug() << "delete out of active rect" << tilePos;
                    removeTile(tilePos);
                }
            }
        }
    }

    QMultiMap<qreal, QGV::GeoTilePos> missing;
    for (int x = mCurRect.left(); x < mCurRect.right(); ++x) {
        for (int y = mCurRect.top(); y < mCurRect.bottom(); ++y) {
            const auto tilePos = QGV::GeoTilePos(mCurZoom, QPoint(x, y));
            if (isTileExists(tilePos)) {
                continue;
            }
            qreal radius = qSqrt(qPow(x - mCurRect.center().x(), 2) + qPow(y - mCurRect.center().y(), 2));
            missing.insert(radius, tilePos);
        }
    }

    for (const QGV::GeoTilePos& tilePos : missing) {
        addTile(tilePos, nullptr);
    }
}

// AetherSDR patch: does `tilePos` — which may be at any zoom level — cover any
// part of the current active rect? Used to cull off-screen fallback tiles once
// horizontal wrap makes tile x unbounded. Everything is done in qint64 so the
// span of a very coarse tile expressed in mCurZoom units cannot overflow.
bool QGVLayerTiles::overlapsActiveRect(const QGV::GeoTilePos& tilePos) const
{
    const int zoom = tilePos.zoom();
    if (zoom == mCurZoom) {
        return mCurRect.contains(tilePos.pos());
    }
    const int deltaZoom = qAbs(zoom - mCurZoom);
    if (deltaZoom > 30) {
        // Far outside any retained window; leave it to removeForPerfomance().
        return true;
    }
    const qint64 factor = Q_INT64_C(1) << deltaZoom;
    qint64 left = 0, right = 0, top = 0, bottom = 0;
    if (zoom > mCurZoom) {
        // Finer than the current level: many child tiles per current-level one.
        const auto floorDiv = [](qint64 value, qint64 by) {
            const qint64 q = value / by;
            return (value % by != 0 && (value < 0) != (by < 0)) ? q - 1 : q;
        };
        left = right = floorDiv(tilePos.pos().x(), factor);
        top = bottom = floorDiv(tilePos.pos().y(), factor);
    } else {
        // Coarser: this one tile spans `factor` current-level tiles per axis.
        left = static_cast<qint64>(tilePos.pos().x()) * factor;
        right = left + factor - 1;
        top = static_cast<qint64>(tilePos.pos().y()) * factor;
        bottom = top + factor - 1;
    }
    return right >= mCurRect.left() && left <= mCurRect.right()
            && bottom >= mCurRect.top() && top <= mCurRect.bottom();
}

void QGVLayerTiles::removeAllAbove(const QGV::GeoTilePos& tilePos)
{
    const int fromZoom = tilePos.zoom() + 1;
    const int toZoom = maxZoomlevel();
    for (int zoom = fromZoom; zoom <= toZoom; ++zoom) {
        for (const QGV::GeoTilePos& target : existingTiles(zoom)) {
            if (!tilePos.contains(target)) {
                continue;
            }
            qgvDebug() << "remove" << target << "above" << tilePos;
            removeTile(target);
        }
    }
}

void QGVLayerTiles::removeWhenCovered(const QGV::GeoTilePos& tilePos)
{
    const int zoomDelta = mCurZoom - tilePos.zoom();
    if (zoomDelta < 1 || zoomDelta > 30) {
        return;
    }
    // A tile has FOUR children per zoom (not 2^(delta+1), which only happens
    // to be correct for a single step). Fast multi-step zooms need all of them.
    const qint64 neededCount = qint64{1} << (2 * zoomDelta);
    qint64 count = neededCount;
    for (const QGV::GeoTilePos& current : existingTiles(mCurZoom)) {
        if (!tilePos.contains(current)) {
            continue;
        }
        if (!isTileFinished(current)) {
            continue;
        }
        count--;
        if (count == 0) {
            break;
        }
    }
    if (count == 0) {
        qgvDebug() << tilePos << "deleted by 100% coverage";
        removeTile(tilePos);
    } else {
        qgvDebug() << tilePos << "covered" << neededCount - count << "/" << neededCount;
    }
}

void QGVLayerTiles::removeForPerfomance(const QGV::GeoTilePos& tilePos)
{
    const auto minZoom = mCurZoom - static_cast<int>(mPerfomanceProfile.VisibleZoomLayersBelowCurrent);
    const auto maxZoom = mCurZoom + static_cast<int>(mPerfomanceProfile.VisibleZoomLayersAboveCurrent);

    if (tilePos.zoom() < minZoom || tilePos.zoom() > maxZoom) {
        qgvDebug() << "delete because of performance request" << minZoom << maxZoom;
        removeTile(tilePos);
    }
}

void QGVLayerTiles::addTile(const QGV::GeoTilePos& tilePos, QGVDrawItem* tileObj)
{
    if (isTileFinished(tilePos)) {
        delete tileObj;
        return;
    }
    if (tileObj == nullptr) {
        qgvDebug() << "request tile" << tilePos;
        mIndex[tilePos.zoom()][tilePos] = nullptr;
        request(tilePos);
    } else {
        qgvDebug() << "add tile" << tilePos;
        mIndex[tilePos.zoom()][tilePos] = tileObj;
        tileObj->setZValue(static_cast<qint16>(tilePos.zoom()));
        addItem(tileObj);
    }
}

void QGVLayerTiles::removeTile(const QGV::GeoTilePos& tilePos)
{
    const auto tile = mIndex[tilePos.zoom()].take(tilePos);
    if (tile == nullptr) {
        qgvDebug() << "cancel tile" << tilePos;
        cancel(tilePos);
    } else {
        qgvDebug() << "remove tile" << tilePos;
        delete tile;
    }
}

bool QGVLayerTiles::isTileExists(const QGV::GeoTilePos& tilePos) const
{
    return mIndex[tilePos.zoom()].contains(tilePos);
}

bool QGVLayerTiles::isTileFinished(const QGV::GeoTilePos& tilePos) const
{
    if (!isTileExists(tilePos)) {
        return false;
    }
    return mIndex[tilePos.zoom()][tilePos] != nullptr;
}

QList<QGV::GeoTilePos> QGVLayerTiles::existingTiles(int zoom) const
{
    return mIndex[zoom].keys();
}
