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

#include <QDebug>
#include <QNetworkAccessManager>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#ifndef QGV_LIB_DECL
#if defined(QGV_EXPORT)
#define QGV_LIB_DECL Q_DECL_EXPORT
#else
#define QGV_LIB_DECL Q_DECL_IMPORT
#endif
#endif

namespace QGV {

enum class Projection
{
    EPSG3857,
};

enum class TilesType
{
    Satellite,
    Schema,
    Hybrid,
};

enum BDGExLayer
{
    ctm25,
    ctm50,
    ctm100,
    ctm250,
    ctmmultiescalas,
    ctmmultiescalas_mercator
};

enum class MapState
{
    Idle,
    Animation,
    Wheel,
    MovingMap,
    MovingObjects,
    SelectionRect,
};

enum class DistanceUnits
{
    Meters,
    Kilometers,
    NauticalMiles,
    Miles,
};

enum class MouseAction : int
{
    Move = 0x1,
    ZoomWheel = 0x2,
    ZoomRect = 0x4,
    Selection = 0x8,
    Tooltip = 0x10,
    ContextMenu = 0x20,
    MoveObjects = 0x40,
    All = 0xFF,
};
Q_DECLARE_FLAGS(MouseActions, MouseAction)

enum class ItemFlag : int
{
    IgnoreScale = 0x1,
    IgnoreAzimuth = 0x2,
    Highlightable = 0x4,
    Highlighted = 0x8,
    HighlightCustom = 0x10,
    SelectCustom = 0x20,
    Transformed = 0x40,
    Clickable = 0x80,
    Movable = 0x100,
};
Q_DECLARE_FLAGS(ItemFlags, ItemFlag)

class QGV_LIB_DECL GeoPos
{
public:
    GeoPos();
    GeoPos(double lat, double lon);

    bool isEmpty() const;

    double latitude() const;
    double longitude() const;

    void setLat(double late);
    void setLon(double lon);

    QString lonToString(const QString& format = QStringLiteral("[+-]d")) const;
    QString latToString(const QString& format = QStringLiteral("[+-]d")) const;

    static QString lonToString(double lon, const QString& format = QStringLiteral("[+-]d"));
    static QString latToString(double lat, const QString& format = QStringLiteral("[+-]d"));

    bool operator==(const GeoPos& rhs);
    bool operator!=(const GeoPos& rhs);

private:
    bool mEmpty;
    double mLat;
    double mLon;
};

class QGV_LIB_DECL GeoRect
{
public:
    GeoRect();
    GeoRect(double lat1, double lon1, double lat2, double lon2);
    GeoRect(GeoPos const& pos1, GeoPos const& pos2);

    bool isEmpty() const;

    GeoPos topLeft() const;
    GeoPos topRight() const;
    GeoPos bottomLeft() const;
    GeoPos bottomRight() const;
    double lonLeft() const;
    double lonRight() const;
    double latBottom() const;
    double latTop() const;

    bool contains(GeoPos const& pos) const;
    bool contains(GeoRect const& rect) const;
    bool intersects(GeoRect const& rect) const;

    bool operator==(const GeoRect& rhs);
    bool operator!=(const GeoRect& rhs);

private:
    GeoPos mTopLeft;
    GeoPos mBottomRight;
};

class QGV_LIB_DECL GeoTilePos
{
public:
    GeoTilePos();
    GeoTilePos(int zoom, const QPoint& pos);
    GeoTilePos(const GeoTilePos& other);
    GeoTilePos(const GeoTilePos&& other);
    GeoTilePos& operator=(const GeoTilePos& other);
    GeoTilePos& operator=(const GeoTilePos&& other);

    bool operator<(const GeoTilePos& other) const;

    int zoom() const;
    QPoint pos() const;

    bool contains(const GeoTilePos& other) const;
    GeoTilePos parent(int parentZoom) const;

    GeoRect toGeoRect() const;
    QString toQuadKey() const;

    static GeoTilePos geoToTilePos(int zoom, const GeoPos& geoPos);

private:
    int mZoom;
    QPoint mPos;
};

QGV_LIB_DECL void setNetworkManager(QNetworkAccessManager* manager);
QGV_LIB_DECL QNetworkAccessManager* getNetworkManager();

// AetherSDR patch: app-identifying User-Agent for online tile requests.
QGV_LIB_DECL void setTileUserAgent(const QByteArray& userAgent);
QGV_LIB_DECL QByteArray getTileUserAgent();

QGV_LIB_DECL QTransform createTransfrom(QPointF const& projAnchor, double scale, double azimuth);
QGV_LIB_DECL QTransform createTransfromScale(QPointF const& projAnchor, double scale);
QGV_LIB_DECL QTransform createTransfromAzimuth(QPointF const& projAnchor, double azimuth);
QGV_LIB_DECL QPainterPath createTextPath(const QRect& rect, const QString& text, const QFont& font, int penWidth);

QGV_LIB_DECL void setDrawDebug(bool enabled);
QGV_LIB_DECL bool isDrawDebug();
QGV_LIB_DECL void setPrintDebug(bool enabled);
QGV_LIB_DECL bool isPrintDebug();

} // namespace QGV

QGV_LIB_DECL QDebug operator<<(QDebug debug, const QGV::GeoPos& value);
QGV_LIB_DECL QDebug operator<<(QDebug debug, const QGV::GeoRect& value);
QGV_LIB_DECL QDebug operator<<(QDebug debug, const QGV::GeoTilePos& value);

Q_DECLARE_METATYPE(QGV::GeoPos)
Q_DECLARE_METATYPE(QGV::GeoRect)
Q_DECLARE_METATYPE(QGV::GeoTilePos)

Q_DECLARE_OPERATORS_FOR_FLAGS(QGV::ItemFlags)

#define qgvDebug                                                                                                       \
    if (QGV::isPrintDebug())                                                                                           \
    qDebug
#define qgvInfo                                                                                                        \
    if (QGV::isPrintDebug())                                                                                           \
    qInfo
#define qgvWarning                                                                                                     \
    if (QGV::isPrintDebug())                                                                                           \
    qWarning
#define qgvCritical qCritical
