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

#include "QGVGlobal.h"

class QGV_LIB_DECL QGVProjection
{
public:
    explicit QGVProjection(const QString& id, const QString& name, const QString& description);
    virtual ~QGVProjection() = default;

    QString getID() const;
    QString getName() const;
    QString getDescription() const;

    virtual QGV::GeoRect boundaryGeoRect() const = 0;
    virtual QRectF boundaryProjRect() const = 0;

    virtual QPointF geoToProj(QGV::GeoPos const& geoPos) const = 0;
    virtual QGV::GeoPos projToGeo(QPointF const& projPos) const = 0;
    virtual QRectF geoToProj(QGV::GeoRect const& geoRect) const = 0;
    virtual QGV::GeoRect projToGeo(QRectF const& projRect) const = 0;
    virtual double geodesicMeters(QPointF const& projPos1, QPointF const& projPos2) const = 0;

private:
    Q_DISABLE_COPY(QGVProjection)
    QString mID;
    QString mName;
    QString mDescription;
};
