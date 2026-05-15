#include "ClientEqParamRow.h"
#include "ClientEqCurveWidget.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>

namespace AetherSDR {

namespace {

QString formatFreq(float hz)
{
    if (hz >= 1000.0f) {
        return QString::number(hz / 1000.0f, 'f', 2) + " kHz";
    }
    return QString::number(static_cast<int>(std::round(hz))) + " Hz";
}

QString formatGain(float db)
{
    return QString::asprintf("%+.1f dB", db);
}

QString formatQ(float q)
{
    return QString::number(q, 'f', 2);
}

} // namespace

// A single column: freq / gain / Q labels stacked vertically. The column
// paints its own outline when selected so we can rely on QLabel's default
// rendering for the text itself.
class ClientEqParamRow::Column : public QWidget {
public:
    Column(int bandIdx, ClientEq* eq, ClientEqParamRow* row,
           QWidget* parent = nullptr)
        : QWidget(parent), m_bandIdx(bandIdx), m_eq(eq), m_row(row)
    {
        // Equal-stretch column — width is set by the parent row splitting
        // its canvas-width across all 8 slots so icon column i aligns
        // with the param column below it.
        setMinimumWidth(70);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        // The strip's open `QWidget { background: #08121d }` rule
        // would otherwise paint a dark fill across the whole column,
        // bleeding upward over the canvas's band-plan strip.  Make
        // the column itself transparent so only the labels show.
        setAttribute(Qt::WA_StyledBackground, false);
        setStyleSheet("background: transparent;");

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(0);

        m_freqLbl = new QLabel;
        m_gainLbl = new QLabel;
        m_qLbl    = new QLabel;
        for (auto* lbl : { m_freqLbl, m_gainLbl, m_qLbl }) {
            lbl->setAlignment(Qt::AlignCenter);
        }
        // Push labels to the bottom of the column.  Without this
        // stretch the QVBoxLayout top-aligns its children, leaving
        // the column's dark background bleeding upward over the
        // canvas's band-plan strip directly above.
        layout->addStretch();
        layout->addWidget(m_freqLbl);
        layout->addWidget(m_gainLbl);
        layout->addWidget(m_qLbl);

        applyStyle();
        refreshValues();
    }

    void setSelected(bool on)
    {
        if (m_selected == on) return;
        m_selected = on;
        applyStyle();
        update();
    }

    void refreshValues()
    {
        if (!m_eq) return;
        const auto bp = m_eq->band(m_bandIdx);
        m_freqLbl->setText(formatFreq(bp.freqHz));
        m_gainLbl->setText(formatGain(bp.gainDb));
        m_qLbl->setText(formatQ(bp.q));
        // Dim disabled bands via label alpha rather than QWidget::setEnabled
        // so the column still accepts clicks (letting the user select /
        // auto-enable it via the canvas or icon).
        m_bandEnabled = bp.enabled;
        applyStyle();
    }

protected:
    void mousePressEvent(QMouseEvent*) override
    {
        if (m_row) emit m_row->bandSelected(m_bandIdx);
    }

    // Right-click → numeric entry menu for this band's Freq / Gain / Q
    // (issue #2655).  Reset action restores this slot's factory preset,
    // matching the canvas's per-handle context menu.
    void contextMenuEvent(QContextMenuEvent* ev) override
    {
        if (!m_eq) { QWidget::contextMenuEvent(ev); return; }
        // Right-click implicitly selects this column too — matches the
        // canvas's right-click-on-handle behaviour.
        if (m_row) emit m_row->bandSelected(m_bandIdx);

        const auto bp = m_eq->band(m_bandIdx);
        const bool isSlope =
            (bp.type == ClientEq::FilterType::LowPass
          || bp.type == ClientEq::FilterType::HighPass);

        QMenu menu(this);
        QAction* freqAct = menu.addAction(tr("Set frequency…"));
        // HP/LP have no gain — match the canvas convention of suppressing
        // the gain handle for slope bands.
        QAction* gainAct = isSlope ? nullptr
                                   : menu.addAction(tr("Set gain (dB)…"));
        QAction* qAct    = menu.addAction(tr("Set Q…"));
        menu.addSeparator();
        QAction* resetAct = menu.addAction(tr("Reset to default"));

        QAction* chosen = menu.exec(ev->globalPos());
        if (!chosen) { ev->accept(); return; }

        // Match the ranges the canvas drag handlers use so numeric and
        // mouse paths can't produce out-of-range values from each other.
        constexpr float kFreqMin = 20.0f,    kFreqMax = 20000.0f;
        constexpr float kGainMin = -18.0f,   kGainMax = 18.0f;
        constexpr float kQMin    = 0.1f,     kQMax    = 18.0f;

        ClientEq::BandParams next = bp;
        bool wrote = false;
        if (chosen == freqAct) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Frequency"),
                tr("Frequency (Hz, %1 to %2):")
                    .arg(kFreqMin, 0, 'f', 0).arg(kFreqMax, 0, 'f', 0),
                static_cast<double>(bp.freqHz),
                static_cast<double>(kFreqMin),
                static_cast<double>(kFreqMax),
                2, &ok);
            if (ok) {
                next.freqHz = std::clamp(static_cast<float>(v),
                                         kFreqMin, kFreqMax);
                next.enabled = true;  // explicit edit = enable
                wrote = true;
            }
        } else if (gainAct && chosen == gainAct) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Gain"),
                tr("Gain (dB, %1 to %2):")
                    .arg(kGainMin, 0, 'f', 0).arg(kGainMax, 0, 'f', 0),
                static_cast<double>(bp.gainDb),
                static_cast<double>(kGainMin),
                static_cast<double>(kGainMax),
                1, &ok);
            if (ok) {
                next.gainDb = std::clamp(static_cast<float>(v),
                                         kGainMin, kGainMax);
                next.enabled = true;
                wrote = true;
            }
        } else if (chosen == qAct) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Q"),
                tr("Q (%1 to %2):")
                    .arg(kQMin, 0, 'f', 2).arg(kQMax, 0, 'f', 1),
                static_cast<double>(bp.q),
                static_cast<double>(kQMin),
                static_cast<double>(kQMax),
                2, &ok);
            if (ok) {
                next.q = std::clamp(static_cast<float>(v), kQMin, kQMax);
                next.enabled = true;
                wrote = true;
            }
        } else if (chosen == resetAct) {
            next = ClientEq::defaultBand(m_bandIdx);
            wrote = true;
        }

        if (wrote) {
            m_eq->setBand(m_bandIdx, next);
            refreshValues();
            if (m_row) emit m_row->bandEdited(m_bandIdx);
        }
        ev->accept();
    }

    void paintEvent(QPaintEvent* ev) override
    {
        QWidget::paintEvent(ev);
        if (!m_selected) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor accent = ClientEqCurveWidget::bandColor(m_bandIdx);
        QPen pen(accent);
        pen.setWidthF(1.2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        // Box around the gain (middle) label — the affordance that mirrors
        // the highlight bar on the curve.
        const QRect gainRect = m_gainLbl->geometry()
                                         .adjusted(-2, -1, 2, 1);
        p.drawRoundedRect(gainRect, 4, 4);
    }

private:
    void applyStyle()
    {
        QColor accent = ClientEqCurveWidget::bandColor(m_bandIdx);
        QColor qCol("#7f93a5");
        if (!m_bandEnabled) {
            // Dim disabled columns so the "available slot" feel is obvious
            // without greying out so much the user can't read the values.
            accent.setAlphaF(0.35f);
            qCol.setAlphaF(0.35f);
        }
        const QString freqStyle = QString(
            "QLabel { color: %1; font-size: 10px; font-weight: bold;"
            " background: transparent; border: none; }")
            .arg(accent.name(QColor::HexArgb));
        const QString gainStyle = QString(
            "QLabel { color: %1; font-size: 12px; font-weight: bold;"
            " background: transparent; border: none; padding: 1px 0px; }")
            .arg(accent.name(QColor::HexArgb));
        const QString qStyle = QString(
            "QLabel { color: %1; font-size: 10px;"
            " background: transparent; border: none; }")
            .arg(qCol.name(QColor::HexArgb));
        m_freqLbl->setStyleSheet(freqStyle);
        m_gainLbl->setStyleSheet(gainStyle);
        m_qLbl->setStyleSheet(qStyle);
    }

    int   m_bandIdx{0};
    bool  m_selected{false};
    bool  m_bandEnabled{true};
    ClientEq* m_eq{nullptr};
    ClientEqParamRow* m_row{nullptr};
    QLabel*  m_freqLbl{nullptr};
    QLabel*  m_gainLbl{nullptr};
    QLabel*  m_qLbl{nullptr};
};

ClientEqParamRow::ClientEqParamRow(QWidget* parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    // Matches ClientEqIconRow spacing so param column i sits directly
    // beneath icon column i (a single visual strip across the editor).
    m_layout->setSpacing(10);
    // Transparent so the strip's wide `QWidget { background: #08121d }`
    // rule doesn't bleed dark fill over the canvas's band-plan strip
    // sitting just above this row.
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet("background: transparent;");
    setFixedHeight(58);
}

void ClientEqParamRow::setEq(ClientEq* eq)
{
    m_eq = eq;
    refresh();
}

void ClientEqParamRow::setSelectedBand(int idx)
{
    if (idx == m_selectedBand) return;
    m_selectedBand = idx;
    for (int i = 0; i < m_layout->count(); ++i) {
        auto* col = dynamic_cast<Column*>(m_layout->itemAt(i)->widget());
        if (col) col->setSelected(false);
    }
    if (idx >= 0) {
        int colIdx = 0;
        for (int i = 0; i < m_layout->count(); ++i) {
            auto* col = dynamic_cast<Column*>(m_layout->itemAt(i)->widget());
            if (!col) continue;
            if (colIdx == idx) col->setSelected(true);
            ++colIdx;
        }
    }
}

void ClientEqParamRow::refresh()
{
    rebuild();
    setSelectedBand(m_selectedBand);
}

void ClientEqParamRow::refreshValues()
{
    for (int i = 0; i < m_layout->count(); ++i) {
        auto* col = dynamic_cast<Column*>(m_layout->itemAt(i)->widget());
        if (col) col->refreshValues();
    }
}

void ClientEqParamRow::rebuild()
{
    while (QLayoutItem* it = m_layout->takeAt(0)) {
        if (auto* w = it->widget()) w->deleteLater();
        delete it;
    }

    if (!m_eq) return;

    // Full-width row with N equal-stretch columns — no side stretches.
    // Column i occupies the same horizontal slot as IconRow icon i so
    // they read as one visual strip.
    const int n = m_eq->activeBandCount();
    for (int i = 0; i < n; ++i) {
        auto* col = new Column(i, m_eq, this, this);
        m_layout->addWidget(col, 1);
    }
}

} // namespace AetherSDR
