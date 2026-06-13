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

#include "QGVMapQGItem.h"
#include "QGVDrawItem.h"

#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPalette>

QGVMapQGItem::QGVMapQGItem(QGVDrawItem* geoObject)
{
    mGeoObject = geoObject;
    setCacheMode(QGraphicsItem::DeviceCoordinateCache);
}

QGVDrawItem* QGVMapQGItem::geoObjectFromQGItem(QGraphicsItem* item)
{
    QGVMapQGItem* qGCItem = dynamic_cast<QGVMapQGItem*>(item);
    return (qGCItem != nullptr) ? qGCItem->mGeoObject : nullptr;
}

void QGVMapQGItem::resetGeometry()
{
    prepareGeometryChange();
}

QRectF QGVMapQGItem::boundingRect() const
{
    return mGeoObject->projShape().boundingRect();
}

void QGVMapQGItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/)
{
    mGeoObject->projPaint(painter);

    if (mGeoObject->isSelected() && !mGeoObject->isFlag(QGV::ItemFlag::SelectCustom)) {
        QPen pen = QPen(mGeoObject->getMap()->palette().highlight(), 1, Qt::DashLine);
        pen.setCosmetic(true);
        QBrush brush = QBrush(mGeoObject->getMap()->palette().light().color(), Qt::Dense4Pattern);
        painter->setPen(pen);
        painter->setBrush(brush);
        painter->drawPath(mGeoObject->projShape());
    }

    if (QGV::isDrawDebug()) {
        mGeoObject->setProperty("paintCount", mGeoObject->property("paintCount").toInt() + 1);
        QPen pen = QPen(Qt::black);
        pen.setWidth(1);
        pen.setCosmetic(true);
        QBrush brush = QBrush(Qt::white);
        painter->setPen(pen);
        painter->setBrush(brush);
        auto rect = boundingRect().toRect();
        auto path = QGV::createTextPath(rect, mGeoObject->projDebug(), QFont(), pen.width());
        path = QGV::createTransfromScale(rect.center(), 0.75).map(path);
        painter->drawPath(path);

        pen = QPen(Qt::black);
        pen.setStyle(Qt::DashLine);
        pen.setWidth(1);
        pen.setCosmetic(true);
        brush = QBrush(Qt::transparent);
        painter->setPen(pen);
        painter->setBrush(brush);
        painter->drawRect(rect);
    }
}

QPainterPath QGVMapQGItem::shape() const
{
    return mGeoObject->projShape();
}

void QGVMapQGItem::hoverEnterEvent(QGraphicsSceneHoverEvent* /*event*/)
{
    if (mGeoObject->isFlag(QGV::ItemFlag::Highlightable)) {
        mGeoObject->setFlag(QGV::ItemFlag::Highlighted);
    }
}

void QGVMapQGItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* /*event*/)
{
    if (mGeoObject->isFlag(QGV::ItemFlag::Highlightable)) {
        mGeoObject->setFlag(QGV::ItemFlag::Highlighted, false);
    }
}
