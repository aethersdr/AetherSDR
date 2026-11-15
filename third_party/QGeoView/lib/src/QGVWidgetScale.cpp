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

#include "QGVWidgetScale.h"
#include "QGVMapQGView.h"
#include "QGVUtils.h"

#include <QPaintEvent>
#include <QPainter>
#include <QtMath>

namespace {
const int defaultLengthPixel = 150;
const int minLengthPixel = 130;
}

QGVWidgetScale::QGVWidgetScale(Qt::Orientation orientation)
{
    mDistanceUnits = QGV::DistanceUnits::Kilometers;
    mUseMetersForSmallDistance = true;

    mAutoAdjust = true;
    mScaleMeters = 0;
    mScalePixels = 0;
    mOrientation = orientation;
    if (mOrientation == Qt::Horizontal) {
        setAnchor(QPoint(10, 10), QSet<Qt::Edge>() << Qt::RightEdge << Qt::BottomEdge);
    } else {
        setAnchor(QPoint(10, 15 + fontMetrics().height()), QSet<Qt::Edge>() << Qt::RightEdge << Qt::BottomEdge);
    }

    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setVisible(false);
}

void QGVWidgetScale::setAutoAdjust(bool autoAdjust)
{
    mAutoAdjust = autoAdjust;
    if (getMap() == nullptr) {
        setVisible(false);
        return;
    }
    const QGVCameraState camState = getMap()->getCamera();
    onCamera(camState, camState);
}

bool QGVWidgetScale::getAutoAdjust() const
{
    return mAutoAdjust;
}

void QGVWidgetScale::setOrientation(Qt::Orientation orientation)
{
    mOrientation = orientation;
    if (getMap() == nullptr) {
        return;
    }
    const QGVCameraState camState = getMap()->getCamera();
    onCamera(camState, camState);
}

Qt::Orientation QGVWidgetScale::getOrientation() const
{
    return mOrientation;
}

void QGVWidgetScale::setDistanceUnits(QGV::DistanceUnits distanceUnits)
{
    mDistanceUnits = distanceUnits;
}

QGV::DistanceUnits QGVWidgetScale::getDistanceUnits() const
{
    return mDistanceUnits;
}

void QGVWidgetScale::setUseMetersForSmallDistance(bool useMetersForSmallDistance)
{
    mUseMetersForSmallDistance = useMetersForSmallDistance;
}

bool QGVWidgetScale::getUseMetersForSmallDistance() const
{
    return mUseMetersForSmallDistance;
}

QString QGVWidgetScale::getDistanceLabel(int meters) const
{
    const double distanceValue = QGV::metersToDistance(meters, mDistanceUnits);
    if (distanceValue >= 1 || (distanceValue < 1 && !mUseMetersForSmallDistance)) {
        const int accuracy = distanceValue >= 1 ? 0 : 2;
        return tr("%1 %2")
                .arg(QString::number(static_cast<double>(distanceValue), 'f', accuracy))
                .arg(QGV::unitToString(mDistanceUnits));
    } else {
        return tr("%1 %2")
                .arg(QString::number(static_cast<double>(meters), 'f', 0))
                .arg(QGV::unitToString(QGV::DistanceUnits::Meters));
    }
}

void QGVWidgetScale::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    QGVWidget::onCamera(oldState, newState);
    const QPoint viewPoint1 = geometry().topLeft();
    QPoint viewPoint2;
    if (mOrientation == Qt::Horizontal) {
        viewPoint2 = QPoint(viewPoint1.x() + defaultLengthPixel, viewPoint1.y());
    } else {
        viewPoint2 = QPoint(viewPoint1.x(), viewPoint1.y() + defaultLengthPixel);
    }

    const QPointF projPoint1 = getMap()->mapToProj(viewPoint1);
    const QPointF projPoint2 = getMap()->mapToProj(viewPoint2);
    const QGVProjection* projection = getMap()->getProjection();
    if (!projection->boundaryProjRect().contains(projPoint1) || !projection->boundaryProjRect().contains(projPoint2)) {
        resize(QSize(0, 0));
        repaint();
        return;
    }

    int newLengthPixels = defaultLengthPixel;
    int newLengthMeters = static_cast<int>(projection->geodesicMeters(projPoint1, projPoint2));
    if (mAutoAdjust) {
        const double metersLog = qMax(1.0, log10(newLengthMeters));
        const int meters10 = static_cast<int>(qPow(10, qFloor(metersLog)));
        const double correction = static_cast<double>(meters10) / newLengthMeters;
        newLengthMeters = meters10;
        newLengthPixels = qCeil(correction * newLengthPixels);
        if (newLengthPixels < minLengthPixel) {
            const double factor = qPow(2, qCeil(log(minLengthPixel / newLengthPixels) * M_LOG2E));
            newLengthMeters *= factor;
            newLengthPixels *= factor;
        }
    }

    if (mScaleMeters != newLengthMeters || mScalePixels != newLengthPixels) {
        const int height = fontMetrics().boundingRect("W").height() + 5;
        mScaleMeters = newLengthMeters;
        mScalePixels = newLengthPixels;
        if (mOrientation == Qt::Horizontal) {
            resize(QSize(mScalePixels, height));
        } else {
            resize(QSize(height, mScalePixels));
        }
        repaint();
    }
}

void QGVWidgetScale::paintEvent(QPaintEvent* /*event*/)
{
    if (size().isEmpty()) {
        return;
    }
    QRect paintRect;
    if (mOrientation == Qt::Horizontal) {
        paintRect = QRect(QPoint(0, 0), size());
    } else {
        paintRect = QRect(QPoint(0, 0), QSize(size().height(), size().width()));
    }
    paintRect.moveCenter(rect().center());

    QPainter painter(this);
    if (mOrientation != Qt::Horizontal) {
        painter.setTransform(QGV::createTransfromAzimuth(rect().center(), -90.0));
    }

    const int lnWidth = 2;
    QRect lineRect = paintRect;
    lineRect.adjust(lnWidth, lnWidth + lineRect.height() / 2, -lnWidth, -lnWidth);
    painter.setPen(QPen(QBrush(Qt::black), lnWidth));
    painter.drawLine(QLine(QPoint(lineRect.left(), lineRect.bottom()), QPoint(lineRect.right(), lineRect.bottom())));
    painter.drawLine(QLine(QPoint(lineRect.left(), lineRect.bottom()), QPoint(lineRect.left(), lineRect.top())));
    painter.drawLine(QLine(QPoint(lineRect.right(), lineRect.bottom()), QPoint(lineRect.right(), lineRect.top())));

    QRect textRect = paintRect;
    textRect.moveCenter(paintRect.center());
    painter.setPen(QPen());
    painter.drawText(textRect, Qt::AlignCenter, getDistanceLabel(mScaleMeters));
}
