#pragma once

#include <QChar>
#include <QPainter>
#include <QRect>
#include <QString>

namespace MasterSDR::SliceLabel {

// Two display modes for slice letters, selected by AppSettings key
// "SliceLetterDisplay":
//
//   "Global"       (default) — show 'A' + globalSliceId.  This is how
//                  MasterSDR has always rendered slice letters and gives
//                  Multi-Flex users immediate awareness of which global
//                  slot they're on.
//   "RadioIndexed" — show the radio-provided per-client letter (from the
//                  slice's `index_letter` status field), with the global
//                  slice id as a subscript so slot awareness survives the
//                  SmartSDR-style per-client lettering.  See #2606.
//
// All three helpers below consult this setting on every call so the
// runtime view follows the user flipping it without restart.

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

} // namespace MasterSDR::SliceLabel
