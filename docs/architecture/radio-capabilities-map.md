# RadioCapabilities — field map

Every field in [`src/core/backends/RadioCapabilities.h`](../../src/core/backends/RadioCapabilities.h):
what each backend declares, and where — if anywhere — the value is actually
read.

Keep this table current when you add a field. A capability that no consumer
reads looks identical, from the backend side, to one that works.

**Legend** — ✅ true · ❌ false · — not set, inherits the struct default
(`false` / `0` / empty).

## The rule this struct exists to enforce

Clients render against what the radio **reports**. No call site asks "is this a
Flex" — no `caps.family` test, no `dynamic_cast<FlexBackend*>`. This is the
structural replacement for the model-impersonation anti-pattern (aetherd RFC §1),
and the header comment on the struct is the normative statement of it.

Two rules that fall out of that, both of which have already caused bugs:

1. **Fields default to `false`. Set every field explicitly in every backend.**
   A backend that omits a field does not inherit something sensible — it
   silently declares the feature absent. When `hasTuner` was added this nearly
   shipped as a Flex regression; FlexBackend escaped only because it happened to
   set it by hand.
2. **Restore the permissive value on disconnect** — `!connected || caps.hasX`.
   With no radio attached there is nothing to be honest about, and a control
   that stays hidden after unplugging reads as a fault.

See [`HERMES.md`](../../HERMES.md) §18 for the worked narrative, including the
traps and why the DAX crash guard is deliberately *not* the DAX capability.

## Wired and consumed

| Field | Flex | HL2 | Sim | Read at | Effect |
|---|:--:|:--:|:--:|---|---|
| `family` | `"flex"` | `"hl2"` | `"sim"` | `MainWindow::rfGainSettingsKey` | Scopes the persisted RF-gain key per family |
| `model` | from provider | `"Hermes-Lite 2"` | `"AetherSDR Demo"` | `FlexBackend::capabilities` | Key into the ModelCapabilities table |
| `tuningMinHz` / `tuningMaxHz` | — (0/0) | 0.1–38.4 MHz | — (0/0) | `MainWindow_Wiring.cpp`, `applyTuningRangeToOverlayMenu` | Refuses band buttons the receiver cannot reach. 0/0 means unconstrained |
| `canTransmit` | ✅ | `m_txAllowed` | ❌ | `RadioModel::setTransmit`, MOX/TUNE key guards | **TX safety gate.** Fail-closed: false denies any keying intent |
| `hostModulates` | — (❌) | ✅ | — (❌) | `TciServer`, `MainWindow_Session` | Mic source collapses to PC; PC-audio lock |
| `canReboot` | ✅ | ❌ | — (❌) | `RadioSetupDialog` | Enables the Reboot button |
| `hasTuner` | ✅ | ❌ | ❌ | `TransmitModel::setHasTuner` → `TxApplet` | ATU / MEM dimming |
| `hasExtendedDsp` | from table | ❌ | ❌ | `RadioModel::hasExtendedDspFilters()` | NRS / RNN / NRF buttons |
| `hasProfiles` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | PROF applet, Profiles menu, Profile Manager, Import/Export |
| `hasDaxStreams` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | DAX + DAX-IQ applets, Autostart DAX |
| `hasRadioSideDsp` | ✅ | ❌ | ❌ | `RadioModel::hasRadioSideDsp()` | NR/NB/ANF/NRL/ANFL/ANFT, the APD row, the WNB row |
| `hasRadioSideWaterfallAutoBlack` | ✅ | ❌ | ❌ | `MainWindow::applyRadioSideDspToPanDisplay` | The HW position of the Display ▸ Black Level button. False cycles Off ↔ SW. **Masks, never rewrites** the stored preference — see below |
| `hasWaveforms` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | File ▸ Waveforms… |
| `hasMultiClientSessions` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Settings ▸ multiFLEX… |
| `hasSupplyVoltageTelemetry` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | PA supply-voltage readout in the status bar |
| `persistsMemories` | ✅ | ❌ | ❌ | `LocalMemoryBank` engagement (#4590) | host-side memory bank vs radio-side slots — the bank's ONE shared document lives at `radio_settings (local, '', MemoryBank)` since RFC #4603 PR 6, covered by settings backup/export; legacy `memories.json` is a frozen import source |
| `clientSettingsDomains` | empty | Tuning\|Passband\|SpanRate\|RfGain\|TxSetpoints\|Memories | empty | `RadioStateMemory::shouldEngage` → `RadioModel::handRestoredStateToBackend` | connect-time operating-state restore + debounced capture (RFC #4603 PR 3): `Hl2Backend::applyRestoredState` seeds rate/freq/LNA at connect, `pushInitialState` applies restored mode+passband (reconciled with #4484 — restored as a pair, so mode and passband cannot disagree) and the start band's drive; per-band LNA/drive maps ride the extension document and follow TX-slice band changes. Memories is declarative only — the bank engages on `persistsMemories` and keeps its own shared document (PR 6). Flex/Sim: no-op by empty declaration. |
| `extensionNamespaces` | `["flex"]` | — | — | `invokeExtension` pre-check | Amp / tuner operate/bypass/autotune verbs |

`MainWindow::applyCapabilitiesToUi()` is the single fan-out for UI visibility. It
is bound to `RadioModel::capabilitiesChanged`, which fires on both connection
edges and on any mid-session revision by the backend. Add a capability by adding
one owning call there — not another connect-time lambda. With several flags in
play, scattered lambdas are how two callers end up both driving one widget's
`setVisible()` and whichever fires last wins.

### What `hasRadioSideDsp` must never hide

The host-side equivalents are *not* gated on it, and must not be: the AetherDSP
noise modules (NR2/NR4/MNR/BNR/DFNR/RN2) and the Aetherial RX/TX EQ tiles
(`ceq` / `ceq-rx`). On a radio reporting `hasRadioSideDsp = false` those are the
**only** audio DSP the operator has, so gating them would leave nothing at all.

The test for whether a control belongs behind this flag is whether its only
effect is to emit a verb the radio's firmware executes.

**The `EQ` applet used to be behind this flag and no longer is.** It looked like
it belonged: `EqualizerModel` emits `eq RXsc` / `eq TXsc`, which reach nothing
without a Flex command plane, so the applet passed the test above. But the test
asks about the CONTROL, and the conclusion was drawn about the COMMANDS. The
equalizer those eight sliders ask for exists on every family — `ClientEq` is
already in both audio paths — so `MainWindow::wireHostModulatedVoiceChain()`
maps the octave bands onto it for any backend without a Flex command plane, and
the applet is now unconditionally visible. Hiding it was removing a working
control rather than an empty one.

The same correction applies to the other Flex-shaped voice controls, none of
which are capability-gated: PROC and its NOR/DX/DX+ level drive `ClientComp`,
and the Phone applet's TX low-cut/high-cut reaches a host modulator through
`IRadioBackend::setTxFilter`.

Two consequences worth knowing. The graphic EQ and the compressor write into the
**same** `ClientEq`/`ClientComp` objects the Aetherial strip edits, so the two
surfaces are two views of one object — moving a graphic-EQ slider replaces the
strip's band layout in slots 0..7, and toggling the strip's compressor lights
PROC. And on Flex both mappings are skipped, so one slider movement never
equalizes or compresses twice.

**Which surface may write is its own question, and it is not a capability.**
`core/HostVoiceChainPolicy.h` answers it, because the family check alone gets it
wrong in both directions:

- `EqualizerModel` and `TransmitModel` have no persistence — their state arrives
  from a Flex `eq` / `transmit` status or from an operator move — while
  `ClientEq` and `ClientComp` *do* persist. So at a connect edge the Flex-shaped
  models sit at their construction defaults (eight bands at 0 dB, every enable
  false), and re-pushing them writes those defaults over the operator's saved
  audio chain.
- `hostModulates` is false for a Flex, so a plain Flex connect reaches the
  family-swap unwind too. Disabling the shared objects there switches off the
  operator's own Aetherial RX EQ, TX EQ and compressor on a session that never
  went near a host-modulating backend — the gating this document says must not
  happen, arriving by the back door.

Both predicates therefore turn on whether the operator has actually moved one of
the Flex-shaped controls in this process.
`tests/host_voice_chain_policy_test.cpp` pins the truth table.

### What `hasRadioSideWaterfallAutoBlack` must never hide

The same rule one plane over. `SW` — the client-side noise-floor estimate — is
not gated on it and must not be: on a radio reporting false it is the only
automatic waterfall floor the operator has, and hiding it would leave only the
manual slider.

The flag is deliberately separate from `hasRadioSideDsp` rather than riding on
it. Both describe work the radio does instead of this host, but one is audio DSP
driven by command-plane verbs and the other is a display-plane computation
embedded in the waterfall stream. A backend could plausibly have either without
the other, and merging them would make the first such backend a rewrite.

### A gate masks; it must not write through

`DisplayWfAutoBlackRadioSide` is the operator's **intent**, and the capability
decides what is **in effect**. `SpectrumWidget` exposes both —
`wfAutoBlackRadioSide()` for the intent, `effectiveWfAutoBlackRadioSide()` for
intent ∧ capability — and only a deliberate operator action reaches the setter
that persists.

The first implementation coerced the mode and let the normal change signals fire,
which reach `setWfAutoBlackRadioSide()` and write AppSettings. Connecting an HL2
once then destroyed a Flex user's stored HW preference for good. **Rule 2 above
implies this generally:** a gate that persists its coercion cannot restore
anything, so no capability gate may write through to settings.

Note the related gap this exposes, deliberately *not* fixed here: display
settings are flat `AppSettings` keys scoped by pan index only
(`SpectrumWidget::settingsKey`), so two radios share one preference. The mask is
what keeps that from doing damage today — nothing writes through it — but the
underlying state is still radio-scoped state living in a flat key.

The answer is **not** to mangle the family into the key string. `AGENTS.md`
§*"Radio-Scoped Feature Documents (`radio_settings`)"* (RFC #4603) is explicit
that radio-scoped configuration does not go in flat keys: it goes in a versioned
JSON feature document addressed by `RadioModel::settingsScope()`, read back
through `scope.feature(...)` at use time. `MainWindow::rfGainSettingsKey` is a
pre-#4603 precedent and should not be copied into new work.

That also dissolves the objection that used to be recorded here — that
`SpectrumWidget` loads its settings at construction, before any backend has
reported a family, so it cannot build a family-scoped key. A feature document is
read at use time, not baked into a key at construction, so the ordering problem
does not arise. It is still its own change, and it applies to more than this one
control.

## Declared, but the consumer bypasses the seam

| Field | Flex | HL2 | Sim | Problem |
|---|:--:|:--:|:--:|---|
| `maxSlices` | `mc.maxSlices` | 1 | 1 | `RadioModel::maxSlices()` reads `capabilitiesFor(m_model)` — the model-**name** table — not the backend |
| `maxPanadapters` | `mc.maxSlices` | 1 | 1 | `RadioModel::maxPanadapters()` does the same, and returns `.maxSlices` |

Every backend sets both, and nothing reads them. The three enforcement sites —
`RigctlProtocol`, `TciServer`, `AutomationServer` — all resolve slice limits from
the name table, so an HL2 declaring `maxSlices = 1` is ignored and its limit
comes from whatever the string `"Hermes-Lite 2"` happens to resolve to.

This is the same bypass `hasExtendedDsp` had before it was reconciled: the field
existed, the backend populated it, and every call site went around it. The fix
has the same shape — read the backend when connected, keep the name table as the
disconnected fallback — but it touches slice/pan limits in TCI, rigctl and
automation, so it is **deliberately deferred to its own PR.**

## Declared, but nothing reads them at all

| Field | Flex | HL2 | Sim | Note |
|---|:--:|:--:|:--:|---|
| `sampleRatesHz` | — | 4 rates | `{}` | HL2 populates it honestly; no consumer exists |
| `txPowerMaxWatts` | — (0.0) | 0.0 | 0.0 | Flex omits it despite transmitting. Wrong, but inert while unread |
| `hasAmplifier` | — (❌) | ❌ | ❌ | The AMP applet is driven by `TunerModel::presenceChanged`, not by this |
| `extensions` | — | — | — | The namespaced vendor bag; never populated |

These are the ones to check first when something "should have worked". Note the
pattern in the Flex column: five fields across this table and the one above are
left at their defaults, and every one of them is correct only by accident or
inert only by luck. That is the trap in rule 1 above, sitting in the tree.

## Where the values come from

- **FlexBackend** seeds `maxSlices`, `maxPanadapters` and `hasExtendedDsp` from
  `ModelCapabilities` — the FlexLib-sourced platform table keyed by model name
  (Principle I). That is derived-from-name truth being used to *seed*
  reported-by-backend truth; the two remain distinct concepts.
- **Hl2Backend** reports `canTransmit` from its own TX gate (`m_txAllowed`) so a
  build with transmit disabled looks RX-only from above the seam.
- **SimBackend** must never look like something that can key a transmitter
  (Principle VI).

## Tests

[`tests/radio_capability_gating_test.cpp`](../../tests/radio_capability_gating_test.cpp)
asserts each backend's declared flags, the relay firing on both edges, and the
permissive-on-disconnect rule. Every assertion reads a **capability** — never
`caps.family`, never a backend type. A test that asserted the family would pass
just as happily against the anti-pattern the struct exists to prevent.
