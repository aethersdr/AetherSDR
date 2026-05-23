#pragma once

// Shared combo box styling for consistent down-arrow appearance across all
// QComboBox instances in MasterSDR. Use applyComboStyle(combo) on any
// QComboBox to get the standard dark-themed look with painted down-arrow.

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QPainter>

namespace MasterSDR {

// Generate a small down-arrow PNG (cached in temp dir, created once).
inline QString comboArrowPath()
{
    static QString path;
    if (!path.isEmpty()) return path;
    path = QDir::temp().filePath("MasterSDR_combo_arrow.png");
    if (QFile::exists(path)) return path;
    QPixmap pm(8, 6);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x8a, 0xa8, 0xc0));
    const QPointF tri[] = {{0, 0}, {8, 0}, {4, 6}};
    p.drawPolygon(tri, 3);
    p.end();
    pm.save(path, "PNG");
    return path;
}

// Standard combo box stylesheet matching the dark theme with painted arrow.
inline QString comboStyleSheet()
{
    return QString(
        "QComboBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
        " padding: 2px 2px 2px 4px; border-radius: 2px; }"
        "QComboBox::drop-down { border: none; width: 14px; }"
        "QComboBox::down-arrow { image: url(%1); width: 8px; height: 6px; }"
        "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8;"
        " selection-background-color: #00b4d8; }")
        .arg(comboArrowPath());
}

// Apply the standard style to a combo box.
inline void applyComboStyle(QComboBox* combo)
{
    combo->setStyleSheet(comboStyleSheet());
}

} // namespace MasterSDR
