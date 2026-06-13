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

#pragma once

#include "QGVCamera.h"
#include "QGVGlobal.h"
#include "QGVMapQGItem.h"
#include "QGVMapRubberBand.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGraphicsView>
#include <QMenu>
#include <QMimeData>

class QGVMap;

class QGV_LIB_DECL QGVMapQGView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit QGVMapQGView(QGVMap* geoMap);

    void setMouseActions(QGV::MouseActions actions);
    QGV::MouseActions getMouseActions() const;

    QGVCameraState getCamera() const;
    void cameraTo(const QGVCameraActions& actions, bool animation);
    double getMinScale() const;
    double getMaxScale() const;
    void setScaleLimits(double minScale, double maxScale);
    void cleanState();

Q_SIGNALS:
    void dropData(QPointF position, const QMimeData* dropData);

private:
    QRectF viewRect() const;
    void changeState(QGV::MapState state);
    void cameraScale(double scale);
    void cameraScale(const QRectF& projRect);
    void cameraRotate(double azimuth);
    void cameraMove(const QPointF& projPos);
    void blockCameraUpdate();
    void unblockCameraUpdate();
    void applyCameraUpdate(const QGVCameraState& oldState);

    void showTooltip(QHelpEvent* helpEvent);
    void zoomByWheel(QWheelEvent* event);
    void startMoving(QMouseEvent* event);
    void startMovingObject(QMouseEvent* event);
    void startSelectionRect(QMouseEvent* event);
    void stopSelectionRect(QMouseEvent* event);
    void stopMovingObject(QMouseEvent* event);
    void zoomArea(QMouseEvent* event, QRect areaRect);
    void selectObjectsByRect(QMouseEvent* event, QRect selRect);
    void objectClick(QMouseEvent* event);
    void objectDoubleClick(QMouseEvent* event);
    void moveForWheel(QMouseEvent* event);
    void moveForRect(QMouseEvent* event);
    void moveMap(QMouseEvent* event);
    void moveObject(QMouseEvent* event);
    void unselectAll(QMouseEvent* event);
    void showMenu(QMouseEvent* event);

    bool event(QEvent* event) override final;
    void wheelEvent(QWheelEvent* event) override final;
    void mousePressEvent(QMouseEvent* event) override final;
    void mouseReleaseEvent(QMouseEvent* event) override final;
    void mouseMoveEvent(QMouseEvent* event) override final;
    void mouseDoubleClickEvent(QMouseEvent* event) override final;
    void resizeEvent(QResizeEvent* event) override final;
    void showEvent(QShowEvent* event) override final;
    void keyPressEvent(QKeyEvent* event) override final;
    void dragEnterEvent(QDragEnterEvent* event) override final;
    void dragMoveEvent(QDragMoveEvent* event) override final;
    void dropEvent(QDropEvent* event) override final;
    void dragLeaveEvent(QDragLeaveEvent* event) override final;

private:
    QGVMap* mGeoMap;
    unsigned int mBlockUpdateCount;
    double mMinScale;
    double mMaxScale;
    double mScale;
    double mAzimuth;
    QGV::MouseActions mMouseActions;
    QRect mViewRect;
    QGV::MapState mState;
    QRect mWheelMouseArea;
    QPointF mWheelProjAnchor;
    double mWheelBestFactor;
    QPointF mMoveProjAnchor;
    QGVDrawItem* mMovingObject;
    QScopedPointer<QGraphicsScene> mQGScene;
    QScopedPointer<QGVMapRubberBand> mSelectionRect;
    QScopedPointer<QMenu> mContextMenu;
};
