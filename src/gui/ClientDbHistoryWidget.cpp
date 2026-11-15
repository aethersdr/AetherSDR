#include "ClientDbHistoryWidget.h"

#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

QColor token(const QWidget* w, const char* name)
{
    return ThemeManager::instance().color(w, name);
}

}  // namespace

ClientDbHistoryWidget::ClientDbHistoryWidget(QWidget* parent)
    : QWidget(parent)
{
}

void ClientDbHistoryWidget::setAxis(float minDb, float maxDb)
{
    m_minDb = minDb;
    m_maxDb = std::max(maxDb, minDb + 1.0f);
    update();
}

void ClientDbHistoryWidget::setReference(float db, const QString& label)
{
    m_referenceDb = db;
    m_referenceLabel = label;
    update();
}

void ClientDbHistoryWidget::push(float db)
{
    m_history[static_cast<std::size_t>(m_head)] = db;
    m_head = (m_head + 1) % kPoints;
    m_count = std::min(m_count + 1, kPoints);
    update();
}

void ClientDbHistoryWidget::clear()
{
    m_count = 0;
    update();
}

void ClientDbHistoryWidget::setActive(bool active)
{
    m_active = active;
    update();
}

void ClientDbHistoryWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(this, "color.background.0"));
    const QRectF r = QRectF(rect()).adjusted(30.0, 6.0, -6.0, -16.0);
    const double span = m_maxDb - m_minDb;
    const auto yFor = [&](double db) {
        return r.bottom()
               - (std::clamp(db, double(m_minDb), double(m_maxDb)) - m_minDb) / span * r.height();
    };

    // Grid every 5 dB; labels every 10 dB.
    p.setPen(QPen(token(this, "color.spectrum.grid"), 1.0));
    for (double db = std::ceil(m_minDb / 5.0) * 5.0; db <= m_maxDb; db += 5.0) {
        p.drawLine(QPointF(r.left(), yFor(db)), QPointF(r.right(), yFor(db)));
    }
    QFont small = font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() - 2.0));
    p.setFont(small);
    p.setPen(token(this, "color.text.label"));
    for (double db = std::ceil(m_minDb / 10.0) * 10.0; db <= m_maxDb; db += 10.0) {
        p.drawText(QRectF(0.0, yFor(db) - 8.0, r.left() - 4.0, 16.0),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0));
    }
    p.drawText(QRectF(r.left(), r.bottom() + 2.0, 60.0, 14.0), Qt::AlignLeft | Qt::AlignTop,
               tr("-5 s"));
    p.drawText(QRectF(r.right() - 60.0, r.bottom() + 2.0, 60.0, 14.0),
               Qt::AlignRight | Qt::AlignTop, tr("now"));

    QPen reference(token(this, "color.accent.warning"), 1.2);
    reference.setStyle(Qt::DashLine);
    p.setPen(reference);
    p.drawLine(QPointF(r.left(), yFor(m_referenceDb)), QPointF(r.right(), yFor(m_referenceDb)));
    if (!m_referenceLabel.isEmpty()) {
        p.drawText(QRectF(r.left() + 4.0, yFor(m_referenceDb) - 15.0, 240.0, 14.0),
                   Qt::AlignLeft | Qt::AlignBottom, m_referenceLabel);
    }

    if (m_count < 2) {
        return;
    }
    QPainterPath line;
    const double step = r.width() / (kPoints - 1);
    for (int i = 0; i < m_count; ++i) {
        // Oldest of the stored points first, ending at the right edge.
        const int index = (m_head - m_count + i + kPoints) % kPoints;
        const QPointF pt(r.right() - (m_count - 1 - i) * step,
                         yFor(m_history[static_cast<std::size_t>(index)]));
        if (i == 0) {
            line.moveTo(pt);
        } else {
            line.lineTo(pt);
        }
    }
    QColor color = token(this, "color.accent");
    if (!m_active) {
        color.setAlpha(110);
    }
    p.setPen(QPen(color, 2.0));
    p.drawPath(line);
}

}  // namespace AetherSDR
