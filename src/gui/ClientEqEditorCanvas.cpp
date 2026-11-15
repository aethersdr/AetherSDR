#include "ClientEqEditorCanvas.h"
#include "core/AudioEngine.h"
#include "core/ClientEq.h"

#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QPointF>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr float kHandleHitRadius = 8.0f;
}

ClientEqEditorCanvas::ClientEqEditorCanvas(QWidget* parent)
    : ClientEqCurveWidget(parent)
{
    setMinimumHeight(260);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
}

void ClientEqEditorCanvas::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
}

int ClientEqEditorCanvas::hitTestHandle(const QPointF& pos) const
{
    if (!m_eq) return -1;
    const int n = m_eq->activeBandCount();
    int best = -1;
    float bestDist = kHandleHitRadius * kHandleHitRadius;
    for (int i = 0; i < n; ++i) {
        const auto bp = m_eq->band(i);
        const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                           || bp.type == ClientEq::FilterType::HighPass);
        const float handleDb = isSlope ? 0.0f : bp.gainDb;
        const QPointF c(freqToX(bp.freqHz), dbToY(handleDb));
        const float dx = static_cast<float>(pos.x() - c.x());
        const float dy = static_cast<float>(pos.y() - c.y());
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDist) {
            bestDist = d2;
            best = i;
        }
    }
    return best;
}

ClientEqEditorCanvas::CutoffEdge
ClientEqEditorCanvas::hitTestCutoffEdge(const QPointF& pos) const
{
    constexpr int kHitTol = 5;
    // Don't intercept clicks in the bottom band-plan strip area.
    if (pos.y() > height() - kAudioBandStripPx) return CutoffEdge::None;

    const int lo = filterLowCutHz();
    const int hi = filterHighCutHz();
    int bestDist = kHitTol + 1;
    CutoffEdge best = CutoffEdge::None;
    if (lo > 0) {
        const int lx = static_cast<int>(freqToX(static_cast<float>(lo)));
        const int dx = static_cast<int>(std::abs(pos.x() - lx));
        if (dx <= kHitTol && dx < bestDist) { bestDist = dx; best = CutoffEdge::Low; }
    }
    if (hi > 0) {
        const int hx = static_cast<int>(freqToX(static_cast<float>(hi)));
        const int dx = static_cast<int>(std::abs(pos.x() - hx));
        if (dx <= kHitTol && dx < bestDist) { bestDist = dx; best = CutoffEdge::High; }
    }
    return best;
}

void ClientEqEditorCanvas::persist()
{
    if (m_audio) m_audio->saveClientEqSettings();
    emit bandsChanged();
    update();
}

void ClientEqEditorCanvas::mousePressEvent(QMouseEvent* ev)
{
    if (!m_eq) { QWidget::mousePressEvent(ev); return; }
    if (ev->button() != Qt::LeftButton) { QWidget::mousePressEvent(ev); return; }

    const int idx = hitTestHandle(ev->position());
    if (idx < 0) {
        // Cutoff-edge drag has lower priority than band handles, but
        // higher than clearing selection.  Start a cutoff drag if the
        // click lands on (or within ~5 px of) one of the dashed lines.
        const CutoffEdge edge = hitTestCutoffEdge(ev->position());
        if (edge != CutoffEdge::None) {
            m_draggingCutoff = edge;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        // Clicking empty canvas clears selection so the icon row + param
        // row lose their highlight. Keeps the UI honest about where focus is.
        setSelectedBand(-1);
        QWidget::mousePressEvent(ev);
        return;
    }

    setSelectedBand(idx);

    auto bp = m_eq->band(idx);
    // Auto-enable on interaction: the 8 default slots start disabled so
    // the EQ is audibly transparent until the user grabs a handle.
    if (!bp.enabled) {
        bp.enabled = true;
        m_eq->setBand(idx, bp);
        emit bandsChanged();
    }
    m_draggingBand = idx;
    m_dragShift    = (ev->modifiers() & Qt::ShiftModifier) != 0;
    m_dragStart    = ev->position();
    m_dragStartFreqHz = bp.freqHz;
    m_dragStartGainDb = bp.gainDb;
    m_dragStartQ      = bp.q;
    setCursor(Qt::ClosedHandCursor);
    ev->accept();
}

void ClientEqEditorCanvas::mouseMoveEvent(QMouseEvent* ev)
{
    // Cutoff-edge drag — convert mouseX to Hz, clamp, and emit the
    // updated audio-domain low/high so the editor can write back to
    // TransmitModel (TX) or the active SliceModel (RX).  The display
    // updates locally via setFilterCutoffs() so the line tracks the
    // cursor without waiting for the radio's status echo.
    if (m_draggingCutoff != CutoffEdge::None) {
        constexpr int kMinHz = 20;
        constexpr int kMaxHz = 10000;
        constexpr int kMinSpan = 50;  // FlexLib's filter-width minimum
        const int newHz = static_cast<int>(std::round(
            xToFreq(static_cast<float>(ev->position().x()))));
        int lo = filterLowCutHz();
        int hi = filterHighCutHz();
        if (m_draggingCutoff == CutoffEdge::Low)
            lo = std::clamp(newHz, kMinHz, std::max(kMinHz, hi - kMinSpan));
        else
            hi = std::clamp(newHz, lo + kMinSpan, kMaxHz);
        setFilterCutoffs(lo, hi);  // local visual update
        emit cutoffsDragged(lo, hi);
        ev->accept();
        return;
    }

    if (m_draggingBand < 0 || !m_eq) {
        // Hover state — show size cursor over a cutoff line so the user
        // knows it's draggable, crosshair otherwise.
        const CutoffEdge edge = hitTestCutoffEdge(ev->position());
        setCursor(edge != CutoffEdge::None ? Qt::SizeHorCursor : Qt::CrossCursor);
        QWidget::mouseMoveEvent(ev); return;
    }

    const auto bp = m_eq->band(m_draggingBand);
    ClientEq::BandParams next = bp;
    const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                       || bp.type == ClientEq::FilterType::HighPass);

    if (m_dragShift) {
        // Shift-drag = vertical Q control. Map vertical delta to
        // exponential Q change so a full-height drag spans ~2 decades.
        const float dy = static_cast<float>(m_dragStart.y() - ev->position().y());
        const float hOver2 = std::max(1.0f, static_cast<float>(height()) * 0.5f);
        const float factor = std::pow(10.0f, dy / hOver2);
        next.q = std::clamp(m_dragStartQ * factor, 0.1f, 18.0f);
    } else {
        // Freq is always horizontal. Gain is vertical for peak/shelf.
        // HP/LP have no gain, so their vertical drag controls Q instead
        // — each handle keeps two degrees of freedom, no shift needed.
        next.freqHz = std::clamp(xToFreq(static_cast<float>(ev->position().x())),
                                 20.0f, 20000.0f);
        if (isSlope) {
            const float dy = static_cast<float>(m_dragStart.y() - ev->position().y());
            const float hOver2 = std::max(1.0f, static_cast<float>(height()) * 0.5f);
            const float factor = std::pow(10.0f, dy / hOver2);
            next.q = std::clamp(m_dragStartQ * factor, 0.1f, 18.0f);
        } else {
            next.gainDb = std::clamp(yToDb(static_cast<float>(ev->position().y())),
                                     -18.0f, 18.0f);
        }
    }
    m_eq->setBand(m_draggingBand, next);
    emit bandsChanged();
    update();
    ev->accept();
}

void ClientEqEditorCanvas::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_draggingCutoff != CutoffEdge::None) {
        m_draggingCutoff = CutoffEdge::None;
        setCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingBand < 0) { QWidget::mouseReleaseEvent(ev); return; }
    m_draggingBand = -1;
    m_dragShift = false;
    setCursor(Qt::CrossCursor);
    persist();
    ev->accept();
}

void ClientEqEditorCanvas::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // Double-click is a no-op under the fixed-8-band model. Handle with
    // a regular press so the user still gets selection + drag on the
    // second click rather than a dead gesture.
    QWidget::mouseDoubleClickEvent(ev);
}

void ClientEqEditorCanvas::contextMenuEvent(QContextMenuEvent* ev)
{
    if (!m_eq) { QWidget::contextMenuEvent(ev); return; }

    const int idx = hitTestHandle(ev->pos());
    if (idx < 0) { QWidget::contextMenuEvent(ev); return; }

    const auto bp = m_eq->band(idx);
    QMenu menu(this);

    auto* typeLabel = menu.addAction(
        QString("Band %1 — %2").arg(idx + 1).arg(
            bp.type == ClientEq::FilterType::Peak      ? "Peak"
          : bp.type == ClientEq::FilterType::LowShelf  ? "Low Shelf"
          : bp.type == ClientEq::FilterType::HighShelf ? "High Shelf"
          : bp.type == ClientEq::FilterType::LowPass   ? "Low Pass"
          : "High Pass"));
    typeLabel->setEnabled(false);
    menu.addSeparator();

    auto* peakAct  = menu.addAction("Set type: Peak");
    auto* lsAct    = menu.addAction("Set type: Low Shelf");
    auto* hsAct    = menu.addAction("Set type: High Shelf");
    auto* lpAct    = menu.addAction("Set type: Low Pass");
    auto* hpAct    = menu.addAction("Set type: High Pass");

    // Slope submenu — only meaningful for HP / LP bands.
    const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                       || bp.type == ClientEq::FilterType::HighPass);
    QMenu* slopeMenu = nullptr;
    QList<QAction*> slopeActions;
    if (isSlope) {
        menu.addSeparator();
        slopeMenu = menu.addMenu(QString("Slope  (%1 dB/oct)")
                                     .arg(bp.slopeDbPerOct));
        for (int s : {12, 24, 36, 48}) {
            auto* a = slopeMenu->addAction(QString("%1 dB/oct").arg(s));
            a->setCheckable(true);
            a->setChecked(bp.slopeDbPerOct == s);
            a->setData(s);
            slopeActions.append(a);
        }
    }

    menu.addSeparator();
    auto* enAct    = menu.addAction(bp.enabled ? "Bypass band" : "Enable band");
    auto* resetAct = menu.addAction("Reset to default");

    QAction* chosen = menu.exec(ev->globalPos());
    if (!chosen) return;

    auto change = [&](ClientEq::FilterType t) {
        // Changing the type also enables the band — the user clearly
        // wants it active if they're reshaping it.
        ClientEq::BandParams p = bp; p.type = t; p.enabled = true;
        m_eq->setBand(idx, p);
    };
    if      (chosen == peakAct) change(ClientEq::FilterType::Peak);
    else if (chosen == lsAct)   change(ClientEq::FilterType::LowShelf);
    else if (chosen == hsAct)   change(ClientEq::FilterType::HighShelf);
    else if (chosen == lpAct)   change(ClientEq::FilterType::LowPass);
    else if (chosen == hpAct)   change(ClientEq::FilterType::HighPass);
    else if (chosen == enAct) {
        ClientEq::BandParams p = bp; p.enabled = !bp.enabled; m_eq->setBand(idx, p);
    }
    else if (chosen == resetAct) {
        // Restore this slot's factory preset — type, freq, Q all reset;
        // band is left disabled so the row returns to the quiet starting
        // state (matches the "disabled default" feel of fresh launch).
        m_eq->setBand(idx, ClientEq::defaultBand(idx));
    }
    else if (slopeActions.contains(chosen)) {
        ClientEq::BandParams p = bp;
        p.slopeDbPerOct = chosen->data().toInt();
        p.enabled = true;  // explicit edit = enable
        m_eq->setBand(idx, p);
    }
    persist();
    ev->accept();
}

} // namespace AetherSDR
