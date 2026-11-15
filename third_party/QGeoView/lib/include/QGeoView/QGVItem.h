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
#include "QGVMap.h"

class QGV_LIB_DECL QGVItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(quint16 zValue READ getZValue WRITE setZValue)
    Q_PROPERTY(double opacity READ getOpacity WRITE setOpacity)
    Q_PROPERTY(bool selected READ isSelected WRITE setSelected)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible)

public:
    explicit QGVItem(QGVItem* parent = nullptr);
    virtual ~QGVItem();

    void setParent(QGVItem* item);
    QGVItem* getParent() const;

    virtual QGVMap* getMap() const;

    void addItem(QGVItem* item);
    void removeItem(QGVItem* item);
    void deleteItems();
    int countItems() const;
    QGVItem* getItem(int index) const;

    void setZValue(qint16 zValue);
    qint16 getZValue() const;
    void bringToFront();
    void sendToBack();

    void setOpacity(double value);
    double getOpacity() const;

    void setSelectable(bool allowed);
    bool isSelectable() const;
    void setSelected(bool selected);
    bool isSelected() const;
    void select();
    void unselect();

    void setVisible(bool visible);
    bool isVisible() const;

    void show();
    void hide();

    double effectiveZValue() const;
    double effectiveOpacity() const;
    bool effectivelyVisible() const;

    void update();

    virtual void onProjection(QGVMap* geoMap);
    virtual void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState);
    virtual void onUpdate();
    virtual void onClean();

private:
    Q_DISABLE_COPY(QGVItem)
    QGVItem* mParent;
    qint16 mZValue;
    double mOpacity;
    bool mVisible;
    bool mSelectable;
    bool mSelected;
    QList<QGVItem*> mChildrens;
};
