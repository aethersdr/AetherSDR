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

#include "QGVGlobal.h"

#include <QGraphicsItem>

class QGVDrawItem;

class QGV_LIB_DECL QGVMapQGItem : public QGraphicsItem
{
public:
    explicit QGVMapQGItem(QGVDrawItem* geoObject);

    static QGVDrawItem* geoObjectFromQGItem(QGraphicsItem* item);

    void resetGeometry();

private:
    QRectF boundingRect() const override final;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = 0) override final;
    QPainterPath shape() const override final;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override final;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override final;

private:
    QGVDrawItem* mGeoObject;
};
