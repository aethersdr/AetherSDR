#pragma once

#include "SupportBundle.h"

#include <QString>

namespace AetherSDR {

// Pre-filled GitHub issue body (#3705): the AI-prompt template's sections (What
// happened / Expected / Steps / Radio / OS) with the SupportBundle snapshot and
// placeholders. A log tail goes in a ```text block, or an "omitted, see support
// bundle" note. The tail is passed through redactPii() here at the render
// boundary even though on-disk lines are already redacted (GHSA-ccrg-j8cp-qhc4);
// radio serial/IP are scrubbed, callsign/model kept. Depends only on redactPii,
// so issue_report_test covers the contract.

// Last N lines of the recent log to include in the issue body.
inline constexpr int kIssueLogTailLines = 100;

// Conservative ceiling on the assembled issues/new URL.  A pre-filled URL
// can only carry text in the query string and browsers/GitHub reject or
// truncate over-long URLs; above this the caller re-renders without the log
// block and delivers the full report via the clipboard/bundle instead.
inline constexpr int kIssueUrlMaxBytes = 8000;

// Build the headed-Markdown issue body.  logTail empty => log block replaced
// by the omission note.  Non-empty => redacted and embedded in a fenced
// block headed "last kIssueLogTailLines lines, secret-redacted".
QString buildIssueReport(const SupportBundle::SystemInfo& sys,
                         const SupportBundle::RadioInfo& radio,
                         const QString& logTail);

} // namespace AetherSDR
