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

#include "QGVLayerBing.h"

#include <QtMath>

namespace {
// clang-format off
const QMap<QGV::TilesType, QStringList> URLTemplates = {
    { QGV::TilesType::Satellite, {
          "http://t0.tiles.virtualearth.net/tiles/a${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t1.tiles.virtualearth.net/tiles/a${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t2.tiles.virtualearth.net/tiles/a${qk}.jpeg?g=181&mkt=${lcl}",}
    },
    { QGV::TilesType::Schema, {
          "http://t0.tiles.virtualearth.net/tiles/r${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t1.tiles.virtualearth.net/tiles/r${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t2.tiles.virtualearth.net/tiles/r${qk}.jpeg?g=181&mkt=${lcl}",}
    },
    { QGV::TilesType::Hybrid, {
          "http://t0.tiles.virtualearth.net/tiles/h${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t1.tiles.virtualearth.net/tiles/h${qk}.jpeg?g=181&mkt=${lcl}",
          "http://t2.tiles.virtualearth.net/tiles/h${qk}.jpeg?g=181&mkt=${lcl}",}
    },
};
// clang-format on
}

QGVLayerBing::QGVLayerBing(QGV::TilesType type, QLocale locale, int serverNumber)
    : mType(type)
    , mLocale(locale)
    , mServerNumber(serverNumber)
{
    createName();
    setDescription("Copyrights ©Microsoft");
}

void QGVLayerBing::setType(QGV::TilesType type)
{
    mType = type;
    createName();
}

void QGVLayerBing::setLocale(const QLocale& locale)
{
    mLocale = locale;
    createName();
}

QGV::TilesType QGVLayerBing::getType() const
{
    return mType;
}

QLocale QGVLayerBing::getLocale() const
{
    return mLocale;
}

void QGVLayerBing::createName()
{
    // clang-format off
    const QMap<QGV::TilesType, QString> adapter = {
        { QGV::TilesType::Satellite, "QGV::Satellite" },
        { QGV::TilesType::Schema, "QGV::Schema" },
        { QGV::TilesType::Hybrid, "QGV::Hybrid" },
    };
    // clang-format on
    setName("Bing Maps (" + adapter[mType] + " " + mLocale.name() + ")");
}

int QGVLayerBing::minZoomlevel() const
{
    return 1;
}

int QGVLayerBing::maxZoomlevel() const
{
    return 19;
}

QString QGVLayerBing::tilePosToUrl(const QGV::GeoTilePos& tilePos) const
{
    const QStringList& list = URLTemplates[mType];
    QString url = list.value(mServerNumber).toLower();
    url.replace("${lcl}", mLocale.name());
    url.replace("${qk}", tilePos.toQuadKey());
    return url;
}
