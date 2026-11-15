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

} // namespace Callsigns
} // namespace AetherSDR
