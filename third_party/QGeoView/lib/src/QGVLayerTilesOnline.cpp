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

#include "QGVLayerTilesOnline.h"
#include "Raster/QGVImage.h"
#include "QGVMap.h"
#include "QGVProjection.h"

#include <limits>

namespace {
constexpr int kDecodedTileCacheBytes = 64 * 1024 * 1024;
}

QGVLayerTilesOnline::QGVLayerTilesOnline()
{
    mDecodedTileCache.setMaxCost(kDecodedTileCacheBytes);
}

QGVLayerTilesOnline::~QGVLayerTilesOnline()
{
    qDeleteAll(mRequest);
}

QRectF QGVLayerTilesOnline::tileProjectionRect(
    const QGV::GeoTilePos& tilePos) const
{
    const int sourceX = QGV::wrapTileX(tilePos.zoom(), tilePos.pos().x());
    const int tileCount = 1 << tilePos.zoom();
    const int worldCopy = (tilePos.pos().x() - sourceX) / tileCount;
    const QGV::GeoTilePos sourceTile(
        tilePos.zoom(), QPoint(sourceX, tilePos.pos().y()));
    const QGVProjection* projection = getMap()->getProjection();
    QRectF rect = projection->geoToProj(sourceTile.toGeoRect());
    rect.translate(worldCopy * projection->boundaryProjRect().width(), 0.0);
    return rect;
}

void QGVLayerTilesOnline::request(const QGV::GeoTilePos& tilePos)
{
    Q_ASSERT(QGV::getNetworkManager());

    const QGV::GeoTilePos sourceTilePos(
        tilePos.zoom(),
        QPoint(QGV::wrapTileX(tilePos.zoom(), tilePos.pos().x()),
               tilePos.pos().y()));
    const QUrl url(tilePosToUrl(sourceTilePos));

    if (const QImage* cached = mDecodedTileCache.object(url)) {
        auto* tile = new QGVImage();
        tile->setGeometry(tileProjectionRect(tilePos));
        tile->loadImage(*cached);
        onTile(tilePos, tile);
        return;
    }

    QNetworkRequest request(url);
    // AetherSDR patch: honor proper TLS verification and an app-identifying
    // User-Agent (OSM tile policy forbids browser impersonation).
    request.setRawHeader("User-Agent", QGV::getTileUserAgent());
    request.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    QNetworkReply* reply = QGV::getNetworkManager()->get(request);

    mRequest[tilePos] = reply;
    connect(reply, &QNetworkReply::finished, reply, [this, reply, tilePos]() { onReplyFinished(reply, tilePos); });

    qgvDebug() << "request" << url;
}

void QGVLayerTilesOnline::cancel(const QGV::GeoTilePos& tilePos)
{
    removeReply(tilePos);
}

void QGVLayerTilesOnline::onReplyFinished(QNetworkReply* reply, const QGV::GeoTilePos& tilePos)
{
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            qgvCritical() << "ERROR" << reply->errorString();
        }
        removeReply(tilePos);
        return;
    }
    const QByteArray rawImage = reply->readAll();
    QImage decodedImage;
    if (!decodedImage.loadFromData(rawImage)) {
        qgvCritical() << "ERROR failed to decode tile" << reply->url();
        removeReply(tilePos);
        return;
    }
    const qsizetype decodedBytes = decodedImage.sizeInBytes();
    const int cacheCost = static_cast<int>(qMin<qsizetype>(
        decodedBytes, std::numeric_limits<int>::max()));
    mDecodedTileCache.insert(reply->url(), new QImage(decodedImage), cacheCost);
    auto tile = new QGVImage();
    tile->setGeometry(tileProjectionRect(tilePos));
    tile->loadImage(decodedImage);
    tile->setProperty("drawDebug",
                      QString("%1\ntile(%2,%3,%4)")
                              .arg(reply->url().toString())
                              .arg(tilePos.zoom())
                              .arg(tilePos.pos().x())
                              .arg(tilePos.pos().y()));
    removeReply(tilePos);
    onTile(tilePos, tile);
}

void QGVLayerTilesOnline::removeReply(const QGV::GeoTilePos& tilePos)
{
    QNetworkReply* reply = mRequest.value(tilePos, nullptr);
    if (reply == nullptr) {
        return;
    }
    mRequest.remove(tilePos);
    reply->abort();
    reply->close();
    reply->deleteLater();
}
