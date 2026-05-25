#include "ClientRxChainWidget.h"

#include "InteractionSettings.h"
#include "core/AppSettings.h"
#include "core/ClientComp.h"
#include "core/ClientDeEss.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// Layout constants — match ClientChainWidget so the TX and RX strips
// look like siblings.  When the user toggles between the two tabs in
// the PooDoo applet the box geometry stays put.
constexpr int   kBoxHeight     = 20;
constexpr int   kBoxWidthMin   = 36;
constexpr int   kBoxWidthMax   = 54;
constexpr int   kBoxGapMin     = 6;
constexpr int   kBoxGapPref    = 10;
constexpr int   kMarginX       = 10;
constexpr int   kRowLeftPad    = 16;
constexpr int   kMarginY       = 4;
constexpr int   kRowGap        = 12;
constexpr int   kTurnOffset    = 4;
constexpr int   kArrowTip      = 3;
constexpr qreal kRadius        = 5.0;

inline QColor kBgBox() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor kBgEndpoint() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBgActive() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBorderIdle() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kBorderActive() { return AetherSDR::ThemeManager::instance().color("color.accent.dim"); }
inline QColor kBorderGrey() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
// Status-tile "active" treatment — matches the MIC-ready green from
// the TX widget so the visual language carries across.
inline QColor kBgStatusActive() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kBorderStatusActive() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kTextStatusActive() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kConnector() { return AetherSDR::ThemeManager::instance().color("color.background.1"); }
inline QColor kTextLabel() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
inline QColor kTextDim() { return AetherSDR::ThemeManager::instance().color("color.background.3"); }
inline QColor kDropIndicator() { return AetherSDR::ThemeManager::instance().color("color.accent.dim"); }

// Distinct from the TX chain's mime so a stray drag from one widget
// can't be dropped on the other.
constexpr const char* kMimeFormat = "application/x-aethersdr-rx-chain-stage";

// Short label per RX stage — kept ≤5 chars to fit inside the narrow box.
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

ClientRxChainWidget::ClientRxChainWidget(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(kBoxHeight + 2 * kMarginY);
    setCursor(Qt::ArrowCursor);
    setAcceptDrops(true);
    setMouseTracking(true);   // hover-cursor over interactive tiles

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

void ClientRxChainWidget::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    update();
}

void ClientRxChainWidget::setPcAudioEnabled(bool on)
{
    if (m_pcAudioOn == on) return;
    m_pcAudioOn = on;
    update();
}

void ClientRxChainWidget::setClientDspActive(bool on, const QString& label)
{
    if (m_dspActive == on && m_dspLabel == label) return;
    m_dspActive = on;
    m_dspLabel  = label;
    update();
}

void ClientRxChainWidget::setOutputUnmuted(bool on)
{
    if (m_outputUnmuted == on) return;
    m_outputUnmuted = on;
    update();
}

QSize ClientRxChainWidget::sizeHint() const
{
    return QSize(260, kBoxHeight * 2 + kRowGap + 2 * kMarginY);
}

bool ClientRxChainWidget::isStageImplemented(AudioEngine::RxChainStage s) const
{
    // Phase 1: only EQ has a working DSP core.  Phase 2-5 will flip
    // their entries to true as each stage's class lands.
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

bool ClientRxChainWidget::isStageBypassed(AudioEngine::RxChainStage s) const
{
    if (!m_audio) return true;
    switch (s) {
        case AudioEngine::RxChainStage::Eq:
            return m_audio->clientEqRx() ? !m_audio->clientEqRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::Gate:
            return m_audio->clientGateRx() ? !m_audio->clientGateRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::Comp:
            return m_audio->clientCompRx() ? !m_audio->clientCompRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::Tube:
            return m_audio->clientTubeRx() ? !m_audio->clientTubeRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::Pudu:
            return m_audio->clientPuduRx() ? !m_audio->clientPuduRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::DeEss:
            return m_audio->clientDeEssRx() ? !m_audio->clientDeEssRx()->isEnabled() : true;
        case AudioEngine::RxChainStage::None:
            return true;
    }
    return true;
}

int ClientRxChainWidget::hitTest(const QPointF& pos) const
{
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].rect.contains(pos)) return i;
    }
    return -1;
}

int ClientRxChainWidget::dropInsertIndex(const QPointF& pos) const
{
    // Return an index in the user-stage list where the dragged stage
    // should be inserted.  Stage tiles live at m_boxes[2 .. size-2]
    // (RADIO+DSP at the head, SPEAK at the tail are non-draggable
    // status tiles).  The processor index = signalIdx - 2.
    if (m_boxes.size() < 4) return 0;
    const int nStages = static_cast<int>(m_boxes.size()) - 3;

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

    const QRectF r = m_boxes[nearestSignalIdx].rect;
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

    // Convert nearest signal-order index → user-stage insert index.
    // Status leaders (RADIO=0, DSP=1) clamp to 0; SPEAK trailer
    // clamps to nStages.  Otherwise: stage signal idx i maps to
    // proc idx (i - 2); "before" inserts at that proc idx, "after"
    // inserts at proc idx + 1.
    int procIdx;
    if (nearestSignalIdx <= 1) {
        procIdx = 0;
    } else if (nearestSignalIdx == m_boxes.size() - 1) {
        procIdx = nStages;
    } else {
        const int p = nearestSignalIdx - 2;
        procIdx = beforeInSignal ? p : p + 1;
    }
    return std::clamp(procIdx, 0, nStages);
}

void ClientRxChainWidget::toggleStageBypass(int boxIdx)
{
    if (!m_audio || boxIdx < 0 || boxIdx >= m_boxes.size()) return;
    const auto& box = m_boxes[boxIdx];

    // DSP status tile toggle: client-side NR is exclusive, so a click
    // here means "turn off whichever module is currently on", and a
    // click while everything is off re-enables the last-active module
    // saved by AetherDspWidget::onDspButtonClicked.
    if (box.kind == TileKind::StatusDsp) {
        const bool anyOn = m_audio->nr2Enabled()  || m_audio->nr4Enabled()
                        || m_audio->mnrEnabled()  || m_audio->dfnrEnabled()
                        || m_audio->rn2Enabled()  || m_audio->bnrEnabled();
        if (anyOn) {
            QMetaObject::invokeMethod(m_audio, [audio = m_audio]() {
                if (audio->nr2Enabled())  audio->setNr2Enabled(false);
                if (audio->nr4Enabled())  audio->setNr4Enabled(false);
                if (audio->mnrEnabled())  audio->setMnrEnabled(false);
                if (audio->dfnrEnabled()) audio->setDfnrEnabled(false);
                if (audio->rn2Enabled())  audio->setRn2Enabled(false);
                if (audio->bnrEnabled())  audio->setBnrEnabled(false);
            });
        } else {
            const QString name = AppSettings::instance()
                .value("LastClientNr", "").toString();
            if (name.isEmpty()) return;
            // NR2 enable goes through MainWindow's wisdom prep path —
            // plain audio-thread setter can crash on bad FFTW plans.
            if (name == "NR2") {
                emit nr2EnableWithWisdomRequested();
                return;
            }
            QMetaObject::invokeMethod(m_audio, [audio = m_audio, name]() {
                if (name == "NR4")       audio->setNr4Enabled(true);
                else if (name == "MNR")  audio->setMnrEnabled(true);
                else if (name == "DFNR") audio->setDfnrEnabled(true);
                else if (name == "RN2")  audio->setRn2Enabled(true);
                else if (name == "BNR")  audio->setBnrEnabled(true);
            });
        }
        return;
    }

    if (box.kind != TileKind::Stage) return;
    if (!isStageImplemented(box.stage)) return;

    const bool willEnable = isStageBypassed(box.stage);
    switch (box.stage) {
        case AudioEngine::RxChainStage::Eq:
            if (auto* eq = m_audio->clientEqRx()) {
                eq->setEnabled(willEnable);
                m_audio->saveClientEqSettings();
            }
            break;
        case AudioEngine::RxChainStage::Gate:
            if (auto* g = m_audio->clientGateRx()) {
                g->setEnabled(willEnable);
                m_audio->saveClientGateRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Comp:
            if (auto* c = m_audio->clientCompRx()) {
                c->setEnabled(willEnable);
                m_audio->saveClientCompRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Tube:
            if (auto* t = m_audio->clientTubeRx()) {
                t->setEnabled(willEnable);
                m_audio->saveClientTubeRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::Pudu:
            if (auto* p = m_audio->clientPuduRx()) {
                p->setEnabled(willEnable);
                m_audio->saveClientPuduRxSettings();
            }
            break;
        case AudioEngine::RxChainStage::DeEss:
            if (auto* d = m_audio->clientDeEssRx()) {
                d->setEnabled(willEnable);
                m_audio->saveClientDeEssRxSettings();
            }
            break;
        default:
            break;
    }
    emit stageEnabledChanged(box.stage, willEnable);
    update();
}

void ClientRxChainWidget::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(ev);
        return;
    }
    const int idx = hitTest(ev->position());
    if (idx < 0) {
        m_pressIndex = -1;
        QWidget::mousePressEvent(ev);
        return;
    }
    const auto& box = m_boxes[idx];
    const bool clickableStage = (box.kind == TileKind::Stage
                                 && isStageImplemented(box.stage));
    const bool clickableDsp   = (box.kind == TileKind::StatusDsp);
    if (!clickableStage && !clickableDsp) {
        // RADIO / SPEAK status tiles + unimplemented stages: nothing on click.
        m_pressIndex = -1;
        QWidget::mousePressEvent(ev);
        return;
    }
    // Record press; mouseMoveEvent decides drag-vs-click based on
    // movement distance, mouseReleaseEvent fires the deferred bypass
    // toggle if no drag happened.
    m_pressPos   = ev->position().toPoint();
    m_pressIndex = idx;
    ev->accept();
}

void ClientRxChainWidget::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_pressIndex < 0 || !(ev->buttons() & Qt::LeftButton)) {
        // Hover → cursor reflects whether the tile is interactive.
        const int idx = hitTest(ev->position());
        const bool interactive = (idx >= 0)
            && m_boxes[idx].kind == TileKind::Stage
            && isStageImplemented(m_boxes[idx].stage);
        setCursor(interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
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

    // Drag pixmap snapshot — same active treatment as the painted tile.
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

    // Cancel any pending click — mouseReleaseEvent won't fire for the
    // drag end, but we want to be defensive.
    if (m_clickTimer && m_clickTimer->isActive()) m_clickTimer->stop();
    m_pendingClickIdx = -1;
    m_pressIndex      = -1;
    drag->exec(Qt::MoveAction);
    m_dropIndex = -1;
    update();
}

void ClientRxChainWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(ev);
        return;
    }
    // If m_pressIndex was cleared by mouseMoveEvent (drag started)
    // the release isn't a click.  Otherwise queue a deferred bypass-
    // toggle that a follow-up double-click can cancel.
    if (m_pressIndex < 0) return;
    const int idx = m_pressIndex;
    m_pressIndex = -1;
    if (idx < 0 || idx >= m_boxes.size()) return;
    const auto& box = m_boxes[idx];
    const bool clickableStage = (box.kind == TileKind::Stage
                                 && isStageImplemented(box.stage));
    const bool clickableDsp   = (box.kind == TileKind::StatusDsp);
    if (!clickableStage && !clickableDsp) return;
    m_pendingClickIdx = idx;
    if (m_clickTimer)
        m_clickTimer->start(clickDiscriminationIntervalMs());
}

void ClientRxChainWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // Cancel the pending single-click bypass so a double-click only
    // opens the editor.
    if (m_clickTimer && m_clickTimer->isActive()) m_clickTimer->stop();
    m_pendingClickIdx = -1;

    const int idx = hitTest(ev->position());
    if (idx < 0) { QWidget::mouseDoubleClickEvent(ev); return; }
    const auto& box = m_boxes[idx];
    if (box.kind == TileKind::StatusDsp) {
        emit dspEditRequested();
        ev->accept();
        return;
    }
    if (box.kind != TileKind::Stage || !isStageImplemented(box.stage)) {
        QWidget::mouseDoubleClickEvent(ev);
        return;
    }
    emit editRequested(box.stage);
    ev->accept();
}

void ClientRxChainWidget::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasFormat(kMimeFormat))
        ev->acceptProposedAction();
}

void ClientRxChainWidget::dragMoveEvent(QDragMoveEvent* ev)
{
    if (!ev->mimeData()->hasFormat(kMimeFormat)) return;
    const int idx = dropInsertIndex(ev->position());
    if (idx != m_dropIndex) { m_dropIndex = idx; update(); }
    ev->acceptProposedAction();
}

void ClientRxChainWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropIndex = -1;
    update();
}

void ClientRxChainWidget::dropEvent(QDropEvent* ev)
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
    // Account for removal-before-insertion shift.
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

void ClientRxChainWidget::leaveEvent(QEvent*)
{
    setCursor(Qt::ArrowCursor);
}

void ClientRxChainWidget::rebuildLayout()
{
    m_boxes.clear();
    if (!m_audio) return;

    // Assemble: [RADIO] [DSP] + user RX stages + [SPEAK]
    const auto stages = m_audio->rxChainStages();
    const int totalBoxes = 3 + stages.size();
    if (totalBoxes <= 0) return;

    const int avail = std::max(0, width() - 2 * kMarginX);
    int gap  = kBoxGapPref;
    int boxW = kBoxWidthMax;

    auto computeBoxesPerRow = [&]() {
        if (boxW <= 0) return 1;
        return std::max(1, (avail + gap) / (boxW + gap));
    };
    int boxesPerRow = std::min(totalBoxes, computeBoxesPerRow());

    if (boxesPerRow < 2 && totalBoxes > 1) {
        gap  = kBoxGapMin;
        boxW = kBoxWidthMin;
        boxesPerRow = std::min(totalBoxes, computeBoxesPerRow());
    }

    const int numRows = (totalBoxes + boxesPerRow - 1) / boxesPerRow;
    const qreal yStart = kMarginY;

    auto boxRect = [&](int visualPos, int row) {
        const qreal x = kMarginX + kRowLeftPad + visualPos * (boxW + gap);
        const qreal y = yStart + row * (kBoxHeight + kRowGap);
        return QRectF(x, y, boxW, kBoxHeight);
    };

    auto addBox = [&](TileKind kind, AudioEngine::RxChainStage s, int signalIdx) {
        const int row = signalIdx / boxesPerRow;
        const int posInRow = signalIdx % boxesPerRow;
        const int visualPos = (row & 1)
            ? (boxesPerRow - 1 - posInRow)
            : posInRow;
        BoxRect b;
        b.kind  = kind;
        b.stage = s;
        b.rect  = boxRect(visualPos, row);
        m_boxes.append(b);
    };

    int idx = 0;
    addBox(TileKind::StatusRadio, AudioEngine::RxChainStage::None, idx++);
    addBox(TileKind::StatusDsp,   AudioEngine::RxChainStage::None, idx++);
    for (auto s : stages) addBox(TileKind::Stage, s, idx++);
    addBox(TileKind::StatusSpeak, AudioEngine::RxChainStage::None, idx++);

    const int desiredH = 2 * kMarginY + numRows * kBoxHeight
                         + (numRows - 1) * kRowGap;
    if (height() != desiredH) setFixedHeight(desiredH);
}

void ClientRxChainWidget::paintEvent(QPaintEvent*)
{
    rebuildLayout();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), AetherSDR::ThemeManager::instance().color("color.background.0"));

    if (m_boxes.isEmpty()) return;

    // Connectors — same snake-aware logic as ClientChainWidget.
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

    // Boxes.
    QFont labelFont = p.font();
    labelFont.setPixelSize(9);
    labelFont.setBold(true);
    p.setFont(labelFont);

    for (const auto& b : m_boxes) {
        QString label;
        bool active = false;
        bool implemented = false;

        switch (b.kind) {
            case TileKind::StatusRadio:
                label = "RADIO";
                active = m_pcAudioOn;
                break;
            case TileKind::StatusDsp:
                // Show the active module's name when on, generic "DSP"
                // when off or when the caller didn't supply a label.
                label = (m_dspActive && !m_dspLabel.isEmpty())
                    ? m_dspLabel
                    : QStringLiteral("DSP");
                active = m_dspActive;
                break;
            case TileKind::StatusSpeak:
                label = "SPEAK";
                active = m_outputUnmuted;
                break;
            case TileKind::Stage:
                label = stageLabel(b.stage);
                implemented = isStageImplemented(b.stage);
                break;
        }

        QBrush bodyBrush(kBgEndpoint());
        QColor borderCol = kBorderGrey();
        QColor textCol   = kTextLabel();

        // The DSP tile reads as a stage indicator (which client-side
        // NR is running) more than a binary status, so it adopts the
        // same blue-ring + green-LED-dot styling as the implemented
        // stage tiles.  RADIO and SPEAK keep the green-when-active
        // status-tile look.
        const bool dspStyleAsStage = (b.kind == TileKind::StatusDsp);

        if (b.kind == TileKind::StatusRadio || b.kind == TileKind::StatusSpeak) {
            if (active) {
                bodyBrush = kBgStatusActive();
                borderCol = kBorderStatusActive();
                textCol   = kTextStatusActive();
            }
        } else if (dspStyleAsStage) {
            // Mirror the implemented-stage paint: dim body when
            // inactive, blue ring when active, with a green LED dot
            // top-right echoing the on/off state.
            bodyBrush = active ? kBgActive() : kBgBox();
            borderCol = active ? kBorderActive() : kBorderIdle();
            textCol   = kTextLabel();
        } else if (implemented) {
            const bool bypassed = isStageBypassed(b.stage);
            bodyBrush = bypassed ? kBgBox() : kBgActive();
            borderCol = bypassed ? kBorderIdle() : kBorderActive();
            textCol   = kTextLabel();
        } else {
            // Coming-soon placeholder: greyed body, dim text — reads as
            // "this slot exists but the DSP isn't here yet."
            bodyBrush = kBgBox();
            borderCol = kBorderGrey();
            textCol   = kTextDim();
        }

        p.setBrush(bodyBrush);
        p.setPen(QPen(borderCol, 1.0));
        p.drawRoundedRect(b.rect, kRadius, kRadius);

        // LED dot top-right — green when active, dim when off.  Drawn
        // for implemented stage tiles AND for the DSP status tile so
        // both share the same visual vocabulary.
        const bool drawLed = (b.kind == TileKind::Stage && implemented)
                          || dspStyleAsStage;
        if (drawLed) {
            const bool ledOn = (b.kind == TileKind::Stage)
                ? !isStageBypassed(b.stage)
                : active;
            const QPointF led(b.rect.right() - 4, b.rect.top() + 4);
            p.setBrush(ledOn ? AetherSDR::ThemeManager::instance().color("color.accent.success") : AetherSDR::ThemeManager::instance().color("color.background.1"));
            p.setPen(Qt::NoPen);
            p.drawEllipse(led, 1.8, 1.8);
        }

        p.setPen(textCol);
        p.drawText(b.rect, Qt::AlignCenter, label);
    }

    // Drop-position indicator — vertical bar between the two boxes
    // that sandwich the drop slot.  m_dropIndex is in user-stage
    // coordinates; convert to signal-order m_boxes coordinates by
    // adding the 2-tile leading status block.
    if (m_dropIndex >= 0 && m_boxes.size() >= 4) {
        const int leftSignal  = m_dropIndex + 1;  // last box that stays "before" the drop
        const int rightSignal = m_dropIndex + 2;  // first box that lands "after" the drop
        if (leftSignal >= 0 && rightSignal < m_boxes.size()) {
            const QRectF lr = m_boxes[leftSignal].rect;
            const QRectF rr = m_boxes[rightSignal].rect;
            const QPointF mid((lr.center().x() + rr.center().x()) * 0.5,
                              (lr.center().y() + rr.center().y()) * 0.5);
            p.setPen(QPen(kDropIndicator(), 3.0));
            p.drawLine(QPointF(mid.x(), rr.top() - 2),
                       QPointF(mid.x(), rr.bottom() + 2));
        }
    }
}

} // namespace AetherSDR
