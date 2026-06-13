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

#include "QGVDrawItem.h"
#include "QGVMapQGItem.h"
#include "QGVMapQGView.h"

namespace {
double highlightScale = 1.15;
}

QGVDrawItem::QGVDrawItem()
    : mDirty{ false }
{
}

void QGVDrawItem::setFlags(QGV::ItemFlags flags)
{
    if (mFlags != flags) {
        mFlags = flags;
        projOnFlags();
        refresh();
    }
}

void QGVDrawItem::setFlag(QGV::ItemFlag flag, bool enabled)
{
    QGV::ItemFlags newFlags = getFlags();
    if (enabled)
        newFlags |= flag;
    else
        newFlags &= ~static_cast<int>(flag);
    setFlags(newFlags);
}

QGV::ItemFlags QGVDrawItem::getFlags() const
{
    return mFlags;
}

bool QGVDrawItem::isFlag(QGV::ItemFlag flag) const
{
    return getFlags().testFlag(flag);
}

void QGVDrawItem::refresh()
{
    if (mQGDrawItem.isNull()) {
        return;
    }
    if (!isVisible()) {
        mQGDrawItem->hide();
        return;
    }

    QTransform userTransform;
    if (isFlag(QGV::ItemFlag::Transformed)) {
        userTransform = projTransform();
    }
    QTransform itemTransform;
    if (isFlag(QGV::ItemFlag::Highlighted) || isFlag(QGV::ItemFlag::IgnoreScale) ||
        isFlag(QGV::ItemFlag::IgnoreAzimuth)) {
        double scale = 1.0;
        double azimuth = 0.0;
        if (isFlag(QGV::ItemFlag::Highlighted) && !isFlag(QGV::ItemFlag::HighlightCustom)) {
            scale *= highlightScale;
        }
        if (isFlag(QGV::ItemFlag::IgnoreScale)) {
            scale *= 1.0 / getMap()->getCamera().scale();
        }
        if (isFlag(QGV::ItemFlag::IgnoreAzimuth)) {
            azimuth += -getMap()->getCamera().azimuth();
        }
        itemTransform = QGV::createTransfrom(projAnchor(), scale, azimuth);
    }
    mQGDrawItem->resetTransform();
    mQGDrawItem->setTransform(userTransform, true);
    mQGDrawItem->setTransform(itemTransform, true);
    mQGDrawItem->setVisible(effectivelyVisible());
    mQGDrawItem->setOpacity(effectiveOpacity());
    mQGDrawItem->setZValue(effectiveZValue());
    mQGDrawItem->setAcceptHoverEvents(isFlag(QGV::ItemFlag::Highlightable));
    mQGDrawItem->update();

    mDirty = false;

    if (QGV::isDrawDebug()) {
        setProperty("updateCount", property("updateCount").toInt() + 1);
    }
}

void QGVDrawItem::repaint()
{
    if (mQGDrawItem.isNull()) {
        return;
    }

    if (mDirty) {
        refresh();
    } else {
        mQGDrawItem->update();
    }
}

void QGVDrawItem::resetBoundary()
{
    if (!mQGDrawItem.isNull()) {
        mQGDrawItem->resetGeometry();
    }

    if (isFlag(QGV::ItemFlag::Transformed) || isFlag(QGV::ItemFlag::Highlighted) ||
        isFlag(QGV::ItemFlag::IgnoreScale) || isFlag(QGV::ItemFlag::IgnoreAzimuth)) {
        mDirty = true;
    }
}

QTransform QGVDrawItem::effectiveTransform() const
{
    if (mQGDrawItem.isNull()) {
        return {};
    }
    return mQGDrawItem->transform();
}

QPointF QGVDrawItem::projAnchor() const
{
    return projShape().boundingRect().center();
}

QTransform QGVDrawItem::projTransform() const
{
    return {};
}

QString QGVDrawItem::projTooltip(const QPointF& /*projPos*/) const
{
    return {};
}

QString QGVDrawItem::projDebug()
{
    return QString("%1\nupdate(%2,%3)")
            .arg(property("drawDebug").toString())
            .arg(property("updateCount").toInt())
            .arg(property("paintCount").toInt())
            .trimmed();
}

void QGVDrawItem::projOnFlags()
{
}

void QGVDrawItem::projOnMouseClick(const QPointF& projPos)
{
    auto geoMap = getMap();
    if (geoMap != nullptr) {
        Q_EMIT geoMap->itemClicked(this, projPos);
    }
}

void QGVDrawItem::projOnMouseDoubleClick(const QPointF& projPos)
{
    auto geoMap = getMap();
    if (geoMap != nullptr) {
        Q_EMIT geoMap->itemDoubleClicked(this, projPos);
    }
}

void QGVDrawItem::projOnObjectStartMove(const QPointF& /*projPos*/)
{
}

void QGVDrawItem::projOnObjectStopMove(const QPointF& /*projPos*/)
{
}

void QGVDrawItem::projOnObjectMovePos(const QPointF& /*projPos*/)
{
}

void QGVDrawItem::onProjection(QGVMap* geoMap)
{
    QGVItem::onProjection(geoMap);
    if (!mQGDrawItem.isNull()) {
        if (mQGDrawItem->scene() != geoMap->geoView()->scene()) {
            onClean();
        }
    }
    if (mQGDrawItem.isNull()) {
        mQGDrawItem.reset(new QGVMapQGItem(this));
        geoMap->geoView()->scene()->addItem(mQGDrawItem.data());
    }
}

void QGVDrawItem::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    QGVItem::onCamera(oldState, newState);
    bool neededUpdate =
            (mFlags.testFlag(QGV::ItemFlag::IgnoreAzimuth) && !qFuzzyCompare(oldState.azimuth(), newState.azimuth())) ||
            (mFlags.testFlag(QGV::ItemFlag::IgnoreScale) && !qFuzzyCompare(oldState.scale(), newState.scale()));
    if (!neededUpdate) {
        return;
    }
    refresh();
}

void QGVDrawItem::onUpdate()
{
    QGVItem::onUpdate();
    refresh();
}

void QGVDrawItem::onClean()
{
    QGVItem::onClean();
    mQGDrawItem.reset(nullptr);
}
