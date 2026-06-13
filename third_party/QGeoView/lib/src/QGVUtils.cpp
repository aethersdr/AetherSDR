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

#include "QGVUtils.h"
#include <QtGlobal>

namespace QGV {

double metersToDistance(const double meters, const DistanceUnits unit)
{
    if (unit == DistanceUnits::Kilometers) {
        return meters / 1000;
    } else if (unit == DistanceUnits::NauticalMiles) {
        return meters / 1852.0;
    } else if (unit == DistanceUnits::Miles) {
        return meters / 1609.0;
    }
    return meters;
}

QString unitToString(const DistanceUnits unit)
{
    if (unit == DistanceUnits::Meters) {
        return QT_TRANSLATE_NOOP("QGV", "m");
    } else if (unit == DistanceUnits::Kilometers) {
        return QT_TRANSLATE_NOOP("QGV", "km");
    } else if (unit == DistanceUnits::NauticalMiles) {
        return QT_TRANSLATE_NOOP("QGV", "nm");
    } else if (unit == DistanceUnits::Miles) {
        return QT_TRANSLATE_NOOP("QGV", "mi");
    }
    return "";
}

} // namespace QGV
