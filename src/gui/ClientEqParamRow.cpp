#include "ClientEqParamRow.h"
#include "ClientEqCurveWidget.h"

#include <QDoubleValidator>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

namespace MasterSDR {

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

// Parse a user-typed number that may carry units ("1.5 kHz", "−3 dB",
// "0.707").  Returns true on a successful parse via either the current
// locale's decimal convention or a fallback that strips non-numeric
// characters and re-parses in the C locale.
bool parseValue(const QString& raw, double& out)
{
    bool ok = false;
    out = QLocale().toDouble(raw.trimmed(), &ok);
    if (ok) return true;
    QString cleaned;
    cleaned.reserve(raw.size());
    for (QChar c : raw) {
        if (c.isDigit() || c == QChar('.') || c == QChar('-')
            || c == QChar('+') || c == QChar('e') || c == QChar('E'))
            cleaned.append(c);
    }
    out = cleaned.toDouble(&ok);
    return ok;
}

// Match the ranges the curve drag handlers use so numeric and mouse
// paths can't produce values that disagree.
constexpr float kFreqMin = 20.0f,    kFreqMax = 20000.0f;
constexpr float kGainMin = -18.0f,   kGainMax = 18.0f;
constexpr float kQMin    = 0.1f,     kQMax    = 18.0f;

} // namespace

// A single column: freq / gain / Q editable line edits stacked vertically.
// Each field is a QLineEdit styled to look like a label until focused —
// clicking a value opens it for typing, Enter or focus-out commits.  The
// column paints its own outline when selected so we can rely on
// QLineEdit's default rendering for the text itself.
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

        m_freqEdit = new QLineEdit;
        m_gainEdit = new QLineEdit;
        m_qEdit    = new QLineEdit;
        for (auto* le : { m_freqEdit, m_gainEdit, m_qEdit }) {
            le->setAlignment(Qt::AlignCenter);
            le->setFrame(false);
            le->installEventFilter(this);
        }

        // Push edits to the bottom of the column.  Without this stretch
        // the QVBoxLayout top-aligns its children, leaving the column's
        // dark background bleeding upward over the canvas's band-plan
        // strip directly above.
        layout->addStretch();
        layout->addWidget(m_freqEdit);
        layout->addWidget(m_gainEdit);
        layout->addWidget(m_qEdit);

        connect(m_freqEdit, &QLineEdit::returnPressed, this,
                [this] { commit(Field::Freq); m_freqEdit->clearFocus(); });
        connect(m_gainEdit, &QLineEdit::returnPressed, this,
                [this] { commit(Field::Gain); m_gainEdit->clearFocus(); });
        connect(m_qEdit, &QLineEdit::returnPressed, this,
                [this] { commit(Field::Q); m_qEdit->clearFocus(); });

        connect(m_freqEdit, &QLineEdit::editingFinished, this,
                [this] { commit(Field::Freq); });
        connect(m_gainEdit, &QLineEdit::editingFinished, this,
                [this] { commit(Field::Gain); });
        connect(m_qEdit, &QLineEdit::editingFinished, this,
                [this] { commit(Field::Q); });

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
        // Don't fight an active edit — leave the focused field alone.
        auto safeSet = [](QLineEdit* le, const QString& text) {
            if (le->hasFocus()) return;
            QSignalBlocker b(le);
            le->setText(text);
        };
        safeSet(m_freqEdit, formatFreq(bp.freqHz));
        safeSet(m_gainEdit, formatGain(bp.gainDb));
        safeSet(m_qEdit,    formatQ(bp.q));
        m_bandEnabled = bp.enabled;
        // HP/LP slope bands have no gain — fade the gain edit and make
        // it read-only so it can't be typed into.
        const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                           || bp.type == ClientEq::FilterType::HighPass);
        m_gainEdit->setReadOnly(isSlope);
        m_gainEdit->setEnabled(!isSlope);
        applyStyle();
    }

protected:
    void mousePressEvent(QMouseEvent*) override
    {
        // Clicks on the column background (between line edits) select
        // the band.  Clicks on the line edits themselves route through
        // eventFilter's FocusIn handler below for the same effect.
        if (m_row) emit m_row->bandSelected(m_bandIdx);
    }

    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        auto* le = qobject_cast<QLineEdit*>(obj);
        if (!le) return QWidget::eventFilter(obj, ev);

        if (ev->type() == QEvent::FocusIn) {
            // Selecting a value focuses the field AND selects this band
            // (matches what a column-background click does, so the
            // operator-experience is the same regardless of where they
            // click).
            if (m_row) emit m_row->bandSelected(m_bandIdx);
            // Replace the formatted display with the bare number so the
            // user types just digits — no need to retype "kHz" / "dB".
            QSignalBlocker b(le);
            if (m_eq) {
                const auto bp = m_eq->band(m_bandIdx);
                if (le == m_freqEdit)
                    le->setText(QString::number(bp.freqHz, 'f', 2));
                else if (le == m_gainEdit)
                    le->setText(QString::number(bp.gainDb, 'f', 1));
                else if (le == m_qEdit)
                    le->setText(QString::number(bp.q, 'f', 2));
            }
            le->selectAll();
        } else if (ev->type() == QEvent::FocusOut) {
            // Restore the formatted display.  commit() may have already
            // fired via editingFinished — refreshValues() is a no-op for
            // the still-focused field, so we call it here for both the
            // commit and the cancel paths.
            refreshValues();
        } else if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                QSignalBlocker b(le);   // suppress editingFinished commit
                refreshValues();
                le->clearFocus();
                return true;
            }
        } else if (ev->type() == QEvent::Wheel) {
            // Forward wheel scroll to the column (currently a no-op,
            // but reserves the gesture for the parent / canvas instead
            // of letting QLineEdit eat it).
            return false;
        }
        return QWidget::eventFilter(obj, ev);
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
        // Box around the gain (middle) edit — the affordance that mirrors
        // the highlight bar on the curve.
        const QRect gainRect = m_gainEdit->geometry()
                                          .adjusted(-2, -1, 2, 1);
        p.drawRoundedRect(gainRect, 4, 4);
    }

private:
    enum class Field { Freq, Gain, Q };

    void commit(Field field)
    {
        if (!m_eq) return;
        static thread_local bool s_committing = false;
        if (s_committing) return;  // re-entry guard: returnPressed + editingFinished
        s_committing = true;

        QLineEdit* le = (field == Field::Freq) ? m_freqEdit
                       : (field == Field::Gain) ? m_gainEdit
                                                : m_qEdit;
        const auto bp = m_eq->band(m_bandIdx);
        const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                           || bp.type == ClientEq::FilterType::HighPass);
        // HP/LP have no gain — silently ignore Gain commits on slope
        // bands (the field is also disabled, but defend in depth).
        if (field == Field::Gain && isSlope) {
            s_committing = false;
            return;
        }

        double v = 0.0;
        if (!parseValue(le->text(), v)) {
            // Bad parse — revert.
            refreshValues();
            s_committing = false;
            return;
        }

        ClientEq::BandParams next = bp;
        bool wrote = false;
        if (field == Field::Freq) {
            // Accept "1.5" as kHz when units present; otherwise treat as
            // Hz.  Simple heuristic: if the raw text contains 'k' (any
            // case) interpret as kHz.
            if (le->text().contains(QChar('k'), Qt::CaseInsensitive))
                v *= 1000.0;
            next.freqHz = std::clamp(static_cast<float>(v), kFreqMin, kFreqMax);
            next.enabled = true;
            wrote = true;
        } else if (field == Field::Gain) {
            next.gainDb = std::clamp(static_cast<float>(v), kGainMin, kGainMax);
            next.enabled = true;
            wrote = true;
        } else {  // Q
            next.q = std::clamp(static_cast<float>(v), kQMin, kQMax);
            next.enabled = true;
            wrote = true;
        }

        if (wrote) {
            m_eq->setBand(m_bandIdx, next);
            refreshValues();
            if (m_row) emit m_row->bandEdited(m_bandIdx);
        }
        s_committing = false;
    }

    void applyStyle()
    {
        QColor accent = ClientEqCurveWidget::bandColor(m_bandIdx);
        QColor qCol("#7f93a5");
        if (!m_bandEnabled) {
            accent.setAlphaF(0.35f);
            qCol.setAlphaF(0.35f);
        }
        const QString accentName = accent.name(QColor::HexArgb);
        const QString qColName   = qCol.name(QColor::HexArgb);
        // Borderless line edits that look exactly like the previous
        // QLabels until focused; subtle dark inset + cyan border on
        // focus to indicate edit mode (matches ClientCompKnob).
        const QString freqStyle = QString(
            "QLineEdit { color: %1; font-size: 10px; font-weight: bold;"
            " background: transparent; border: 1px solid transparent;"
            " border-radius: 2px; padding: 0;"
            " selection-background-color: #0070c0; }"
            "QLineEdit:focus { background: #0a0a18; border: 1px solid #00b4d8; }"
            ).arg(accentName);
        const QString gainStyle = QString(
            "QLineEdit { color: %1; font-size: 12px; font-weight: bold;"
            " background: transparent; border: 1px solid transparent;"
            " border-radius: 2px; padding: 1px 0;"
            " selection-background-color: #0070c0; }"
            "QLineEdit:focus { background: #0a0a18; border: 1px solid #00b4d8; }"
            "QLineEdit:read-only { color: %2; }"
            ).arg(accentName, accentName);
        const QString qStyle = QString(
            "QLineEdit { color: %1; font-size: 10px;"
            " background: transparent; border: 1px solid transparent;"
            " border-radius: 2px; padding: 0;"
            " selection-background-color: #0070c0; }"
            "QLineEdit:focus { background: #0a0a18; border: 1px solid #00b4d8; }"
            ).arg(qColName);
        m_freqEdit->setStyleSheet(freqStyle);
        m_gainEdit->setStyleSheet(gainStyle);
        m_qEdit->setStyleSheet(qStyle);
    }

    int   m_bandIdx{0};
    bool  m_selected{false};
    bool  m_bandEnabled{true};
    ClientEq* m_eq{nullptr};
    ClientEqParamRow* m_row{nullptr};
    QLineEdit* m_freqEdit{nullptr};
    QLineEdit* m_gainEdit{nullptr};
    QLineEdit* m_qEdit{nullptr};
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

} // namespace MasterSDR
