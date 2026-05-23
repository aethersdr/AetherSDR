#include "SliceLabel.h"

#include "core/AppSettings.h"

#include <QFont>
#include <QFontMetrics>
#include <algorithm>

namespace MasterSDR::SliceLabel {

namespace {

// Resolve the display letter for a given slice in the active mode.
// Returns the global letter ('A' + id) in Global mode, or the radio's
// per-client letter (falling back to global) in RadioIndexed mode.
QChar resolveLetter(int globalSliceId, const QString& radioLetter,
                    Mode mode)
{
    const QChar fallback = (globalSliceId >= 0 && globalSliceId < 26)
                               ? QChar('A' + globalSliceId)
                               : QChar('?');
    if (mode == Mode::Global) return fallback;
    if (radioLetter.isEmpty()) return fallback;
    return radioLetter.at(0);
}

} // namespace

Mode currentMode()
{
    // Default to "Global" so existing users see no change until they
    // opt into RadioIndexed mode in Settings.
    const QString s = AppSettings::instance()
                          .value("SliceLetterDisplay", "Global").toString();
    return (s == "RadioIndexed") ? Mode::RadioIndexed : Mode::Global;
}

int displayColorIndex(int globalSliceId, const QString& radioLetter)
{
    if (currentMode() == Mode::Global || radioLetter.isEmpty()) {
        return globalSliceId;
    }
    const QChar c = radioLetter.at(0).toUpper();
    if (c >= QChar('A') && c <= QChar('H')) {
        return c.unicode() - 'A';
    }
    return globalSliceId;
}

QString richText(int globalSliceId, const QString& radioLetter)
{
    const Mode mode = currentMode();
    const QChar letter = resolveLetter(globalSliceId, radioLetter, mode);
    if (mode == Mode::Global || globalSliceId < 0) return QString(letter);
    // Subscript is 1-based for human-facing display: global slot 0 →
    // "A₁", slot 1 → "A₂", etc.  Matches how operators count slices.
    return QString("%1<sub>%2</sub>").arg(letter).arg(globalSliceId + 1);
}

QString unicodeForm(int globalSliceId, const QString& radioLetter)
{
    const Mode mode = currentMode();
    const QChar letter = resolveLetter(globalSliceId, radioLetter, mode);
    if (mode == Mode::Global) return QString(letter);
    // U+2080..U+2089 are SUBSCRIPT ZERO..NINE.  1-based (see richText);
    // valid range covers global slots 0..8 (Flex hardware tops out at 8).
    if (globalSliceId < 0 || globalSliceId > 8) return QString(letter);
    return QString(letter) + QChar(0x2080 + globalSliceId + 1);
}

void drawSliceBadge(QPainter& p, const QRect& rect,
                    int globalSliceId, const QString& radioLetter)
{
    const Mode mode = currentMode();
    const QChar letter = resolveLetter(globalSliceId, radioLetter, mode);
    const QFont mainFont = p.font();

    if (mode == Mode::Global || globalSliceId < 0) {
        // Plain centred letter — matches today's rendering.
        p.drawText(rect, Qt::AlignCenter, QString(letter));
        return;
    }

    QFont subFont = mainFont;
    // Subscript ~70% of main font, floored at 7px so it stays readable
    // on the smallest badge sites.  Callers that set their badge font
    // via setPointSize() rather than setPixelSize() get -1 from
    // pixelSize(); fall back to the line height so the subscript still
    // scales with the visible letter.
    int mainPx = mainFont.pixelSize();
    if (mainPx <= 0) mainPx = QFontMetrics(mainFont).height();
    subFont.setPixelSize(std::max(7, (mainPx * 7) / 10));

    const QFontMetrics fmMain(mainFont);
    const QFontMetrics fmSub(subFont);

    const QString letterStr(letter);
    const QString subStr = QString::number(globalSliceId + 1);  // 1-based

    const int mainW = fmMain.horizontalAdvance(letterStr);
    const int subW  = fmSub.horizontalAdvance(subStr);
    const int gap   = 1;
    const int totalW = mainW + gap + subW;

    const int x = rect.x() + (rect.width() - totalW) / 2;
    // Baseline of the main letter sits centred vertically in the rect.
    const int yMainBaseline = rect.y() + (rect.height() + fmMain.ascent()) / 2;
    // Subscript baseline drops slightly below the main letter's baseline.
    const int ySubBaseline  = yMainBaseline + fmSub.descent();

    p.drawText(x, yMainBaseline, letterStr);
    p.setFont(subFont);
    p.drawText(x + mainW + gap, ySubBaseline, subStr);
    p.setFont(mainFont);
}

} // namespace MasterSDR::SliceLabel
