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

#include "QGVWidget.h"
#include "QGVMapQGView.h"

QGVWidget::QGVWidget()
{
    mGeoMap = nullptr;
    mAnchor = QPoint(0, 0);
    mEdge.clear();
}

QGVWidget::~QGVWidget()
{
    if (mGeoMap != nullptr) {
        mGeoMap->removeWidget(this);
    }
}

void QGVWidget::setMap(QGVMap* geoMap)
{
    mGeoMap = geoMap;
    if (mGeoMap != nullptr) {
        setParent(mGeoMap->geoView());
    } else {
        setParent(nullptr);
    }
    onProjection(mGeoMap);
    anchoreWidget();
}

QGVMap* QGVWidget::getMap() const
{
    return mGeoMap;
}

void QGVWidget::setAnchor(QPoint anchor, QSet<Qt::Edge> edge)
{
    mAnchor = anchor;
    mEdge = edge;
    anchoreWidget();
}

QPointF QGVWidget::getAnchor() const
{
    return mAnchor;
}

QSet<Qt::Edge> QGVWidget::getEdge() const
{
    return mEdge;
}

bool QGVWidget::isAnchorLeft() const
{
    return (mEdge.contains(Qt::LeftEdge) && !mEdge.contains(Qt::RightEdge));
}

bool QGVWidget::isAnchorRight() const
{
    return (!mEdge.contains(Qt::LeftEdge) && mEdge.contains(Qt::RightEdge));
}

bool QGVWidget::isAnchorHCenter() const
{
    return !isAnchorLeft() && !isAnchorRight();
}

bool QGVWidget::isAnchorTop() const
{
    return (mEdge.contains(Qt::TopEdge) && !mEdge.contains(Qt::BottomEdge));
}

bool QGVWidget::isAnchorBottom() const
{
    return (!mEdge.contains(Qt::TopEdge) && mEdge.contains(Qt::BottomEdge));
}

bool QGVWidget::isAnchorVCenter() const
{
    return !isAnchorTop() && !isAnchorBottom();
}

void QGVWidget::anchoreWidget()
{
    if (parentWidget() == nullptr) {
        return;
    }
    const int parentWidth = parentWidget()->width();
    const int parentHeight = parentWidget()->height();
    int leftOffset = 0;
    int topOffset = 0;
    if (isAnchorLeft()) {
        leftOffset = mAnchor.x();
    }
    if (isAnchorRight()) {
        leftOffset = parentWidth - width() - mAnchor.x();
    }
    if (isAnchorHCenter()) {
        leftOffset = parentWidth / 2 - width() / 2 + mAnchor.x();
    }
    if (isAnchorTop()) {
        topOffset = mAnchor.y();
    }
    if (isAnchorBottom()) {
        topOffset = parentHeight - height() - mAnchor.y();
    }
    if (isAnchorVCenter()) {
        topOffset = parentHeight / 2 - height() / 2 + mAnchor.y();
    }
    move(leftOffset, topOffset);
}

void QGVWidget::onProjection(QGVMap* /*geoMap*/)
{
}

void QGVWidget::onCamera(const QGVCameraState& /*oldState*/, const QGVCameraState& /*newState*/)
{
}

void QGVWidget::resizeEvent(QResizeEvent* /*event*/)
{
    anchoreWidget();
}
