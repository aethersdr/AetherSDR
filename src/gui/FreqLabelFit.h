#pragma once

// Shrink-to-fit helper for the DSEG7 frequency readouts shared by VfoWidget
// (the slice flag) and RxApplet (the RX Controls box).  Both render the
// frequency as a plain QLabel whose font-family/colour come from the theme
// tokens (font.family.freq / font.size.freq) but whose container is a
// fixed-width / fixed-column box.  Without a fit step a frequency string wider
// than the box silently clips its leading (most-significant) digits — the box
// is right-aligned, so the overflow falls off the LEFT edge (#3463/#3515).
//
// IMPORTANT Qt detail (learned the hard way): you CANNOT drive the size with
// QWidget::setFont() when the label also has a stylesheet that sets
// `font-family`.  A stylesheet that names any font property makes Qt recompute
// the widget font from the QSS on every re-polish, discarding setFont() and
// falling back to the default app size.  So the size MUST go through the
// stylesheet too — the callers re-apply their freq label QSS with a literal
// `font-size: <fitFreqPixelSize(...)>px`.

#include <QFont>
#include <QFontMetrics>
#include <QString>

namespace AetherSDR {

// Largest pixel size in [minPx, maxPx] for which `text` rendered bold in
// `family` advances no wider than `availW`.  Caps at maxPx so the digits never
// grow beyond the requested size — it only shrinks to avoid clipping.
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

}  // namespace AetherSDR
