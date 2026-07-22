#pragma once

#include "SupportBundle.h"

#include <QString>

namespace AetherSDR {

// Pre-filled GitHub issue body (#3705).
//
// buildIssueReport() renders the same headed-Markdown sections the AI-prompt
// template uses (What happened / Expected / Steps / Radio / OS) with the
// SupportBundle snapshot filled in and clear placeholders for the user's
// prose.  When a log tail is supplied it is embedded as a fenced ```text
// block; when it is empty a short "omitted — see support bundle" note is
// substituted so the "graceful degrade" path still reads sensibly.
//
// Redaction guarantee: the log tail is passed through redactPii() here, at
// the render boundary, before it can reach the body or the clipboard — the
// same scrub applied to every on-disk log line (GHSA-ccrg-j8cp-qhc4).  The
// on-disk tail is already redacted at capture; this second pass is
// belt-and-suspenders so the guarantee holds no matter where the tail came
// from.  Radio serial/IP are scrubbed too; callsign/model are public and
// left intact.  This TU depends only on redactPii (no RadioModel), so the
// redaction contract is unit-testable in isolation (issue_report_test).

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
