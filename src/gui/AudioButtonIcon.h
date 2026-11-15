#pragma once

#include "core/ThemeManager.h"

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>

namespace AetherSDR {

// Font-independent audio marks follow the current theme and display scale.
enum class AudioIcon { Speaker, Headphone };

inline QPixmap buildAudioIcon(AudioIcon kind, bool muted, const QColor& stroke, qreal dpr)
{
    constexpr int kBox = 20;
    QPixmap pm(int(kBox * dpr), int(kBox * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(stroke, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == AudioIcon::Speaker) {
        // Cone: a small rectangle at the pivot with a flared face.
        QPainterPath cone;
        cone.moveTo(3.0, 8.0);
        cone.lineTo(6.5, 8.0);
        cone.lineTo(10.5, 4.5);
        cone.lineTo(10.5, 15.5);
        cone.lineTo(6.5, 12.0);
        cone.lineTo(3.0, 12.0);
        cone.closeSubpath();
        p.drawPath(cone);
        if (!muted) {
            // Two radiating arcs.  QPainter angles are sixteenths of a degree.
            p.drawArc(QRectF(9.0, 5.5, 6.0, 9.0), -60 * 16, 120 * 16);
            p.drawArc(QRectF(9.0, 2.5, 9.0, 15.0), -55 * 16, 110 * 16);
        } else {
            p.drawLine(QPointF(13.0, 7.0), QPointF(17.5, 13.0));
            p.drawLine(QPointF(17.5, 7.0), QPointF(13.0, 13.0));
        }
    } else {
        // Headband arc plus two ear cups.
        p.drawArc(QRectF(3.5, 3.5, 13.0, 13.0), 0, 180 * 16);
        p.drawRoundedRect(QRectF(3.0, 10.0, 3.6, 6.5), 1.6, 1.6);
        p.drawRoundedRect(QRectF(13.4, 10.0, 3.6, 6.5), 1.6, 1.6);
        if (muted) {
            p.drawLine(QPointF(3.5, 16.5), QPointF(16.5, 3.5));
        }
    }
    p.end();
    return pm;
}

// Set (or re-set) an audio button's icon at the current DPR and theme colour.
// Called on construction, on every mute toggle, and on theme change.
inline void applyAudioIcon(QPushButton* button, AudioIcon kind, bool muted, int size = 20)
{
    if (!button) {
        return;
    }
    const QColor stroke =
        AetherSDR::ThemeManager::instance().color(button,
                                                  QStringLiteral("color.text.secondary"));
    button->setIcon(QIcon(buildAudioIcon(kind, muted, stroke,
                                         button->devicePixelRatioF())));
    button->setIconSize(QSize(size, size));
}

} // namespace AetherSDR
