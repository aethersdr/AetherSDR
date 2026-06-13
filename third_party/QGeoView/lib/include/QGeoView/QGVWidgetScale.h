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

#include "QGVWidget.h"

class QGV_LIB_DECL QGVWidgetScale : public QGVWidget
{
    Q_OBJECT

public:
    QGVWidgetScale(Qt::Orientation orientation = Qt::Horizontal);

    void setAutoAdjust(bool autoAdjust);
    bool getAutoAdjust() const;

    void setOrientation(Qt::Orientation orientation);
    Qt::Orientation getOrientation() const;

    void setDistanceUnits(QGV::DistanceUnits distanceUnits);
    QGV::DistanceUnits getDistanceUnits() const;

    void setUseMetersForSmallDistance(bool useMetersForSmallDistance);
    bool getUseMetersForSmallDistance() const;

protected:
    QString getDistanceLabel(int meters) const;

    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override;
    void paintEvent(QPaintEvent* event) override;

private:
    Qt::Orientation mOrientation;
    bool mAutoAdjust;
    int mScaleMeters;
    int mScalePixels;
    QGV::DistanceUnits mDistanceUnits;
    bool mUseMetersForSmallDistance;
};
