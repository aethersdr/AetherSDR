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

#include "QGVMapQGView.h"
#include "QGVDrawItem.h"
#include "QGVMap.h"
#include "QGVMapQGItem.h"
#include "QGVMapQGView.h"
#include "QGVMapRubberBand.h"
#include "QGVWidget.h"

#include <QApplication>
#include <QParallelAnimationGroup>
#include <QScrollBar>
#include <QSequentialAnimationGroup>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

namespace {
int wheelAreaMargin = 10;
double wheelExponentDown = qPow(2, 1.0 / 2.0);
double wheelExponentUp = qPow(2, 1.0 / 1.5);
}

QGVMapQGView::QGVMapQGView(QGVMap* geoMap)
    : QGraphicsView(geoMap)
{
    Q_ASSERT(geoMap);
    mGeoMap = geoMap;
    mBlockUpdateCount = 0;
    mMinScale = 1e-8;
    mMaxScale = 1e+2;
    mScale = 1.0;
    mAzimuth = 0.0;
    mMouseActions = QGV::MouseAction::All;
    mViewRect = viewport()->rect();
    mState = QGV::MapState::Idle;
    mQGScene.reset(new QGraphicsScene(this));
    mSelectionRect.reset(new QGVMapRubberBand(this));
    mSelectionRect->setMinSelection(QSize(5, 5));
    mContextMenu.reset(new QMenu(this));
    setScene(mQGScene.data());
    setContextMenuPolicy(Qt::NoContextMenu);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setOptimizationFlag(DontSavePainterState, true);
    setOptimizationFlag(DontAdjustForAntialiasing, true);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setRenderHint(QPainter::Antialiasing, true);
    setCacheMode(QGraphicsView::CacheBackground);
    setMouseTracking(true);
    setBackgroundBrush(QBrush(Qt::lightGray));
    setAcceptDrops(true);
}

void QGVMapQGView::setMouseActions(QGV::MouseActions actions)
{
    mMouseActions = actions;
}

QGV::MouseActions QGVMapQGView::getMouseActions() const
{
    return mMouseActions;
}

QGVCameraState QGVMapQGView::getCamera() const
{
    const bool animation = mState == QGV::MapState::Animation;
    return QGVCameraState(mGeoMap, mAzimuth, mScale, viewRect(), animation);
}

void QGVMapQGView::cameraTo(const QGVCameraActions& actions, bool animation)
{
    const QGVCameraState oldState = getCamera();
    blockCameraUpdate();
    changeState((animation) ? QGV::MapState::Animation : QGV::MapState::Idle);
    cameraScale(actions.scale());
    cameraMove(actions.projCenter());
    cameraRotate(actions.azimuth());
    unblockCameraUpdate();
    applyCameraUpdate(oldState);
}

double QGVMapQGView::getMinScale() const
{
    return mMinScale;
}

double QGVMapQGView::getMaxScale() const
{
    return mMaxScale;
}

void QGVMapQGView::setScaleLimits(double minScale, double maxScale)
{
    mMinScale = minScale;
    mMaxScale = maxScale;
    cameraScale(mScale);
}

void QGVMapQGView::cleanState()
{
    changeState(QGV::MapState::Idle);
}

QRectF QGVMapQGView::viewRect() const
{
    return mapToScene(mViewRect).boundingRect();
}

void QGVMapQGView::changeState(QGV::MapState state)
{
    if (mState == state) {
        return;
    }
    if (mState == QGV::MapState::Animation) {
        QGVCameraState oldCamera = getCamera();
        mState = state;
        applyCameraUpdate(oldCamera);
    } else {
        mState = state;
    }
    if (mState == QGV::MapState::Idle) {
        mWheelMouseArea = QRect();
        mWheelProjAnchor = QPointF();
        mWheelBestFactor = getMinScale();
        mMoveProjAnchor = QPointF();
        mMovingObject = nullptr;
        mSelectionRect->hideRect();
    }
    mGeoMap->onMapState(mState);
}

void QGVMapQGView::cameraScale(double scale)
{
    const QGVCameraState oldState = getCamera();
    const double oldScale = mScale;
    const double newScale = qMax(mMinScale, qMin(mMaxScale, scale));
    if (qFuzzyCompare(oldScale, newScale)) {
        return;
    }
    const double deltaScale = newScale / oldScale;
    QGraphicsView::scale(deltaScale, deltaScale);
    mScale = newScale;
    applyCameraUpdate(oldState);
    qgvDebug() << "cameraScale" << scale;
}

void QGVMapQGView::cameraRotate(double azimuth)
{
    const QGVCameraState oldState = getCamera();
    const double oldAzimuth = fmod(mAzimuth, 360);
    const double newAzimuth = fmod(azimuth, 360);
    if (qFuzzyCompare(oldAzimuth, newAzimuth)) {
        return;
    }
    QGraphicsView::rotate(newAzimuth - oldAzimuth);
    mAzimuth = newAzimuth;
    applyCameraUpdate(oldState);
    qgvDebug() << "cameraRotate" << azimuth;
}

void QGVMapQGView::cameraMove(const QPointF& projPos)
{
    const QGVCameraState oldState = getCamera();
    const QPointF oldCenter = viewRect().center();
    if (oldCenter != projPos) {
        QGraphicsView::centerOn(projPos);
        applyCameraUpdate(oldState);
        qgvDebug() << "cameraMove" << projPos;
    }
}

void QGVMapQGView::blockCameraUpdate()
{
    mBlockUpdateCount++;
}

void QGVMapQGView::unblockCameraUpdate()
{
    if (mBlockUpdateCount == 0) {
        return;
    }
    mBlockUpdateCount--;
}

void QGVMapQGView::applyCameraUpdate(const QGVCameraState& oldState)
{
    if (mBlockUpdateCount > 0) {
        return;
    }
    QGVCameraState newState = getCamera();
    if (oldState == newState) {
        return;
    }
    mGeoMap->onMapCamera(oldState, newState);
}

void QGVMapQGView::showTooltip(QHelpEvent* helpEvent)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Tooltip)) {
        return;
    }
    helpEvent->accept();
    const QPointF projMouse = mapToScene(helpEvent->pos());
    QGraphicsItem* item = itemAt(helpEvent->pos());
    QGVDrawItem* geoObject = QGVMapQGItem::geoObjectFromQGItem(item);
    QString toolTip = QString();
    if (geoObject != nullptr) {
        toolTip = geoObject->projTooltip(projMouse);
    }
    if (!toolTip.isEmpty()) {
        QToolTip::showText(helpEvent->globalPos(), toolTip);
    } else {
        QToolTip::hideText();
    }
}

void QGVMapQGView::zoomByWheel(QWheelEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::ZoomWheel)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPoint eventPos(qRound(event->position().x()), qRound(event->position().y()));
    int eventDelta = event->angleDelta().y();
    if (eventDelta == 0) {
        eventDelta = event->pixelDelta().y();
    }
#else
    const QPoint eventPos(event->pos().x(), event->pos().y());
    const auto eventDelta = event->delta();
#endif

    if (mState != QGV::MapState::Wheel) {
        changeState(QGV::MapState::Wheel);
        mWheelMouseArea = QRect(eventPos, QSize(1, 1))
                                  .adjusted(-wheelAreaMargin, -wheelAreaMargin, wheelAreaMargin, wheelAreaMargin);
        mWheelProjAnchor = mapToScene(eventPos);
        mWheelBestFactor = mScale;
    } else {
        if (mWheelBestFactor < mScale) {
            mWheelProjAnchor = mapToScene(eventPos);
            mWheelBestFactor = mScale;
        }
    }

    const QGVCameraState oldState = getCamera();
    blockCameraUpdate();
    double newScale = mScale;

    if (eventDelta > 0) {
        newScale *= qPow(wheelExponentDown, eventDelta / 120.0);
    } else if (eventDelta < 0) {
        newScale /= qPow(wheelExponentUp, -eventDelta / 120.0);
    }
    cameraScale(newScale);

    const QPointF projMouse = mapToScene(eventPos);
    const double xDelta = (projMouse.x() - mWheelProjAnchor.x());
    const double yDelta = (projMouse.y() - mWheelProjAnchor.y());
    if (!qFuzzyIsNull(xDelta) || !qFuzzyIsNull(yDelta)) {
        cameraMove(viewRect().center() - QPointF(xDelta, yDelta));
    }
    unblockCameraUpdate();
    applyCameraUpdate(oldState);
}

void QGVMapQGView::startMoving(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Move)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    changeState(QGV::MapState::MovingMap);
    mMoveProjAnchor = mapToScene(event->pos());
}

void QGVMapQGView::startMovingObject(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::MoveObjects)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    const QPointF projPos = mapToScene(event->pos());
    auto geoObjects = mGeoMap->search(projPos, Qt::ContainsItemShape);
    if (geoObjects.isEmpty()) {
        return;
    }
    auto* geoObject = geoObjects.first();
    if (!geoObject->isFlag(QGV::ItemFlag::Movable)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    mMovingObject = geoObject;
    mMovingObject->projOnObjectStartMove(projPos);
    changeState(QGV::MapState::MovingObjects);
}

void QGVMapQGView::startSelectionRect(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Selection) && !mMouseActions.testFlag(QGV::MouseAction::ZoomRect)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    changeState(QGV::MapState::SelectionRect);
    mSelectionRect->setStartPoint(event->pos());
    mSelectionRect->showRect();
}

void QGVMapQGView::stopSelectionRect(QMouseEvent* event)
{
    const QRect rect = mSelectionRect->getRect();
    changeState(QGV::MapState::Idle);
    if (event->modifiers() == Qt::NoModifier) {
        zoomArea(event, rect);
    } else if (event->modifiers() == Qt::ControlModifier || event->modifiers() == Qt::ShiftModifier) {
        selectObjectsByRect(event, rect);
    }
}

void QGVMapQGView::stopMovingObject(QMouseEvent* event)
{
    Q_ASSERT(mMovingObject);
    const QPointF projPos = mapToScene(event->pos());
    mMovingObject->projOnObjectStopMove(projPos);
    changeState(QGV::MapState::Idle);
}

void QGVMapQGView::zoomArea(QMouseEvent* event, QRect areaRect)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::ZoomRect)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    const QRectF newProjRect = mapToScene(areaRect).boundingRect();
    const QGVCameraState oldState = getCamera();
    const QRectF oldProjRect = oldState.projRect();
    const double scaleFactor =
            qMin(qAbs(oldProjRect.width() / newProjRect.width()), qAbs(oldProjRect.height() / newProjRect.height()));
    auto fly =
            new QGVCameraSimpleAnimation(QGVCameraActions(mGeoMap).scaleBy(scaleFactor).moveTo(newProjRect.center()));
    fly->setDuration(1500);
    fly->start(QAbstractAnimation::DeleteWhenStopped);
}

void QGVMapQGView::selectObjectsByRect(QMouseEvent* event, QRect selRect)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Selection)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    if (event->modifiers() == Qt::ShiftModifier) {
        mGeoMap->unselectAll();
    }

    const QPolygonF projSelPolygon = QPolygonF() << mapToScene(selRect.topLeft()) << mapToScene(selRect.topRight())
                                                 << mapToScene(selRect.bottomRight())
                                                 << mapToScene(selRect.bottomLeft()) << mapToScene(selRect.topLeft());

    auto selList = mGeoMap->search(projSelPolygon, Qt::ContainsItemShape);
    for (auto* geoObject : selList) {
        geoObject->setSelected(!geoObject->isSelected());
    }
}

void QGVMapQGView::objectClick(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF projPos = mapToScene(event->pos());
    auto geoObjects = mGeoMap->search(projPos, Qt::ContainsItemShape);
    if (geoObjects.isEmpty()) {
        return;
    }
    auto* geoObject = geoObjects.first();
    if (mMouseActions.testFlag(QGV::MouseAction::Selection) && geoObject->isSelectable()) {
        if (event->button() == Qt::LeftButton) {
            const bool wasSelect = geoObject->isSelected();
            if (event->modifiers() == Qt::NoModifier) {
                geoObject->getMap()->unselectAll();
                geoObject->setSelected(!wasSelect);
            }
            if (event->modifiers() == Qt::ControlModifier || event->modifiers() == Qt::ShiftModifier) {
                geoObject->setSelected(!wasSelect);
            }
        }
    }
    if (geoObject->isFlag(QGV::ItemFlag::Clickable)) {
        if (event->button() == Qt::LeftButton) {
            geoObject->projOnMouseClick(projPos);
        }
    }
}

void QGVMapQGView::objectDoubleClick(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF projPos = mapToScene(event->pos());
    auto geoObjects = mGeoMap->search(projPos, Qt::ContainsItemShape);
    if (geoObjects.isEmpty()) {
        return;
    }
    auto* geoObject = geoObjects.first();
    if (geoObject->isFlag(QGV::ItemFlag::Clickable)) {
        if (event->button() == Qt::LeftButton) {
            geoObject->projOnMouseDoubleClick(projPos);
        }
    }
}

void QGVMapQGView::moveForWheel(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::ZoomWheel)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    if (!mWheelMouseArea.contains(event->pos())) {
        changeState(QGV::MapState::Idle);
    }
}

void QGVMapQGView::moveForRect(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Selection) && !mMouseActions.testFlag(QGV::MouseAction::ZoomRect)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    const QPoint endPoint = event->pos();
    mSelectionRect->setEndPoint(endPoint);
}

void QGVMapQGView::moveMap(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Move)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    const QPointF projCenter = viewRect().center();
    const QPointF projMouse = mapToScene(event->pos());
    const double xDelta = (mMoveProjAnchor.x() - projMouse.x());
    const double yDelta = (mMoveProjAnchor.y() - projMouse.y());
    cameraMove(projCenter + QPointF(xDelta, yDelta));
}

void QGVMapQGView::moveObject(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::MoveObjects)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    Q_ASSERT(mMovingObject);
    event->accept();
    const QPointF projMouse = mapToScene(event->pos());
    mMovingObject->projOnObjectMovePos(projMouse);
}

void QGVMapQGView::unselectAll(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::Selection)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    mGeoMap->unselectAll();
    changeState(QGV::MapState::Idle);
}

void QGVMapQGView::showMenu(QMouseEvent* event)
{
    if (!mMouseActions.testFlag(QGV::MouseAction::ContextMenu)) {
        changeState(QGV::MapState::Idle);
        return;
    }
    event->accept();
    changeState(QGV::MapState::Idle);
    if (mGeoMap->actions().isEmpty()) {
        return;
    }
    mContextMenu->clear();
    mContextMenu->insertActions(0, mGeoMap->actions());
    mContextMenu->exec(mapToGlobal(event->pos()));
}

bool QGVMapQGView::event(QEvent* event)
{
    event->ignore();
    if (event->type() == QEvent::ToolTip) {
        showTooltip(static_cast<QHelpEvent*>(event));
    }
    return QGraphicsView::event(event);
}

void QGVMapQGView::wheelEvent(QWheelEvent* event)
{
    event->ignore();
    zoomByWheel(event);
    QWidget::wheelEvent(event);
}

void QGVMapQGView::mousePressEvent(QMouseEvent* event)
{
    event->ignore();
    objectClick(event);
    if (event->button() == Qt::LeftButton) {
        if (event->modifiers() == Qt::AltModifier) {
            startMovingObject(event);
        } else if (event->modifiers() == Qt::NoModifier) {
            startMoving(event);
        }
    } else if (event->button() == Qt::RightButton) {
        startSelectionRect(event);
    }
    QGraphicsView::mousePressEvent(event);
}

void QGVMapQGView::mouseReleaseEvent(QMouseEvent* event)
{
    event->ignore();
    if (mState == QGV::MapState::MovingObjects) {
        stopMovingObject(event);
    } else if (mState == QGV::MapState::SelectionRect) {
        if (mSelectionRect->isSelection()) {
            stopSelectionRect(event);
        } else {
            showMenu(event);
        }
    } else {
        changeState(QGV::MapState::Idle);
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void QGVMapQGView::mouseMoveEvent(QMouseEvent* event)
{
    event->ignore();
    if (mState == QGV::MapState::Wheel) {
        moveForWheel(event);
    } else if (mState == QGV::MapState::MovingMap) {
        moveMap(event);
    } else if (mState == QGV::MapState::MovingObjects) {
        moveObject(event);
    } else if (mState == QGV::MapState::SelectionRect) {
        moveForRect(event);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void QGVMapQGView::mouseDoubleClickEvent(QMouseEvent* event)
{
    event->ignore();
    unselectAll(event);
    objectDoubleClick(event);
    QGraphicsView::mouseDoubleClickEvent(event);
}

void QGVMapQGView::resizeEvent(QResizeEvent* event)
{
    const QGVCameraState oldState = getCamera();
    QGraphicsView::resizeEvent(event);
    mViewRect = viewport()->rect();
    mGeoMap->anchoreWidgets();
    applyCameraUpdate(oldState);
}

void QGVMapQGView::showEvent(QShowEvent* event)
{
    const QGVCameraState oldState = getCamera();
    QGraphicsView::showEvent(event);
    applyCameraUpdate(oldState);
}

void QGVMapQGView::keyPressEvent(QKeyEvent* event)
{
    QWidget::keyPressEvent(event);
}

void QGVMapQGView::dragEnterEvent(QDragEnterEvent* event)
{
    event->accept();
}

void QGVMapQGView::dragMoveEvent(QDragMoveEvent* event)
{
    event->accept();
}

void QGVMapQGView::dropEvent(QDropEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    Q_EMIT dropData(event->position(), event->mimeData());
#else
    QPointF dropPoint = event->posF();
    Q_EMIT dropData(dropPoint, event->mimeData());
#endif
    event->accept();
}

void QGVMapQGView::dragLeaveEvent(QDragLeaveEvent* event)
{
    event->accept();
}
