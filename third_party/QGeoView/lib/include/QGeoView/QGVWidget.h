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

#include <QSet>
#include <QWidget>

#include "QGVGlobal.h"
#include "QGVMap.h"

class QGV_LIB_DECL QGVWidget : public QWidget
{
    Q_OBJECT

public:
    QGVWidget();
    virtual ~QGVWidget();

    void setMap(QGVMap* geoMap);
    QGVMap* getMap() const;

    void setAnchor(QPoint anchor, QSet<Qt::Edge> edge);
    QPointF getAnchor() const;
    QSet<Qt::Edge> getEdge() const;

    bool isAnchorLeft() const;
    bool isAnchorRight() const;
    bool isAnchorHCenter() const;

    bool isAnchorTop() const;
    bool isAnchorBottom() const;
    bool isAnchorVCenter() const;

    void anchoreWidget();

    virtual void onProjection(QGVMap* geoMap);
    virtual void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QGVMap* mGeoMap;
    QPoint mAnchor;
    QSet<Qt::Edge> mEdge;
};
