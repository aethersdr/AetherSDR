#include "AprsSymbolIcons.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace AetherSDR::aprsicons {

namespace {

// All shapes are sketched on a 16x16 logical canvas, rendered at 2x.
constexpr int kLogicalSize = 16;
constexpr qreal kDpr = 2.0;

void drawHouse(QPainter& p)
{
    QPainterPath roof;
    roof.moveTo(2, 8);
    roof.lineTo(8, 2.5);
    roof.lineTo(14, 8);
    p.drawPath(roof);
    p.drawRect(QRectF(3.5, 8, 9, 5.5));
    p.drawRect(QRectF(6.8, 10, 2.4, 3.5)); // door
}

void drawWheels(QPainter& p, qreal x1, qreal x2, qreal y, qreal r)
{
    p.drawEllipse(QPointF(x1, y), r, r);
    p.drawEllipse(QPointF(x2, y), r, r);
}

void drawCar(QPainter& p)
{
    QPainterPath body;
    body.moveTo(2, 11);
    body.lineTo(2, 9.2);
    body.lineTo(4.6, 8.6);
    body.lineTo(6.2, 6);
    body.lineTo(10.2, 6);
    body.lineTo(11.8, 8.6);
    body.lineTo(14, 9.2);
    body.lineTo(14, 11);
    p.drawPath(body);
    drawWheels(p, 5, 11, 11.3, 1.7);
}

void drawJeep(QPainter& p)
{
    p.drawRect(QRectF(2.5, 6.5, 11, 4.5)); // flat box body
    p.drawLine(QPointF(5, 6.5), QPointF(5, 11));   // windshield post
    p.drawLine(QPointF(2.5, 8.5), QPointF(5, 8.5)); // hood line
    drawWheels(p, 5, 11.2, 11.6, 1.7);
}

void drawPickup(QPainter& p)
{
    QPainterPath body;
    body.moveTo(2, 11);
    body.lineTo(2, 6.5);
    body.lineTo(7, 6.5);
    body.lineTo(8.5, 8.6);
    body.lineTo(14, 8.6);
    body.lineTo(14, 11);
    p.drawPath(body);
    drawWheels(p, 4.6, 11.4, 11.3, 1.7);
}

void drawSemi(QPainter& p)
{
    p.drawRect(QRectF(2, 7.5, 3.6, 3.7));  // cab
    p.drawRect(QRectF(6.2, 5.5, 7.8, 5.7)); // trailer
    p.drawEllipse(QPointF(3.8, 12.3), 1.4, 1.4);
    p.drawEllipse(QPointF(8.4, 12.3), 1.4, 1.4);
    p.drawEllipse(QPointF(12, 12.3), 1.4, 1.4);
}

void drawVan(QPainter& p)
{
    QPainterPath body;
    body.addRoundedRect(QRectF(2, 5.8, 12, 5.4), 2, 2);
    p.drawPath(body);
    p.drawLine(QPointF(4.8, 5.8), QPointF(4.8, 11.2)); // windshield divide
    drawWheels(p, 5, 11.2, 11.5, 1.6);
}

void drawRv(QPainter& p)
{
    p.drawRect(QRectF(2, 5, 12, 6.5));
    p.drawRect(QRectF(4, 6.8, 2.2, 2.2));   // window
    p.drawRect(QRectF(7.5, 6.8, 2.2, 2.2)); // window
    drawWheels(p, 5, 11.5, 11.8, 1.5);
}

void drawMotorcycle(QPainter& p)
{
    drawWheels(p, 4, 12, 11, 2.2);
    p.drawLine(QPointF(4, 11), QPointF(8, 8));
    p.drawLine(QPointF(8, 8), QPointF(12, 11));
    p.drawLine(QPointF(10.5, 6), QPointF(12, 11)); // fork
    p.drawLine(QPointF(9.5, 6), QPointF(11.5, 6)); // handlebar
}

void drawBicycle(QPainter& p)
{
    drawWheels(p, 4.2, 11.8, 11, 2.4);
    p.drawLine(QPointF(4.2, 11), QPointF(7, 7));    // seat tube
    p.drawLine(QPointF(7, 7), QPointF(11, 7));      // top tube
    p.drawLine(QPointF(11, 7), QPointF(11.8, 11));  // fork
    p.drawLine(QPointF(7, 7), QPointF(8.6, 11));    // down tube
    p.drawLine(QPointF(8.6, 11), QPointF(4.2, 11)); // chain stay
}

void drawJogger(QPainter& p)
{
    p.drawEllipse(QPointF(8.6, 3.4), 1.6, 1.6);     // head
    p.drawLine(QPointF(8.2, 5), QPointF(7.4, 9));   // torso
    p.drawLine(QPointF(8, 6), QPointF(11, 7.4));    // front arm
    p.drawLine(QPointF(8, 6.4), QPointF(5.4, 7.6)); // back arm
    p.drawLine(QPointF(7.4, 9), QPointF(10.4, 11.4)); // front leg
    p.drawLine(QPointF(10.4, 11.4), QPointF(11, 13.6));
    p.drawLine(QPointF(7.4, 9), QPointF(5.6, 11.6)); // back leg
    p.drawLine(QPointF(5.6, 11.6), QPointF(3.6, 12.4));
}

void drawPowerBoat(QPainter& p)
{
    QPainterPath hull;
    hull.moveTo(2, 9);
    hull.lineTo(14, 9);
    hull.lineTo(11.6, 12.4);
    hull.lineTo(4, 12.4);
    hull.closeSubpath();
    p.drawPath(hull);
    p.drawRect(QRectF(5.5, 6, 4, 3)); // cabin
}

void drawSailboat(QPainter& p)
{
    p.drawLine(QPointF(8.5, 2), QPointF(8.5, 10)); // mast
    QPainterPath sail;
    sail.moveTo(8.5, 2.5);
    sail.lineTo(8.5, 9.4);
    sail.lineTo(3.4, 9.4);
    sail.closeSubpath();
    p.drawPath(sail);
    QPainterPath hull;
    hull.moveTo(2.6, 10.6);
    hull.lineTo(13.4, 10.6);
    hull.lineTo(11.4, 13.2);
    hull.lineTo(4.6, 13.2);
    hull.closeSubpath();
    p.drawPath(hull);
}

void drawAircraft(QPainter& p)
{
    // Top view: vertical fuselage, straight wing, small tailplane.
    p.drawLine(QPointF(8, 2), QPointF(8, 13.4));
    p.drawLine(QPointF(2, 7), QPointF(14, 7));      // wing
    p.drawLine(QPointF(5.6, 12.2), QPointF(10.4, 12.2)); // tail
    p.drawEllipse(QPointF(8, 2.6), 0.9, 0.9);       // nose
}

void drawHelicopter(QPainter& p)
{
    p.drawEllipse(QRectF(4, 7, 7, 4));               // body
    p.drawLine(QPointF(2, 4.4), QPointF(13.4, 4.4)); // rotor
    p.drawLine(QPointF(7.5, 7), QPointF(7.5, 4.4));  // mast
    p.drawLine(QPointF(11, 9), QPointF(14, 8));      // tail boom
    p.drawLine(QPointF(4.4, 12.6), QPointF(11, 12.6)); // skid
}

void drawBalloon(QPainter& p)
{
    p.drawEllipse(QPointF(8, 5.6), 3.8, 3.8);
    p.drawLine(QPointF(5.6, 8.6), QPointF(6.8, 11.8));
    p.drawLine(QPointF(10.4, 8.6), QPointF(9.2, 11.8));
    p.drawRect(QRectF(6.6, 11.8, 2.8, 2));
}

void drawTower(QPainter& p)
{
    p.drawLine(QPointF(8, 3), QPointF(4.6, 13.6));
    p.drawLine(QPointF(8, 3), QPointF(11.4, 13.6));
    p.drawLine(QPointF(6.5, 7.6), QPointF(9.5, 7.6));
    p.drawLine(QPointF(5.6, 10.6), QPointF(10.4, 10.6));
    p.drawLine(QPointF(6.5, 7.6), QPointF(10.4, 10.6));
    p.drawLine(QPointF(9.5, 7.6), QPointF(5.6, 10.6));
}

void drawRepeater(QPainter& p)
{
    drawTower(p);
    // Radio waves off the top.
    p.drawArc(QRectF(5, 0.6, 6, 5), 30 * 16, 120 * 16);
    p.drawArc(QRectF(3.4, -1, 9.2, 8), 30 * 16, 120 * 16);
}

void drawYagi(QPainter& p)
{
    p.drawLine(QPointF(8, 14), QPointF(8, 7));       // mast
    p.drawLine(QPointF(3, 7), QPointF(13, 7));       // boom (slight up-tilt skipped)
    p.drawLine(QPointF(4.2, 4.6), QPointF(4.2, 9.4));   // reflector
    p.drawLine(QPointF(8, 5), QPointF(8, 9));            // driven element
    p.drawLine(QPointF(11.6, 5.4), QPointF(11.6, 8.6));  // director
}

void drawCloud(QPainter& p)
{
    QPainterPath cloud;
    cloud.moveTo(3.4, 10.4);
    cloud.arcTo(QRectF(2, 7.8, 4, 4), 90, 180);
    cloud.arcTo(QRectF(3.6, 5.2, 5, 5), 160, -140);
    cloud.arcTo(QRectF(7.6, 4.6, 5.4, 5.4), 150, -150);
    cloud.arcTo(QRectF(10.4, 7.6, 4, 4.2), 90, -180);
    cloud.closeSubpath();
    p.drawPath(cloud);
}

void drawWxStation(QPainter& p)
{
    drawCloud(p);
    p.drawLine(QPointF(5.4, 12.6), QPointF(4.6, 14.6));
    p.drawLine(QPointF(8.2, 12.6), QPointF(7.4, 14.6));
    p.drawLine(QPointF(11, 12.6), QPointF(10.2, 14.6));
}

void drawWxService(QPainter& p)
{
    drawCloud(p);
    QPainterPath bolt;
    bolt.moveTo(8.8, 11.6);
    bolt.lineTo(6.8, 13.6);
    bolt.lineTo(8.2, 13.8);
    bolt.lineTo(7, 15.6);
    p.drawPath(bolt);
}

void drawAmbulance(QPainter& p)
{
    p.drawRect(QRectF(2, 6, 12, 5.4));
    p.drawLine(QPointF(8, 7.2), QPointF(8, 10.2));   // cross
    p.drawLine(QPointF(6.5, 8.7), QPointF(9.5, 8.7));
    drawWheels(p, 4.8, 11.4, 11.7, 1.5);
}

void drawFireTruck(QPainter& p)
{
    p.drawRect(QRectF(2, 6.5, 12, 4.9));
    p.drawLine(QPointF(3.4, 5), QPointF(12, 5));     // ladder
    p.drawLine(QPointF(5, 5), QPointF(5, 6.5));
    p.drawLine(QPointF(8, 5), QPointF(8, 6.5));
    p.drawLine(QPointF(10.6, 5), QPointF(10.6, 6.5));
    drawWheels(p, 4.8, 11.4, 11.7, 1.5);
}

void drawShield(QPainter& p)
{
    QPainterPath shield;
    shield.moveTo(8, 2.4);
    shield.cubicTo(10, 3.8, 12, 4.2, 13.2, 4.2);
    shield.cubicTo(13.2, 9.6, 11.4, 12.4, 8, 14);
    shield.cubicTo(4.6, 12.4, 2.8, 9.6, 2.8, 4.2);
    shield.cubicTo(4, 4.2, 6, 3.8, 8, 2.4);
    p.drawPath(shield);
}

void drawBus(QPainter& p)
{
    QPainterPath body;
    body.addRoundedRect(QRectF(2, 4.8, 12, 6.6), 1.6, 1.6);
    p.drawPath(body);
    p.drawLine(QPointF(2, 7.6), QPointF(14, 7.6)); // window line
    drawWheels(p, 4.8, 11.4, 11.9, 1.5);
}

void drawTrain(QPainter& p)
{
    p.drawRect(QRectF(3, 4, 10, 7));
    p.drawRect(QRectF(5, 5.6, 6, 2.2)); // window band
    p.drawEllipse(QPointF(5.6, 12.6), 1.3, 1.3);
    p.drawEllipse(QPointF(10.4, 12.6), 1.3, 1.3);
    p.drawLine(QPointF(2, 14.2), QPointF(14, 14.2)); // rail
}

void drawPhone(QPainter& p)
{
    QPainterPath handset;
    handset.moveTo(3.4, 5.6);
    handset.arcTo(QRectF(2.6, 3, 4, 4), 130, 140);
    handset.lineTo(11.2, 3.8);
    handset.arcTo(QRectF(9.4, 3, 4, 4), 90, -140);
    p.drawPath(handset);
    p.drawRect(QRectF(5.2, 7.4, 5.6, 6.4)); // base
}

void drawWindyCloud(QPainter& p)
{
    drawCloud(p);
    p.drawLine(QPointF(3.4, 12.8), QPointF(9.4, 12.8));
    p.drawLine(QPointF(6, 14.6), QPointF(12.6, 14.6));
}

// Fallback: rounded badge with the raw symbol character.
void drawBadge(QPainter& p, char code, const QColor& color)
{
    p.drawRoundedRect(QRectF(2.4, 2.4, 11.2, 11.2), 3, 3);
    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(color);
    p.drawText(QRectF(2.4, 2.4, 11.2, 11.2), Qt::AlignCenter,
               QString(QLatin1Char(code)));
}

} // namespace

namespace {

QHash<quint64, QIcon>& iconCache()
{
    static QHash<quint64, QIcon> cache;
    return cache;
}

} // namespace

QIcon symbolIcon(char symbolTable, char symbolCode, const QColor& color)
{
    Q_UNUSED(symbolTable); // alternate-table codes share the primary drawing
    QHash<quint64, QIcon>& cache = iconCache();
    const quint64 key = (quint64(quint8(symbolCode)) << 32) | color.rgba();
    if (const auto it = cache.constFind(key); it != cache.constEnd())
        return it.value();

    QPixmap pm(int(kLogicalSize * kDpr), int(kLogicalSize * kDpr));
    pm.setDevicePixelRatio(kDpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        switch (symbolCode) {
        case '-':  drawHouse(p); break;
        case '>':  drawCar(p); break;
        case 'j':  drawJeep(p); break;
        case 'k':  drawPickup(p); break;
        case 'u':  drawSemi(p); break;
        case 'v':  drawVan(p); break;
        case 'R':  drawRv(p); break;
        case '<':  drawMotorcycle(p); break;
        case 'b':  drawBicycle(p); break;
        case '[':  drawJogger(p); break;
        case 's':  drawPowerBoat(p); break;
        case 'Y':  drawSailboat(p); break;
        case '\'': drawAircraft(p); break;
        case 'g':  drawAircraft(p); break;   // glider — close enough at 16 px
        case 'X':  drawHelicopter(p); break;
        case 'O':  drawBalloon(p); break;
        case '#':  drawTower(p); break;      // digipeater
        case 'r':  drawRepeater(p); break;
        case 'm':  drawRepeater(p); break;   // Mic-E repeater
        case 'y':  drawYagi(p); break;
        case '_':  drawWxStation(p); break;
        case 'W':  drawWxService(p); break;
        case 'a':  drawAmbulance(p); break;
        case 'f':  drawFireTruck(p); break;
        case '!':  drawShield(p); break;     // police / emergency
        case 'P':  drawShield(p); break;     // police
        case 'U':  drawBus(p); break;
        case '=':  drawTrain(p); break;
        case '$':  drawPhone(p); break;
        default:   drawBadge(p, symbolCode, color); break;
        }
    }
    const QIcon icon(pm);
    cache.insert(key, icon);
    return icon;
}

QIcon weatherIcon(int condition, const QColor& color)
{
    condition = qBound(0, condition, 2);
    QHash<quint64, QIcon>& cache = iconCache();
    // Conditions cache under control-character pseudo-codes 0x01-0x03, which
    // can never collide with printable APRS symbol codes.
    const quint64 key = (quint64(quint8(condition + 1)) << 32) | color.rgba();
    if (const auto it = cache.constFind(key); it != cache.constEnd())
        return it.value();

    QPixmap pm(int(kLogicalSize * kDpr), int(kLogicalSize * kDpr));
    pm.setDevicePixelRatio(kDpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        switch (condition) {
        case 1:  drawWxStation(p); break;  // cloud with rain
        case 2:  drawWindyCloud(p); break; // cloud with wind streaks
        default: drawCloud(p); break;
        }
    }
    const QIcon icon(pm);
    cache.insert(key, icon);
    return icon;
}

} // namespace AetherSDR::aprsicons
