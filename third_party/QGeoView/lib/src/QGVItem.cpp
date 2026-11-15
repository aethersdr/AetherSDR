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

#include "QGVItem.h"
#include <limits>

QGVItem::QGVItem(QGVItem* parent)
{
    mParent = parent;
    mZValue = 0;
    mOpacity = 1.0;
    mVisible = true;
    mSelectable = false;
    mSelected = false;
}

QGVItem::~QGVItem()
{
    deleteItems();
    if (mParent != nullptr) {
        mParent->mChildrens.removeAll(this);
    }
}

void QGVItem::setParent(QGVItem* item)
{
    if (mParent == item) {
        return;
    }
    setSelected(false);
    if (mParent != nullptr) {
        mParent->mChildrens.removeAll(this);
    }
    auto oldParent = mParent;
    mParent = item;
    if (mParent != nullptr) {
        mParent->mChildrens.append(this);
    }
    auto geoMap = getMap();
    if (geoMap != nullptr) {
        if (oldParent != nullptr) {
            Q_EMIT geoMap->itemsChanged(oldParent);
        }
        if (mParent != nullptr) {
            Q_EMIT geoMap->itemsChanged(mParent);
        }
        onProjection(geoMap);
        update();
    } else {
        onClean();
    }
}

QGVItem* QGVItem::getParent() const
{
    return mParent;
}

QGVMap* QGVItem::getMap() const
{
    if (mParent != nullptr) {
        return mParent->getMap();
    }
    return nullptr;
}

void QGVItem::addItem(QGVItem* item)
{
    Q_ASSERT(item);
    item->setParent(this);
}

void QGVItem::removeItem(QGVItem* item)
{
    Q_ASSERT(item);
    if (item->getParent() != this) {
        return;
    }
    item->setParent(nullptr);
}

void QGVItem::deleteItems()
{
    auto copy = mChildrens;
    qDeleteAll(copy.begin(), copy.end());
    mChildrens.clear();
}

int QGVItem::countItems() const
{
    return mChildrens.count();
}

QGVItem* QGVItem::getItem(int index) const
{
    return mChildrens.at(index);
}

void QGVItem::setZValue(qint16 zValue)
{
    if (mZValue != zValue) {
        mZValue = zValue;
        update();
    }
}

qint16 QGVItem::getZValue() const
{
    return mZValue;
}

void QGVItem::bringToFront()
{
    mZValue = std::numeric_limits<decltype(mZValue)>::max();
    update();
}

void QGVItem::sendToBack()
{
    mZValue = std::numeric_limits<decltype(mZValue)>::min();
    update();
}

void QGVItem::setOpacity(double value)
{
    value = qMin(1.0, qMax(0.0, value));
    if (qFuzzyCompare(mOpacity, value)) {
        return;
    }
    mOpacity = value;
    update();
}

double QGVItem::getOpacity() const
{
    return mOpacity;
}

void QGVItem::setSelectable(bool allowed)
{
    if (mSelectable == allowed) {
        return;
    }
    mSelectable = allowed;
    if (!mSelectable) {
        setSelected(false);
    }
    onUpdate();
}

bool QGVItem::isSelectable() const
{
    return mSelectable;
}

void QGVItem::setSelected(bool selected)
{
    if (mSelected == selected || !isSelectable()) {
        return;
    }
    mSelected = selected;
    auto geoMap = getMap();
    if (geoMap != nullptr) {
        if (mSelected) {
            geoMap->select(this);
        } else {
            geoMap->unselect(this);
        }
    }
    update();
}

bool QGVItem::isSelected() const
{
    return mSelected;
}

void QGVItem::select()
{
    setSelected(true);
}

void QGVItem::unselect()
{
    setSelected(false);
}

void QGVItem::setVisible(bool visible)
{
    if (mVisible == visible) {
        return;
    }
    mVisible = visible;
    update();
}

bool QGVItem::isVisible() const
{
    return mVisible;
}

void QGVItem::show()
{
    setVisible(true);
}

void QGVItem::hide()
{
    setVisible(false);
}

double QGVItem::effectiveZValue() const
{
    if (mParent == nullptr) {
        return mZValue;
    }
    const auto den = std::numeric_limits<decltype(mZValue)>::max() - std::numeric_limits<decltype(mZValue)>::min();
    double range = 1.0;
    for (QGVItem* obj = mParent->getParent(); obj != nullptr; obj = obj->getParent()) {
        range *= 1.0 / den;
    }
    const double efZValue = range * mZValue / den;
    return mParent->effectiveZValue() + efZValue;
}

double QGVItem::effectiveOpacity() const
{
    if (mParent == nullptr) {
        return mOpacity;
    }
    return mOpacity * mParent->effectiveOpacity();
}

bool QGVItem::effectivelyVisible() const
{
    if (mParent == nullptr) {
        return mVisible;
    }
    return mVisible && mParent->effectivelyVisible();
}

void QGVItem::update()
{
    if (getMap() == nullptr) {
        return;
    }
    for (QGVItem* obj : mChildrens) {
        obj->update();
    }
    onUpdate();
}

void QGVItem::onProjection(QGVMap* geoMap)
{
    for (QGVItem* obj : mChildrens) {
        obj->onProjection(geoMap);
    }
}

void QGVItem::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    for (QGVItem* obj : mChildrens) {
        if (obj->isVisible()) {
            obj->onCamera(oldState, newState);
        }
    }
}

void QGVItem::onUpdate()
{
}

void QGVItem::onClean()
{
    for (QGVItem* obj : mChildrens) {
        obj->onClean();
    }
}
