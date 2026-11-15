#include "ClientLevelerHistoryWidget.h"

#include "core/ClientLeveler.h"
#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace AetherSDR {

namespace {

QColor token(const QWidget* w, const char* name)
{
    return ThemeManager::instance().color(w, name);
}

}  // namespace

ClientLevelerHistoryWidget::ClientLevelerHistoryWidget(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName(tr("LVLR gain over the last five seconds"));
}

void ClientLevelerHistoryWidget::push(float gainDb)
{
    m_history[static_cast<std::size_t>(m_head)] = gainDb;
    m_head = (m_head + 1) % kPoints;
    m_count = std::min(m_count + 1, kPoints);
    update();
}

void ClientLevelerHistoryWidget::clear()
{
    m_count = 0;
    update();
}

void ClientLevelerHistoryWidget::setMaxGainDb(float db)
{
    m_maxGainDb = db;
    update();
}

void ClientLevelerHistoryWidget::setActive(bool active)
{
    m_active = active;
    update();
}

void ClientLevelerHistoryWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(this, "color.background.0"));
    const QRectF r = QRectF(rect()).adjusted(30.0, 6.0, -6.0, -16.0);
    const double top = ClientLeveler::kMaxGainDb;
    const auto yFor = [&](double db) {
        return r.bottom() - std::clamp(db, 0.0, top) / top * r.height();
    };

    p.setPen(QPen(token(this, "color.spectrum.grid"), 1.0));
    for (double db : {5.0, 10.0, 15.0, 20.0}) {
        p.drawLine(QPointF(r.left(), yFor(db)), QPointF(r.right(), yFor(db)));
    }
    p.setPen(QPen(token(this, "color.border.strong"), 1.0));
    p.drawLine(QPointF(r.left(), yFor(0.0)), QPointF(r.right(), yFor(0.0)));

    QFont small = font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() - 2.0));
    p.setFont(small);
    p.setPen(token(this, "color.text.label"));
    for (double db : {0.0, 10.0, 20.0}) {
        p.drawText(QRectF(0.0, yFor(db) - 8.0, r.left() - 4.0, 16.0),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0));
    }
    p.drawText(QRectF(r.left(), r.bottom() + 2.0, 60.0, 14.0), Qt::AlignLeft | Qt::AlignTop,
               tr("-5 s"));
    p.drawText(QRectF(r.right() - 60.0, r.bottom() + 2.0, 60.0, 14.0),
               Qt::AlignRight | Qt::AlignTop, tr("now"));

    QPen ceiling(token(this, "color.accent.warning"), 1.2);
    ceiling.setStyle(Qt::DashLine);
    p.setPen(ceiling);
    p.drawLine(QPointF(r.left(), yFor(m_maxGainDb)), QPointF(r.right(), yFor(m_maxGainDb)));

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
