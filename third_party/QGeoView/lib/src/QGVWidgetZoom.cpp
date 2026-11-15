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

#include "QGVWidgetZoom.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QVBoxLayout>
#include <QtMath>

namespace {
QSize iconSize = QSize(24, 24);
double zoomExponentDown = qPow(2, 1.0 / 5.0);
double zoomExponentUp = 1.0 / qPow(2, 1.0 / 5.0);
}

QGVWidgetZoom::QGVWidgetZoom()
{
    mButtonPlus.reset(new QToolButton());
    mButtonPlus->setAutoRepeat(true);
    mButtonPlus->setIconSize(iconSize);
    mButtonPlus->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mButtonPlus->setIcon(QIcon(createPixmap(iconSize * 0.75, "+")));
    connect(mButtonPlus.data(), &QToolButton::clicked, this, &QGVWidgetZoom::zoomPlus);

    mButtonMinus.reset(new QToolButton());
    mButtonMinus->setAutoRepeat(true);
    mButtonMinus->setIconSize(iconSize);
    mButtonMinus->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mButtonMinus->setIcon(QIcon(createPixmap(iconSize * 0.75, "-")));
    connect(mButtonMinus.data(), &QToolButton::clicked, this, &QGVWidgetZoom::zoomMinus);

    setOrientation(Qt::Vertical);
    setAnchor(QPoint(10, 0), { Qt::LeftEdge });
}

void QGVWidgetZoom::setOrientation(Qt::Orientation orientation)
{
    if (layout() != nullptr) {
        delete layout();
    }
    mOrientation = orientation;
    if (mOrientation == Qt::Horizontal) {
        setLayout(new QHBoxLayout(this));
    } else {
        setLayout(new QVBoxLayout(this));
    }
    layout()->setSpacing(0);
    layout()->setSizeConstraint(QLayout::SetMinimumSize);
    layout()->setContentsMargins(0, 0, 0, 0);
    layout()->addWidget(mButtonPlus.data());
    layout()->addWidget(mButtonMinus.data());
}

Qt::Orientation QGVWidgetZoom::getOrientation() const
{
    return mOrientation;
}

QToolButton* QGVWidgetZoom::plus()
{
    return mButtonPlus.data();
}

QToolButton* QGVWidgetZoom::minus()
{
    return mButtonMinus.data();
}

QPixmap QGVWidgetZoom::createPixmap(const QSize& size, const QString& text)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(qRgba(0, 0, 0, 0));
    QPixmap pixmap = QPixmap::fromImage(image, Qt::NoFormatConversion);
    QPainter painter(&pixmap);
    QPen pen = QPen(Qt::black);
    pen.setWidth(1);
    pen.setCosmetic(true);
    painter.setPen(pen);
    QBrush brush = QBrush(Qt::blue);
    painter.setBrush(brush);
    const auto path = QGV::createTextPath(QRect(QPoint(0, 0), size), text, font(), pen.width());
    painter.drawPath(path);
    return pixmap;
}

void QGVWidgetZoom::zoomPlus()
{
    getMap()->cameraTo(QGVCameraActions(getMap()).scaleBy(zoomExponentDown));
}

void QGVWidgetZoom::zoomMinus()
{
    getMap()->cameraTo(QGVCameraActions(getMap()).scaleBy(zoomExponentUp));
}
