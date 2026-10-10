#pragma once

// Shared QComboBox styling with a painted down-arrow: applyComboStyle(combo).
// Colours come from theme tokens and re-apply on theme change through
// ThemeManager::applyStyleSheet; the arrow PNG is regenerated per theme.

#include "ThemedImageCache.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>

namespace AetherSDR {

namespace detail {

// The down-arrow PNG coloured by the current theme's color.text.secondary
// token, cached per colour by ThemedImageCache.h so each theme gets its own
// file. Empty if it could not be written.
inline QString comboArrowPath()
{
    const QColor colour = ThemeManager::instance().color("color.text.secondary");
    return themedImagePath(QStringLiteral("combo-arrows"), QStringLiteral("arrow"),
                           colour.name(QColor::HexRgb).remove(QLatin1Char('#')),
                           QSize(8, 6), 1, [colour](QPainter& p) {
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        const QPointF tri[] = {{0, 0}, {8, 0}, {4, 6}};
        p.drawPolygon(tri, 3);
    });
}

} // namespace detail

// The down-arrow rule, or nothing if the arrow could not be written, so the
// combo keeps Qt's own arrow rather than naming a missing file. Quoted: a
// cache path can hold spaces.
inline QString comboArrowRule()
{
    const QString path = detail::comboArrowPath();
    return path.isEmpty() ? QString()
        : QStringLiteral("QComboBox::down-arrow { image: url(\"%1\"); width: 8px; height: 6px; }")
              .arg(path);
}

// Stylesheet template — references token placeholders rather than baked-in
// hex.  ThemeManager::resolve() expands the {{...}} placeholders at apply
// time.  The arrow rule is arg()ed in because its image path depends on
// the active theme's text.secondary colour (the cached PNG varies per theme),
// and is left out if the PNG could not be written.
// `extraRules` is appended verbatim, so a caller can override the base rules for
// its own context — a taller field wants more padding than the compact applet
// combos this was shaped for. Token placeholders work there too; the whole
// string is resolved together.
inline QString comboStyleTemplate(const QString& extraRules = QString())
{
    return QStringLiteral(
        "QComboBox { background: {{color.background.1}};"
        " color: {{color.text.primary}};"
        " border: 1px solid {{color.background.2}};"
        " padding: 2px 2px 2px 4px; border-radius: 2px; }"
        // Disabled combos must read as disabled — the base rule sets an explicit
        // colour, which otherwise overrides Qt's native disabled greying. This is
        // also render()-compatible (unlike a QGraphicsEffect), so a disabled combo
        // stays dimmed when a widget is rasterized into an image (GPU flag sprites).
        "QComboBox:disabled { color: {{color.text.secondary}};"
        " border: 1px solid {{color.background.1}}; }"
        "QComboBox::drop-down { border: none; width: 14px; }"
        "%1"
        "QComboBox QAbstractItemView { background: {{color.background.1}};"
        " color: {{color.text.primary}};"
        " selection-background-color: {{color.accent}}; }"
        // An EDITABLE combo puts a real QLineEdit inside itself, and without a
        // rule it keeps the platform's own frame and background — a white box
        // inside a dark combo. Neutralised here rather than per caller, because
        // every editable combo has this problem and none of them wants it.
        "QComboBox QLineEdit { border: none; padding: 0; margin: 0;"
        " background: transparent; color: {{color.text.primary}}; }")
        .arg(comboArrowRule())
        + extraRules;
}

// Applies the themed style via ThemeManager::applyStyleSheet (live re-theme).
// Also connects themeChanged → refresh, because the arrow URL in the template
// depends on the active theme and token re-resolution alone would keep the
// stale URL.
inline void applyComboStyle(QComboBox* combo, const QString& extraRules = QString())
{
    if (!combo) return;

    ThemeManager::instance().applyStyleSheet(combo, comboStyleTemplate(extraRules));

    // QPointer guards against the combo being destroyed before the theme
    // change fires.  The combo is also the receiver-context for the
    // connection so Qt cleans up the lambda when the combo dies.
    QPointer<QComboBox> guard(combo);
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                     combo, [guard, extraRules]() {
        if (guard) {
            ThemeManager::instance().applyStyleSheet(guard,
                                                     comboStyleTemplate(extraRules));
        }
    });
}

} // namespace AetherSDR
