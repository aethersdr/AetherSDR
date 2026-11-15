# Agent guide — settings, persistence, and radio state

Part of [`AGENTS.md`](../../AGENTS.md). Read this before persisting anything:
`AppSettings`, `radio_settings` documents, `RadioStateMemory`, settings
migrations, credentials, or a setter that might write a radio-echoed value.

## Settings Persistence (AppSettings — NOT QSettings)

**Do NOT use `QSettings` anywhere in AetherSDR.** `Static checks` enforces this
for `src/` with `tools/check_qsettings.py`; the only allowed uses are listed,
with reasons, in its `ALLOWED` table. Test fixtures that drive the legacy-store
migration use `QSettings` deliberately and are not scanned. All client-side settings are
stored via `AppSettings` (`src/core/AppSettings.h`), which persists to a
**SQLite database** named `AetherSDR.db` in `SettingsPaths::configDir()` —
`QStandardPaths::GenericConfigLocation` + `/AetherSDR`, i.e.
`~/.config/AetherSDR/` on Linux, `~/Library/Preferences/AetherSDR/` on macOS
and `%LOCALAPPDATA%\AetherSDR\` on Windows. Always route store paths through
`SettingsPaths`, never through `QStandardPaths` directly (RFC #4603; design
doc: `docs/settings-store-sqlite-design.md`). Key names use PascalCase (e.g.
`LastConnectedRadioSerial`, `DisplayFftFillColor`). Boolean values are stored
as `"True"` / `"False"` strings.

```cpp
auto& s = AppSettings::instance();
s.setValue("MyFeatureEnabled", "True");
bool on = s.value("MyFeatureEnabled", "False").toString() == "True";
s.save();   // commits the dirty rows in one transaction (cheap; still required)
```

New configuration follows Constitution Principle V: one object per feature,
not another loose flat key.

Rules that come with the store:

- **Never include `sqlite3.h` outside `src/core/SettingsDatabase.cpp`** — the
  engine is a single-point seam (see `third_party/sqlite/README.md`).
- **Credentials never go in the settings store.** QtKeychain (service
  `"AetherSDR"`) is the only persistent credential store; without keychain
  support a credential is session-only via
  `AppSettings::setSessionCredential()`. The known credential names live in
  ONE table — `src/core/SettingsCredentialPolicy.h` — shared by the import
  exodus, the export sanitizer, the `setValue()` seam guard, and the CLI, so
  add new credentials THERE. Follow the patterns in
  `MqttSettings`/`AutomationBridgeSettings`/`CopyAssistSettings`.
- The legacy XML file (`AetherSDR.settings`) is a **frozen snapshot** from the
  one-time import — never write to it, never delete it outside Reset Settings.
- Pre-`QApplication` code reads via `SettingsBootstrap::readValue()` and paths
  come from `SettingsPaths` — never hand-build a config path.
- The `AetherSDR --config <list|get|set|unset|export|features|path>` CLI
  inspects and repairs the store without starting the GUI (the recovery path
  when a stored value breaks startup).

## Radio-Scoped Feature Documents (`radio_settings`)

Radio-scoped configuration — state that belongs to one physical radio or one
backend family — does NOT go in flat `AppSettings` keys. It goes in the
`radio_settings` table as **one versioned JSON document per feature per scope**
(Constitution Principle V):

```cpp
const RadioSettingsScope scope = m_radioModel.settingsScope();  // (family, serial)
// Writers read the EXACT row (no family-wide fallback) and check the result.
QJsonObject doc = scope.featureExact("MyFeature");
doc.insert("field", newValue);
if (!scope.setFeature("MyFeature", kMySchemaVersion, doc)) {
    qWarning() << "MyFeature: settings write did not persist";
}
// scope.feature() — exact → family-wide → {} — is for CONSUMERS reading
// effective config, not for writers.
```

- Identity comes from `RadioModel::settingsScope()` (Flex serial / HL2 MAC /
  Kiwi UUID) — never re-derive it. An empty `radio_id` row is the family-wide
  default; guard against writing one by accident when the serial isn't known
  yet (see `BandStackSettings` for the pattern).
- **Check the write result.** `setFeature()` can refuse (read-only store,
  reset in progress); a mutation that silently doesn't persist while the UI
  repaints from the store is the worst failure shape. Log loudly at minimum.
- Writers judging the row they're about to replace use the **exact** read
  (`featureExact()`), and never overwrite a document whose `schema_version` is
  newer than theirs — refuse and log (see `RadioStateMemory::store()`).
- Precedents: the HL2 `OperatingState` document, the `Identity` nickname
  document, `BandStack`, and the shared memory bank at
  `(local, '', MemoryBank)`.

## Client-Side Radio State Memory (capture/restore)

For radios that persist nothing themselves, the client is the radio's memory —
but only through the one sanctioned pipeline:

- A backend declares WHICH state the client owns via
  `RadioCapabilities::clientSettingsDomains` (typed per-domain flags; empty =
  restore nothing). **Flex and Sim declare explicitly empty** — a CI test
  guards this; a non-empty Flex declaration re-introduces the
  re-assert-stale-state bug class (#4261).
- `RadioStateMemory` is the ONLY reader/writer of the `OperatingState`
  document; engagement is `shouldEngage(caps)` — capability-shaped, **never a
  family-name check**. `RadioModel` hands restored state to the backend
  unconditionally before `connectRadio()` (an empty state is the reset that
  prevents same-family radio-swap bleed), and debounces capture (2 s trailing
  + 10 s max-wait) with an explicit flush on disconnect AND in
  `MainWindow::closeEvent()` (quit doesn't pump the queued path).
- The backend validates everything it restores at its own boundary
  (Principle VII) and **restore never keys transmit** (Principle VI) —
  restored values are setpoints; the TX gate is untouched.
- The extension document's top level is domain-named sub-objects gated
  per-domain by the engine; the CONTENTS of each sub-object are the backend's
  own (opaque above the seam).

## Settings Migration

One-time migrations when renaming or restructuring keys:

```cpp
auto& s = AppSettings::instance();
if (s.contains("OldKey") && !s.contains("NewKey")) {
    s.setValue("NewKey", s.value("OldKey", "default").toString());
    s.remove("OldKey");
    s.save();
}
```

Run once at app or feature startup, not on every access. (The XML→SQLite store
migration is automatic inside `AppSettings::load()`.)

**Migrating a legacy side file into scoped documents** follows the
claim-and-freeze pattern (precedents: `Hl2Discovery` nicknames,
`BandStackSettings`, `LocalMemoryBank`):

- Claim lazily, per scope, on first access — the document needs the radio's
  FAMILY, which only the live scope knows.
- The document's existence is the migration marker; a **present-but-empty**
  document blocks re-import.
- The legacy source stays **frozen** (or per-section-pruned, for multi-radio
  files) as the downgrade snapshot — never rewritten with new data.
- Memoize only *settled* states (document exists, claim succeeded, section
  confirmed absent); every retryable condition (file missing, unparseable,
  write refused) must retry on the next access.

## Settings Authority Policy (radio-authoritative vs client-owned)

**The radio is always authoritative for any setting it can store**
(Constitution Principles II & III). The deciding test is *whether THIS radio
can save and restore the value* — a **declared capability, not a family
assumption**:

- **On a radio that persists its own state (Flex)**: never save, recall, or
  override radio-side settings from client-side persistence.
  `clientSettingsDomains` is declared EMPTY.
- **On a radio that persists nothing and declares so (HL2 today)**: the client
  IS the radio's memory — for exactly the declared domains, persisted ONLY
  through `RadioStateMemory`'s `OperatingState` document. Never in flat
  `AppSettings` keys, never via ad-hoc code paths.

**Radio-authoritative on Flex (do NOT persist client-side):** frequency, mode,
filter, step size, AGC, squelch, DSP flags, antennas, TX power, panadapter
*count* and per-pan state (center, bandwidth, min/max dBm, FFT
average/FPS/weighted-average, and waterfall line duration).

**Client-authoritative everywhere (persist in AppSettings):** window geometry,
layout arrangement (`PanadapterLayout`, applet order/visibility), client-side
DSP (NR2/RN2/NR4/DFNR), UI preferences, client-only display appearance
preferences, spot settings.

**Why:** when both the client and a self-persisting radio store the same
setting, they fight on reconnect — the radio's GUIClientID session restore is
always more current than our saved copy.

**Anti-pattern:** do not write a radio-echoed status value into a setter that
*also* persists it to `AppSettings`. That makes the client re-assert stale
state on reconnect / profile load. For a radio-authoritative field, route
status straight to the display (a plain member + signal) and never call
`AppSettings::setValue()` in its setter. A display setter that genuinely
persists (e.g. waterfall *appearance*: color gain, black level) must hold a
client-only value — never one the radio also echoes.
