#pragma once

#include <QLatin1String>

namespace AetherSDR {

// THE single source of truth for "which log fields get scrubbed", mirroring
// how SettingsCredentialPolicy backs SettingsSanitizer (#5480). redactPii()
// in AsyncLogWriter.cpp generates its patterns from these tables, so adding a
// field is one row here plus one case in async_log_writer_test.
//
// WHY A TABLE AND NOT MORE REGEX LITERALS. The redactor grew one hand-written
// pattern at a time as log sites were added, and the patterns drifted apart:
// the name and coordinate rules learned to match quoted and JSON-shaped values
// while the token rule never did, so the same value was scrubbed in one
// spelling and survived in another. Generating every field rule from one
// shared value grammar removes that class of gap by construction.
//
// WHAT THIS DOES NOT COVER is as important as what it does; see
// docs/log-redaction.md for the limitations reviewers must check a new log
// site against. In particular a keyword table cannot recognise a value that
// carries no keyword — raw binary or hex packet dumps are outside it.
namespace LogRedactionPolicy {

// A keyword whose VALUE is scrubbed wherever it appears as `keyword=value`,
// `keyword: value`, `"keyword": value`, or their Qt-escaped (\"keyword\")
// spellings.
//
// keepPrefixChars: how many leading characters of the value survive, so a
// reader can still correlate the same value across lines without recovering
// it. The prefix is emitted ONLY when the value is strictly longer than it
// (see redactValue in AsyncLogWriter.cpp) — otherwise a short value would be
// "redacted" to itself, which is how a 4-character value used to pass through
// intact.
//
// KEYWORD MUST USE NON-CAPTURING GROUPS ONLY. redactField() assembles
// `\b(keyword)<sep><scheme>(value)` and reads fixed group numbers; a stray
// capturing "(...)" in a keyword shifts them, and the failure is silent —
// the rule still matches, it just redacts the wrong span. Write "(?:...)".
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
