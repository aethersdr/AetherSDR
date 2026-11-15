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

#include "QGVWidgetCompass.h"

#include <QBitmap>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

namespace {
const int defaultRadius = 4;
const double stickyAngleMargin = 5;
}

QGVWidgetCompass::QGVWidgetCompass()
{
    setMouseTracking(true);
    setAnchor(QPoint(10, 10), { Qt::LeftEdge, Qt::TopEdge });
    setPixmap(QPixmap());
    mTracking = false;
    mOffset = 0;
    setPixmap(createPixmap());
}

void QGVWidgetCompass::setPixmap(QPixmap pixmap)
{
    mPixmap = pixmap;
    if (mPixmap.isNull()) {
        resize(QSize(defaultRadius, defaultRadius));
    } else {
        resize(mPixmap.size());
    }
}

QPixmap QGVWidgetCompass::createPixmap()
{
    static const QPolygon polygon = QVector<QPoint>{
        QPoint(0, 60), QPoint(20, 0), QPoint(40, 60), QPoint(20, 45), QPoint(00, 60),
    };

    int maxSize = polygon.boundingRect().height();
    maxSize = qSqrt(qPow(maxSize, 2) + qPow(maxSize, 2));
    const QRect rect = QRect(QPoint(0, 0), QSize(maxSize, maxSize));
    const QPointF centerDelta = rect.center() - polygon.boundingRect().center();

    QImage image(rect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(qRgba(0, 0, 0, 0));
    QPixmap pixmap = QPixmap::fromImage(image, Qt::NoFormatConversion);
    QPainter painter(&pixmap);
    QPen pen = QPen(Qt::black);
    pen.setWidth(1);
    pen.setCosmetic(true);
    painter.setPen(pen);
    QBrush brush = QBrush(Qt::blue);
    painter.setBrush(brush);
    QPainterPath path;
    path.addPolygon(polygon);
    path.translate(centerDelta + QPointF(pen.width(), pen.width()));
    painter.drawPath(path);
    return pixmap;
}

double QGVWidgetCompass::mouseToAzimuth(const QPoint& pos, double offset) const
{
    const QLineF northLine(rect().center(), QPointF(rect().center().x(), rect().top()));
    const QLineF posLine(rect().center(), pos);
    if (posLine.length() >= 10) {
        double azimuth = posLine.angleTo(northLine) + offset;
        if (qAbs(fmod(azimuth, 90.0)) <= stickyAngleMargin || qAbs(fmod(azimuth, 90.0)) >= 90.0 - stickyAngleMargin) {
            azimuth = 90.0 * qRound(azimuth / 90.0);
        }
        return azimuth;
    }
    return getMap()->getCamera().azimuth();
}

void QGVWidgetCompass::onCamera(const QGVCameraState& oldState, const QGVCameraState& newState)
{
    QGVWidget::onCamera(oldState, newState);
    if (qFuzzyCompare(oldState.azimuth(), newState.azimuth())) {
        return;
    }
    mTransfrom = QGV::createTransfromAzimuth(mPixmap.rect().center(), newState.azimuth());
    repaint();
}

void QGVWidgetCompass::paintEvent(QPaintEvent* /*event*/)
{
    if (mPixmap.isNull()) {
        return;
    }
    QPainter painter(this);
    if (mTracking) {
        painter.setOpacity(0.5);
    }
    painter.setTransform(mTransfrom);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(0, 0, width(), height()), mPixmap.toImage(), mPixmap.rect());
}

void QGVWidgetCompass::mouseMoveEvent(QMouseEvent* event)
{
    if (mTracking) {
        const double azimuth = mouseToAzimuth(event->pos(), -mOffset);
        getMap()->cameraTo(QGVCameraActions(getMap()).rotateTo(azimuth));
    }
}

void QGVWidgetCompass::mousePressEvent(QMouseEvent* event)
{
    const double azimuth = getMap()->getCamera().azimuth();
    mOffset = mouseToAzimuth(event->pos(), -azimuth);
    mTracking = true;
    repaint();
}

void QGVWidgetCompass::mouseReleaseEvent(QMouseEvent* /*event*/)
{
    mTracking = false;
    repaint();
}

void QGVWidgetCompass::mouseDoubleClickEvent(QMouseEvent* /*event*/)
{
    getMap()->cameraTo(QGVCameraActions(getMap()).rotateTo(0));
}
