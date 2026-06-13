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

#include <QGVProjection.h>

QGVProjection::QGVProjection(const QString& id, const QString& name, const QString& description)
    : mID(id)
    , mName(name)
    , mDescription(description)
{
}

QString QGVProjection::getID() const
{
    return mID;
}

QString QGVProjection::getName() const
{
    return mName;
}

QString QGVProjection::getDescription() const
{
    return mDescription;
}
