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

#pragma once

#include "QGVProjection.h"

class QGV_LIB_DECL QGVProjectionEPSG3857 : public QGVProjection
{
public:
    QGVProjectionEPSG3857();
    virtual ~QGVProjectionEPSG3857() = default;

private:
    QGV::GeoRect boundaryGeoRect() const override final;
    QRectF boundaryProjRect() const override final;

    QPointF geoToProj(QGV::GeoPos const& geoPos) const override final;
    QGV::GeoPos projToGeo(QPointF const& projPos) const override final;
    QRectF geoToProj(QGV::GeoRect const& geoRect) const override final;
    QGV::GeoRect projToGeo(QRectF const& projRect) const override final;

    double geodesicMeters(QPointF const& projPos1, QPointF const& projPos2) const override final;

private:
    double mEarthRadius;
    double mOriginShift;
    QGV::GeoRect mGeoBoundary;
    QRectF mProjBoundary;
};
