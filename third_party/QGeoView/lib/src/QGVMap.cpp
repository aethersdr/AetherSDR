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

#include "QGVMap.h"
#include "QGVItem.h"
#include "QGVMapQGItem.h"
#include "QGVMapQGView.h"
#include "QGVProjectionEPSG3857.h"
#include "QGVWidget.h"

#include <QMouseEvent>
#include <QVBoxLayout>

class RootItem : public QGVItem
{
public:
    explicit RootItem(QGVMap* geoMap)
        : mGeoMap(geoMap)
    {
    }
    virtual ~RootItem();

    QGVMap* getMap() const override final
    {
        return mGeoMap;
    }

private:
    QGVMap* mGeoMap;
};
RootItem::~RootItem() = default;

QGVMap::QGVMap(QWidget* parent)
    : QWidget(parent)
{
    mProjection.reset(new QGVProjectionEPSG3857());
    mQGView.reset(new QGVMapQGView(this));
    mRootItem.reset(new RootItem(this));
    setLayout(new QVBoxLayout(this));
    layout()->addWidget(mQGView.data());
    layout()->setContentsMargins(0, 0, 0, 0);
    refreshProjection();
    connect(mQGView.data(), &QGVMapQGView::dropData, this, &QGVMap::handleDropDataOnQGVMapQGView);
}

QGVMap::~QGVMap()
{
    deleteItems();
    deleteWidgets();
}

const QGVCameraState QGVMap::getCamera() const
{
    return geoView()->getCamera();
}

void QGVMap::cameraTo(const QGVCameraActions& actions, bool animation)
{
    geoView()->cameraTo(actions, animation);
}

void QGVMap::flyTo(const QGVCameraActions& actions)
{
    geoView()->cleanState();
    auto* fly = new QGVCameraFlyAnimation(actions);
    fly->start(QAbstractAnimation::DeleteWhenStopped);
}

void QGVMap::setProjection(QGV::Projection id)
{
    mProjection.reset(nullptr);
    switch (id) {
        case QGV::Projection::EPSG3857:
            setProjection(new QGVProjectionEPSG3857());
            break;
    }
}

void QGVMap::setProjection(QGVProjection* projection)
{
    Q_ASSERT(projection);
    mProjection.reset(projection);
    refreshProjection();
    Q_EMIT projectionChanged();
}

QGVProjection* QGVMap::getProjection() const
{
    return mProjection.data();
}

void QGVMap::setMouseActions(QGV::MouseActions actions)
{
    geoView()->setMouseActions(actions);
}

void QGVMap::setMouseAction(QGV::MouseAction action, bool enabled)
{
    QGV::MouseActions newActions = getMouseActions();
    if (enabled)
        newActions |= action;
    else
        newActions &= ~static_cast<int>(action);
    setMouseActions(newActions);
}

QGV::MouseActions QGVMap::getMouseActions() const
{
    return geoView()->getMouseActions();
}

bool QGVMap::isMouseAction(QGV::MouseAction action) const
{
    return getMouseActions().testFlag(action);
}

QGVItem* QGVMap::rootItem() const
{
    return mRootItem.data();
}

QGVMapQGView* QGVMap::geoView() const
{
    return mQGView.data();
}

void QGVMap::addItem(QGVItem* item)
{
    Q_ASSERT(item);
    mRootItem->addItem(item);
}

void QGVMap::removeItem(QGVItem* item)
{
    Q_ASSERT(item);
    mRootItem->removeItem(item);
}

void QGVMap::deleteItems()
{
    mRootItem->deleteItems();
}

int QGVMap::countItems() const
{
    return mRootItem->countItems();
}

QGVItem* QGVMap::getItem(int index) const
{
    return mRootItem->getItem(index);
}

void QGVMap::addWidget(QGVWidget* widget)
{
    Q_ASSERT(widget);
    if (widget->getMap() != nullptr && widget->getMap() != this) {
        widget->getMap()->removeWidget(widget);
    }

    mWidgets.append(widget);
    widget->setMap(this);
    widget->show();
}

void QGVMap::removeWidget(QGVWidget* widget)
{
    if (!mWidgets.contains(widget))
        return;

    mWidgets.removeAll(widget);
    widget->setMap(nullptr);
}

void QGVMap::deleteWidgets()
{
    auto copy = mWidgets;
    qDeleteAll(copy.begin(), copy.end());
    mWidgets.clear();
}

int QGVMap::countWidgets() const
{
    return mWidgets.count();
}

QGVWidget* QGVMap::getWigdet(int index) const
{
    return mWidgets.at(index);
}

void QGVMap::select(QGVItem* item)
{
    item->select();
    if (item->isSelected()) {
        mSelections.insert(item);
    }
}

void QGVMap::unselect(QGVItem* item)
{
    item->unselect();
    if (!item->isSelected()) {
        mSelections.remove(item);
    }
}

void QGVMap::unselectAll()
{
    auto selections = mSelections;
    for (QGVItem* item : selections) {
        item->unselect();
    }
}

QSet<QGVItem*> QGVMap::getSelections() const
{
    return mSelections;
}

QList<QGVDrawItem*> QGVMap::search(const QPointF& projPos, Qt::ItemSelectionMode mode) const
{
    QList<QGVDrawItem*> result;
    for (QGraphicsItem* item : geoView()->scene()->items(projPos, mode)) {
        QGVDrawItem* geoObject = QGVMapQGItem::geoObjectFromQGItem(item);
        if (geoObject)
            result << geoObject;
    }
    return result;
}

QList<QGVDrawItem*> QGVMap::search(const QRectF& projRect, Qt::ItemSelectionMode mode) const
{
    QList<QGVDrawItem*> result;
    for (QGraphicsItem* item : geoView()->scene()->items(projRect, mode)) {
        QGVDrawItem* geoObject = QGVMapQGItem::geoObjectFromQGItem(item);
        if (geoObject)
            result << geoObject;
    }
    return result;
}

QList<QGVDrawItem*> QGVMap::search(const QPolygonF& projPolygon, Qt::ItemSelectionMode mode) const
{
    QList<QGVDrawItem*> result;
    for (QGraphicsItem* item : geoView()->scene()->items(projPolygon, mode)) {
        QGVDrawItem* geoObject = QGVMapQGItem::geoObjectFromQGItem(item);
        if (geoObject)
            result << geoObject;
    }
    return result;
}

QPixmap QGVMap::grabMapView(bool includeWidgets) const
{
    const QPixmap pixmap = (includeWidgets) ? geoView()->grab(geoView()->rect())
                                            : geoView()->viewport()->grab(geoView()->viewport()->rect());
    return pixmap;
}

QPointF QGVMap::mapToProj(QPoint pos)
{
    const auto viewPos = geoView()->mapFromParent(pos);
    const auto projPos = geoView()->mapToScene(viewPos);
    return projPos;
}

QPoint QGVMap::mapFromProj(QPointF projPos)
{
    const auto viewPos = geoView()->mapFromScene(projPos);
    const auto mapPos = geoView()->mapToParent(viewPos);
    return mapPos;
}

void QGVMap::refreshMap()
{
    mRootItem->update();
}

void QGVMap::refreshProjection()
{
    QRectF sceneRect = mProjection->boundaryProjRect();

    const double viewXSize = 640;
    const double viewYSize = 480;

    const double projXSize = sceneRect.width();
    const double projYSize = sceneRect.height();

    const double newMinScaleFactor0 = 1.0;
    const double newMinScaleFactor1 = qAbs(viewXSize / projXSize);
    const double newMinScaleFactor2 = qAbs(viewYSize / projYSize);
    const double minScale = qMin(newMinScaleFactor0, qMin(newMinScaleFactor1, newMinScaleFactor2));
    const double maxScale = 16.0;
    geoView()->setScaleLimits(minScale, maxScale);

    const double offset = 1;
    sceneRect.adjust(-sceneRect.width() * offset,
                     -sceneRect.height() * offset,
                     +sceneRect.width() * offset,
                     +sceneRect.height() * offset);
    geoView()->scene()->setSceneRect(sceneRect);

    auto root = static_cast<RootItem*>(rootItem());
    root->onProjection(this);

    for (QGVWidget* widget : mWidgets) {
        widget->onProjection(this);
    }
}

void QGVMap::anchoreWidgets()
{
    for (QGVWidget* widget : mWidgets) {
        widget->anchoreWidget();
    }
}

void QGVMap::onMapState(QGV::MapState state)
{
    Q_EMIT stateChanged(state);
}

void QGVMap::onMapCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    if (!qFuzzyCompare(oldState.azimuth(), newState.azimuth())) {
        Q_EMIT azimuthChanged();
    }
    if (!qFuzzyCompare(oldState.scale(), newState.scale())) {
        Q_EMIT scaleChanged();
    }
    if (oldState.projRect() != newState.projRect()) {
        Q_EMIT areaChanged();
    }

    auto root = static_cast<RootItem*>(rootItem());
    if (root->isVisible()) {
        root->onCamera(oldState, newState);
    }
    for (QGVWidget* widget : mWidgets) {
        if (widget->isVisible()) {
            widget->onCamera(oldState, newState);
        }
    }

    if (hasMouseTracking()) {
        const auto pos = mapFromGlobal(QCursor::pos());
        Q_EMIT mapMouseMove(mapToProj(pos));
    }
}

void QGVMap::mouseMoveEvent(QMouseEvent* event)
{
    if (hasMouseTracking()) {
        Q_EMIT mapMouseMove(mapToProj(event->pos()));
    }
    event->ignore();
    QWidget::mouseMoveEvent(event);
}

void QGVMap::mousePressEvent(QMouseEvent* event)
{
    if (hasMouseTracking()) {
        Q_EMIT mapMousePress(mapToProj(event->pos()));
    }
    event->ignore();
    QWidget::mousePressEvent(event);
}

void QGVMap::handleDropDataOnQGVMapQGView(QPointF position, const QMimeData* dropData)
{
    const auto mapToProjectionPos = mapToProj(QPoint(position.rx(), position.ry()));
    auto geoPos = getProjection()->projToGeo(mapToProjectionPos);
    Q_EMIT dropOnMap(geoPos, dropData);
}

void QGVMap::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (hasMouseTracking()) {
        Q_EMIT mapMouseDoubleClicked(mapToProj(event->pos()));
    }
    event->ignore();
    QWidget::mouseDoubleClickEvent(event);
}
