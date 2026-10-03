#pragma once

// Shared QComboBox styling with a painted down-arrow: applyComboStyle(combo).
// Colours come from theme tokens and re-apply on theme change through
// ThemeManager::applyStyleSheet; the arrow PNG is regenerated per theme.

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
        "QComboBox::down-arrow { image: url(%1); width: 8px; height: 6px; }"
        "QComboBox QAbstractItemView { background: {{color.background.1}};"
        " color: {{color.text.primary}};"
        " selection-background-color: {{color.accent}}; }"
        // An EDITABLE combo puts a real QLineEdit inside itself, and without a
        // rule it keeps the platform's own frame and background — a white box
        // inside a dark combo. Neutralised here rather than per caller, because
        // every editable combo has this problem and none of them wants it.
        "QComboBox QLineEdit { border: none; padding: 0; margin: 0;"
        " background: transparent; color: {{color.text.primary}}; }")
        .arg(detail::comboArrowPath())
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
