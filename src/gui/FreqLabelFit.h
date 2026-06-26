#pragma once

// Shrink-to-fit helper for the DSEG7 frequency readouts shared by VfoWidget
// (the slice flag) and RxApplet (the RX Controls box).  Both render the
// frequency as a plain QLabel whose font-family/colour come from the theme
// tokens (font.family.freq / font.size.freq) but whose container is a
// fixed-width / fixed-column box.  Without a fit step a frequency string wider
// than the box silently clips its leading (most-significant) digits — the box
// is right-aligned, so the overflow falls off the LEFT edge (#3463/#3515).
//
// Qt detail this relies on: a stylesheet that sets `font-size` OVERRIDES
// QWidget::setFont(), but a stylesheet that sets only `font-family` lets
// setFont() win for the size.  So the callers drop `font-size` from the freq
// label's QSS and drive the (width-fitted) pixel size through setFont() here.

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QString>

namespace AetherSDR {

// Largest pixel size in [minPx, maxPx] for which `text` rendered bold in
// `family` advances no wider than `availW`.  Caps at maxPx so the digits never
// grow beyond the theme's nominal size — it only shrinks to avoid clipping.
// availW <= 0 (not laid out yet) or empty text returns maxPx unchanged.
inline int fitFreqPixelSize(const QString& family, const QString& text,
                            int availW, int maxPx, int minPx = 10)
{
    if (maxPx < minPx)
        maxPx = minPx;
    if (availW <= 0 || text.isEmpty())
        return maxPx;
    QFont f(family);
    f.setBold(true);
    for (int px = maxPx; px > minPx; --px) {
        f.setPixelSize(px);
        if (QFontMetrics(f).horizontalAdvance(text) <= availW)
            return px;
    }
    return minPx;
}

// Fit `label`'s font to its own width so the current text never clips.
// `family` is font.family.freq, `maxPx` is font.size.freq (the theme nominal),
// `pad` reserves border+padding+safety so the glyphs clear the box edge.  The
// label's QSS must set font-family but NOT font-size for this to take effect.
inline void applyFittedFreqFont(QLabel* label, const QString& family,
                                int maxPx, int pad)
{
    if (!label)
        return;
    QFont f(family.isEmpty() ? label->font().family() : family);
    f.setBold(true);
    const int availW = label->width() - pad;
    f.setPixelSize(fitFreqPixelSize(f.family(), label->text(), availW, maxPx));
    label->setFont(f);
}

}  // namespace AetherSDR
