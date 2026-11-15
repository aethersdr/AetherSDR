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
#include "QGVItem.h"
#include "QGVMap.h"
#include "QGVMapQGItem.h"

class QGV_LIB_DECL QGVDrawItem : public QGVItem
{
    Q_OBJECT
    Q_PROPERTY(QGV::ItemFlags flags READ getFlags WRITE setFlags)

public:
    QGVDrawItem();

    void setFlags(QGV::ItemFlags flags);
    void setFlag(QGV::ItemFlag flag, bool enabled = true);
    QGV::ItemFlags getFlags() const;
    bool isFlag(QGV::ItemFlag flag) const;

    void refresh();
    void repaint();
    void resetBoundary();
    QTransform effectiveTransform() const;

    virtual QPainterPath projShape() const = 0;
    virtual void projPaint(QPainter* painter) = 0;
    virtual QPointF projAnchor() const;
    virtual QTransform projTransform() const;
    virtual QString projTooltip(const QPointF& projPos) const;
    virtual QString projDebug();
    virtual void projOnFlags();
    virtual void projOnMouseClick(const QPointF& projPos);
    virtual void projOnMouseDoubleClick(const QPointF& projPos);
    virtual void projOnObjectStartMove(const QPointF& projPos);
    virtual void projOnObjectMovePos(const QPointF& projPos);
    virtual void projOnObjectStopMove(const QPointF& projPos);

protected:
    void onProjection(QGVMap* geoMap) override;
    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override;
    void onUpdate() override;
    void onClean() override;

private:
    QGV::ItemFlags mFlags;
    QScopedPointer<QGVMapQGItem> mQGDrawItem;
    bool mDirty;
};
