#pragma once

// Canon check boxes and radio buttons for stylesheets (style guide, RFC
// #6226): canonIndicatorRules() returns QCheckBox / QRadioButton indicator
// rules whose images are painted here in color.canon.* colours.
//
// Painted, not drawn with stylesheet borders: a QSS border cannot carry a tick
// or a dot, and a translucent canon hairline drawn as a rounded QSS border
// breaks into dashes around a circle. PNGs rather than SVG, because the SVG
// image plugin is not deployed everywhere; each is written at 1x, 2x and 3x so
// Qt picks the sharp one for the screen. Files are cached in the temp
// directory keyed by their colours, so a theme switch gets its own set — call
// canonIndicatorRules() again after ThemeManager::themeChanged.

#include "core/ThemeManager.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

#include <functional>

namespace AetherSDR {

namespace detail {

// Draws one indicator on an 18x18 logical canvas.
using IndicatorPainter = std::function<void(QPainter&)>;

inline QString canonIndicatorPath(const QString& name, const QString& colourKey,
                                  const IndicatorPainter& paint)
{
    constexpr int kSize = 18;
    const QString base = QDir::temp().filePath(
        QStringLiteral("aethersdr_canon_%1_%2").arg(name, colourKey));
    const QString path = base + QStringLiteral(".png");
    if (QFile::exists(path) && QFile::exists(base + QStringLiteral("@3x.png"))) {
        return path;
    }
    for (int scale = 1; scale <= 3; ++scale) {
        QPixmap pm(kSize * scale, kSize * scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        paint(p);
        p.end();
        pm.save(scale == 1 ? path : base + QStringLiteral("@%1x.png").arg(scale), "PNG");
    }
    return path;
}

inline QColor canonColour(const char* token)
{
    return ThemeManager::instance().color(QStringLiteral("color.canon.") + QLatin1String(token));
}

} // namespace detail

inline QString canonIndicatorRules()
{
    using detail::canonColour;
    const QColor control = canonColour("control");
    const QColor lineHi  = canonColour("lineHi");
    const QColor line    = canonColour("line");
    const QColor cyan    = canonColour("cyan");
    const QColor aqua    = canonColour("aqua");
    const QColor onAccent = canonColour("onAccent");
    const QColor muted   = canonColour("muted");
    const QString key = QString(control.name(QColor::HexArgb) + lineHi.name(QColor::HexArgb)
                                + cyan.name(QColor::HexArgb) + onAccent.name(QColor::HexArgb)
                                + muted.name(QColor::HexArgb)).remove(QLatin1Char('#'));

    const QRectF box(1.0, 1.0, 16.0, 16.0);
    const auto checkBox = [&](const QColor& fill, const QColor& edge, const QColor* tick) {
        return [=](QPainter& p) {
            p.setPen(QPen(edge, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);
            if (tick) {
                QPainterPath path;
                path.moveTo(5.0, 9.2);
                path.lineTo(7.9, 12.0);
                path.lineTo(13.0, 6.2);
                p.setPen(QPen(*tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
        };
    };
    const auto radio = [&](const QColor& edge, qreal edgeWidth, const QColor* dot) {
        return [=](QPainter& p) {
            p.setPen(QPen(edge, edgeWidth));
            p.setBrush(control);
            const qreal inset = 0.5 + edgeWidth / 2.0;
            p.drawEllipse(box.adjusted(inset, inset, -inset, -inset));
            if (dot) {
                p.setPen(Qt::NoPen);
                p.setBrush(*dot);
                p.drawEllipse(QPointF(9.0, 9.0), 4.0, 4.0);
            }
        };
    };

    using detail::canonIndicatorPath;
    const QString checkOff = canonIndicatorPath(QStringLiteral("check_off"), key,
                                                checkBox(control, lineHi, nullptr));
    const QString checkOn = canonIndicatorPath(QStringLiteral("check_on"), key,
                                               checkBox(cyan, aqua, &onAccent));
    const QString checkOffDis = canonIndicatorPath(QStringLiteral("check_off_dis"), key,
                                                   checkBox(control, line, nullptr));
    const QString checkOnDis = canonIndicatorPath(QStringLiteral("check_on_dis"), key,
                                                  checkBox(muted, muted, &control));
    const QString radioOff = canonIndicatorPath(QStringLiteral("radio_off"), key,
                                                radio(lineHi, 1.0, nullptr));
    const QString radioOn = canonIndicatorPath(QStringLiteral("radio_on"), key,
                                               radio(cyan, 1.5, &cyan));
    const QString radioOffDis = canonIndicatorPath(QStringLiteral("radio_off_dis"), key,
                                                   radio(line, 1.0, nullptr));
    const QString radioOnDis = canonIndicatorPath(QStringLiteral("radio_on_dis"), key,
                                                  radio(muted, 1.5, &muted));

    return QStringLiteral(
        "QCheckBox::indicator, QRadioButton::indicator {"
        " width: 18px; height: 18px; border: none; background: transparent; }"
        "QCheckBox::indicator:unchecked { image: url(%1); }"
        "QCheckBox::indicator:checked { image: url(%2); }"
        "QCheckBox::indicator:unchecked:disabled { image: url(%3); }"
        "QCheckBox::indicator:checked:disabled { image: url(%4); }"
        "QRadioButton::indicator:unchecked { image: url(%5); }"
        "QRadioButton::indicator:checked { image: url(%6); }"
        "QRadioButton::indicator:unchecked:disabled { image: url(%7); }"
        "QRadioButton::indicator:checked:disabled { image: url(%8); }")
        .arg(checkOff, checkOn, checkOffDis, checkOnDis,
             radioOff, radioOn, radioOffDis, radioOnDis);
}

} // namespace AetherSDR
