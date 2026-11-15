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

#include "QGVMapRubberBand.h"

QGVMapRubberBand::QGVMapRubberBand(QWidget* parent)
    : QObject(parent)
{
    setMinSelection(QSize(0, 0));
    setStartPoint(QPoint(0, 0));
}

void QGVMapRubberBand::setMinSelection(const QSize& min)
{
    mMin = min;
}

void QGVMapRubberBand::setStartPoint(const QPoint& point)
{
    mStartPoint = point;
    mEndPoint = point;
    updateRect();
}

void QGVMapRubberBand::setEndPoint(const QPoint& point)
{
    mEndPoint = point;
    updateRect();
}

bool QGVMapRubberBand::isActive() const
{
    return mRubberBand != nullptr;
}

bool QGVMapRubberBand::isSelection() const
{
    if (isActive() == false) {
        return false;
    }
    const QRect rect = getRect();
    return rect.width() > mMin.width() && rect.height() > mMin.height();
}

QRect QGVMapRubberBand::getRect() const
{
    QRect result;
    if (mStartPoint.x() <= mEndPoint.x()) {
        result.setLeft(mStartPoint.x());
        result.setRight(mEndPoint.x());
    } else {
        result.setLeft(mEndPoint.x());
        result.setRight(mStartPoint.x());
    }
    if (mStartPoint.y() <= mEndPoint.y()) {
        result.setTop(mStartPoint.y());
        result.setBottom(mEndPoint.y());
    } else {
        result.setTop(mEndPoint.y());
        result.setBottom(mStartPoint.y());
    }
    return result;
}

void QGVMapRubberBand::showRect()
{
    QWidget* myParent = nullptr;
    if (parent() != nullptr) {
        myParent = dynamic_cast<QWidget*>(parent());
    }
    if (myParent == nullptr) {
        return;
    }
    mRubberBand.reset(new QRubberBand(QRubberBand::Rectangle, myParent));
    updateRect();
}

void QGVMapRubberBand::hideRect()
{
    mRubberBand.reset(nullptr);
}

void QGVMapRubberBand::updateRect()
{
    if (isActive()) {
        mRubberBand->setGeometry(getRect());
        if (isSelection()) {
            mRubberBand->show();
        } else {
            mRubberBand->hide();
        }
    }
}
