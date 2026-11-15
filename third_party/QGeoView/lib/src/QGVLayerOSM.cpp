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

#include "QGVLayerOSM.h"

#include <QtMath>

namespace {
// clang-format off
const QStringList URLTemplates = {
    "https://tile.openstreetmap.org/${z}/${x}/${y}.png",
    "https://tile.openstreetmap.org/${z}/${x}/${y}.png",
    "https://tile.openstreetmap.org/${z}/${x}/${y}.png",
};
// clang-format on
}

QGVLayerOSM::QGVLayerOSM(int serverNumber)
    : mUrl(URLTemplates.value(serverNumber))
{
    setName("OpenStreetMap");
    setDescription("Copyrights ©OpenStreetMap");
}

QGVLayerOSM::QGVLayerOSM(const QString& url)
    : mUrl(url)
{
    setName("Custom");
    setDescription("OSM-like map");
}

void QGVLayerOSM::setUrl(const QString& url)
{
    mUrl = url;
}

QString QGVLayerOSM::getUrl() const
{
    return mUrl;
}

int QGVLayerOSM::minZoomlevel() const
{
    return 0;
}

int QGVLayerOSM::maxZoomlevel() const
{
    return 20;
}

QString QGVLayerOSM::tilePosToUrl(const QGV::GeoTilePos& tilePos) const
{
    QString url = mUrl.toLower();
    url.replace("${z}", QString::number(tilePos.zoom()));
    url.replace("${x}", QString::number(tilePos.pos().x()));
    url.replace("${y}", QString::number(tilePos.pos().y()));
    return url;
}
