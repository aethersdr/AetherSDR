#include "ClientEqIconRow.h"
#include "ClientEqCurveWidget.h"
#include "core/AudioEngine.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace AetherSDR {

// A small custom-painted button drawing one filter's curve shape. The
// shape is derived from the filter type; colour comes from the band's
// palette slot. Click cycles type.
class ClientEqIconRow::IconButton : public QWidget {
public:
    IconButton(int bandIdx, ClientEq* eq, AudioEngine* audio,
               ClientEqIconRow* row, QWidget* parent = nullptr)
        : QWidget(parent), m_bandIdx(bandIdx),
          m_eq(eq), m_audio(audio), m_row(row)
    {
        // Fixed height, stretchable width — each of the 8 row columns
        // gets equal share of the canvas width so icons align with the
        // bottom param columns.
        setFixedHeight(52);
        setMinimumWidth(40);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setSelected(bool on) { if (m_selected != on) { m_selected = on; update(); } }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        if (!m_eq) return;
        const auto bp = m_eq->band(m_bandIdx);
        const QRect r = rect();

        // Background — darker pill, brightened when selected.
        QColor bg = m_selected ? QColor("#1a2e42") : QColor("#0e1b28");
        p.setPen(QPen(m_selected ? ClientEqCurveWidget::bandColor(m_bandIdx)
                                 : QColor("#243a4e"), 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4.0, 4.0);

        // Draw the filter-shape glyph in the band's palette colour.
        QColor accent = ClientEqCurveWidget::bandColor(m_bandIdx);
        if (!bp.enabled) accent.setAlphaF(0.35f);
        QPen pen(accent);
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        const QRectF glyph = QRectF(r).adjusted(5, 5, -5, -5);
        const float gx = glyph.x();
        const float gy = glyph.y();
        const float gw = glyph.width();
        const float gh = glyph.height();
        const float midY = gy + gh * 0.5f;

        QPainterPath path;
        switch (bp.type) {
        case ClientEq::FilterType::Peak: {
            // Small bell centered.
            path.moveTo(gx, midY);
            path.cubicTo(gx + gw * 0.30f, midY,
                         gx + gw * 0.40f, gy + gh * 0.15f,
                         gx + gw * 0.50f, gy + gh * 0.15f);
            path.cubicTo(gx + gw * 0.60f, gy + gh * 0.15f,
                         gx + gw * 0.70f, midY,
                         gx + gw,        midY);
            break;
        }
        case ClientEq::FilterType::LowShelf: {
            // Low end raised, flat on right.
            path.moveTo(gx,            gy + gh * 0.18f);
            path.lineTo(gx + gw * 0.30f, gy + gh * 0.18f);
            path.cubicTo(gx + gw * 0.45f, gy + gh * 0.18f,
                         gx + gw * 0.45f, midY,
                         gx + gw * 0.60f, midY);
            path.lineTo(gx + gw,        midY);
            break;
        }
        case ClientEq::FilterType::HighShelf: {
            // Flat on left, high end raised.
            path.moveTo(gx,            midY);
            path.lineTo(gx + gw * 0.40f, midY);
            path.cubicTo(gx + gw * 0.55f, midY,
                         gx + gw * 0.55f, gy + gh * 0.18f,
                         gx + gw * 0.70f, gy + gh * 0.18f);
            path.lineTo(gx + gw,        gy + gh * 0.18f);
            break;
        }
        case ClientEq::FilterType::LowPass: {
            // Flat passband on the left at the shelf-top level, falls on
            // the right.  Start-point height matches the HS icon's
            // end-point so the LP+HS pair reads as a continuous curve
            // across adjacent slots.
            path.moveTo(gx,            gy + gh * 0.18f);
            path.lineTo(gx + gw * 0.40f, gy + gh * 0.18f);
            path.cubicTo(gx + gw * 0.60f, gy + gh * 0.18f,
                         gx + gw * 0.70f, gy + gh * 0.85f,
                         gx + gw,        gy + gh * 0.85f);
            break;
        }
        case ClientEq::FilterType::HighPass: {
            // Rises on the left, flat passband on the right at the shelf-
            // top level.  End-point matches the LS icon's start-point so
            // the HP+LS pair reads as a continuous curve across adjacent
            // slots.
            path.moveTo(gx,            gy + gh * 0.85f);
            path.cubicTo(gx + gw * 0.30f, gy + gh * 0.85f,
                         gx + gw * 0.40f, gy + gh * 0.18f,
                         gx + gw * 0.60f, gy + gh * 0.18f);
            path.lineTo(gx + gw,        gy + gh * 0.18f);
            break;
        }
        }
        p.drawPath(path);
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        if (!m_eq) return;
        if (ev->button() == Qt::LeftButton) {
            auto bp = m_eq->band(m_bandIdx);
            // Cycle: Peak -> LS -> HS -> LP -> HP -> Peak.
            // Shift reverses.
            const int kTypes = 5;
            int t = static_cast<int>(bp.type);
            t += (ev->modifiers() & Qt::ShiftModifier) ? -1 : 1;
            t = (t % kTypes + kTypes) % kTypes;
            bp.type = static_cast<ClientEq::FilterType>(t);
            // Clicking an icon is an explicit interaction — auto-enable
            // the band so the default disabled layout activates in place.
            bp.enabled = true;
            m_eq->setBand(m_bandIdx, bp);
            if (m_audio) m_audio->saveClientEqSettings();
            update();
        }
        if (m_row) emit m_row->bandSelected(m_bandIdx);
    }

private:
    int          m_bandIdx{0};
    ClientEq*    m_eq{nullptr};
    AudioEngine* m_audio{nullptr};
    ClientEqIconRow* m_row{nullptr};
    bool         m_selected{false};
};

ClientEqIconRow::ClientEqIconRow(QWidget* parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    // 10 px gap between icon cells for visual breathing room.  The
    // ClientEqParamRow uses the same spacing so column i in both rows
    // occupies the same horizontal slot.
    m_layout->setSpacing(10);
    setFixedHeight(60);
}

void ClientEqIconRow::setEq(ClientEq* eq)
{
    m_eq = eq;
    refresh();
}

void ClientEqIconRow::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    refresh();
}

void ClientEqIconRow::setSelectedBand(int idx)
{
    if (idx == m_selectedBand) return;
    m_selectedBand = idx;
    for (int i = 0; i < m_layout->count(); ++i) {
        auto* btn = dynamic_cast<IconButton*>(m_layout->itemAt(i)->widget());
        if (btn) btn->setSelected(false);
    }
    // Simple linear scan — rebuild tracks order.
    if (idx >= 0) {
        for (int i = 0; i < m_layout->count(); ++i) {
            auto* btn = dynamic_cast<IconButton*>(m_layout->itemAt(i)->widget());
            if (!btn) continue;
            if (i == idx) btn->setSelected(true);
        }
    }
}

void ClientEqIconRow::refresh()
{
    rebuild();
    setSelectedBand(m_selectedBand);
}

void ClientEqIconRow::rebuild()
{
    // Clear existing items
    while (QLayoutItem* it = m_layout->takeAt(0)) {
        if (auto* w = it->widget()) w->deleteLater();
        delete it;
    }

    if (!m_eq) return;

    const int n = m_eq->activeBandCount();
    // Fill the full row width with n equal-stretch cells — no leading or
    // trailing stretch — so icon column i aligns with param column i in
    // the ClientEqParamRow below the canvas.
    for (int i = 0; i < n; ++i) {
        auto* btn = new IconButton(i, m_eq, m_audio, this, this);
        m_layout->addWidget(btn, 1);
    }
}

} // namespace AetherSDR
