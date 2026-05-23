#include "StripRxChainWidget.h"
#include "core/ClientComp.h"
#include "core/ClientDeEss.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QTimer>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

// Same dimensions as the TX strip's `StripChainWidget` so the two
// modes paint at matching sizes when the strip toggles between them.
constexpr int   kBoxHeight     = 30;
constexpr int   kBoxWidthMin   = 50;
constexpr int   kBoxWidthMax   = 50;
constexpr int   kBoxGapMin     = 6;
constexpr int   kBoxGapPref    = 10;
constexpr int   kMarginX       = 10;
constexpr int   kRowLeftPad    = 16;
constexpr int   kMarginY       = 4;
constexpr int   kArrowTip      = 3;

const char*     kMimeFormat  = "application/x-MasterSDR-rxchain-stage";
constexpr qreal kRadius      = 5.0;

const QColor kBgBox        ("#0e1b28");
const QColor kBgEndpoint   ("#1a2030");
const QColor kBgActive     ("#14253a");
const QColor kBorderIdle   ("#2a3a4a");
const QColor kBorderActive ("#4db8d4");
const QColor kBorderGrey   ("#1e2a38");
// Status-tile "active" colour — green, matches the TX-side mic-ready
// tone so the language reads consistently across both strips.
const QColor kBgStatusOn      ("#006040");
const QColor kBorderStatusOn  ("#00a060");
const QColor kTextStatusOn    ("#00ff88");
const QColor kConnector    ("#2a3a4a");
const QColor kTextLabel    ("#c8d8e8");
const QColor kTextDim      ("#506070");
const QColor kLedActive    ("#00ff88");
const QColor kLedBypass    ("#2a3a4a");
const QColor kDropIndicator("#4db8d4");

// User-facing short label for each RX stage.  Mirrors the docked
// `ClientRxChainWidget::stageLabel` so both surfaces read the same.
QString stageLabel(AudioEngine::RxChainStage s)
{
    switch (s) {
        case AudioEngine::RxChainStage::Eq:    return "EQ";
        case AudioEngine::RxChainStage::Gate:  return "AGC-G";
        case AudioEngine::RxChainStage::Comp:  return "AGC-C";
        case AudioEngine::RxChainStage::Tube:  return "TUBE";
        case AudioEngine::RxChainStage::Pudu:  return "EVO";
        case AudioEngine::RxChainStage::DeEss: return "DESS";
        case AudioEngine::RxChainStage::None:  return "";
    }
    return "";
}

} // namespace

StripRxChainWidget::StripRxChainWidget(QWidget* parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(kBoxHeight + 2 * kMarginY);

    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingClickIdx >= 0) {
            const int idx = m_pendingClickIdx;
            m_pendingClickIdx = -1;
            // ADSP single-click toggles cluster bypass; stages do
            // their own bypass toggle.  Double-click on ADSP opens
            // the MasterDsp dialog (handled separately below).
            if (idx >= 0 && idx < m_boxes.size()) {
                if (m_boxes[idx].kind == TileKind::StatusAdsp) {
                    toggleAdspBypass();
                } else if (m_boxes[idx].kind == TileKind::Stage) {
                    toggleStageBypass(idx);
                }
            }
        }
    });
}

void StripRxChainWidget::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    rebuildLayout();
    update();
}

void StripRxChainWidget::setPcAudioEnabled(bool on)
{
    if (m_pcAudioOn == on) return;
    m_pcAudioOn = on;
    update();
}

void StripRxChainWidget::setClientDspActive(bool active, const QString& label)
{
    if (m_dspActive == active && m_dspLabel == label) return;
    m_dspActive = active;
    m_dspLabel  = label;
    update();
}

void StripRxChainWidget::setOutputUnmuted(bool on)
{
    if (m_outputUnmuted == on) return;
    m_outputUnmuted = on;
    update();
}

bool StripRxChainWidget::isStageImplemented(AudioEngine::RxChainStage s) const
{
    // All six user-controllable RX DSP classes exist now (#2425
    // landed DeEss in the RX path).
    switch (s) {
        case AudioEngine::RxChainStage::Eq:    return true;
        case AudioEngine::RxChainStage::Gate:  return true;
        case AudioEngine::RxChainStage::Comp:  return true;
        case AudioEngine::RxChainStage::Tube:  return true;
        case AudioEngine::RxChainStage::Pudu:  return true;
        case AudioEngine::RxChainStage::DeEss: return true;
        case AudioEngine::RxChainStage::None:  return false;
    }
    return false;
}

bool StripRxChainWidget::isStageBypassed(AudioEngine::RxChainStage s) const
{
    if (!m_audio) return true;
    switch (s) {
        case AudioEngine::RxChainStage::Eq:
            return !(m_audio->clientEqRx() && m_audio->clientEqRx()->isEnabled());
        case AudioEngine::RxChainStage::Gate:
            return !(m_audio->clientGateRx() && m_audio->clientGateRx()->isEnabled());
        case AudioEngine::RxChainStage::Comp:
            return !(m_audio->clientCompRx() && m_audio->clientCompRx()->isEnabled());
        case AudioEngine::RxChainStage::Tube:
            return !(m_audio->clientTubeRx() && m_audio->clientTubeRx()->isEnabled());
        case AudioEngine::RxChainStage::Pudu:
            return !(m_audio->clientPuduRx() && m_audio->clientPuduRx()->isEnabled());
        case AudioEngine::RxChainStage::DeEss:
            return !(m_audio->clientDeEssRx() && m_audio->clientDeEssRx()->isEnabled());
        case AudioEngine::RxChainStage::None:
            return true;
    }
    return true;
}

void StripRxChainWidget::rebuildLayout()
{
    m_boxes.clear();
    if (!m_audio) return;

    // Signal-order list: RADIO, ADSP, then user stages, then SPEAK.
    const auto stages = m_audio->rxChainStages();
    const int totalBoxes = 3 + stages.size();
    if (totalBoxes <= 0) return;

    // Single-row layout.  Box widths shrink toward kBoxWidthMin and
    // gaps tighten to fit; never wraps because the strip runs full
    // window width.
    const int avail = std::max(0, width() - 2 * kMarginX - kRowLeftPad);
    int gap = kBoxGapPref;
    int boxW;
    if (totalBoxes <= 1) {
        boxW = kBoxWidthMax;
    } else {
        const int totalGap = (totalBoxes - 1) * gap;
        const int per = std::max(1, (avail - totalGap) / totalBoxes);
        boxW = std::clamp(per, kBoxWidthMin, kBoxWidthMax);
        if (totalBoxes * boxW + (totalBoxes - 1) * gap > avail) {
            gap = kBoxGapMin;
            const int totalGap2 = (totalBoxes - 1) * gap;
            const int per2 = std::max(1, (avail - totalGap2) / totalBoxes);
            boxW = std::max(8, std::min(per2, kBoxWidthMax));
        }
    }

    auto boxRect = [&](int visualPos) {
        const qreal x = kMarginX + kRowLeftPad + visualPos * (boxW + gap);
        const qreal y = kMarginY;
        return QRectF(x, y, boxW, kBoxHeight);
    };

    auto addBox = [&](TileKind kind,
                      AudioEngine::RxChainStage stage,
                      int signalIdx) {
        BoxRect b;
        b.kind  = kind;
        b.stage = stage;
        b.rect  = boxRect(signalIdx);
        m_boxes.append(b);
    };

    int idx = 0;
    addBox(TileKind::StatusRadio, AudioEngine::RxChainStage::None, idx++);
    addBox(TileKind::StatusAdsp,  AudioEngine::RxChainStage::None, idx++);
    for (auto s : stages) addBox(TileKind::Stage, s, idx++);
    addBox(TileKind::StatusSpeak, AudioEngine::RxChainStage::None, idx++);

    const int desiredH = 2 * kMarginY + kBoxHeight;
    if (height() != desiredH) setFixedHeight(desiredH);
}

int StripRxChainWidget::hitTest(const QPointF& pos) const
{
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].rect.contains(pos)) return i;
    }
    return -1;
}

int StripRxChainWidget::dropInsertIndex(const QPointF& pos) const
{
    // Stage tiles live at m_boxes[2 .. size-2]: RADIO=0, ADSP=1 at
    // the head, SPEAK at the tail.  Convert nearest box → user-stage
    // insert index.
    if (m_boxes.size() < 4) return 0;
    const int nStages = m_boxes.size() - 3;

    int nearest = -1;
    qreal nearestDist2 = std::numeric_limits<qreal>::max();
    for (int i = 0; i < m_boxes.size(); ++i) {
        const QPointF c = m_boxes[i].rect.center();
        const qreal dx = pos.x() - c.x();
        const qreal dy = pos.y() - c.y();
        const qreal d2 = dx * dx + dy * dy;
        if (d2 < nearestDist2) {
            nearestDist2 = d2;
            nearest = i;
        }
    }
    if (nearest < 0) return 0;

    const QRectF r = m_boxes[nearest].rect;
    const bool cursorLeftOfBox = pos.x() < r.center().x();

    int procIdx;
    if (nearest <= 1) {
        procIdx = 0;
    } else if (nearest == m_boxes.size() - 1) {
        procIdx = nStages;
    } else {
        const int p = nearest - 2;
        procIdx = cursorLeftOfBox ? p : p + 1;
    }
    return std::clamp(procIdx, 0, nStages);
}

void StripRxChainWidget::paintEvent(QPaintEvent*)
{
    rebuildLayout();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor("#0f0f1a"));

    if (m_boxes.isEmpty()) return;

    auto drawArrowHead = [&](QPointF tip, QPointF from) {
        const QPointF dir = tip - from;
        const qreal   len = std::hypot(dir.x(), dir.y());
        if (len < 0.01) return;
        const QPointF u(dir.x() / len, dir.y() / len);
        const QPointF perp(-u.y(), u.x());
        QPainterPath head;
        head.moveTo(tip);
        head.lineTo(tip.x() - u.x() * kArrowTip + perp.x() * kArrowTip,
                    tip.y() - u.y() * kArrowTip + perp.y() * kArrowTip);
        head.lineTo(tip.x() - u.x() * kArrowTip - perp.x() * kArrowTip,
                    tip.y() - u.y() * kArrowTip - perp.y() * kArrowTip);
        head.closeSubpath();
        p.setBrush(kConnector);
        p.setPen(Qt::NoPen);
        p.drawPath(head);
    };

    p.setPen(QPen(kConnector, 2.0));
    for (int i = 0; i + 1 < m_boxes.size(); ++i) {
        const QRectF a = m_boxes[i].rect;
        const QRectF b = m_boxes[i + 1].rect;
        const QPointF from(a.right(), a.center().y());
        const QPointF to(b.left() - 1, b.center().y());
        p.setPen(QPen(kConnector, 2.0));
        p.drawLine(from, to);
        drawArrowHead(to, from);
    }

    QFont labelFont = p.font();
    labelFont.setPixelSize(12);
    labelFont.setBold(true);
    p.setFont(labelFont);

    for (int i = 0; i < m_boxes.size(); ++i) {
        const auto& b = m_boxes[i];

        if (b.kind != TileKind::Stage) {
            // Status / launcher tile.  RADIO greens with PC Audio on,
            // SPEAK greens with output unmuted, ADSP greens whenever
            // any client NR module is active.
            bool   isOn      = false;
            QString labelText;
            switch (b.kind) {
                case TileKind::StatusRadio:
                    isOn = m_pcAudioOn;
                    labelText = "RADIO";
                    break;
                case TileKind::StatusAdsp:
                    // Render the ADSP tile as a Stage-style toggle
                    // (same look as EQ): kBgActive + LED when at
                    // least one NR module is on; kBgBox + dim border
                    // when bypassed.  Label rotates the active NR
                    // module's short name; falls back to "ADSP".
                    {
                        const bool bypassed = isAdspBypassed();
                        labelText = (m_dspLabel.isEmpty() || bypassed)
                                  ? QStringLiteral("ADSP")
                                  : m_dspLabel;
                        p.setBrush(bypassed ? kBgBox : kBgActive);
                        p.setPen(QPen(bypassed ? kBorderIdle : kBorderActive, 1.0));
                        p.drawRoundedRect(b.rect, kRadius, kRadius);
                        const QPointF led(b.rect.right() - 4, b.rect.top() + 4);
                        p.setBrush(bypassed ? kLedBypass : kLedActive);
                        p.setPen(Qt::NoPen);
                        p.drawEllipse(led, 1.8, 1.8);
                        p.setPen(kTextLabel);
                        p.drawText(b.rect, Qt::AlignCenter, labelText);
                        continue;
                    }
                case TileKind::StatusSpeak:
                    isOn = m_outputUnmuted;
                    labelText = "SPEAK";
                    break;
                case TileKind::Stage:
                    break;  // unreachable
            }

            QBrush body(kBgEndpoint);
            QColor borderCol = kBorderGrey;
            QColor textCol   = kTextLabel;
            if (isOn) {
                body      = kBgStatusOn;
                borderCol = kBorderStatusOn;
                textCol   = kTextStatusOn;
            }
            p.setBrush(body);
            p.setPen(QPen(borderCol, 1.0));
            p.drawRoundedRect(b.rect, kRadius, kRadius);
            p.setPen(textCol);
            p.drawText(b.rect, Qt::AlignCenter, labelText);
            continue;
        }

        const bool implemented = isStageImplemented(b.stage);
        const bool bypassed    = isStageBypassed(b.stage);

        p.setBrush(bypassed ? kBgBox : kBgActive);
        p.setPen(QPen(implemented ? (bypassed ? kBorderIdle : kBorderActive)
                                  : kBorderGrey, 1.0));
        p.drawRoundedRect(b.rect, kRadius, kRadius);

        if (implemented) {
            const QPointF led(b.rect.right() - 4, b.rect.top() + 4);
            p.setBrush(bypassed ? kLedBypass : kLedActive);
            p.setPen(Qt::NoPen);
            p.drawEllipse(led, 1.8, 1.8);
        }

        p.setPen(implemented ? kTextLabel : kTextDim);
        p.drawText(b.rect, Qt::AlignCenter, stageLabel(b.stage));
    }

    if (m_dropIndex >= 0 && m_boxes.size() >= 4) {
        // Drop indicator sits between the box just before and just
        // after the insertion point.  Stage list lives at indices
        // [2 .. size-2].
        const int leftIdx  = 1 + m_dropIndex;       // signal index after-RADIO+ADSP
        const int rightIdx = leftIdx + 1;
        if (leftIdx >= 0 && rightIdx < m_boxes.size()) {
            const QRectF lr = m_boxes[leftIdx].rect;
            const QRectF rr = m_boxes[rightIdx].rect;
            const qreal x = (lr.right() + rr.left()) * 0.5;
            p.setPen(QPen(kDropIndicator, 3.0));
            p.drawLine(QPointF(x, rr.top() - 2),
                       QPointF(x, rr.bottom() + 2));
        }
    }
}

void StripRxChainWidget::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { QWidget::mousePressEvent(ev); return; }
    const int idx = hitTest(ev->position());
    if (idx < 0) {
        m_pressIndex = -1;
        return;
    }
    // RADIO and SPEAK are non-interactive; ADSP and stages capture.
    if (m_boxes[idx].kind == TileKind::StatusRadio
     || m_boxes[idx].kind == TileKind::StatusSpeak) {
        m_pressIndex = -1;
        return;
    }
    m_pressPos   = ev->position().toPoint();
    m_pressIndex = idx;
    ev->accept();
}

void StripRxChainWidget::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_pressIndex < 0 || !(ev->buttons() & Qt::LeftButton)) {
        const int idx = hitTest(ev->position());
        const bool clickable = (idx >= 0)
            && (m_boxes[idx].kind == TileKind::Stage
             || m_boxes[idx].kind == TileKind::StatusAdsp);
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(ev);
        return;
    }
    if ((ev->position().toPoint() - m_pressPos).manhattanLength() < 6) return;

    // ADSP doesn't drag — it's a launcher.  Only stages reorder.
    if (m_boxes[m_pressIndex].kind != TileKind::Stage) return;

    const auto& b = m_boxes[m_pressIndex];
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData;
    mime->setData(kMimeFormat,
                  QByteArray::number(static_cast<int>(b.stage)));
    drag->setMimeData(mime);

    QPixmap pix(b.rect.size().toSize());
    pix.fill(Qt::transparent);
    {
        QPainter pp(&pix);
        pp.setRenderHint(QPainter::Antialiasing, true);
        pp.setBrush(kBgActive);
        pp.setPen(QPen(kBorderActive, 1.0));
        pp.drawRoundedRect(QRectF(0, 0, pix.width() - 1, pix.height() - 1),
                           kRadius, kRadius);
        QFont f = pp.font();
        f.setPixelSize(12);
        f.setBold(true);
        pp.setFont(f);
        pp.setPen(kTextLabel);
        pp.drawText(pix.rect(), Qt::AlignCenter, stageLabel(b.stage));
    }
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(pix.width() / 2, pix.height() / 2));

    m_pressIndex = -1;
    drag->exec(Qt::MoveAction);
    m_dropIndex = -1;
    update();
}

void StripRxChainWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(ev); return; }
    if (m_pressIndex < 0) return;
    const int idx = m_pressIndex;
    m_pressIndex = -1;
    if (idx < 0 || idx >= m_boxes.size()) return;
    const TileKind kind = m_boxes[idx].kind;
    if (kind != TileKind::Stage && kind != TileKind::StatusAdsp) return;
    m_pendingClickIdx = idx;
    if (m_clickTimer) {
        m_clickTimer->start(QApplication::doubleClickInterval());
    }
    ev->accept();
}

void StripRxChainWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    if (m_clickTimer && m_clickTimer->isActive()) m_clickTimer->stop();
    m_pendingClickIdx = -1;

    const int idx = hitTest(ev->position());
    if (idx < 0) return;
    const auto& b = m_boxes[idx];
    if (b.kind == TileKind::StatusAdsp) {
        emit dspEditRequested();
        ev->accept();
        return;
    }
    if (b.kind != TileKind::Stage) return;
    if (!isStageImplemented(b.stage)) return;
    emit editRequested(b.stage);
    ev->accept();
}

void StripRxChainWidget::toggleStageBypass(int boxIdx)
{
    if (!m_audio) return;
    if (boxIdx < 0 || boxIdx >= m_boxes.size()) return;
    if (m_boxes[boxIdx].kind != TileKind::Stage) return;
    const auto stage = m_boxes[boxIdx].stage;
    if (!isStageImplemented(stage)) return;

    const bool newEnabled = isStageBypassed(stage);
    switch (stage) {
        case AudioEngine::RxChainStage::Eq:
            if (auto* eq = m_audio->clientEqRx()) {
                eq->setEnabled(newEnabled);
                m_audio->saveClientEqSettings();
            }
            break;
        case AudioEngine::RxChainStage::Gate:
            if (auto* g = m_audio->clientGateRx()) {
                g->setEnabled(newEnabled);
                m_audio->saveClientGateRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Comp:
            if (auto* c = m_audio->clientCompRx()) {
                c->setEnabled(newEnabled);
                m_audio->saveClientCompRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Tube:
            if (auto* t = m_audio->clientTubeRx()) {
                t->setEnabled(newEnabled);
                m_audio->saveClientTubeRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Pudu:
            if (auto* p = m_audio->clientPuduRx()) {
                p->setEnabled(newEnabled);
                m_audio->saveClientPuduRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::DeEss:
            if (auto* d = m_audio->clientDeEssRx()) {
                d->setEnabled(newEnabled);
                m_audio->saveClientDeEssRxSettings();
            }
            break;
        default:
            return;
    }
    emit stageEnabledChanged(stage, newEnabled);
    update();
}

void StripRxChainWidget::contextMenuEvent(QContextMenuEvent* ev)
{
    const int idx = hitTest(ev->pos());
    if (idx < 0 || m_boxes[idx].kind != TileKind::Stage) {
        QWidget::contextMenuEvent(ev);
        return;
    }
    const auto stage = m_boxes[idx].stage;
    if (!isStageImplemented(stage) || !m_audio) {
        QToolTip::showText(ev->globalPos(),
            QString("<b>%1</b> — not implemented yet").arg(stageLabel(stage)),
            this);
        return;
    }

    QMenu menu(this);
    auto* header = menu.addAction(
        QString("Stage — %1").arg(stageLabel(stage)));
    header->setEnabled(false);
    menu.addSeparator();

    const bool bypassed = isStageBypassed(stage);
    auto* bypassAct = menu.addAction(bypassed ? "Enable" : "Bypass");
    auto* editAct   = menu.addAction("Edit…");

    QAction* chosen = menu.exec(ev->globalPos());
    if (!chosen) return;
    if (chosen == editAct) {
        emit editRequested(stage);
        return;
    }
    if (chosen == bypassAct) {
        toggleStageBypass(idx);
    }
}

void StripRxChainWidget::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasFormat(kMimeFormat)) {
        ev->acceptProposedAction();
    }
}

void StripRxChainWidget::dragMoveEvent(QDragMoveEvent* ev)
{
    if (!ev->mimeData()->hasFormat(kMimeFormat)) return;
    const int idx = dropInsertIndex(ev->position());
    if (idx != m_dropIndex) { m_dropIndex = idx; update(); }
    ev->acceptProposedAction();
}

void StripRxChainWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropIndex = -1;
    update();
}

void StripRxChainWidget::dropEvent(QDropEvent* ev)
{
    if (!m_audio || !ev->mimeData()->hasFormat(kMimeFormat)) {
        m_dropIndex = -1;
        update();
        return;
    }
    const auto stage = static_cast<AudioEngine::RxChainStage>(
        ev->mimeData()->data(kMimeFormat).toInt());
    auto stages = m_audio->rxChainStages();
    const int from = stages.indexOf(stage);
    if (from < 0) { m_dropIndex = -1; update(); return; }

    int to = dropInsertIndex(ev->position());
    stages.removeAt(from);
    if (to > from) --to;
    to = std::clamp(to, 0, static_cast<int>(stages.size()));
    stages.insert(to, stage);

    m_audio->setRxChainStages(stages);
    m_dropIndex = -1;
    rebuildLayout();
    update();
    emit chainReordered();
    ev->acceptProposedAction();
}

void StripRxChainWidget::leaveEvent(QEvent*)
{
    setCursor(Qt::ArrowCursor);
}

QSize StripRxChainWidget::sizeHint() const
{
    const int totalBoxes = m_audio
        ? 3 + static_cast<int>(m_audio->rxChainStages().size())
        : 9;   // RADIO + ADSP + 6 stages + SPEAK
    return QSize(2 * kMarginX + kRowLeftPad
                                + totalBoxes * kBoxWidthMin
                                + std::max(0, totalBoxes - 1) * kBoxGapMin,
                 2 * kMarginY + kBoxHeight);
}

// ── ADSP cluster bypass — same single-click pattern as a Stage tile ─

bool StripRxChainWidget::isAdspBypassed() const
{
    if (!m_audio) return true;
    return !(m_audio->nr2Enabled()  || m_audio->nr4Enabled() ||
             m_audio->mnrEnabled()  || m_audio->dfnrEnabled() ||
             m_audio->rn2Enabled()  || m_audio->bnrEnabled());
}

void StripRxChainWidget::toggleAdspBypass()
{
    if (!m_audio) return;
    if (!isAdspBypassed()) {
        // Snapshot the active NR modules then disable them all.
        m_adspBypassSnapshot.clear();
        if (m_audio->nr2Enabled())  m_adspBypassSnapshot << "NR2";
        if (m_audio->nr4Enabled())  m_adspBypassSnapshot << "NR4";
        if (m_audio->mnrEnabled())  m_adspBypassSnapshot << "MNR";
        if (m_audio->dfnrEnabled()) m_adspBypassSnapshot << "DFNR";
        if (m_audio->rn2Enabled())  m_adspBypassSnapshot << "RN2";
        if (m_audio->bnrEnabled())  m_adspBypassSnapshot << "BNR";
        m_audio->setNr2Enabled(false);
        m_audio->setNr4Enabled(false);
        m_audio->setMnrEnabled(false);
        m_audio->setDfnrEnabled(false);
        m_audio->setRn2Enabled(false);
        m_audio->setBnrEnabled(false);
    } else {
        // Restore snapshot.  If nothing was on at bypass-time (or no
        // snapshot persisted), fall back to NR2 — sensible default
        // matching the MasterDspWidget's first-use behaviour.
        if (m_adspBypassSnapshot.isEmpty()) {
            m_audio->setNr2Enabled(true);
        } else {
            for (const QString& m : m_adspBypassSnapshot) {
                if (m == "NR2")  m_audio->setNr2Enabled(true);
                else if (m == "NR4")  m_audio->setNr4Enabled(true);
                else if (m == "MNR")  m_audio->setMnrEnabled(true);
                else if (m == "DFNR") m_audio->setDfnrEnabled(true);
                else if (m == "RN2")  m_audio->setRn2Enabled(true);
                else if (m == "BNR")  m_audio->setBnrEnabled(true);
            }
        }
        m_adspBypassSnapshot.clear();
    }
    update();
}

} // namespace MasterSDR
