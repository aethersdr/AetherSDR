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

#include "QGVLayerBDGEx.h"

#include <QtMath>

namespace {
// clang-format off
const QStringList URLTemplates = {
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctm25&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctm50&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctm100&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctm250&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctm250&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctmmultiescalas&srs=EPSG%3A4326&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng",
    "http://bdgex.eb.mil.br/mapcache?request=GetMap&service=WMS&version=1.1.1&layers=ctmmultiescalas_mercator&srs=EPSG%3A3857&bbox=lonLeft,latBottom,lonRight,latTop&width=WIDTH&height=HEIGHT&format=image%2Fpng"
};
// clang-format on
}

QGVLayerBDGEx::QGVLayerBDGEx(int serverNumber)
    : mUrl(URLTemplates.value(serverNumber))
{
    setName("Banco de Dados Geográfico do Exército");
    setDescription("Copyrights: \"Termo de Uso do BDGEx\"");
}

QGVLayerBDGEx::QGVLayerBDGEx(const QString& url)
    : mUrl(url)
{
    setName("Padrão");
    setDescription("Carta Topográfica Matricial");
}

void QGVLayerBDGEx::setUrl(const QString& url)
{
    mUrl = url;
}

QString QGVLayerBDGEx::getUrl() const
{
    return mUrl;
}

int QGVLayerBDGEx::minZoomlevel() const
{
    return 0;
}

int QGVLayerBDGEx::maxZoomlevel() const
{
    return 20;
}

QString QGVLayerBDGEx::tilePosToUrl(const QGV::GeoTilePos& tilePos) const
{
    QString url = mUrl;
    QGV::GeoRect rect = tilePos.toGeoRect();
    url.replace("lonLeft", QString::number(rect.lonLeft(), 'f', 6));
    url.replace("latBottom", QString::number(rect.latBottom(), 'f', 6));
    url.replace("lonRight", QString::number(rect.lonRight(), 'f', 6));
    url.replace("latTop", QString::number(rect.latTop(), 'f', 6));
    double m_width = rect.lonRight() - rect.lonLeft();
    double m_height = rect.latTop() - rect.latBottom();
    double ratio = m_width / m_height;
    int width_pixels = 900;
    int height_pixels = (int)((double)width_pixels / ratio);
    url.replace("WIDTH", QString::number(width_pixels));
    url.replace("HEIGHT", QString::number(height_pixels));
    return url;
}
