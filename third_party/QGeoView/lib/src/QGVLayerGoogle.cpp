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

#include "QGVLayerGoogle.h"

#include <QtMath>

namespace {
// clang-format off
const QMap<QGV::TilesType, QStringList> URLTemplates = {
    {QGV::TilesType::Satellite, {
         "https://mts0.google.com/vt/lyrs=s@186112443&hl=${lcl}&x=${x}&y=${y}&z=${z}&s=Galile",
         "https://mts1.google.com/vt/lyrs=s@186112443&hl=${lcl}&x=${x}&y=${y}&z=${z}&s=Galile",
         "https://mts2.google.com/vt/lyrs=s@186112443&hl=${lcl}&x=${x}&y=${y}&z=${z}&s=Galile",
         }
    },
    {QGV::TilesType::Schema, {
         "http://mt1.google.com/vt/lyrs=m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         "http://mt2.google.com/vt/lyrs=m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         "http://mt3.google.com/vt/lyrs=m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         }
    },
    {QGV::TilesType::Hybrid, {
         "http://mt1.google.com/vt/lyrs=s,m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         "http://mt2.google.com/vt/lyrs=s,m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         "http://mt3.google.com/vt/lyrs=s,m@110&hl=${lcl}&x=${x}&y=${y}&z=${z}",
         }
    },
};
// clang-format on
}

QGVLayerGoogle::QGVLayerGoogle(QGV::TilesType type, QLocale locale, int serverNumber)
    : mType(type)
    , mLocale(locale)
    , mServerNumber(serverNumber)
{
    createName();
    setDescription("Copyrights ©Google");
}

void QGVLayerGoogle::setType(QGV::TilesType type)
{
    mType = type;
    createName();
}

void QGVLayerGoogle::setLocale(const QLocale& locale)
{
    mLocale = locale;
    createName();
}

QGV::TilesType QGVLayerGoogle::getType() const
{
    return mType;
}

QLocale QGVLayerGoogle::getLocale() const
{
    return mLocale;
}

void QGVLayerGoogle::createName()
{
    // clang-format off
    const QMap<QGV::TilesType, QString> adapter = {
        { QGV::TilesType::Satellite, "QGV::Satellite" },
        { QGV::TilesType::Schema, "QGV::Schema" },
        { QGV::TilesType::Hybrid, "QGV::Hybrid" },
    };
    // clang-format on
    setName("Google Maps (" + adapter[mType] + " " + mLocale.name() + ")");
}

int QGVLayerGoogle::minZoomlevel() const
{
    return 0;
}

int QGVLayerGoogle::maxZoomlevel() const
{
    return 21;
}

QString QGVLayerGoogle::tilePosToUrl(const QGV::GeoTilePos& tilePos) const
{
    const QStringList& list = URLTemplates[mType];
    QString url = list.value(mServerNumber).toLower();
    url.replace("${lcl}", mLocale.name());
    url.replace("${z}", QString::number(tilePos.zoom()));
    url.replace("${x}", QString::number(tilePos.pos().x()));
    url.replace("${y}", QString::number(tilePos.pos().y()));
    return url;
}
