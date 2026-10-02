#pragma once

#include <QChar>
#include <QPainter>
#include <QRect>
#include <QString>

namespace AetherSDR::SliceLabel {

// Slice letter display, AppSettings "SliceLetterDisplay" (read on every call):
//   "Global" (default) - 'A' + globalSliceId.
//   "RadioIndexed"     - the radio's per-client `index_letter`, with the global
//                        slice id as a subscript (#2606).

enum class Mode {
    Global,
    RadioIndexed,
};

Mode currentMode();

// Return the SliceColorManager index that should be used to colour
// this slice's badge / marker for the *current* user.  In Global mode
// returns globalSliceId (today's behaviour); in RadioIndexed mode
// returns the letter's 0-based position ('A' → 0, 'B' → 1, …) so the
// user's per-client letter and badge colour stay paired regardless of
// which physical slot the radio assigned.  See #2606.
int displayColorIndex(int globalSliceId, const QString& radioLetter);

// Render the mode-aware display letter without HTML or Unicode subscript.
// Use this for prose/tooltips that should name the same slice letter the
// visible badge is using.
QString plainText(int globalSliceId, const QString& radioLetter = QString());

// Render the slice label as HTML rich text.  Caller must
// `setTextFormat(Qt::RichText)` on the target QLabel.
//
// In Global mode `radioLetter` is ignored; result is the plain global
// letter.  In RadioIndexed mode the result is `radioLetter<sub>id</sub>`
// (falling back to 'A' + globalSliceId for radioLetter when empty).
QString richText(int globalSliceId, const QString& radioLetter = QString());

// Render the slice label using a single string with a Unicode subscript
// code point (U+2080..U+2089) for the global slice id in RadioIndexed
// mode.  Use this for widgets that don't honour HTML rich text via
// setText() — QToolButton / QPushButton.  In Global mode this is just
// the plain global letter.
QString unicodeForm(int globalSliceId, const QString& radioLetter = QString());

// Painter helper for custom-paint widgets (SpectrumWidget slice markers,
// VfoWidget collapsed badge, etc.).  In Global mode draws just the
// global letter centred in `rect`.  In RadioIndexed mode draws the
// per-client letter with the global slice id as a smaller subscript.
//
// The painter's font is restored before returning.
void drawSliceBadge(QPainter& p, const QRect& rect,
                    int globalSliceId,
                    const QString& radioLetter = QString());

} // namespace AetherSDR::SliceLabel
