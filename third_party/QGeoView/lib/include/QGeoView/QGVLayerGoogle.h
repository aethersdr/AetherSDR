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

#include "QGVLayerTilesOnline.h"

class QGV_LIB_DECL QGVLayerGoogle : public QGVLayerTilesOnline
{
    Q_OBJECT

public:
    explicit QGVLayerGoogle(QGV::TilesType type = QGV::TilesType::Schema,
                            QLocale locale = QLocale(),
                            int serverNumber = 0);

    void setType(QGV::TilesType type);
    void setLocale(const QLocale& locale);

    QGV::TilesType getType() const;
    QLocale getLocale() const;

private:
    void createName();
    int minZoomlevel() const override;
    int maxZoomlevel() const override;
    QString tilePosToUrl(const QGV::GeoTilePos& tilePos) const override;

private:
    QGV::TilesType mType;
    QLocale mLocale;
    int mServerNumber;
};
