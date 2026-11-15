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

#include "QGVProjectionEPSG3857.h"

#include <QLineF>
#include <QtMath>

QGVProjectionEPSG3857::QGVProjectionEPSG3857()
    : QGVProjection("EPSG3857",
                    "WGS84 Web Mercator",
                    "QGVProjection used in web mapping applications like "
                    "Google/Bing/OpenStreetMap/etc. Sometimes known as "
                    "EPSG:900913.")
{
    mEarthRadius = 6378137.0; /* meters */
    mOriginShift = 2.0 * M_PI * mEarthRadius / 2.0;
    mGeoBoundary = QGV::GeoRect(85, -180, -85, +180);
    mProjBoundary = geoToProj(mGeoBoundary);
}

QGV::GeoRect QGVProjectionEPSG3857::boundaryGeoRect() const
{
    return mGeoBoundary;
}

QRectF QGVProjectionEPSG3857::boundaryProjRect() const
{
    return mProjBoundary;
}

QPointF QGVProjectionEPSG3857::geoToProj(const QGV::GeoPos& geoPos) const
{
    const double lon = geoPos.longitude();
    const double lat = (geoPos.latitude() > mGeoBoundary.topLeft().latitude()) ? mGeoBoundary.topLeft().latitude()
                                                                               : geoPos.latitude();
    const double x = lon * mOriginShift / 180.0;
    const double preY = -qLn(qTan((90.0 + lat) * M_PI / 360.0)) / (M_PI / 180.0);
    const double y = preY * mOriginShift / 180.0;
    return QPointF(x, y);
}

QGV::GeoPos QGVProjectionEPSG3857::projToGeo(const QPointF& projPos) const
{
    const double lon = (projPos.x() / mOriginShift) * 180.0;
    const double preLat = (-projPos.y() / mOriginShift) * 180.0;
    const double lat = 180.0 / M_PI * (2.0 * qAtan(qExp(preLat * M_PI / 180.0)) - M_PI / 2.0);
    return QGV::GeoPos(lat, lon);
}

QRectF QGVProjectionEPSG3857::geoToProj(const QGV::GeoRect& geoRect) const
{
    QRectF rect;
    rect.setTopLeft(geoToProj(geoRect.topLeft()));
    rect.setBottomRight(geoToProj(geoRect.bottomRight()));
    return rect;
}

QGV::GeoRect QGVProjectionEPSG3857::projToGeo(const QRectF& projRect) const
{
    return QGV::GeoRect(projToGeo(projRect.topLeft()), projToGeo(projRect.bottomRight()));
}

double QGVProjectionEPSG3857::geodesicMeters(const QPointF& projPos1, const QPointF& projPos2) const
{
    const QGV::GeoPos geoPos1 = projToGeo(projPos1);
    const QGV::GeoPos geoPos2 = projToGeo(projPos2);
    const double latitudeArc = (geoPos1.latitude() - geoPos2.latitude()) * M_PI / 180.0;
    const double longitudeArc = (geoPos1.longitude() - geoPos2.longitude()) * M_PI / 180.0;
    const double latitudeH = qPow(sin(latitudeArc * 0.5), 2);
    const double lontitudeH = qPow(sin(longitudeArc * 0.5), 2);
    const double lonFactor = cos(geoPos1.latitude() * M_PI / 180.0) * cos(geoPos2.latitude() * M_PI / 180.0);
    const double arcInRadians = 2.0 * asin(sqrt(latitudeH + lonFactor * lontitudeH));
    return mEarthRadius * arcInRadians;
}
