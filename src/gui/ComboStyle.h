#pragma once

// Shared combo box styling for consistent down-arrow appearance across all
// QComboBox instances in AetherSDR.  Use applyComboStyle(combo) on any
// QComboBox to get the standard themed look with painted down-arrow.
//
// Theme-aware (RFC #3076 Phase 2): colours come from canonical tokens
// (color.background.1, color.text.primary, color.background.2,
// color.accent, color.text.secondary).  The combo's stylesheet re-applies
// automatically when the user switches themes via ThemeManager::applyStyleSheet
// (free live-reload through the widget-tracked reverse-map).  The custom
// down-arrow PNG is also regenerated whenever the theme changes so its
// colour stays in sync with the rest of the combo.

#include "core/ThemeManager.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QPointer>

namespace AetherSDR {

namespace detail {

// Generate (or reuse) the down-arrow PNG coloured by the current theme's
// color.text.secondary token.  Cached under /tmp with the colour hex in
// the filename so each theme's arrow gets its own cache entry — switching
// themes back and forth doesn't re-encode the PNG every time.
inline QString comboArrowPath()
{
    const QString colourHex = ThemeManager::instance()
                                  .color("color.text.secondary")
                                  .name(QColor::HexRgb)
                                  .remove(QLatin1Char('#'));
    const QString path = QDir::temp().filePath(
        QStringLiteral("aethersdr_combo_arrow_%1.png").arg(colourHex));
    if (QFile::exists(path)) return path;

    QPixmap pm(8, 6);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(ThemeManager::instance().color("color.text.secondary"));
    const QPointF tri[] = {{0, 0}, {8, 0}, {4, 6}};
    p.drawPolygon(tri, 3);
    p.end();
    pm.save(path, "PNG");
    return path;
}

} // namespace detail

// Stylesheet template — references token placeholders rather than baked-in
// hex.  ThemeManager::resolve() expands the {{...}} placeholders at apply
// time.  The arrow URL is arg()ed in by the caller because it depends on
// the active theme's text.secondary colour (cached PNG path varies per
// theme).
inline QString comboStyleTemplate()
{
    return QStringLiteral(
        "QComboBox { background: {{color.background.1}};"
        " color: {{color.text.primary}};"
        " border: 1px solid {{color.background.2}};"
        " padding: 2px 2px 2px 4px; border-radius: 2px; }"
        "QComboBox::drop-down { border: none; width: 14px; }"
        "QComboBox::down-arrow { image: url(%1); width: 8px; height: 6px; }"
        "QComboBox QAbstractItemView { background: {{color.background.1}};"
        " color: {{color.text.primary}};"
        " selection-background-color: {{color.accent}}; }")
        .arg(detail::comboArrowPath());
}

// Apply the themed style to a combo box.  Routes through
// ThemeManager::applyStyleSheet so the combo gets free live re-theme on
// theme changes via the widget-tracked reverse-map.
//
// Additionally connects themeChanged → refresh because the arrow URL
// embedded in the template depends on the active theme.  Without the
// extra connection, theme changes would re-resolve the {{tokens}} but
// keep the stale arrow URL.  The double-apply is wasteful but small; a
// Phase 5 follow-up could introduce a "computed token" mechanism to
// avoid the duplicate apply if combos become a hot path.
inline void applyComboStyle(QComboBox* combo)
{
    if (!combo) return;

    ThemeManager::instance().applyStyleSheet(combo, comboStyleTemplate());

    // QPointer guards against the combo being destroyed before the theme
    // change fires.  The combo is also the receiver-context for the
    // connection so Qt cleans up the lambda when the combo dies.
    QPointer<QComboBox> guard(combo);
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                     combo, [guard]() {
        if (guard) {
            ThemeManager::instance().applyStyleSheet(guard, comboStyleTemplate());
        }
    });
}

} // namespace AetherSDR
