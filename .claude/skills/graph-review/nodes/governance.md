# Node: governance — project canon, in priority order

Read `stance.md` first. Read the diff against each of these and cite the
specific rule when flagging:

- **CONSTITUTION.md** — the principles are binding. Most commonly implicated:
  I (FlexLib is protocol/model authority — `reference/FlexLib_*` is the
  answer, and for HL2 the gateware RTL is the analogue), II/III
  (radio-authoritative state: the client never re-asserts what the radio
  owns; persistence must be **capability-shaped**, never family-checked),
  V (feature-owned config: one versioned JSON document, one owner, one
  migration point), VI (TX safety: nothing restores or automates into a
  keyed transmitter).
- **AGENTS.md** — Settings Persistence (SQLite store; flat keys are
  app-global only; per-radio state goes in `radio_settings` feature
  documents via `RadioModel::settingsScope()`; check the write result),
  Settings Migration (one-shot claim-and-freeze; no perpetual legacy
  fallbacks), credentials (never in the store — `SettingsCredentialPolicy.h`
  is THE table; QtKeychain only), capability declarations
  (`RadioCapabilities` + caps-map doc + gating test, per that file's
  ADDING-A-FIELD contract).
- **CMake contract** — any target compiling `AppSettings.cpp` uses
  `${AETHER_SETTINGS_SOURCES}` and joins `AETHER_SETTINGS_CONSUMERS`; tests
  isolate via `TestSettingsProfile.h` (`AETHER_SETTINGS_DIR`).
- **docs/style/dialog-patterns.md** — new dialogs ride `PersistentDialog`
  (#2605); geometry base64; frameless propagation.
- **docs/a11y.md** — accessible names on interactive widgets, throttled
  `updateAccessibility`, no interactive QLabels.
- **CONTRIBUTING.md** — tests for behavior changes, touchpoint manifest regen
  when applicable, cross-platform unless solving a platform-specific problem.
- **GOVERNANCE.md** — should this change have had an RFC/issue first? Is it
  within the scope a maintainer already ruled on?
- **Socket-test obligations** (`AGENTS.md` "Test-layer boundary"): if the
  coordinator's preflight flagged a socket-owning test of AetherSDR's own
  server surfaces (rigctld, CAT, TCI server, bridge transport), check the
  three obligations and report any missing: disclosed in the PR body; its
  `tests.cmake` block names the socket it binds; it fails fast or skips with
  exit 77 (`SKIP_RETURN_CODE 77`) when it cannot bind. A synthetic peer
  standing in for third-party firmware is a blocker under that section —
  quote it.

### Cite the sentence, or downgrade it to a nit

Before returning anything as a governance `blocker`, **find the sentence in
canon that states the rule** and put it in `ruleCited`. If you cannot, it is
a convention at most, and conventions are nits.

Do not infer a rule from `git log`. `git log -- <path>` returns *only*
commits that touched that path, so "N of the last N commits did X" computed
that way is circular and always returns 100%. This exact error once turned a
nonexistent CHANGELOG rule into a merge blocker on someone else's PR.

Every rule you checked and found satisfied goes in `attacksSurvived`, named
by document and rule, so the synthesis node can say what was checked.
