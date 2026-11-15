#include "ClientChainWidget.h"
#include "InteractionSettings.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientDeEss.h"
#include "core/ClientTube.h"
#include "core/ClientPudu.h"
#include "core/ClientReverb.h"

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
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// Height matches the RX/TX tab buttons in the CEQ applet.  The chain
// wraps into multiple rows snake-style — row 0 reads left-to-right,
// row 1 right-to-left, and so on — so the full 8-box chain fits
// inside a 260 px applet panel without crushing the boxes.
constexpr int   kBoxHeight     = 20;
constexpr int   kBoxWidthMin   = 36;
constexpr int   kBoxWidthMax   = 54;
constexpr int   kBoxGapMin     = 6;
constexpr int   kBoxGapPref    = 10;
// kMarginX must exceed kTurnOffset so row-transition connectors don't
// clip off the widget's left / right edges.
constexpr int   kMarginX       = 10;
// Extra horizontal offset added to each row's leftmost box.  Shifts
// the whole snake right so the layout reads more centred inside the
// applet panel without resizing the boxes.
constexpr int   kRowLeftPad    = 16;
constexpr int   kMarginY       = 4;
constexpr int   kRowGap        = 12;     // vertical space between wrapped rows
constexpr int   kTurnOffset    = 4;      // horizontal stub length at a row transition
constexpr int   kArrowTip      = 3;

const char*     kMimeFormat  = "application/x-aethersdr-chain-stage";
constexpr qreal kRadius      = 5.0;

inline QColor kBgBox() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor kBgEndpoint() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBgActive() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBorderIdle() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBorderActive() { return AetherSDR::ThemeManager::instance().color("color.accent.dim"); }
inline QColor kBorderGrey() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
// MIC endpoint "ready" state — matches RxApplet's SQL-button green
// so users recognise the visual language across the sidebar.
inline QColor kBgMicReady() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kBorderMicReady() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kTextMicReady() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
// TX endpoint "active" state — red, pulsing via setTxActive().
// Two shades lerped by the pulse factor give a breathing effect.
inline QColor kBgTxActiveDim() { return AetherSDR::ThemeManager::instance().color("color.background.tx"); }
inline QColor kBgTxActiveHot() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kBorderTxActive() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kTextTxActive() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kConnector() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kTextLabel() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
inline QColor kTextDim() { return AetherSDR::ThemeManager::instance().color("color.background.3"); }
inline QColor kLedActive() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kLedBypass() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kDropIndicator() { return AetherSDR::ThemeManager::instance().color("color.accent.dim"); }

// User-facing short label for each stage.  Kept 4 chars or fewer so
// it fits inside the narrow boxes when the chain is crowded.
QString stageLabel(AudioEngine::TxChainStage s)
{
    switch (s) {
        case AudioEngine::TxChainStage::Gate:   return "GATE";
        case AudioEngine::TxChainStage::Eq:     return "EQ";
        case AudioEngine::TxChainStage::DeEss:  return "DESS";
        case AudioEngine::TxChainStage::Comp:   return "COMP";
        case AudioEngine::TxChainStage::Tube:   return "TUBE";
        case AudioEngine::TxChainStage::Enh:    return "EVO";
        case AudioEngine::TxChainStage::Reverb: return "VERB";
        case AudioEngine::TxChainStage::None:   return "";
    }
    return "";
}

} // namespace

ClientChainWidget::ClientChainWidget(QWidget* parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Height is computed in rebuildLayout() once we know the box count
    // and the snake row count — start with a single-row default.
    setMinimumHeight(kBoxHeight + 2 * kMarginY);

    // Deferred single-click timer — toggles bypass on fire, cancelled
    // by a double-click arriving within the double-click interval.
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingClickIdx >= 0) {
            const int idx = m_pendingClickIdx;
            m_pendingClickIdx = -1;
            toggleStageBypass(idx);
        }
    });
}

void ClientChainWidget::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    rebuildLayout();
    update();
}

void ClientChainWidget::setMicInputReady(bool ready)
{
    if (m_micInputReady == ready) return;
    m_micInputReady = ready;
    update();
}

void ClientChainWidget::setTxActive(bool active)
{
    if (m_txActive == active) return;
    m_txActive = active;
    if (active) {
        // Lazy-create the pulse timer once, then keep it for reuse.
        if (!m_txPulseTimer) {
            m_txPulseTimer = new QTimer(this);
            m_txPulseTimer->setInterval(33);   // ~30 fps
            m_txPulseTimer->setTimerType(Qt::PreciseTimer);
            connect(m_txPulseTimer, &QTimer::timeout, this, [this]() {
                m_txPulsePhase += 0.033f;       // seconds per tick
                update();
            });
        }
        m_txPulsePhase = 0.0f;
        m_txPulseTimer->start();
    } else if (m_txPulseTimer) {
        m_txPulseTimer->stop();
    }
    update();
}

bool ClientChainWidget::isStageImplemented(AudioEngine::TxChainStage s) const
{
    // All six TX DSP stages are now implemented.  The Enh slot hosts
    // the PUDU exciter (Phase 5) — the enum name is legacy; the
    // user-facing label is PUDU (see stageLabel above).
    return s == AudioEngine::TxChainStage::Eq
        || s == AudioEngine::TxChainStage::Comp
        || s == AudioEngine::TxChainStage::Gate
        || s == AudioEngine::TxChainStage::DeEss
        || s == AudioEngine::TxChainStage::Tube
        || s == AudioEngine::TxChainStage::Enh
        || s == AudioEngine::TxChainStage::Reverb;
}

bool ClientChainWidget::isStageBypassed(AudioEngine::TxChainStage s) const
{
    if (!m_audio) return true;
    switch (s) {
        case AudioEngine::TxChainStage::Eq:
            return !(m_audio->clientEqTx() && m_audio->clientEqTx()->isEnabled());
        case AudioEngine::TxChainStage::Comp:
            return !(m_audio->clientCompTx() && m_audio->clientCompTx()->isEnabled());
        case AudioEngine::TxChainStage::Gate:
            return !(m_audio->clientGateTx() && m_audio->clientGateTx()->isEnabled());
        case AudioEngine::TxChainStage::DeEss:
            return !(m_audio->clientDeEssTx() && m_audio->clientDeEssTx()->isEnabled());
        case AudioEngine::TxChainStage::Tube:
            return !(m_audio->clientTubeTx() && m_audio->clientTubeTx()->isEnabled());
        case AudioEngine::TxChainStage::Enh:   // PUDU slot
            return !(m_audio->clientPuduTx() && m_audio->clientPuduTx()->isEnabled());
        case AudioEngine::TxChainStage::Reverb:
            return !(m_audio->clientReverbTx() && m_audio->clientReverbTx()->isEnabled());
        default:
            return true;
    }
}

void ClientChainWidget::rebuildLayout()
{
    m_boxes.clear();
    if (!m_audio) return;

    // Assemble the signal-order list — MIC first, then user's chain,
    // then TX sink.
    const auto stages = m_audio->txChainStages();
    const int totalBoxes = 2 + stages.size();
    if (totalBoxes <= 0) return;

    // Snake layout: wrap the chain into multiple rows so 8 boxes fit
    // inside the 260 px applet panel comfortably.  Box width is
    // preferred size (kBoxWidthMax); gap shrinks to fit if needed;
    // boxesPerRow falls out of how many fit in the available width.
    const int avail = std::max(0, width() - 2 * kMarginX);
    int gap  = kBoxGapPref;
    int boxW = kBoxWidthMax;

    auto computeBoxesPerRow = [&]() {
        // floor((avail + gap) / (boxW + gap)) — the +gap accounts for
        // no gap after the last box.
        if (boxW <= 0) return 1;
        return std::max(1, (avail + gap) / (boxW + gap));
    };
    int boxesPerRow = std::min(totalBoxes, computeBoxesPerRow());

    // If even the minimum box fits <2 per row, shrink the gap first,
    // then fall back to single-row cramming.
    if (boxesPerRow < 2 && totalBoxes > 1) {
        gap  = kBoxGapMin;
        boxW = kBoxWidthMin;
        boxesPerRow = std::min(totalBoxes, computeBoxesPerRow());
    }

    const int numRows = (totalBoxes + boxesPerRow - 1) / boxesPerRow;
    const qreal yStart = kMarginY;

    auto boxRect = [&](int visualPos, int row) {
        // visualPos is 0..boxesPerRow-1 left-to-right on screen,
        // regardless of which signal-order entry it holds.  A small
        // fixed left pad on every row pushes the whole snake right so
        // it reads more centred in the applet panel.
        const qreal x = kMarginX + kRowLeftPad + visualPos * (boxW + gap);
        const qreal y = yStart + row * (kBoxHeight + kRowGap);
        return QRectF(x, y, boxW, kBoxHeight);
    };

    auto addBox = [&](AudioEngine::TxChainStage s, bool endpoint, int signalIdx) {
        const int row = signalIdx / boxesPerRow;
        const int posInRow = signalIdx % boxesPerRow;
        // Even rows (0, 2, ...) read left-to-right.  Odd rows (1, 3,
        // ...) reverse direction so signal flow continues naturally
        // from the end of the previous row.
        const int visualPos = (row & 1)
            ? (boxesPerRow - 1 - posInRow)
            : posInRow;
        BoxRect b;
        b.stage      = s;
        b.isEndpoint = endpoint;
        b.rect       = boxRect(visualPos, row);
        m_boxes.append(b);
    };

    int idx = 0;
    addBox(AudioEngine::TxChainStage::None, true, idx++);
    for (auto s : stages) addBox(s, false, idx++);
    addBox(AudioEngine::TxChainStage::None, true, idx++);

    // Resize widget height to fit all rows.
    const int desiredH = 2 * kMarginY + numRows * kBoxHeight
                         + (numRows - 1) * kRowGap;
    if (height() != desiredH) setFixedHeight(desiredH);
}

int ClientChainWidget::hitTest(const QPointF& pos) const
{
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].rect.contains(pos)) return i;
    }
    return -1;
}

int ClientChainWidget::dropInsertIndex(const QPointF& pos) const
{
    // Return an index in the *processor list* (excluding endpoints)
    // where the dragged stage should be inserted.  Processor boxes
    // live at m_boxes[1 .. size-2].  In the snake layout, the nearest
    // box is the one whose centre is closest to the cursor (2D
    // distance), and the drop lands on the "before" side of that box
    // if the cursor is to its left (or above-then-left on odd rows),
    // "after" side otherwise.
    if (m_boxes.size() < 3) return 0;

    int nearestSignalIdx = -1;
    qreal nearestDist2 = std::numeric_limits<qreal>::max();
    for (int i = 0; i < m_boxes.size(); ++i) {
        const QPointF c = m_boxes[i].rect.center();
        const qreal dx = pos.x() - c.x();
        const qreal dy = pos.y() - c.y();
        const qreal d2 = dx * dx + dy * dy;
        if (d2 < nearestDist2) {
            nearestDist2 = d2;
            nearestSignalIdx = i;
        }
    }
    if (nearestSignalIdx < 0) return 0;

    // Decide whether the cursor sits on the "before" or "after" side
    // of the nearest box in signal-order terms.  On even rows that's
    // left/right; on odd rows it's flipped because the row reads
    // right-to-left.
    const QRectF r = m_boxes[nearestSignalIdx].rect;
    // Row parity inferred from y — compare to neighbours.
    bool rowIsOdd = false;
    if (nearestSignalIdx > 0) {
        rowIsOdd =
            m_boxes[nearestSignalIdx].rect.right() <
            m_boxes[nearestSignalIdx - 1].rect.right() - 1.0 &&
            qFuzzyCompare(m_boxes[nearestSignalIdx].rect.center().y(),
                          m_boxes[nearestSignalIdx - 1].rect.center().y());
    }
    const bool cursorLeftOfBox = pos.x() < r.center().x();
    const bool beforeInSignal  = rowIsOdd ? !cursorLeftOfBox : cursorLeftOfBox;

    // Convert signal-order index of nearest box to processor-list
    // insert index.  Processor indices = signal index - 1 (since MIC
    // is signal index 0).  "Before" means insert at nearestProcIdx;
    // "after" means insert at nearestProcIdx + 1.  MIC / TX endpoints
    // clamp to the first / last valid processor insertion slots.
    const int nProc = m_boxes.size() - 2;
    int procIdx;
    if (nearestSignalIdx == 0) {
        procIdx = 0;                              // anchored at MIC → before first
    } else if (nearestSignalIdx == m_boxes.size() - 1) {
        procIdx = nProc;                          // anchored at TX → after last
    } else {
        const int p = nearestSignalIdx - 1;       // processor-list index
        procIdx = beforeInSignal ? p : p + 1;
    }
    return std::clamp(procIdx, 0, nProc);
}

void ClientChainWidget::paintEvent(QPaintEvent*)
{
    rebuildLayout();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), AetherSDR::ThemeManager::instance().color("color.background.0"));

    if (m_boxes.isEmpty()) return;

    // Connector lines between adjacent boxes — handles three cases:
    //   - same row, both visual-left-to-right (even row)
    //   - same row, both visual-right-to-left (odd row)
    //   - row transition: a small L / U bend with a downward arrow
    auto drawArrowHead = [&](QPointF tip, QPointF from) {
        // Filled triangle pointing from `from` toward `tip`.
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
        p.setBrush(kConnector());
        p.setPen(Qt::NoPen);
        p.drawPath(head);
    };

    p.setPen(QPen(kConnector(), 2.0));
    for (int i = 0; i + 1 < m_boxes.size(); ++i) {
        const QRectF a = m_boxes[i].rect;
        const QRectF b = m_boxes[i + 1].rect;
        const bool sameRow = qFuzzyCompare(a.center().y(), b.center().y());
        if (sameRow) {
            // Horizontal connector — direction depends on row parity.
            // Even row: a is left, b is right → arrow into b's left edge.
            // Odd row: a is right, b is left → arrow into b's right edge.
            const bool rtl = a.center().x() > b.center().x();
            QPointF from, to;
            if (rtl) {
                from = QPointF(a.left(), a.center().y());
                to   = QPointF(b.right() + 1, b.center().y());
            } else {
                from = QPointF(a.right(), a.center().y());
                to   = QPointF(b.left() - 1, b.center().y());
            }
            p.setPen(QPen(kConnector(), 2.0));
            p.drawLine(from, to);
            drawArrowHead(to, from);
        } else {
            // Row transition — snake layout always turns at whichever
            // side of the widget the boxes currently occupy.  Compare
            // the box centre to the widget centre: right half → turn
            // right, left half → turn left.  (A naïve a.right() vs
            // b.right() comparison picks the wrong side when the
            // transition boxes are at the widget's LEFT edge — both
            // right edges match but the turn still has to go left.)
            const bool turnRight = a.center().x() > rect().center().x();
            const QPointF aEdge = turnRight
                ? QPointF(a.right(), a.center().y())
                : QPointF(a.left(),  a.center().y());
            const QPointF bEdge = turnRight
                ? QPointF(b.right(), b.center().y())
                : QPointF(b.left(),  b.center().y());
            const qreal turnX = turnRight ? aEdge.x() + kTurnOffset
                                          : aEdge.x() - kTurnOffset;
            p.setPen(QPen(kConnector(), 2.0));
            p.drawLine(aEdge,                       QPointF(turnX, aEdge.y()));
            p.drawLine(QPointF(turnX, aEdge.y()),   QPointF(turnX, bEdge.y()));
            p.drawLine(QPointF(turnX, bEdge.y()),   bEdge);
            drawArrowHead(bEdge, QPointF(turnX, bEdge.y()));
        }
    }

    // Boxes.  9 px bold matches the RX/TX tab label scale.
    QFont labelFont = p.font();
    labelFont.setPixelSize(9);
    labelFont.setBold(true);
    p.setFont(labelFont);

    for (int i = 0; i < m_boxes.size(); ++i) {
        const auto& b = m_boxes[i];
        const bool firstEndpoint = (i == 0);
        const bool lastEndpoint  = (i == m_boxes.size() - 1);

        if (b.isEndpoint) {
            // MIC / TX sinks.  Colour treatment depends on which
            // endpoint + current state:
            //   MIC + ready    → SQL-green (routed through PooDoo)
            //   TX  + tx active → pulsing red (1 Hz sine breathing)
            //   otherwise      → dim endpoint grey
            const bool micReady = firstEndpoint && m_micInputReady;
            const bool txActive = lastEndpoint && m_txActive;

            QBrush bodyBrush(kBgEndpoint());
            QColor borderCol = kBorderGrey();
            QColor textCol   = kTextLabel();

            if (micReady) {
                bodyBrush = kBgMicReady();
                borderCol = kBorderMicReady();
                textCol   = kTextMicReady();
            } else if (txActive) {
                // Smooth 1 Hz breathing: pulse factor sweeps
                // sin(2πt) rescaled to 0..1.  Lerp the body fill
                // between dim-red and hot-red so it "glows."
                const float pulse = 0.5f + 0.5f *
                    std::sin(m_txPulsePhase * 2.0f * 3.14159265f);
                const int r = static_cast<int>(kBgTxActiveDim().red()   +
                    pulse * (kBgTxActiveHot().red()   - kBgTxActiveDim().red()));
                const int g = static_cast<int>(kBgTxActiveDim().green() +
                    pulse * (kBgTxActiveHot().green() - kBgTxActiveDim().green()));
                const int bl = static_cast<int>(kBgTxActiveDim().blue() +
                    pulse * (kBgTxActiveHot().blue()  - kBgTxActiveDim().blue()));
                bodyBrush = QColor(r, g, bl);
                borderCol = kBorderTxActive();
                textCol   = kTextTxActive();
            }

            p.setBrush(bodyBrush);
            p.setPen(QPen(borderCol, 1.0));
            p.drawRoundedRect(b.rect, kRadius, kRadius);
            p.setPen(textCol);
            p.drawText(b.rect, Qt::AlignCenter,
                       firstEndpoint ? "MIC" : (lastEndpoint ? "TX" : ""));
            continue;
        }

        const bool implemented = isStageImplemented(b.stage);
        const bool bypassed    = isStageBypassed(b.stage);

        // Body.
        p.setBrush(bypassed ? kBgBox() : kBgActive());
        p.setPen(QPen(implemented ? (bypassed ? kBorderIdle() : kBorderActive())
                                  : kBorderGrey(), 1.0));
        p.drawRoundedRect(b.rect, kRadius, kRadius);

        // LED dot (top-right) — green when active, dim when bypassed,
        // absent on unimplemented stages.  Shrunk to fit the compact
        // box size without crowding the label.
        if (implemented) {
            const QPointF led(b.rect.right() - 4, b.rect.top() + 4);
            p.setBrush(bypassed ? kLedBypass() : kLedActive());
            p.setPen(Qt::NoPen);
            p.drawEllipse(led, 1.8, 1.8);
        }

        // Label — unimplemented stages paint in dim text; the "soon"
        // secondary caption no longer fits in a 20 px box, so we drop
        // it and rely on the greyed-out appearance + tooltip instead.
        p.setPen(implemented ? kTextLabel() : kTextDim());
        p.drawText(b.rect, Qt::AlignCenter, stageLabel(b.stage));
    }

    // Drop-position indicator — a thick vertical bar at the midpoint
    // between the two signal-adjacent boxes that sandwich the drop.
    // Sits on the row of the box it's inserting before so it reads as
    // "the dragged stage lands here" no matter which row we're in.
    if (m_dropIndex >= 0 && m_boxes.size() >= 2) {
        const int left  = m_dropIndex;      // signal-order index of box before drop
        const int right = m_dropIndex + 1;  // signal-order index of box after drop
        if (left >= 0 && right < m_boxes.size()) {
            const QRectF lr = m_boxes[left].rect;
            const QRectF rr = m_boxes[right].rect;
            const QPointF mid((lr.center().x() + rr.center().x()) * 0.5,
                              (lr.center().y() + rr.center().y()) * 0.5);
            // Use the "after" box's row so the indicator visually
            // precedes where the new stage will go.
            p.setPen(QPen(kDropIndicator(), 3.0));
            p.drawLine(QPointF(mid.x(), rr.top() - 2),
                       QPointF(mid.x(), rr.bottom() + 2));
        }
    }
}

void ClientChainWidget::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { QWidget::mousePressEvent(ev); return; }
    const int idx = hitTest(ev->position());
    if (idx < 0 || m_boxes[idx].isEndpoint) {
        m_pressIndex = -1;
        return;
    }
    m_pressPos   = ev->position().toPoint();
    m_pressIndex = idx;
    ev->accept();
}

void ClientChainWidget::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_pressIndex < 0 || !(ev->buttons() & Qt::LeftButton)) {
        // Hover → update cursor over interactive boxes.
        const int idx = hitTest(ev->position());
        setCursor((idx > 0 && !m_boxes[idx].isEndpoint)
                      ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(ev);
        return;
    }
    // Distinguish drag from click — require ~6 px of movement.
    if ((ev->position().toPoint() - m_pressPos).manhattanLength() < 6) return;

    const auto& b = m_boxes[m_pressIndex];
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData;
    mime->setData(kMimeFormat,
                  QByteArray::number(static_cast<int>(b.stage)));
    drag->setMimeData(mime);

    // Drag pixmap — snapshot the box so the user sees what they're moving.
    QPixmap pix(b.rect.size().toSize());
    pix.fill(Qt::transparent);
    {
        QPainter pp(&pix);
        pp.setRenderHint(QPainter::Antialiasing, true);
        pp.setBrush(kBgActive());
        pp.setPen(QPen(kBorderActive(), 1.0));
        pp.drawRoundedRect(QRectF(0, 0, pix.width() - 1, pix.height() - 1),
                           kRadius, kRadius);
        QFont f = pp.font();
        f.setPixelSize(9);
        f.setBold(true);
        pp.setFont(f);
        pp.setPen(kTextLabel());
        pp.drawText(pix.rect(), Qt::AlignCenter, stageLabel(b.stage));
    }
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(pix.width() / 2, pix.height() / 2));

    m_pressIndex = -1;
    drag->exec(Qt::MoveAction);
    m_dropIndex = -1;
    update();
}

void ClientChainWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(ev); return; }
    // If m_pressIndex was cleared by mouseMoveEvent (drag started) the
    // release isn't a click — skip.  Otherwise it's a genuine single
    // click; defer the bypass-toggle until the double-click interval
    // elapses so a follow-up double-click can cancel it and open the
    // editor instead.
    if (m_pressIndex < 0) return;
    const int idx = m_pressIndex;
    m_pressIndex = -1;
    if (idx < 0 || idx >= m_boxes.size()) return;
    if (m_boxes[idx].isEndpoint) return;
    m_pendingClickIdx = idx;
    if (m_clickTimer) {
        m_clickTimer->start(clickDiscriminationIntervalMs());
    }
    ev->accept();
}

void ClientChainWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // Double-click = open editor.  Cancel the deferred bypass toggle
    // queued by the preceding single-click release.
    if (ev->button() != Qt::LeftButton) return;
    if (m_clickTimer && m_clickTimer->isActive()) m_clickTimer->stop();
    m_pendingClickIdx = -1;

    const int idx = hitTest(ev->position());
    if (idx < 0 || m_boxes[idx].isEndpoint) return;
    if (!isStageImplemented(m_boxes[idx].stage)) return;
    emit editRequested(m_boxes[idx].stage);
    ev->accept();
}

void ClientChainWidget::toggleStageBypass(int boxIdx)
{
    if (!m_audio) return;
    if (boxIdx < 0 || boxIdx >= m_boxes.size()) return;
    if (m_boxes[boxIdx].isEndpoint) return;
    const auto stage = m_boxes[boxIdx].stage;
    if (!isStageImplemented(stage)) return;

    const bool newEnabled = isStageBypassed(stage);   // was off → turn on
    switch (stage) {
        case AudioEngine::TxChainStage::Eq:
            if (m_audio->clientEqTx()) {
                m_audio->clientEqTx()->setEnabled(newEnabled);
                m_audio->saveClientEqSettings();
            }
            break;
        case AudioEngine::TxChainStage::Comp:
            if (m_audio->clientCompTx()) {
                m_audio->clientCompTx()->setEnabled(newEnabled);
                m_audio->saveClientCompSettings();
            }
            break;
        case AudioEngine::TxChainStage::Gate:
            if (m_audio->clientGateTx()) {
                m_audio->clientGateTx()->setEnabled(newEnabled);
                m_audio->saveClientGateSettings();
            }
            break;
        case AudioEngine::TxChainStage::DeEss:
            if (m_audio->clientDeEssTx()) {
                m_audio->clientDeEssTx()->setEnabled(newEnabled);
                m_audio->saveClientDeEssSettings();
            }
            break;
        case AudioEngine::TxChainStage::Tube:
            if (m_audio->clientTubeTx()) {
                m_audio->clientTubeTx()->setEnabled(newEnabled);
                m_audio->saveClientTubeSettings();
            }
            break;
        case AudioEngine::TxChainStage::Enh:
            if (m_audio->clientPuduTx()) {
                m_audio->clientPuduTx()->setEnabled(newEnabled);
                m_audio->saveClientPuduSettings();
            }
            break;
        case AudioEngine::TxChainStage::Reverb:
            if (m_audio->clientReverbTx()) {
                m_audio->clientReverbTx()->setEnabled(newEnabled);
                m_audio->saveClientReverbSettings();
            }
            break;
        default:
            return;
    }
    emit stageEnabledChanged(stage, newEnabled);
    update();
}

void ClientChainWidget::contextMenuEvent(QContextMenuEvent* ev)
{
    const int idx = hitTest(ev->pos());
    if (idx < 0 || m_boxes[idx].isEndpoint) { QWidget::contextMenuEvent(ev); return; }
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
        const bool newEnabled = bypassed;   // was bypassed → user wants it enabled
        if (stage == AudioEngine::TxChainStage::Eq && m_audio->clientEqTx()) {
            m_audio->clientEqTx()->setEnabled(newEnabled);
            m_audio->saveClientEqSettings();
        } else if (stage == AudioEngine::TxChainStage::Comp && m_audio->clientCompTx()) {
            m_audio->clientCompTx()->setEnabled(newEnabled);
            m_audio->saveClientCompSettings();
        } else if (stage == AudioEngine::TxChainStage::Gate && m_audio->clientGateTx()) {
            m_audio->clientGateTx()->setEnabled(newEnabled);
            m_audio->saveClientGateSettings();
        } else if (stage == AudioEngine::TxChainStage::DeEss && m_audio->clientDeEssTx()) {
            m_audio->clientDeEssTx()->setEnabled(newEnabled);
            m_audio->saveClientDeEssSettings();
        } else if (stage == AudioEngine::TxChainStage::Tube && m_audio->clientTubeTx()) {
            m_audio->clientTubeTx()->setEnabled(newEnabled);
            m_audio->saveClientTubeSettings();
        } else if (stage == AudioEngine::TxChainStage::Enh && m_audio->clientPuduTx()) {
            m_audio->clientPuduTx()->setEnabled(newEnabled);
            m_audio->saveClientPuduSettings();
        } else if (stage == AudioEngine::TxChainStage::Reverb && m_audio->clientReverbTx()) {
            m_audio->clientReverbTx()->setEnabled(newEnabled);
            m_audio->saveClientReverbSettings();
        }
        emit stageEnabledChanged(stage, newEnabled);
        update();
    }
}

void ClientChainWidget::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasFormat(kMimeFormat)) {
        ev->acceptProposedAction();
    }
}

void ClientChainWidget::dragMoveEvent(QDragMoveEvent* ev)
{
    if (!ev->mimeData()->hasFormat(kMimeFormat)) return;
    const int idx = dropInsertIndex(ev->position());
    if (idx != m_dropIndex) { m_dropIndex = idx; update(); }
    ev->acceptProposedAction();
}

void ClientChainWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropIndex = -1;
    update();
}

void ClientChainWidget::dropEvent(QDropEvent* ev)
{
    if (!m_audio || !ev->mimeData()->hasFormat(kMimeFormat)) {
        m_dropIndex = -1;
        update();
        return;
    }
    const auto stage = static_cast<AudioEngine::TxChainStage>(
        ev->mimeData()->data(kMimeFormat).toInt());
    auto stages = m_audio->txChainStages();
    const int from = stages.indexOf(stage);
    if (from < 0) { m_dropIndex = -1; update(); return; }

    int to = dropInsertIndex(ev->position());
    // Account for removal-before-insertion shift.
    stages.removeAt(from);
    if (to > from) --to;
    to = std::clamp(to, 0, static_cast<int>(stages.size()));
    stages.insert(to, stage);

    m_audio->setTxChainStages(stages);
    m_dropIndex = -1;
    rebuildLayout();
    update();
    emit chainReordered();
    ev->acceptProposedAction();
}

void ClientChainWidget::leaveEvent(QEvent*)
{
    setCursor(Qt::ArrowCursor);
}

QSize ClientChainWidget::sizeHint() const
{
    // Minimum-height-only hint; the widget scales box widths to fit
    // whatever horizontal space the parent layout allots (260 px in
    // the applet panel is plenty).
    return QSize(2 * kMarginX + 8 * kBoxWidthMin + 7 * kBoxGapMin,
                 2 * kMarginY + kBoxHeight);
}

} // namespace AetherSDR
