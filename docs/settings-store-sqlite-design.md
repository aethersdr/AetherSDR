# Client Settings Store — SQLite Design (RFC #4603)

Status: **implemented through PR 6** (storage engine #4612, scoped store +
authority #4614, HL2 state memory #4619, BandStack #4621, memory bank #4623 —
all merged; the Settings Browser is the remaining phase)
RFC / tracking issue: [#4603](https://github.com/aethersdr/AetherSDR/issues/4603)
Closes: [#4602](https://github.com/aethersdr/AetherSDR/issues/4602) (thread safety)

## Why

The XML store (`AetherSDR.settings`) had four structural problems:

1. **Key charset trap** — top-level keys were XML *element names*; anything
   outside `[A-Za-z_][A-Za-z0-9_]*` was silently dropped at save. Four
   independent workarounds existed (MAC sanitizing, hex-encoded alias keys,
   the BandStack side file, composite keys hidden in JSON blobs).
2. **Full-file rewrite per save** — all ~400 keys serialized, re-parsed,
   validated, and renamed on every `save()` (#3032), 330 call sites deep.
3. **No scoping** — app-global, per-backend-family, and per-physical-radio
   state shared one flat namespace by naming convention.
4. **No thread safety** — the audio and spot worker threads raced the main
   thread on an unguarded `QHash` (#4602).

The deeper driver is the backend split: Flex radios persist their operating
state on-radio (Constitution II/III — the client must never re-assert it),
while the Hermes-Lite 2 persists nothing — the client is the radio's memory.
The scoped schema below is the substrate for that (RFC proposals A/B; the
consuming `RadioStateMemory`/`ClientSettingsDomain` work lands in PR 2+).

## Architecture

```
                    AppSettings (public API unchanged — 970 call sites)
                    ├── in-memory cache (QMap) + dirty-key tracking
                    │     guarded by QReadWriteLock (reads) + save mutex (I/O)
                    ├── session credential vault (never persisted)
                    └── SettingsDatabase  ←— the ONLY sqlite3.h consumer
                          └── third_party/sqlite (vendored amalgamation)

  SettingsPaths       — single source of truth for every store path
  SettingsBootstrap   — pre-QApplication reads (UiScale, GpuSelector):
                        DB read-only when present, legacy-XML scan fallback
  SettingsSanitizer   — recursive secret redaction (support bundle + CLI export)
```

- **Engine**: vendored SQLite amalgamation (`third_party/sqlite/`, public
  domain, version pinned in its README). Deliberately not Qt6::Sql: no driver
  plugin to deploy on any platform, and usable before QApplication exists.
- **File**: `<config>/AetherSDR/AetherSDR.db`, WAL journal,
  `synchronous=NORMAL`, `busy_timeout=5s`, 0600 permissions
  (`SQLITE_DEFAULT_FILE_PERMISSIONS`).
- **Writes**: `setValue()` mutates the cache and marks the key dirty;
  `save()` commits exactly the dirty rows in one transaction (a no-op when
  nothing changed, which keeps the 330 legacy `save()` calls cheap).

## Schema (v1 — `PRAGMA user_version = 1`)

```sql
meta             (key TEXT PRIMARY KEY, value TEXT NOT NULL)
app_settings     (key TEXT PRIMARY KEY, value TEXT NOT NULL)
station_settings (station, key, value; PRIMARY KEY (station, key))
radio_settings   (family, radio_id, feature, schema_version, value;
                  PRIMARY KEY (family, radio_id, feature))
```

All four tables exist from v1 so the file format is stable. `radio_settings`
holds one **versioned JSON document per feature per scope** (Constitution
Principle V) and is live: the HL2 `OperatingState` document (universal fields
plus per-band drive/LNA maps in domain-gated extension sub-objects), the
`Identity` nickname document, and `BandStack` (#4621); and the shared memory bank
at `(local, '', MemoryBank)` (#4623 — deliberately ONE shared document
preserving #4590's cross-radio channel list; nigelfenton's review supplied the
stronger justification: the bank engages on exactly the radios whose identity
is least stable, so per-radio keying would fragment channels on a NIC swap). Writers judge the exact row they
replace (`radioFeatureExact`, no family-wide fallback) and refuse to overwrite
a newer `schema_version` — read-only toward the future, at both the store and
the document layer. `radio_id` is
the per-family canonical identity (Flex serial / HL2 MAC / Kiwi profile UUID);
`radio_id=''` rows are family-wide defaults. Values are TEXT throughout; the
`"True"`/`"False"` boolean convention is unchanged.

Schema evolution: additive only, keyed off `user_version`. An **older binary
opening a newer-schema database runs read-only** — reads work, `save()`
refuses, the user sees a notice. It never "repairs" the file downward.

## Upgrade migration (XML → SQLite)

Automatic and silent on first launch, inside `AppSettings::load()`. Governing
rules: the XML is never modified or deleted; the DB is never trusted until the
import transaction commits.

1. The XML-era recovery ladder runs first (promote a validated `.tmp`, fall
   back to `.bak`) so the best-available snapshot is imported.
2. One **exclusive transaction**: import all keys + the station section,
   verify by per-key readback + row count, stamp `meta`
   (`migrated_from_xml`, `migrated_at`, `migrated_by_version`,
   `migrated_key_count`, `source_path`, `source_mtime`, `source_sha256`),
   commit. Any failure rolls back and the session runs read-only from the
   parsed XML values; the import retries next launch.
3. **The XML stays in place as a frozen snapshot.** Downgrade contract is
   snapshot-only: an older release finds its file (settings-as-of-upgrade-day)
   and runs normally. If it saves into the file, the next new-version launch
   detects the changed hash and shows a one-time "changes were not carried
   forward" notice — no silent merging in either direction. A hand-deleted DB
   re-imports the snapshot (restore-to-upgrade-day fallback).
4. Two instances racing the first launch: the exclusive transaction plus an
   in-transaction marker re-check make exactly one import win.

## Credentials (never in the database)

The store must never contain a credential — QtKeychain (service `"AetherSDR"`)
is the only persistent credential store. The import performs the **exodus**:

| Legacy location | Destination |
|---|---|
| `AutomationBridgeToken` (flat) | keychain `automation_bridge_token` |
| `MqttPass` (flat) | keychain `mqtt_password` |
| `AsrRemoteApiKey` (flat, and the field inside the `CopyAssist` document) | keychain `asr_remote_api_key` |

Diverted values also land in the in-memory **session vault**
(`takeSessionCredential`/`setSessionCredential`) so features keep working the
same session even if a keychain write fails. Builds without keychain support
hold credentials session-only — there is no plaintext-to-store fallback path
anymore (MQTT dialog/applet, automation bridge, and CopyAssist were all
converted). The support bundle and `--config export` apply **recursive**
redaction (`SettingsSanitizer`): a secret-shaped *field* inside a JSON
document value is redacted, not just secret-named rows.

## Integrity, backups, recovery

- Startup `PRAGMA quick_check`, escalating to `integrity_check`.
- Backups only via `VACUUM INTO` (never copying a live WAL database), each
  verified before it counts: after the import (`*-postmigration.db`), at most
  weekly (`*-auto.db`, bounded to 5), and before Reset Settings deletes
  anything (`*-prereset.db`, bounded to 3, kept through the purge).
- Confirmed corruption (open failure or failed integrity check — never mere
  locks): the DB/WAL/SHM set moves to `settings-quarantine/` timestamped, the
  newest verified backup is restored, and the user sees a notice naming the
  backup and its age. With no usable backup, the frozen XML re-imports.
- `Reset Settings` checkpoints and closes the connection first (Windows file
  locks), writes the pre-reset backup, then removes the DB, sidecars, frozen
  XML + its artifacts, rolling backups, and quarantine.

## Threading (closes #4602)

All public accessors are safe from any thread: a `QReadWriteLock` guards the
cache + dirty sets, and a mutex serializes `save()`/`load()` I/O. `save()`
steals the dirty state under the lock, commits outside it, and re-merges on
failure. Do not call `save()` from the audio *render callback* (it does I/O);
worker-thread event-loop code is fine — that is precisely the #4602 case.

## Tooling

- `AetherSDR --config <list|get|set|unset|export|path>` — inspect/repair the
  store with no GUI (RFC proposal D MVO). `set`/`unset` verify persistence by
  reading the row back from the file. `export` is sanitized diagnostic output,
  not a backup.
- The support bundle's `settings.txt` is the same sanitized dump plus the
  current load notice and migration stamps.

## Shipped hardening beyond the original design (review rounds)

- **Credential seam**: `SettingsCredentialPolicy.h` is the single table of
  credential names, shared by the import exodus, the sanitizer (exact names +
  shape regex, recursive over JSON values), the `setValue()` seam guard
  (flat keys divert to the session vault; document fields are stripped), and
  the CLI's `set` refusal.
- **Busy is not corrupt**: `SQLITE_BUSY/LOCKED` anywhere in the open sequence
  fails the session instead of quarantining (POSIX `rename()` succeeds on
  open files — quarantining a live store split-brains a concurrent writer);
  quarantine+restore is serialized under a `QLockFile`.
- **Newer-schema stores reopen `SQLITE_OPEN_READONLY`** — engine-enforced,
  and `close()` never checkpoints (writes) a newer binary's file.
- **Test isolation**: `SettingsPaths::configDir()` honors the
  `AETHER_SETTINGS_DIR` override on every platform (Windows ignores env
  redirects through `QStandardPaths`); `TestSettingsProfile` sets it, and
  every store path — including side-file consumers like `BandStackSettings` —
  must route through `SettingsPaths`.
- **Capture/restore lifecycle** (PR 3): `RadioStateMemory` engagement is
  capability-shaped (`clientSettingsDomains`, empty = inert, Flex/Sim guarded
  by CI); restore is handed over unconditionally pre-connect (empty = the
  radio-swap reset); capture debounces 2 s with a 10 s max-wait, flushes on
  disconnect and explicitly in `closeEvent`; the connect-time power push is a
  seeded value-identical echo that records nothing (bench-found on real
  hardware — the review record on #4619 is the case study).

## Testing

`tests/settings_browser_dialog_test.cpp` drives the real Settings Browser
widget for the guarantees the automation bridge cannot reach (it has no
double-click or key-injection verb): masked rows are read-only with a clean
control row that stays editable, `cellActivated` is structurally unconnected
(so no style can pop the modal on a single click), and an unparseable
document is labelled rather than rendered as empty with Edit disabled.

`tests/app_settings_safety_test.cpp` — 17 one-process-per-scenario cases
(CMake `foreach`) covering the matrix above; `tests/TestSettingsProfile.h`
still provides isolation. The migration was additionally soak-tested against
a real 391-key production settings file with a key-for-key parity diff
(391/391, station rows intact, no credential rows).
