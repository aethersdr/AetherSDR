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
        processCamera();
    }
}

void QGVLayerTiles::onUpdate()
{
    QGVLayer::onUpdate();
    processCamera();
}

void QGVLayerTiles::onClean()
{
    QGVLayer::onClean();
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
    const QRectF areaProjRect = camera.projRect().intersected(projection->boundaryProjRect());
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
    const QPoint topLeft = QGV::GeoTilePos::geoToTilePos(mCurZoom, areaGeoRect.topLeft()).pos();
    const QPoint bottomRight = QGV::GeoTilePos::geoToTilePos(mCurZoom, areaGeoRect.bottomRight()).pos();
    QRect activeRect = QRect(topLeft, bottomRight);
    activeRect = activeRect.adjusted(-margin, -margin, margin, margin);
    activeRect = activeRect.intersected(maxRect);
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
    const int zoomDelta = mCurZoom - tilePos.zoom() + 1;
    const int neededCount = static_cast<int>(qPow(2, zoomDelta));
    int count = neededCount;
    for (const QGV::GeoTilePos& current : existingTiles(mCurZoom)) {
        if (!tilePos.contains(current)) {
            continue;
        }
        if (!isTileFinished(current)) {
            break;
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
