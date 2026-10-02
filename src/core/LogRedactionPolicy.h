#pragma once

#include <QLatin1String>

namespace AetherSDR {

// The single source of truth for which log fields are scrubbed, as
// SettingsCredentialPolicy backs SettingsSanitizer (#5480). redactPii()
// (AsyncLogWriter.cpp) generates every field rule from these tables and one
// shared value grammar, so quoted/JSON/plain spellings are all covered; adding a
// field is one row here plus a case in async_log_writer_test. Limits (e.g. values
// with no keyword, hex dumps) are in docs/log-redaction.md.
namespace LogRedactionPolicy {

// A keyword whose VALUE is scrubbed in `keyword=value`, `keyword: value`,
// `"keyword": value` and Qt-escaped (\"keyword\") spellings. keepPrefixChars
// survive for correlation, emitted only when the value is strictly longer (see
// redactValue). KEYWORD MUST USE NON-CAPTURING GROUPS ONLY: redactField() reads
// fixed group numbers from `\b(keyword)<sep><scheme>(value)`, and a capturing
// group silently shifts them. Write "(?:...)".
struct ValueField {
    const char* keyword;          // regex alternation, matched case-insensitively
    int         keepPrefixChars;
};

// Opaque identifiers. A prefix aids cross-line correlation and, being an
// identifier rather than prose, leaks nothing on its own at four characters.
inline constexpr ValueField kOpaqueValueFields[] = {
    {R"(id_token|access_token|refresh_token|token|authorization|auth)", 4},
    {R"(api[_-]?key|apikey)",                                           4},
    {R"(session[_-]?id|sessionid)",                                     4},
};

// Human-meaningful values. No prefix: four characters of a surname, a grid
// square or a home directory is still the thing itself.
inline constexpr ValueField kSensitiveValueFields[] = {
    {R"(pass(?:word|wd|phrase)?)",                                      0},
    {R"(first_?name|last_?name|full_?name|user_?name|owner)",           0},
    {R"((?:gps[_-]?)?(?:lat(?:itude)?|lon(?:gitude)?))",                0},
    {R"(grid(?:[_-]?square)?|gridsquare|locator|maidenhead)",           0},
    {R"(e?mail(?:[_-]?address)?)",                                      0},
};

// Keywords after which the NEXT token is a peer hostname. Kept separate from
// the value fields because the separator is whitespace, not "=" or ":", and
// because the host is frequently followed by ":port" that stays readable.
//
// The generated rule additionally requires the captured token to LOOK like a
// host - a dotted name, or a single label followed by ":port". These keywords
// are ordinary English too, and without that shape check they ate the next
// word of prose: "disconnected from PipeWire", "resolving multiFLEX conflict".
// "resolving" is deliberately absent for the same reason.
inline constexpr const char* kHostContextKeywords[] = {
    "connecting to", "connected to", "reconnecting to", "disconnected from",
    "connect to", "disconnecting from", "pin for",
};

}  // namespace LogRedactionPolicy
}  // namespace AetherSDR
