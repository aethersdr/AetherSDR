#pragma once

// Canon check boxes and radio buttons for stylesheets (style guide, RFC
// #6226): canonIndicatorRules() returns QCheckBox / QRadioButton indicator
// rules whose images are painted here in color.canon.* colours,
// canonItemCheckRules() the same check boxes for an item view's check column,
// and canonSpinArrowRules() the spin box steppers to go with them.
//
// Painted, not drawn with stylesheet borders: a QSS border cannot carry a tick
// or a dot, and a translucent canon hairline drawn as a rounded QSS border
// breaks into dashes around a circle. PNGs rather than SVG, because the SVG
// image plugin is not deployed everywhere; each is written at 1x, 2x and 3x so
// Qt picks the sharp one for the screen. They are cached by ThemedImageCache.h
// (the user's own cache directory, atomic writes) keyed by every colour they
// paint with, so a theme switch gets its own set; apply through
// applyCanonSheet(), which rebuilds the sheet on a theme change. If they cannot
// be written the rules come back empty and the window keeps Qt's own
// indicators: a missing image would draw nothing at all.

#include "ThemedImageCache.h"
#include "core/ThemeManager.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>

namespace AetherSDR {

namespace detail {

// Draws one indicator on an 18x18 logical canvas.
using IndicatorPainter = ThemedImagePainter;

// One indicator: 18 px logical, written at 1x-3x through ThemedImageCache.h,
// which fails closed (an empty path) if it cannot write. Bump the version when
// the drawing changes: the cache is keyed by colours, so an old file would
// otherwise outlive a new design.
inline QString canonIndicatorPath(const QString& name, const QString& colourKey,
                                  const IndicatorPainter& paint)
{
    constexpr int kDrawingVersion = 1;
    return themedImagePath(QStringLiteral("canon-indicators"),
                           QStringLiteral("%1_v%2").arg(name).arg(kDrawingVersion),
                           colourKey, QSize(18, 18), 3, paint);
}

inline QColor canonColour(const char* token)
{
    return ThemeManager::instance().color(QStringLiteral("color.canon.") + QLatin1String(token));
}

// The eight indicator images, check boxes then radio buttons, each off, on,
// off disabled and on disabled.
struct CanonIndicatorImages {
    QString checkOff, checkOn, checkOffDis, checkOnDis;
    QString radioOff, radioOn, radioOffDis, radioOnDis;
};

// All eight, or none if any could not be written.
inline std::optional<CanonIndicatorImages> canonIndicatorImages()
{
    const QColor control = canonColour("control");
    const QColor lineHi  = canonColour("lineHi");
    const QColor line    = canonColour("line");
    const QColor cyan    = canonColour("cyan");
    const QColor aqua    = canonColour("aqua");
    const QColor onAccent = canonColour("onAccent");
    const QColor muted   = canonColour("muted");
    // Every colour painted below, so two themes never share a file.
    const QString key = QString(control.name(QColor::HexArgb) + lineHi.name(QColor::HexArgb)
                                + line.name(QColor::HexArgb) + cyan.name(QColor::HexArgb)
                                + aqua.name(QColor::HexArgb) + onAccent.name(QColor::HexArgb)
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

    for (const QString* p : {&checkOff, &checkOn, &checkOffDis, &checkOnDis,
                             &radioOff, &radioOn, &radioOffDis, &radioOnDis}) {
        if (p->isEmpty()) {
            return std::nullopt;   // Qt's own indicators rather than blank ones
        }
    }
    return CanonIndicatorImages{checkOff, checkOn, checkOffDis, checkOnDis,
                                radioOff, radioOn, radioOffDis, radioOnDis};
}

} // namespace detail

inline QString canonIndicatorRules()
{
    const std::optional<detail::CanonIndicatorImages> img = detail::canonIndicatorImages();
    if (!img) {
        return {};
    }
    // Quoted: a cache path can hold spaces (a Windows profile name).
    return QStringLiteral(
        "QCheckBox::indicator, QRadioButton::indicator {"
        " width: 18px; height: 18px; border: none; background: transparent; }"
        "QCheckBox::indicator:unchecked { image: url(\"%1\"); }"
        "QCheckBox::indicator:checked { image: url(\"%2\"); }"
        "QCheckBox::indicator:unchecked:disabled { image: url(\"%3\"); }"
        "QCheckBox::indicator:checked:disabled { image: url(\"%4\"); }"
        "QRadioButton::indicator:unchecked { image: url(\"%5\"); }"
        "QRadioButton::indicator:checked { image: url(\"%6\"); }"
        "QRadioButton::indicator:unchecked:disabled { image: url(\"%7\"); }"
        "QRadioButton::indicator:checked:disabled { image: url(\"%8\"); }")
        .arg(img->checkOff, img->checkOn, img->checkOffDis, img->checkOnDis,
             img->radioOff, img->radioOn, img->radioOffDis, img->radioOnDis);
}

// The canon check images for an item view's checkable items (a table's check
// column), which a QCheckBox rule does not reach: `view` is the view's class
// name, as the selector takes it (QTableWidget). Empty, like
// canonIndicatorRules(), when the images could not be written.
inline QString canonItemCheckRules(const QString& view)
{
    const std::optional<detail::CanonIndicatorImages> img = detail::canonIndicatorImages();
    if (!img) {
        return {};
    }
    return QStringLiteral(
        "%1::indicator { width: 18px; height: 18px; }"
        "%1::indicator:unchecked { image: url(\"%2\"); }"
        "%1::indicator:checked { image: url(\"%3\"); }"
        "%1::indicator:unchecked:disabled { image: url(\"%4\"); }"
        "%1::indicator:checked:disabled { image: url(\"%5\"); }")
        .arg(view, img->checkOff, img->checkOn, img->checkOffDis, img->checkOnDis);
}

// A spin box with a styled frame draws its steppers from the sheet or not at
// all: without these rules Qt falls back to two specks. Each stepper is a
// column at the field's right with a painted triangle in the canon ink,
// regenerated per theme like the indicators above. Left out if an
// arrow could not be written, so the field keeps Qt's own.
inline QString canonSpinArrowRules()
{
    const QColor color = ThemeManager::instance().color(QStringLiteral("color.canon.inkSoft"));
    const QString key = color.name(QColor::HexRgb).remove(QLatin1Char('#'));
    auto arrow = [&](const QString& name, bool up) {
        return themedImagePath(QStringLiteral("spin-arrows"), name, key, QSize(8, 5), 1,
                               [color, up](QPainter& p) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            const QPointF upTri[] = {{0, 5}, {8, 5}, {4, 0}};
            const QPointF downTri[] = {{0, 0}, {8, 0}, {4, 5}};
            p.drawPolygon(up ? upTri : downTri, 3);
        });
    };
    const QString up = arrow(QStringLiteral("up"), true);
    const QString down = arrow(QStringLiteral("down"), false);
    if (up.isEmpty() || down.isEmpty()) {
        return {};
    }
    return QStringLiteral(
        "QSpinBox, QDoubleSpinBox { padding-right: 16px; }"
        "QSpinBox::up-button, QDoubleSpinBox::up-button,"
        " QSpinBox::down-button, QDoubleSpinBox::down-button {"
        " subcontrol-origin: border; width: 16px; border: none;"
        " border-left: 1px solid {{color.canon.line}}; background: transparent; }"
        "QSpinBox::up-button, QDoubleSpinBox::up-button {"
        " subcontrol-position: top right; border-top-right-radius: 4px; }"
        "QSpinBox::down-button, QDoubleSpinBox::down-button {"
        " subcontrol-position: bottom right; border-bottom-right-radius: 4px; }"
        "QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,"
        " QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {"
        " background: {{color.canon.nested}}; }"
        "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
        " image: url(\"%1\"); width: 8px; height: 5px; }"
        "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
        " image: url(\"%2\"); width: 8px; height: 5px; }"
        "QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled,"
        " QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled,"
        " QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off,"
        " QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image: none; }")
        .arg(up, down);
}

// Apply a sheet built by `build` (which embeds canonIndicatorRules()) to
// `widget`, and rebuild it whenever the theme changes, so the painted
// indicators follow the new theme's colours. ThemeManager re-resolves the
// tracked sheet on themeChanged too, still carrying the old theme's image
// paths; its connection is made in its constructor, so it runs first and this
// rebuild lands last.
inline void applyCanonSheet(QWidget* widget, std::function<QString()> build)
{
    auto& tm = ThemeManager::instance();
    tm.applyStyleSheet(widget, build());
    QObject::connect(&tm, &ThemeManager::themeChanged, widget,
                     [widget, build] {
        ThemeManager::instance().applyStyleSheet(widget, build());
    });
}

} // namespace AetherSDR
