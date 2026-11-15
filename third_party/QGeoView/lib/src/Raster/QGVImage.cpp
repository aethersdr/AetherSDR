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

#include "Raster/QGVImage.h"
#include "QGVMap.h"

#include <QPainter>

QGVImage::QGVImage()
    : mCeilingOnScale{ true }
{
}

void QGVImage::setGeometry(const QGV::GeoRect& geoRect)
{
    mGeoRect = geoRect;
    mProjRect = {};
    calculateGeometry();
}

void QGVImage::setGeometry(const QRectF& projRect)
{
    mGeoRect = {};
    mProjRect = projRect;
    calculateGeometry();
}

QImage QGVImage::getImage() const
{
    return mImage;
}

bool QGVImage::isImage() const
{
    return !mImage.isNull();
}

void QGVImage::loadImage(const QByteArray& rawData)
{
    QImage image;
    image.loadFromData(rawData);
    loadImage(image);
}

void QGVImage::loadImage(const QImage& image)
{
    mImage = image;
    calculateGeometry();
}

void QGVImage::setCeilingOnScale(bool enabled)
{
    mCeilingOnScale = enabled;
}

void QGVImage::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    calculateGeometry();
}

QPainterPath QGVImage::projShape() const
{
    QPainterPath path;
    path.addRect(mProjRect);
    return path;
}

void QGVImage::projPaint(QPainter* painter)
{
    if (mImage.isNull() || mProjRect.isEmpty()) {
        return;
    }

    QRectF paintRect = mProjRect;

    if (mCeilingOnScale && !isFlag(QGV::ItemFlag::IgnoreScale)) {
        const double pixelFactor = 1.0 / getMap()->getCamera().scale();
        paintRect.setSize(paintRect.size() + QSizeF(pixelFactor, pixelFactor));
    }

    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(paintRect, getImage());
}

void QGVImage::calculateGeometry()
{
    if (getMap() == nullptr) {
        return;
    }

    if (!mGeoRect.isEmpty()) {
        mProjRect = getMap()->getProjection()->geoToProj(mGeoRect);
    }

    resetBoundary();
    refresh();
}
