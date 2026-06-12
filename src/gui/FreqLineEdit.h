#pragma once

#include "core/ThemeManager.h"

#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QFont>
#include <QRect>
#include <QString>

// QLineEdit for direct frequency entry. Typed text renders in the seven-segment
// frequency font (font.family.freq, e.g. "DSEG7 Modern") to match the VFO
// readout. A normal prose hint rendered in that segment font, however, paints as
// garbage -- DSEG7 has no glyphs for letters/parentheses/space, so a placeholder
// like "MHz (e.g. 14.225)" turns into corrupted segments the moment the field is
// cleared. (Reported on both the VfoWidget VFO and the RxApplet side applet.)
//
// So instead of Qt's setPlaceholderText() -- which paints in the widget's
// (segment) font -- this widget paints its OWN hint in the UI font
// (font.family.ui) when the field is empty. The hint family is read from the
// token at paint time, so it stays in lockstep with Theme Editor font changes,
// mirroring how the field's stylesheet re-themes via font.family.freq.
class FreqLineEdit : public QLineEdit {
public:
    explicit FreqLineEdit(QWidget* parent = nullptr) : QLineEdit(parent) {}

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
        QFont hintFont(AetherSDR::ThemeManager::instance().value("font.family.ui"));
        hintFont.setPixelSize(m_hintPixelSize);
        painter.setFont(hintFont);
        painter.setPen(palette().placeholderText().color());

        QRect r = contentsRect().marginsRemoved(textMargins());
        r.adjust(4, 0, -4, 0);
        const Qt::Alignment align =
            (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter;
        painter.drawText(r, align, m_hint);
    }

private:
    QString m_hint;
    int m_hintPixelSize{12};
};
