#pragma once

#include <QRegularExpression>
#include <QString>

namespace AetherSDR {
namespace Callsigns {

// Amateur callsign shape: optional prefix ("VP2E/"), 1-3 char prefix with at
// least one letter, digit, suffix letters, optional portable designator
// ("/P", "/7", "/QRP").  Deliberately permissive — validation gates a lookup,
// it doesn't police licensing.  The pattern requires the base call to end in
// a letter, which rejects most CW garble ("5NN", "73", "599").
inline const QRegularExpression& regex()
{
    static const QRegularExpression re(
        QStringLiteral("(?:[A-Z0-9]{1,4}/)?"                 // DX prefix
                       "(?:[A-Z]{1,2}|[A-Z][0-9]|[0-9][A-Z])" // ITU prefix
                       "[0-9][A-Z0-9]{0,3}[A-Z]"             // digit + suffix
                       "(?:/[A-Z0-9]{1,4})?"));              // portable suffix
    return re;
}

inline bool isLikelyCallsign(const QString& text)
{
    const QString s = text.trimmed().toUpper();
    const QRegularExpressionMatch m = regex().match(s);
    return m.hasMatch() && m.capturedStart() == 0 && m.capturedLength() == s.size();
}

// Canonical cache key / lookup form: uppercase, no whitespace.
inline QString normalized(const QString& call)
{
    return call.trimmed().toUpper();
}

// The callsign under a text cursor, or empty. `cursor` is a position between
// characters, as QTextCursor::position() reports it; the word runs to the
// whitespace on either side, so "K1ABC/P" is one word. Punctuation around the
// word ("K1ABC?") is not part of the call.
inline QString callsignAt(const QString& text, qsizetype cursor)
{
    if (cursor < 0 || cursor > text.size()) { return {}; }
    qsizetype begin = cursor;
    qsizetype end = cursor;
    while (begin > 0 && !text.at(begin - 1).isSpace()) { --begin; }
    while (end < text.size() && !text.at(end).isSpace()) { ++end; }
    const auto partOfCall = [](QChar ch) { return ch.isLetterOrNumber() || ch == QLatin1Char('/'); };
    while (begin < end && !partOfCall(text.at(begin))) { ++begin; }
    while (end > begin && !partOfCall(text.at(end - 1))) { --end; }
    const QString word = normalized(text.mid(begin, end - begin));
    return isLikelyCallsign(word) ? word : QString();
}

} // namespace Callsigns
} // namespace AetherSDR
