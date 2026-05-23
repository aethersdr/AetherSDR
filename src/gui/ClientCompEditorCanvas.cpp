#include "ClientCompEditorCanvas.h"
#include "core/ClientComp.h"

#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kHandleHitRadius = 10.0f;

const QColor kThresholdHandle("#e8a540");
const QColor kRatioHandle    ("#4db8d4");
const QColor kHandleOutline  ("#ffffff");

} // namespace

ClientCompEditorCanvas::ClientCompEditorCanvas(QWidget* parent)
    : ClientCompCurveWidget(parent)
{
    setMinimumHeight(220);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
}

bool ClientCompEditorCanvas::thresholdHandleHit(const QPointF& pos) const
{
    if (!m_comp) return false;
    const float T = m_comp->thresholdDb();
    // Threshold is grabbable along the entire vertical guide line, not
    // just on the chevron at the bottom — making the whole column a
    // drag target means the user can grab the threshold from anywhere
    // on its guide, which is where their eye is already looking.
    const float tx = dbToX(T);
    const float dx = std::fabs(static_cast<float>(pos.x() - tx));
    return dx < kHandleHitRadius;
}

bool ClientCompEditorCanvas::ratioHandleHit(const QPointF& pos) const
{
    if (!m_comp) return false;
    const float T = m_comp->thresholdDb();
    const float outDb = curveOutputDb(T);
    const QPointF h(dbToX(T), dbToY(outDb));
    const float dx = static_cast<float>(pos.x() - h.x());
    const float dy = static_cast<float>(pos.y() - h.y());
    return (dx * dx + dy * dy) < kHandleHitRadius * kHandleHitRadius;
}

void ClientCompEditorCanvas::paintEvent(QPaintEvent* ev)
{
    ClientCompCurveWidget::paintEvent(ev);
    if (!m_comp) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Threshold visual — full-height dashed amber guide line + a
    // chevron at the bottom.  Previously the handle was a tiny
    // triangle that was easy to miss; the guide line now reads as
    // "the threshold lives at this x" from a glance, and its whole
    // column is draggable (see thresholdHandleHit).
    const float T  = m_comp->thresholdDb();
    const float tx = dbToX(T);

    const float curveY = dbToY(std::clamp(curveOutputDb(T), kMinDb, kMaxDb));
    QColor guideColor = kThresholdHandle;
    guideColor.setAlpha(160);
    QPen guidePen(guideColor, 1.5, Qt::DashLine);
    guidePen.setDashPattern({4.0, 3.0});
    p.setPen(guidePen);
    p.drawLine(QPointF(tx, curveY), QPointF(tx, rect().bottom() - 8.0f));

    const float by = rect().bottom() - 2.0f;
    QPainterPath tri;
    tri.moveTo(tx,          by - 14.0f);
    tri.lineTo(tx - 9.0f,   by);
    tri.lineTo(tx + 9.0f,   by);
    tri.closeSubpath();
    p.setBrush(kThresholdHandle);
    p.setPen(QPen(kHandleOutline, 1.5));
    p.drawPath(tri);

    // Ratio handle — larger filled dot at the knee centre so it reads
    // as "drag me to change the slope."  White outline so it stays
    // visible against the cyan curve even when GR is 0.
    const float outDb = curveOutputDb(T);
    const QPointF rh(tx, dbToY(outDb));
    p.setBrush(kRatioHandle);
    p.setPen(QPen(kHandleOutline, 1.2));
    p.drawEllipse(rh, 6.5, 6.5);
}

void ClientCompEditorCanvas::mousePressEvent(QMouseEvent* ev)
{
    if (!m_comp || ev->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(ev);
        return;
    }
    if (ratioHandleHit(ev->position())) {
        m_drag = Drag::Ratio;
        m_dragStart = ev->position();
        m_dragStartValue = m_comp->ratio();
        setCursor(Qt::SizeVerCursor);
        ev->accept();
        return;
    }
    if (thresholdHandleHit(ev->position())) {
        m_drag = Drag::Threshold;
        m_dragStart = ev->position();
        m_dragStartValue = m_comp->thresholdDb();
        setCursor(Qt::SizeHorCursor);
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientCompEditorCanvas::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_drag == Drag::None || !m_comp) {
        QWidget::mouseMoveEvent(ev);
        return;
    }

    if (m_drag == Drag::Threshold) {
        const float t = std::clamp(xToDb(static_cast<float>(ev->position().x())),
                                   kMinDb, kMaxDb);
        emit thresholdChanged(t);
        update();
    } else if (m_drag == Drag::Ratio) {
        // Vertical pixels → ratio.  Dragging up increases ratio
        // (steeper compression / limiter behaviour), dragging down
        // decreases it toward 1:1.  Exponential feels best — a small
        // motion near 1:1 stays subtle and a big motion near the top
        // pushes into limiter territory.
        const float dy = static_cast<float>(m_dragStart.y() - ev->position().y());
        const float scale = (ev->modifiers() & Qt::ShiftModifier) ? 0.25f : 1.0f;
        const float factor = std::pow(4.0f, scale * dy / 200.0f);
        const float next = std::clamp(m_dragStartValue * factor, 1.0f, 20.0f);
        emit ratioChanged(next);
        update();
    }
    ev->accept();
}

void ClientCompEditorCanvas::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_drag == Drag::None) {
        QWidget::mouseReleaseEvent(ev);
        return;
    }
    m_drag = Drag::None;
    setCursor(Qt::CrossCursor);
    ev->accept();
}

bool ClientCompEditorCanvas::event(QEvent* ev)
{
    if (ev->type() == QEvent::ToolTip && m_comp) {
        auto* help = static_cast<QHelpEvent*>(ev);
        if (ratioHandleHit(help->pos())) {
            const float ratio = m_comp->ratio();
            QToolTip::showText(help->globalPos(),
                QString("<b>Ratio — %1 :1</b><br>"
                        "Amount of compression above the threshold. "
                        "Higher = harder squeeze.<br>"
                        "<i>Drag up/down to change. Hold Shift for fine adjust.</i>")
                    .arg(ratio, 0, 'f', 2),
                this);
            return true;
        }
        if (thresholdHandleHit(help->pos())) {
            const float T = m_comp->thresholdDb();
            QToolTip::showText(help->globalPos(),
                QString("<b>Threshold — %1 dBFS</b><br>"
                        "Audio level where compression begins. "
                        "Anything louder gets reduced.<br>"
                        "<i>Drag the chevron left/right, or grab anywhere along "
                        "the dashed guide line.</i>")
                    .arg(T, 0, 'f', 1),
                this);
            return true;
        }
        QToolTip::hideText();
        ev->ignore();
        return true;
    }
    return ClientCompCurveWidget::event(ev);
}

} // namespace MasterSDR
