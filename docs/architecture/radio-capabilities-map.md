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
| `hasRadioSideDsp` | ✅ | ❌ | ❌ | `RadioModel::hasRadioSideDsp()` | NR/NB/ANF/NRL/ANFL/ANFT, the APD row, the WNB row, the 8-band hardware EQ applet |
| `hasWaveforms` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | File ▸ Waveforms… |
| `hasMultiClientSessions` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Settings ▸ multiFLEX… |
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
(`ceq` / `ceq-rx`, distinct from the `EQ` applet). On a radio reporting
`hasRadioSideDsp = false` those are the **only** audio DSP the operator has —
an HL2 uses the Aetherial EQ in place of the radio's hardware EQ — so gating
them would leave nothing at all.

The test for whether a control belongs behind this flag is whether its only
effect is to emit a verb the radio's firmware executes.

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
