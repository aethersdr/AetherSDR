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

#include "QGVGlobal.h"
#include "QGVMap.h"

#include <QTransform>
#include <QtGlobal>
#include <QtMath>

namespace {
bool drawDebugEnabled = false;
bool printDebugEnabled = false;
QNetworkAccessManager* networkManager = nullptr;
}

namespace QGV {

GeoPos::GeoPos()
    : mEmpty(true)
    , mLat(0)
    , mLon(0)
{
}

GeoPos::GeoPos(double lat, double lon)
    : mEmpty(false)
{
    setLat(lat);
    setLon(lon);
}

bool GeoPos::isEmpty() const
{
    return mEmpty;
}

double GeoPos::latitude() const
{
    return mLat;
}
double GeoPos::longitude() const
{
    return mLon;
}

void GeoPos::setLat(double lat)
{
    mLat = qMax(-90.0, lat);
    mLat = qMin(90.0, lat);
}

void GeoPos::setLon(double lon)
{
    if (lon > 180.000001) {
        lon = fmod((180.0 + lon), 360.0) - 180.0;
    } else {
        if (lon < -180.000001) {
            lon = 180.0 - fmod((180.0 - lon), 360.0);
        }
    }
    mLon = lon;
}

QString GeoPos::lonToString(const QString& format) const
{
    return lonToString(longitude(), format);
}

QString GeoPos::latToString(const QString& format) const
{
    return latToString(latitude(), format);
}

/*!
 * Longitude to string
 * Format:
 * [+-] - sign
 * d -  degree (unsigned)
 * di - degree integer part only(unsigned)
 * m -  minute only(unsigned)
 * mi - minute integer part only(unsigned)
 * s -  second (unsigned)
 * si - second integer part only(unsigned)
 * \brief lonToString
 * \param lon
 * \param aFormat
 * \return
 */
QString GeoPos::lonToString(double lon, const QString& format)
{
    QString result = format;

    const QString signSymb = (lon < 0) ? "-" : QString();
    const double degreePart = qAbs(lon);
    const double minPart = (degreePart - static_cast<int>(degreePart)) * 60.0;
    const double secPart = (minPart - static_cast<int>(minPart)) * 60.0;

    result.replace("[+-]", signSymb);
    result.replace("di", QString::number(static_cast<int>(degreePart)));
    result.replace("d", QString::number(degreePart, 'f', 6));
    result.replace("mi", QString::number(static_cast<int>(minPart)));
    result.replace("m", QString::number(minPart, 'f', 4));
    result.replace("si", QString::number(static_cast<int>(secPart)));
    result.replace("s", QString::number(secPart, 'f', 3));

    return result;
}

/*!
 * Latitude to string
 * Format:
 * [+-] - sign
 * [NS] - North or South
 * d -  degree (unsigned)
 * di - degree integer part only(unsigned)
 * m -  minute only(unsigned)
 * mi - minute integer part only(unsigned)
 * s -  second (unsigned)
 * si - second integer part only(unsigned)
 */
QString GeoPos::latToString(double lat, const QString& format)
{
    QString result = format;

    const QString signSymb = (lat < 0) ? "-" : QString();
    const QString northSouth = (lat < 0) ? "N" : "S";
    const double degreePart = qAbs(lat);
    const double minPart = (degreePart - static_cast<int>(degreePart)) * 60.0;
    const double secPart = (minPart - static_cast<int>(minPart)) * 60.0;

    result.replace("[+-]", signSymb);
    result.replace("[NS]", northSouth);
    result.replace("di", QString::number(static_cast<int>(degreePart)));
    result.replace("d", QString::number(degreePart, 'f', 6));
    result.replace("mi", QString::number(static_cast<int>(minPart)));
    result.replace("m", QString::number(minPart, 'f', 4));
    result.replace("si", QString::number(static_cast<int>(secPart)));
    result.replace("s", QString::number(secPart, 'f', 3));

    return result;
}

bool GeoPos::operator==(const GeoPos& rhs)
{
    return (mEmpty && rhs.mEmpty) || (mLat == rhs.mLat && mLon == rhs.mLon);
}

bool GeoPos::operator!=(const GeoPos& rhs)
{
    return !(*this == rhs);
}

GeoRect::GeoRect()
{
}

GeoRect::GeoRect(double lat1, double lon1, double lat2, double lon2)
{
    mTopLeft = GeoPos(qMax(lat1, lat2), qMin(lon1, lon2));
    mBottomRight = GeoPos(qMin(lat1, lat2), qMax(lon1, lon2));
}

GeoRect::GeoRect(GeoPos const& pos1, GeoPos const& pos2)
{
    mTopLeft = GeoPos(qMax(pos1.latitude(), pos2.latitude()), qMin(pos1.longitude(), pos2.longitude()));
    mBottomRight = GeoPos(qMin(pos1.latitude(), pos2.latitude()), qMax(pos1.longitude(), pos2.longitude()));
}

bool GeoRect::isEmpty() const
{
    return mTopLeft.isEmpty() || mBottomRight.isEmpty();
}

GeoPos GeoRect::topLeft() const
{
    return mTopLeft;
}

GeoPos GeoRect::topRight() const
{
    return GeoPos(mTopLeft.latitude(), mBottomRight.longitude());
}

GeoPos GeoRect::bottomLeft() const
{
    return GeoPos(mBottomRight.latitude(), mTopLeft.longitude());
}

GeoPos GeoRect::bottomRight() const
{
    return mBottomRight;
}

double GeoRect::lonLeft() const
{
    return mTopLeft.longitude();
}

double GeoRect::lonRight() const
{
    return mBottomRight.longitude();
}

double GeoRect::latBottom() const
{
    return mBottomRight.latitude();
}

double GeoRect::latTop() const
{
    return mTopLeft.latitude();
}

bool GeoRect::contains(GeoPos const& pos) const
{
    return (lonLeft() <= pos.longitude() && pos.longitude() < lonRight() && latBottom() < pos.latitude() &&
            pos.latitude() <= latTop());
}

bool GeoRect::contains(GeoRect const& rect) const
{
    return (lonLeft() <= rect.lonLeft() && rect.lonRight() <= lonRight() && latBottom() <= rect.latBottom() &&
            rect.latTop() <= latTop());
}

bool GeoRect::intersects(const GeoRect& rect) const
{
    return contains(rect.topLeft()) || contains(rect.topRight()) || contains(rect.bottomLeft()) ||
           contains(rect.bottomRight()) || rect.contains(topLeft()) || rect.contains(topRight()) ||
           rect.contains(bottomLeft()) || rect.contains(bottomRight());
}

bool GeoRect::operator==(const GeoRect& rhs)
{
    return mTopLeft == rhs.mTopLeft && mBottomRight == rhs.mBottomRight;
}

bool GeoRect::operator!=(const GeoRect& rhs)
{
    return !(*this == rhs);
}

GeoTilePos::GeoTilePos()
    : mZoom(-1)
{
}

GeoTilePos::GeoTilePos(int zoom, const QPoint& pos)
    : mZoom(zoom)
    , mPos(pos)
{
}

GeoTilePos::GeoTilePos(const GeoTilePos& other)
    : mZoom(other.mZoom)
    , mPos(other.mPos)
{
}

GeoTilePos::GeoTilePos(const GeoTilePos&& other)
    : mZoom(std::move(other.mZoom))
    , mPos(std::move(other.mPos))
{
}

GeoTilePos& GeoTilePos::operator=(const GeoTilePos& other)
{
    mZoom = other.mZoom;
    mPos = other.mPos;
    return *this;
}

GeoTilePos& GeoTilePos::operator=(const GeoTilePos&& other)
{
    mZoom = std::move(other.mZoom);
    mPos = std::move(other.mPos);
    return *this;
}

bool GeoTilePos::operator<(const GeoTilePos& other) const
{
    if (mZoom < other.mZoom) {
        return true;
    }
    if (mZoom > other.mZoom) {
        return false;
    }
    if (mPos.x() < other.mPos.x()) {
        return true;
    }
    if (mPos.x() > other.mPos.x()) {
        return false;
    }
    return mPos.y() < other.mPos.y();
}

int GeoTilePos::zoom() const
{
    return mZoom;
}

QPoint GeoTilePos::pos() const
{
    return mPos;
}

bool GeoTilePos::contains(const GeoTilePos& other) const
{
    if (zoom() >= other.zoom()) {
        return false;
    }
    GeoTilePos parentTile = other.parent(zoom());
    return (pos().x() == parentTile.pos().x() && pos().y() == parentTile.pos().y());
}

GeoTilePos GeoTilePos::parent(int parentZoom) const
{
    if (parentZoom >= zoom()) {
        return GeoTilePos();
    }
    const int deltaZoom = zoom() - parentZoom;
    const int factor = static_cast<int>(qPow(2, deltaZoom));
    const int x = static_cast<int>(qFloor(pos().x() / factor));
    const int y = static_cast<int>(qFloor(pos().y() / factor));
    return GeoTilePos(parentZoom, QPoint(x, y));
}

GeoRect GeoTilePos::toGeoRect() const
{
    const auto leftTop = [](const GeoTilePos& tilePos) -> GeoPos {
        const int zoom = tilePos.zoom();
        const int x = tilePos.pos().x();
        const int y = tilePos.pos().y();
        const double lon = x / pow(2.0, zoom) * 360.0 - 180;
        const double n = M_PI - 2.0 * M_PI * y / pow(2.0, zoom);
        const double lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
        return GeoPos(lat, lon);
    };
    const GeoPos pos1 = leftTop(*this);
    const GeoPos pos2 = leftTop(GeoTilePos(mZoom, mPos + QPoint(1, 1)));
    return GeoRect(pos1, pos2);
}

QString GeoTilePos::toQuadKey() const
{
    const int x = mPos.x();
    const int y = mPos.y();
    QString quadKey;
    for (int i = mZoom; i > 0; i--) {
        char cDigit = '0';
        int iMask = 1 << (i - 1);
        if ((x & iMask) != 0) {
            cDigit++;
        }
        if ((y & iMask) != 0) {
            cDigit++;
            cDigit++;
        }
        quadKey.append(cDigit);
    }
    return quadKey;
}

GeoTilePos GeoTilePos::geoToTilePos(int zoom, const GeoPos& geoPos)
{
    const double lon = geoPos.longitude();
    const double lat = geoPos.latitude();
    const double x = floor((lon + 180.0) / 360.0 * pow(2.0, zoom));
    const double y =
            floor((1.0 - log(tan(lat * M_PI / 180.0) + 1.0 / cos(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, zoom));
    return GeoTilePos(zoom, QPoint(static_cast<int>(x), static_cast<int>(y)));
}

QTransform createTransfrom(const QPointF& projAnchor, double scale, double azimuth)
{
    const bool scaleChanged = !qFuzzyCompare(scale, 1.0);
    const bool azimuthChanged = !qFuzzyIsNull(azimuth);
    if (!scaleChanged && !azimuthChanged) {
        return {};
    }
    QTransform transform;
    transform.translate(projAnchor.x(), projAnchor.y());
    if (scaleChanged) {
        transform.scale(scale, scale);
    }
    if (azimuthChanged) {
        transform.rotate(azimuth);
    }
    transform.translate(-projAnchor.x(), -projAnchor.y());
    return transform;
}

QTransform createTransfromScale(const QPointF& projAnchor, double scale)
{
    return createTransfrom(projAnchor, scale, 0.0);
}

QTransform createTransfromAzimuth(const QPointF& projAnchor, double azimuth)
{
    return createTransfrom(projAnchor, 1.0, azimuth);
}

QPainterPath createTextPath(const QRect& rect, const QString& text, const QFont& font, int penWidth)
{
    const QStringList txtList = text.split("\n");
    const int lineHeight = QFontMetrics(font).height();
    QPainterPath path;
    for (int i = 0; i < txtList.size(); i++) {
        path.addText(QPoint(0, i * lineHeight), font, txtList[i]);
    }
    const QPointF centerDelta = rect.center() - path.boundingRect().center();
    const double scaleDelta = (rect.width() - penWidth) / path.boundingRect().width();
    path.translate(centerDelta + QPointF(penWidth, penWidth) / scaleDelta);
    path = createTransfromScale(rect.center(), scaleDelta).map(path);
    return path;
}

void setDrawDebug(bool enabled)
{
    drawDebugEnabled = enabled;
}

bool isDrawDebug()
{
    return drawDebugEnabled;
}

void setPrintDebug(bool enabled)
{
    printDebugEnabled = enabled;
}

bool isPrintDebug()
{
    return printDebugEnabled;
}

void setNetworkManager(QNetworkAccessManager* manager)
{
    networkManager = manager;
}

QNetworkAccessManager* getNetworkManager()
{
    return networkManager;
}

// AetherSDR patch: configurable tile User-Agent (OSM tile policy requires a
// unique, app-identifying UA; the upstream hardcoded MSIE string is a
// documented blocking offense).
namespace {
QByteArray tileUserAgent = "QGeoView";
}

void setTileUserAgent(const QByteArray& userAgent)
{
    tileUserAgent = userAgent;
}

QByteArray getTileUserAgent()
{
    return tileUserAgent;
}

} // namespace QGV

QDebug operator<<(QDebug debug, const QGV::GeoPos& value)
{
    QDebugStateSaver saver(debug);
    debug.nospace().noquote() << "QGV::GeoPos(" << value.latToString() << ", " << value.lonToString() << ")";
    return debug;
}

QDebug operator<<(QDebug debug, const QGV::GeoRect& value)
{
    QDebugStateSaver saver(debug);
    debug.nospace().noquote() << "QGV::GeoRect(" << value.topLeft() << ", " << value.bottomRight() << ")";
    return debug;
}

QDebug operator<<(QDebug debug, const QGV::GeoTilePos& value)
{
    QDebugStateSaver saver(debug);
    debug.nospace().noquote() << "QGV::GeoTilePos(" << value.zoom() << ", " << value.pos() << ")";
    return debug;
}
