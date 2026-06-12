#pragma once

#include "core/ThemeManager.h"

#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QFont>
#include <QRect>
#include <QString>
#include <QStringList>

// QLineEdit for direct frequency entry. Typed text renders in the seven-segment
// frequency font (font.family.freq, e.g. "DSEG7 Modern") to match the VFO
// readout. A normal prose hint rendered in that segment font, however, paints as
// garbage -- DSEG7 has no glyphs for letters/parentheses/space, so a placeholder
// like "MHz (e.g. 14.225)" turns into corrupted segments the moment the field is
// cleared. (Reported on both the VfoWidget VFO and the RxApplet side applet.)
//
// So instead of Qt's setPlaceholderText() -- which paints in the widget's
// (segment) font -- this widget paints its OWN hint in the UI font
// (font.family.ui) when the field is empty. The hint family is read widget-scoped
// (honoring any themeContainer override in the ancestry, like the field's own
// stylesheet does) at paint time, and the constructor wires ThemeManager's
// themeChanged signal to update(), so the hint stays in lockstep with live Theme
// Editor font changes -- mirroring how the field's stylesheet re-themes via
// font.family.freq.
class FreqLineEdit : public QLineEdit {
public:
    explicit FreqLineEdit(QWidget* parent = nullptr) : QLineEdit(parent) {
        auto& tm = AetherSDR::ThemeManager::instance();
        // Advertise the token we paint with so the Phase 5 theme inspector can
        // answer "what paints this widget?" -- applyStyleSheet's reverse-map only
        // sees stylesheet-driven widgets, never custom paintEvent code.
        tm.declareWidgetTokens(this, {"font.family.ui"});
        // Custom-paint widgets are NOT auto-repainted on themeChanged, so wire it
        // ourselves -- this is what actually keeps the hint in lockstep with live
        // Theme Editor font changes. (4-arg connect to a lambda with `this` as
        // context: no Q_OBJECT/moc needed, stays header-only; auto-disconnects on
        // destruction.)
        connect(&tm, &AetherSDR::ThemeManager::themeChanged, this,
                [this] { update(); });
    }

    // Hint shown (in the UI font) while the field is empty. Deliberately NOT
    // Qt's placeholderText(), which would render in the segment font.
    void setHintText(const QString& text) { m_hint = text; update(); }
    QString hintText() const { return m_hint; }

protected:
    void paintEvent(QPaintEvent* ev) override {
        QLineEdit::paintEvent(ev);
        if (m_hint.isEmpty() || !text().isEmpty())
            return;

        QPainter painter(this);
        QFont hintFont(AetherSDR::ThemeManager::instance().value(this, "font.family.ui"));
        hintFont.setPixelSize(m_hintPixelSize);
        painter.setFont(hintFont);
        painter.setPen(palette().placeholderText().color());

        QRect r = contentsRect().marginsRemoved(textMargins());
        // Keep this 4px inset in sync with the field's stylesheet "padding: 0 4px"
        // (RxApplet/VfoWidget) so the hint starts exactly where typed digits do.
        r.adjust(4, 0, -4, 0);
        const Qt::Alignment align =
            (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter;
        painter.drawText(r, align, m_hint);
    }

private:
    QString m_hint;
    int m_hintPixelSize{12};
};
