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

// AetherSDR patch: the canonical (unwrapped) tile every repeated world copy
// resolves to. Requests are keyed on this so N visible copies of the same tile
// share one GET instead of issuing N identical ones.
QGV::GeoTilePos QGVLayerTilesOnline::canonicalTile(
    const QGV::GeoTilePos& tilePos)
{
    return QGV::GeoTilePos(
        tilePos.zoom(),
        QPoint(QGV::wrapTileX(tilePos.zoom(), tilePos.pos().x()),
               tilePos.pos().y()));
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

    const QGV::GeoTilePos sourceTilePos = canonicalTile(tilePos);
    const QUrl url(tilePosToUrl(sourceTilePos));

    if (const QImage* cached = mDecodedTileCache.object(url)) {
        auto* tile = new QGVImage();
        tile->setGeometry(tileProjectionRect(tilePos));
        tile->loadImage(*cached);
        // AetherSDR patch: this re-enters onTile() SYNCHRONOUSLY, unlike every
        // other path here, which arrives from a queued reply. onTile() calls
        // addTile() and then removeAllAbove()/removeWhenCovered(), all of which
        // mutate mIndex while QGVLayerTiles::processCamera() may still be
        // iterating its `missing` list. That is safe today only because
        // `missing` is a separate container, existingTiles() returns keys by
        // value, and neither remover touches the zoom level being added — none
        // of which upstream guarantees. Callers must not be iterating mIndex.
        onTile(tilePos, tile);
        return;
    }

    // AetherSDR patch: coalesce the repeated world copies. Every visible copy of
    // this tile resolves to the same canonical URL, so keying the in-flight map
    // on the unwrapped position would fire one identical GET per copy. Key on
    // the canonical tile and fan the finished reply out to everyone waiting.
    // Copies of one tile differ only in x (same zoom, same y), and GeoTilePos
    // has no operator==, so the waiter list holds the unwrapped x values.
    auto waiting = mWaiting.find(sourceTilePos);
    if (waiting != mWaiting.end()) {
        if (!waiting->contains(tilePos.pos().x())) {
            waiting->append(tilePos.pos().x());
        }
        return;
    }
    mWaiting.insert(sourceTilePos, { tilePos.pos().x() });

    QNetworkRequest request(url);
    // AetherSDR patch: honor proper TLS verification and an app-identifying
    // User-Agent (OSM tile policy forbids browser impersonation).
    request.setRawHeader("User-Agent", QGV::getTileUserAgent());
    request.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    QNetworkReply* reply = QGV::getNetworkManager()->get(request);

    mRequest[sourceTilePos] = reply;
    connect(reply, &QNetworkReply::finished, reply,
            [this, reply, sourceTilePos]() { onReplyFinished(reply, sourceTilePos); });

    qgvDebug() << "request" << url;
}

void QGVLayerTilesOnline::cancel(const QGV::GeoTilePos& tilePos)
{
    // AetherSDR patch: one reply can serve several world copies, so only abort
    // it once the last copy waiting on it has gone away.
    const QGV::GeoTilePos sourceTilePos = canonicalTile(tilePos);
    auto waiting = mWaiting.find(sourceTilePos);
    if (waiting != mWaiting.end()) {
        waiting->removeAll(tilePos.pos().x());
        if (!waiting->isEmpty()) {
            return;
        }
        mWaiting.erase(waiting);
    }
    removeReply(sourceTilePos);
}

void QGVLayerTilesOnline::onReplyFinished(QNetworkReply* reply, const QGV::GeoTilePos& tilePos)
{
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            qgvCritical() << "ERROR" << reply->errorString();
        }
        mWaiting.remove(tilePos);
        removeReply(tilePos);
        return;
    }
    const QByteArray rawImage = reply->readAll();
    QImage decodedImage;
    if (!decodedImage.loadFromData(rawImage)) {
        qgvCritical() << "ERROR failed to decode tile" << reply->url();
        mWaiting.remove(tilePos);
        removeReply(tilePos);
        return;
    }
    const qsizetype decodedBytes = decodedImage.sizeInBytes();
    const int cacheCost = static_cast<int>(qMin<qsizetype>(
        decodedBytes, std::numeric_limits<int>::max()));
    const QUrl url = reply->url();
    mDecodedTileCache.insert(url, new QImage(decodedImage), cacheCost);

    // Detach the waiter list and drop the reply BEFORE dispatching: onTile()
    // can re-enter removeTile() -> cancel(), which mutates mWaiting.
    const QList<int> copies = mWaiting.take(tilePos);
    removeReply(tilePos);

    for (const int copyX : copies) {
        const QGV::GeoTilePos copy(tilePos.zoom(),
                                   QPoint(copyX, tilePos.pos().y()));
        auto* tile = new QGVImage();
        tile->setGeometry(tileProjectionRect(copy));
        tile->loadImage(decodedImage);
        tile->setProperty("drawDebug",
                          QString("%1\ntile(%2,%3,%4)")
                                  .arg(url.toString())
                                  .arg(copy.zoom())
                                  .arg(copy.pos().x())
                                  .arg(copy.pos().y()));
        onTile(copy, tile);
    }
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
