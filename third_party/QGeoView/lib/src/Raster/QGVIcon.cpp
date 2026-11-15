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

#include "Raster/QGVIcon.h"
#include "QGVMap.h"

#include <QPainter>

QGVIcon::QGVIcon()
{
    setFlag(QGV::ItemFlag::IgnoreScale);
    setFlag(QGV::ItemFlag::IgnoreAzimuth);
}

void QGVIcon::setGeometry(const QGV::GeoPos& geoPos, const QSizeF& imageSize)
{
    mGeoPos = geoPos;
    mProjPos = {};
    mImageSize = imageSize;
    mProjRect = {};
    calculateGeometry();
}

void QGVIcon::setGeometry(const QPointF& projPos, const QSizeF& imageSize)
{
    mGeoPos = {};
    mProjPos = projPos;
    mImageSize = imageSize;
    mProjRect = {};
    calculateGeometry();
}

QImage QGVIcon::getImage() const
{
    return mImage;
}

bool QGVIcon::isImage() const
{
    return !mImage.isNull();
}

void QGVIcon::loadImage(const QByteArray& rawData)
{
    QImage image;
    image.loadFromData(rawData);
    loadImage(image);
}

void QGVIcon::loadImage(const QImage& image)
{
    mImage = image;
    calculateGeometry();
}

void QGVIcon::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    calculateGeometry();
}

QPainterPath QGVIcon::projShape() const
{
    QPainterPath path;
    path.addRect(mProjRect);
    return path;
}

void QGVIcon::projPaint(QPainter* painter)
{
    if (mImage.isNull() || mProjRect.isEmpty()) {
        return;
    }

    QRectF paintRect = mProjRect;

    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(paintRect, getImage());
}

void QGVIcon::calculateGeometry()
{
    if (getMap() == nullptr) {
        return;
    }

    if (!mGeoPos.isEmpty()) {
        mProjPos = getMap()->getProjection()->geoToProj(mGeoPos);
    }

    const QSizeF baseSize = !mImageSize.isEmpty() ? mImageSize : mImage.size();
    const QPointF baseAnchor = QPointF(baseSize.width() / 2, baseSize.height() / 2);

    mProjRect = QRectF(mProjPos - baseAnchor, baseSize);

    resetBoundary();
    refresh();
}
