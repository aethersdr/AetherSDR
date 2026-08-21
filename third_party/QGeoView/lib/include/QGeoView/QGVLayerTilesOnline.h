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

#include "QGVLayerTiles.h"

#include <QCache>
#include <QImage>
#include <QUrl>

#include <QNetworkReply>

class QGV_LIB_DECL QGVLayerTilesOnline : public QGVLayerTiles
{
    Q_OBJECT

public:
    QGVLayerTilesOnline();
    ~QGVLayerTilesOnline();

protected:
    virtual QString tilePosToUrl(const QGV::GeoTilePos& tilePos) const = 0;

private:
    static QGV::GeoTilePos canonicalTile(const QGV::GeoTilePos& tilePos);
    QRectF tileProjectionRect(const QGV::GeoTilePos& tilePos) const;
    void request(const QGV::GeoTilePos& tilePos) override;
    void cancel(const QGV::GeoTilePos& tilePos) override;
    void onReplyFinished(QNetworkReply* reply, const QGV::GeoTilePos& tilePos);
    void removeReply(const QGV::GeoTilePos& tilePos);

private:
    // AetherSDR patch: both maps are keyed on the CANONICAL tile, so the world
    // copies of one tile share a single GET. mWaiting holds the unwrapped x of
    // every copy still expecting that reply (copies differ only in x).
    QMap<QGV::GeoTilePos, QNetworkReply*> mRequest;
    QMap<QGV::GeoTilePos, QList<int>> mWaiting;
    QCache<QUrl, QImage> mDecodedTileCache;
};
