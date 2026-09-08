# Log redaction — what `redactPii()` does and does not scrub

`AsyncLogWriter::formatLine()` passes every log line through `redactPii()`
before it reaches disk or the mirrored stderr stream, so a `qCDebug`/`qCInfo`
call does not have to redact at the call site. This page is the reference for
checking a **new** log site against that guarantee. It exists because the
guarantee is narrower than "logs are safe", and reviewers were reasonably
reading a passing redactor unit test as if it meant the latter.

The rule tables live in [`src/core/LogRedactionPolicy.h`](../src/core/LogRedactionPolicy.h);
adding a field is one row there plus one case in `tests/async_log_writer_test.cpp`.

## Covered

| Shape | Result |
|---|---|
| IPv4 address | `*.*.*. <last octet>` |
| IPv6 literal — compressed, bracketed, `%scope`, IPv4-mapped | `[v6-redacted]` |
| MAC address | `**:**:**:**:**:<last octet>` |
| Radio serial `nnnn-nnnn-nnnn-nnnn` | `****-****-****-<last group>` |
| Home directory prefix (`/home/x`, `/Users/x`, `C:\Users\x`, both slash styles) | `~` |
| Email address, in prose or as a field value | `***REDACTED***` |
| `token` / `id_token` / `access_token` / `refresh_token` / `authorization` / `auth` / `api_key` / `session_id` | first 4 characters kept, rest redacted |
| `pass` / `password` / `passwd` / `passphrase` | fully redacted |
| `first_name` / `last_name` / `full_name` / `user_name` / `owner`, and a **quoted** `user "<name>"` | fully redacted |
| `lat` / `latitude` / `lon` / `longitude`, and a `location=`/`gps=` coordinate pair | fully redacted |
| `grid` / `grid_square` / `locator` / `maidenhead`, keyword or whitespace form | fully redacted |
| Hostname after `connecting to`, `connected to`, `reconnecting to`, `disconnected from`, `connect to`, `disconnecting from`, `pin for` — **only when the token looks like a host** (dotted name, or label followed by `:port`) | fully redacted, `:port` preserved |
| URL authority (`scheme://user:pass@host`) | fully redacted, path preserved |

Each of these is matched in bare, single-quoted, double-quoted, JSON and
Qt-escaped (`\"value\"`) spellings, because they all share one value grammar.
That grammar is escape-aware, and the two quoted spellings escape differently:
in ordinary text a value runs past `\"` to its real closing quote, while in
QDebug's spelling `\"` *is* the delimiter. Both are pinned by tests.

Redaction is **idempotent**: re-running it over an already-scrubbed line is a
no-op, which is what lets the support bundle re-scrub older logs on the way
out. The marker `***REDACTED***` is explicitly excluded from the value grammar
to keep that true — without it a second pass consumed its own marker and a
short value became `token=***R***REDACTED***`.

Redaction runs on **every** log line, so the generated patterns are compiled
once and cached. `testRedactionThroughput` guards that: building them per call
cost roughly 1 ms a line, which `SupportBundle`'s per-line export loop then
inherited synchronously.

## Deliberately NOT redacted

These are diagnostic and are not the operator's to hide. Removing them has
broken real triage before, so do not "fix" them:

- Callsign — FCC public record, and the primary way a report is identified
- Radio model, firmware and software version (including 4-part build numbers)
- Port numbers, slice/stream ids, frequencies, modes
- Identifiers that merely end in a keyword, e.g. `keytoken=`
- **C++ qualified names** — `WanConnection::sendCommand`, `std::vector`. 48 log
  sites stream `Class::method` as the literal start of their message, and an
  under-anchored IPv6 rule rewrote every one of them. The compressed-address
  rule requires a hextet adjacent to the `::`.
- **Ordinary prose after a connection keyword** — "disconnected from PipeWire",
  "resolving multiFLEX conflict". The host-context rules require the captured
  token to look like a host (a dotted name, or a label followed by `:port`).

## Limitations — check a new log site against these

The redactor is a **keyword and shape** matcher operating on formatted text.
It cannot help with:

1. **Binary or hex packet dumps.** A value with no keyword and no
   distinguishing shape is invisible to it. A log site that dumps raw protocol
   bytes must decide at the call site what is safe to emit; the redactor will
   not save it.
2. **Arbitrary user-authored text** — station notes, memory-channel labels,
   chat/spot comment fields. Anything a user typed may contain anything.
3. **Startup and fallback stderr.** Lines emitted before the writer is
   running, or after a rotation failure forces the mirror path, do not all
   pass through `formatLine()`.
4. **Files that are not the timestamped app log.** The SpotHub feed logs
   (`<config>/AetherSDR/spothub/…`) are written directly by
   `DxClusterClient`, `N1MMSpotClient`, `SpotCollectorClient`, `WsjtxClient`
   and `FreeDvClient`, and never pass through `AsyncLogWriter`. They are not
   collected into a support bundle, which is why they are a documented
   limitation here rather than a redactor change.
5. **Third-party library logging** that writes to stderr on its own.

## Egress points

Three places take log content out of the machine. All of them re-scrub, so a
gap in one rule does not depend on which path a user takes:

- `SupportBundle::createBundle` — historical logs are streamed through
  `redactPii()` into the archive rather than copied, so a bundle generated
  after an upgrade does not carry pre-upgrade lines out verbatim. The source
  log on disk is left untouched.
- `IssueReport::buildIssueReport` — re-scrubs the log tail at render time.
- `AutomationServer` log-event serialization — re-scrubs each event's message.
