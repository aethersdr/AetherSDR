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

See [`HERMES.md`](../HERMES.md) §18 for the worked narrative, including the
traps and why the DAX crash guard is deliberately *not* the DAX capability.

## Wired and consumed

| Field | Flex | HL2 | Sim | Read at | Effect |
|---|:--:|:--:|:--:|---|---|
| `family` | `"flex"` | `"hl2"` | `"sim"` | `MainWindow::rfGainSettingsKey` | Scopes the persisted RF-gain key per family |
| `model` | from provider | `"Hermes-Lite 2"` | `"AetherSDR Demo"` | `FlexBackend::capabilities` | Key into the ModelCapabilities table |
| `manufacturer` | `"FlexRadio"` | `"Hermes-Lite"` | `"AetherSDR"` | `MainWindow::refreshRadioIdentityLabels` | Status-bar make row ABOVE the model, shown only when the model string does not already carry the brand (`FLEX-8400M` does, `IC-705` does not). Display only — nothing branches on it. Icom: `"Icom"` |
| `tuningMinHz` / `tuningMaxHz` | — (0/0) | 0.1–38.4 MHz | — (0/0) | `MainWindow_Wiring.cpp`, `applyTuningRangeToOverlayMenu` | Refuses band buttons the receiver cannot reach. 0/0 means unconstrained |
| `declaredBandRanges` | — (empty) | — (empty) | — (empty) | `SpectrumOverlayMenu::setDeclaredBands` | Optional canonical-name + native coverage list used for backend-authoritative band labels. Icom publishes the IC-9700's three discontinuous deck ranges; empty keeps canonical labels and existing Flex/HL2/Sim presentation |
| `canTransmit` | ✅ | `m_txAllowed` | ❌ | `RadioModel::setTransmit`, MOX/TUNE key guards | **TX safety gate.** Fail-closed: false denies any keying intent |
| `receiveOnlyModes` | — (empty) | — (empty) | — (empty) | `RadioModel::refuseKeyInReceiveOnlyMode` (MOX / TUNE / CW-key / `setTransmit`) | Modes the radio **demodulates but will not transmit in**, in the neutral vocabulary. Empty = transmits in everything it receives. Refusing here (not in the backend) is what rolls back `TransmitModel`'s optimistic MOX/TUNE state — a backend cannot reach `TransmitModel`, so a refusal made down there leaves the TX indicator lit and TUNE latched. Icom: `["WFM"]` on the IC-705, which receives 76–108 MHz broadcast and does not transmit there (#5040) |
| `hostModulates` | — (❌) | ✅ | — (❌) | `TciServer`, `MainWindow_Session` | Mic source collapses to PC; PC-audio lock. **Not the same question as `takesTxAudioOverSeam`** — see below |
| `takesTxAudioOverSeam` | ❌ | ✅ | ❌ | `MainWindow_Session` (capture, TX stream, PC-audio lock), `AudioEngine::setHostModulation`, `RadioModel::ensureDaxTxStream` | Whether transmit audio leaves through `submitTxAudio` rather than a DAX/VITA-49 stream. Icom: ✅ |
| `hasSelectableMicInputs` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` → `PhoneCwApplet::setSelectableMicInputs` | The MIC/BAL/LINE/ACC/PC list. False collapses it to PC and adopts that into TransmitModel. Icom: ❌ (the radio picks its own input) |
| `hasDownwardExpander` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` → `PhoneApplet::setDexpVisible`; `AutomationServer` transmit snapshot | The radio has an authoritative DEXP/downward-expander command and read-back path. False hides the complete row and omits `dexp`/`dexpLevel` from automation state. Icom: ❌ until a model profile evidences and implements that full path; the IC-9700 does not borrow Flex's compander surface |
| `rxFilterWidthsHz` | empty | empty | empty | `MainWindow::applyCapabilitiesToUi` → `RxApplet::setRadioFilterWidths` **and** `VfoWidget::setRadioFilterWidths` | The RX filter widths a radio can actually reach, **narrowest first**. **Empty = continuous or unknown**, and the operator's configurable list stays in force. Icom publishes the selected slot's actual 1A 03 width plus factory defaults for the two unselected slots that CI-V cannot read, and republishes on mode, slot, or width changes. Both filter surfaces read it — the VFO grid did not, which is how the two disagreed about what the radio could do |
| `hasTxFilterControls` | ✅ | ✅ | ❌ | `MainWindow::applyCapabilitiesToUi` → `PhoneApplet::setTxFilterControlsAvailable` | Independent TX low/high cutoff controls. Icom: true only for model profiles with a verified low/high edge register; false hides the complete row (including IC-9700, whose documented SSB TX bandwidth is WIDE/MID/NAR rather than independent cutoffs) |
| `txFilterLowEdgesHz` / `txFilterHighEdgesHz` | empty | empty | empty | `MainWindow::applyCapabilitiesToUi` → `PhoneApplet::setTxFilterEdges` | The discrete TX passband edges a radio can actually reach, ascending. **Empty = continuous or unknown**. Icom publishes per-model tables only where the model's own CI-V guide defines the WIDE/MID/NAR/SSB-D settings; the Phone applet steps through those values and rejects an exact typed value outside the list |
| `canReboot` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the Reboot row and enables its button only while connected. Icom: ❌ because power-off over Wi-Fi is a one-way trip, not a remote reboot. |
| `hasRadioDialLock` | ❌ | ❌ | ❌ | `RadioModel` → every `SliceModel` lock surface | Radio-authoritative global dial lock. Icom: ✅ for the profiled IC-705, IC-7300MK2, and IC-9700 `16 50` paths; front-panel/readback state fans out to RX Controls and every slice VFO |
| `hasRemoteOnControl` | ✅ | ❌ | ❌ | `RadioSetupDialog`, `RadioModel::setRemoteOnEnabled` | Shows the Remote On row and permits the corresponding radio command. Icom: ❌. |
| `canUpgradeFirmware` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the Firmware Update group and its experimental-warning notice. Icom: ❌. |
| `hasSmartLink` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the SmartLink certificate-management page while connected; disconnected Settings remains permissive so certificate recovery is always reachable. |
| `hasLicenseInfo` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows SmartSDR entitlement and licensed-version information. |
| `hasClientNetworkConfig` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Enables DHCP/static-IP writes. Unsupported backends retain a visibly disabled read-only group. |
| `hasFlexControlIntegration` | ✅ | ❌ | ❌ | `MainWindow`, `RadioSetupDialog` | Gates AetherControl, FlexControl, and their Settings deep links without branching on backend family. |
| `hasAudioCompression` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the selectable SmartLink radio-audio compression group. |
| `hasSharpFilters` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the radio-side sharp/low-latency filter settings page. |
| `usesVita49Transport` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Declares the family's VITA-49 transport and shows its receive-socket buffer and Network MTU controls. The MTU remains locally persisted and is sent to Flex as `client set enforce_network_mtu=1 network_mtu=…`; Icom, HL2, and Sim do not use this transport path. |
| `hasNetworkConfigurationReadback` | ✅ | ❌ | ❌ | `IcomCivBackend`, `RadioSetupDialog` | Declares that the backend can read radio-authoritative network information and gates the Network identity group. Icom is profile-driven: IC-9700 uses documented `1A 05 0139–0141` plus Network Name `0144`; IC-7300MK2 uses `0102–0104` plus `0107`; IC-705 is ❌ because its CI-V guide does not expose these WLAN settings. Network Name has dedicated session-owned model state and does not overwrite the operator-facing radio nickname. |
| `hasPrivateIpConnectionPolicy` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the SmartSDR `enforce_private_ip_connections` control. Icom: ❌; having a command plane does not imply support for this Flex command. |
| `hasTuner` | ✅ | ❌ | per profile | `TransmitModel::setHasTuner` → `TxApplet` | Shared ATU matching control and Success/Byp indicators remain visible on every radio. False renders them dimmed/unavailable; true permits grey inactive or enabled active state. Icom opts in the evidenced IC-705, IC-7300MK2, IC-7300, IC-7610, and IC-785x tuner paths; IC-9700, IC-905, and unidentified models fail closed. ANAN-G2 explicitly reports false because it has no internal ATU. |
| `hasTunerMemories` | ✅ | ❌ | ❌ | `TransmitModel::setHasTunerMemories` → `TxApplet` | Independent availability for the shared MEM control, Mem indicator, and memory-only ATU menu actions. This is Flex's radio-side memory recall/database contract; an Icom `1C 01` matching path does not imply it. False keeps those shared surfaces visible but dimmed. ANAN-G2 explicitly reports false. |
| `forwardPowerRequiresSmoothing` | ✅ | ✅ | ❌ | `TxApplet::updateMeters` | Applies the established client-side PEP response only when the backend's forward-power samples require it. Icom: ✅ for native-watt profiles; ❌ for the IC-9700's already-indicated relative Po samples. The default is ❌ and every backend declares the choice explicitly |
| `hasExtendedDsp` | from table | ❌ | ❌ | `RadioModel::hasExtendedDspFilters()` | NRS / RNN / NRF buttons |
| `hasProfiles` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | PROF applet, Profiles menu, Profile Manager, Import/Export |
| `hasDaxStreams` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | DAX + DAX-IQ applets, Autostart DAX |
| `hasRadioSideDsp` | ✅ | ❌ | ❌ | `RadioModel::hasRadioSideDsp()` | NR/NB/ANF/NRL/ANFL/ANFT, the APD row, the WNB row |
| `hasLmsNoiseFilters` | ✅ | ❌ | ❌ | `RadioModel::hasLmsNoiseFilters()` → `VfoWidget::setHasLmsNoiseFilters` | NRL / ANFL / ANFT alone — the WDSP LMS/FFT family, a THIRD tier under `hasRadioSideDsp`. Icom: ❌ (it has NR, NB and both notches, and no register these three could reach). Keeps `hasRadioSideDsp`'s permissive-on-disconnect rule |
| `hasManualNotch` | ❌ | ❌ | ❌ | `RadioModel::hasManualNotch()` → `VfoWidget::setHasManualNotch` | The MN button and the shared level slider re-targeted to notch POSITION. Icom: ✅ (`16 48` enable, `14 0D` position, `16 57` width). **Not** permissive on disconnect — a new button must not appear on a radio that has not claimed it. Distinct from the TNFs, which are pinned to absolute frequencies, and from the auto notch, which finds its own tone |
| `speechProcessorLevelMaximum` | `2` | `2` | `2` | `RadioModel::publishCapabilities`, `MainWindow::applyCapabilitiesToUi` | Defines the speech-processor level shape without a GUI model-name check. Flex/HL2/Sim retain NOR/DX/DX+ (`0..2`). Icom defaults to `2`; only the IC-9700 profile declares its evidenced continuous `0..100` COMP level. |
| `speechProcessorLabel` | `PROC` | `PROC` | `PROC` | `MainWindow::applyCapabilitiesToUi` | Radio-native label for the shared speech-processing control. Only the IC-9700 profile declares `COMP`; unidentified and sibling Icom models retain `PROC`. |
| `hasTransmitFrequencyCheck` | ❌ | ❌ | ❌ | `VfoWidget` and `RxApplet` through `RadioModel` | Replaces persistent REV with momentary XFC on verified radios. Icom: ✅ for IC-705 and IC-9700 (`1C 02`); radio readback/polling owns the visual state, and release always sends OFF |
| `fmTonePresentation` | Legacy | Legacy | Legacy | `VfoWidget`, `RxApplet` | Icom is model-profile driven: IC-705 and IC-9700 expose their documented extended CTCSS/DTCS surface; basic repeater models retain Legacy; unattested models hide tone controls |
| `fmToneModes` | empty | empty | empty | `VfoWidget`, `RxApplet` | Authoritative selectable access-mode vocabulary. IC-705 and IC-9700 publish all eight states documented for each model at `16 5D`; IC-7300MK2 retains its narrower activated path |
| `fmDtcsCodes` | empty | empty | empty | `VfoWidget`, `RxApplet` | Authoritative operator-intent vocabulary for the DTCS selector. Every backend declares empty explicitly; the activated IC-705 and IC-9700 extended profiles publish the standard 104-code set. Empty means no DTCS control, never an invented default |
| `hasHostNoiseBlanker` | ❌ | ✅ | ❌ | `RadioModel::hasHostNoiseBlanker()` → `VfoWidget::setHasHostNoiseBlanker` | **THIS HOST** blanks impulse noise in the radio's IQ (WDSP ANB, ahead of the demodulator). OR'd with `hasRadioSideDsp` at the NB button, so a direct-sampling radio gets NB without claiming firmware DSP it does not have — the same exception the manual notch makes. Requires an IQ path this host demodulates: a backend fed finished audio has nothing to blank. Icom: ❌ (the radio's own blanker, under `hasRadioSideDsp`). **Not** permissive on disconnect — it can only ADD the button |
| `hasDdcPanEdgeRolloff` | ❌ | ❌ | ❌ | `MainWindow::onConnectionStateChanged()` → `SpectrumWidget::setPanEdgeTaperEnabled()` | Real, bench-measured attenuation baked into the sampled data itself toward the extreme edges of the panadapter bandwidth — not a display artifact. True only for ANAN-G2, the first (and so far only) DDC-based backend; Flex/HL2/Sim report false since none of their receive chains have this shape. A capability flag rather than a family-string check, so a future DDC backend gets the same cosmetic edge fade automatically. Icom: ❌ (CI-V ships finished audio, not a decimated IQ stream with an edge to taper) |
| `hasRadioSideWaterfallAutoBlack` | ✅ | ❌ | ❌ | `MainWindow::applyRadioSideDspToPanDisplay` | The HW position of the Display ▸ Black Level button. False cycles Off ↔ SW. **Masks, never rewrites** the stored preference — see below |
| `hasRadioSideCwKeyer` | ✅ | ❌ | ❌ | `RadioModel::hasRadioSideCwKeyer()` | Status-bar text-keyer indicator and every text-send entry point. Icom: ✅ only for the verified IC-705 / IC-7300MK2 command-17 profiles |
| `cwTextKeyerName`, ranges and support flags | CWX, 5–100 WPM, progress/macros/live/modifiers | defaults (unused) | defaults (unused) | `MainWindow::applyCapabilitiesToUi`, CAT/TCI/rigctl/automation adapters | Shapes the shared surface without a family branch. Icom: CWK, 6–48 WPM, 30 chars, no progress/stored macros/live typing/speed modifiers; unsupported text is rejected rather than rewritten |
| `hasVoiceKeyer` | ✅ | ❌ | ❌ | `RadioModel::hasVoiceKeyer()` | Status-bar DVK indicator, the DVK panel, and its F1-F12 arming. ANDed *ahead of* the SmartSDR+ entitlement gate — see below |
| `hasFullDuplex` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Status-bar FDX indicator |
| `hasWaveforms` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | File ▸ Waveforms… |
| `hasMultiClientSessions` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Settings ▸ multiFLEX… |
| `alwaysUseClientSideSpots` | ❌ | ❌ | ❌ | `MainWindow_Spots.cpp`, `MainWindow_Wiring.cpp` through `SpotCommandPolicy` | Forces SpotHub and manual spots into the existing passive-local `SpotModel` instead of emitting Flex `spot add` commands. Icom: ✅ because CI-V has no compatible spot service. Flex, HL2, and Sim remain under the existing operator Passive toggle. |
| `hasGpsLocation` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi`, `RadioModel::hasGpsHardware`, GPS dashboard | Family/model can provide coordinates. Flex combines this with per-unit GPSDO/GNSS presence; IC-705 is ✅ from verified `23 00` |
| `hasGpsSatelliteTelemetry` | ✅ | ❌ | ❌ | `GpsLocationDialog::refreshGps` | Shows tracked/visible satellite metrics. Icom: ❌ because the IC-705 CI-V surface provides neither counts nor SNR |
| `hasGpsFrequencyReference` | ✅ | ❌ | ❌ | `MainWindow` status stack, `GpsLocationDialog::refreshGps` | Treats GPS as a 10 MHz/GPSDO reference. Icom: ❌; its receiver reports position/time and does not discipline RF |
| `hasGpsTimeConfiguration` | ❌ | ❌ | ❌ | `GpsLocationDialog::updateGpsTimeControls`, `RadioModel` GPS clock intents | Shows the radio-owned NTP client, configured server address, GPS Time Correct, and Sync Now controls. IC-705: ✅; explicit writes are read back before display |
| `hasGpsHardware` | ✅ | ❌ | ❌ | `RadioSetupDialog` | Shows the GPS page and the GPS entry under Radio › Options only when the backend declares hardware. Icom: ✅ only for the IC-705 profile; IC-9700 and IC-7300MK2 are ❌. Separate from `hasGpsLocation`: hardware presence drives the Setup page, while `hasGpsLocation` (IC-705 `23 00`) drives the live dashboard, so a future model can declare a receiver without a position readout. |
| `gpsHardwareRequiresPresence` | ✅ | ❌ | ❌ | `RadioModel::hasGpsSetupHardware` | Flex GPS hardware is optional per unit and needs live oscillator/GPS presence; fixed-profile hardware such as the IC-705 does not. |
| `hasSupplyVoltageTelemetry` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | PA supply-voltage readout in the status bar. Icom: ✅ only when the active model profile provides a calibrated Vd curve; unprofiled models neither publish nor poll Vd/Id |
| `hasPaTemperatureTelemetry` | ✅ | ✅ | ❌ | `MainWindow::applyCapabilitiesToUi` | PA-temperature gauge and °C/°F selector in Radio Vitals. Icom: ❌ until a model profile declares and implements a PA-temperature meter; the IC-9700 has no such declared telemetry |
| `hasPaCurrentTelemetry` | ❌ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Calibrated PA drain-current face in Radio Vitals, used only when PA-temperature telemetry is unavailable. Icom: ✅ only for the IC-9700 profile's documented 0–20 A Id calibration. Flex remains ❌ because its PACURRENT meter is known to clip below real full-power draw |
| `hasMainFanTelemetry` | ✅ | ❌ | ❌ | `MainWindow::applyCapabilitiesToUi` | Main Fan gauge in Radio Vitals. All current Icom models are ❌ because the backend does not publish fan-speed telemetry |
| `hostFrequencyCalibration` | ❌ | ✅ | ❌ | `RadioSetupDialog` (Calibration page), `AutomationServer::doFreqCal` | Shows the Calibration page and enables the `freqcal` bridge verb. Means "**the client** owns the frequency-error correction", not "this radio has an error" — every radio does. Flex is ❌ because it calibrates itself (`radio set cal_freq` / `pll_start`), and that surface stays in the Frequency Offset group on the Receive page. HL2 is ✅ because its 76.8 MHz NCO scale is a `localparam` in the bitstream (`radio.v` M2) and no register in the HPSDR map accepts a correction — see `docs/architecture/hl2-frequency-calibration.md` |
| `persistsMemories` | ✅ | ❌ | ❌ | `LocalMemoryBank` engagement (#4590) | host-side memory bank vs radio-side slots — the bank's ONE shared document lives at `radio_settings (local, '', MemoryBank)` since RFC #4603 PR 6, covered by settings backup/export; legacy `memories.json` is a frozen import source. Icom is profile-gated for IC-705, IC-7300MK2, and IC-9700; unprofiled Icom models remain ❌. |
| `canWriteMemories` | ✅ | ❌ | ❌ | `RadioModel::memoriesWritable`, memory dialog and panadapter memory panel | Separates radio ownership from mutation support. The initial Icom implementation is read-only, so Add, Import, inline edits, and Remove cannot leak vendor-incompatible memory commands. |
| `canApplyMemories` | ✅ | ❌ | ❌ | `RadioModel::tryMemoryCommand` | True means the backend accepts its native memory-apply command. Initial Icom support is ❌ and applies recallable cached fields through the existing neutral slice setters instead of entering vendor Memory mode; split/RPS/DV/DD records are display-only. |
| `canRefreshMemories` | ❌ | ❌ | ❌ | Memory Channels dialog → `RadioModel::refreshMemories` | Explicit, button-only radio-memory snapshots. IC-7300MK2 reads 99 channels; IC-9700 reads all 297 or one selected band; IC-705 requires one selected group and reads only its 100 channels. No memory scan runs during connection. |
| `clientSettingsDomains` | empty | Tuning\|Passband\|SpanRate\|RfGain\|TxSetpoints\|Memories\|Agc | empty | `RadioStateMemory::shouldEngage` → `RadioModel::handRestoredStateToBackend` | connect-time operating-state restore + debounced capture (RFC #4603 PR 3): `Hl2Backend::applyRestoredState` seeds rate/freq/LNA at connect, `pushInitialState` applies restored mode+passband (reconciled with #4484 — restored as a pair, so mode and passband cannot disagree) and the start band's drive; per-band LNA/drive maps ride the extension document and follow TX-slice band changes. `Agc` (#4909) carries the mode + threshold pair as typed universal fields — FLAT, not per-band, and seeded onto EVERY receiver by `Hl2Backend::seedReceiverAgc()`, because the AGC runs in host-side WDSP and no HPSDR register can be asked what it is. Seeding runs from `connectRadio` when the connect SERIAL changes or the receivers were rebuilt from nothing — never on a plain auto-reconnect, because `handRestoredStateToBackend` re-hands the document before every connect and `buildReceivers` preserves live receiver state, so an unconditional seed flattened per-receiver AGC on each dropped link. Memories is declarative only — the bank engages on `persistsMemories` and keeps its own shared document (PR 6). Flex/Sim: no-op by empty declaration. |
| `extensionNamespaces` | `["flex"]` | `["hl2"]` | — | No production reader or general `invokeExtension` pre-check yet | Flex: amp / tuner operate/bypass/autotune verbs. HL2: `freqcal.get` / `.set` / `.set_live`, behind the `freqcal` bridge verb and the Calibration page. Icom: `["icom"]`, with PC-audio, tuner, scope, control-map, scheduler and diagnostic verbs. RadioModel's Icom PC-audio wrappers and the Flex accessory routes still pre-check by family string; #5262 M1 converts those consumers. |
| `maxNotchFilters` | 1000 | 1024 | 0 | `MainWindow::applyCapabilitiesToUi`, `SpectrumWidget::setNotchCapabilities` | The sidebar `+TNF` button and the panadapter's add/remove-notch entries. **0 hides them.** Flex's figure is a UI sanity limit (neither FlexLib nor the wire declares one); HL2's is WDSP's real notch-database size |
| `notchHasDepth` | ✅ | ❌ | ❌ | `SpectrumWidget::setNotchCapabilities` | The depth submenu on a notch's right-click menu. A WDSP notch is a full null with no depth to set |
| `notchMinWidthHz` / `notchMaxWidthHz` | 10 / 6000 | 50 / 6000 | 0 / 0 | `SpectrumWidget::setNotchCapabilities` | Clamps drag-resize and the width presets. HL2's floor is set by the RX filter length and WDSP **silently widens** anything narrower, so a UI offering less draws a notch narrower than the one being heard |

### RTL-SDR experimental profile

RTL-SDR is receive-only and mostly inherits the false/empty capability
defaults. Its non-default declarations are kept separately so adding the
experimental family does not duplicate or stale the main cross-family table.

| Field | RTL-SDR value | Effect |
|---|---|---|
| `family` / `model` / `manufacturer` | `"rtl"` / USB product / USB vendor (fallback `"Realtek"`) | Identifies the local device in the shared radio model and status bar |
| `tuningMinHz` / `tuningMaxHz` | 24 kHz / 1.766 GHz | Bounds tune requests; frequencies below 24 MHz select Q-branch direct sampling |
| `sampleRatesHz` | 225001, 250000, 300000, 1000000, 1536000, 1843200, 2000000, 2400000, 3000000 | Publishes only legal `librtlsdr` detents |
| `canTransmit` / `txPowerMaxWatts` / `hostModulates` | false / 0 / false | Fails closed on every transmit path and never opens the microphone |
| `maxSlices` / `maxPanadapters` | 1 / 1 | Matches the single in-process DDC |
| `persistsMemories` / `hasSupplyVoltageTelemetry` / `hasMultiClientSessions` | false / false / false | Avoids fabricating radio-side services or telemetry |
| `clientSettingsDomains` | Tuning\|Passband\|SpanRate\|RfGain\|Memories | Restores only state the USB receiver cannot persist itself |
| `extensionNamespaces` | `["rtl"]` | Declares gain, PPM, direct-sampling, offset-tuning, and sample-rate controls |

`MainWindow::applyCapabilitiesToUi()` is the single fan-out for UI visibility. It
is bound to `RadioModel::capabilitiesChanged`, which fires on both connection
edges and on any mid-session revision by the backend. Add a capability by adding
one owning call there — not another connect-time lambda. With several flags in
play, scattered lambdas are how two callers end up both driving one widget's
`setVisible()` and whichever fires last wins.

### `hostModulates` vs `takesTxAudioOverSeam`

They look like one question and are two, and conflating them cost a working
transmitter on the Icom bring-up. `hostModulates` asks **who runs the
modulator**; `takesTxAudioOverSeam` asks **how transmit audio reaches the
radio**. There are three cases, not two:

| | modulator | audio route | `hostModulates` | `takesTxAudioOverSeam` |
|---|---|---|---|---|
| Flex | radio | DAX / VITA-49 | ❌ | ❌ |
| HL2 | host | the seam | ✅ | ✅ |
| Icom | radio | the seam | ❌ | ✅ |

`AudioEngine` gated its entire transmit chain on the first flag, and
`MainWindow_Session` gated capture, the TX stream and the PC-audio lock on it
too. An Icom therefore captured nothing, processed nothing and keyed with no
modulation at all, while TCI's transmit path asked for a DAX stream and failed
with "this radio has no command plane". Everything about the AUDIO now keys off
the second flag; `hostModulates` keeps only the questions that are genuinely
about the modulator.

### `hasRadioSideDsp` is three claims, not one

It began as one flag meaning "the radio runs its own receive DSP", and grew a
second meaning nobody wrote down: "the radio runs *FlexRadio's particular set*
of it". Those came apart the moment a second radio-side family arrived.

An IC-705 is emphatically the first and not the second. It has noise reduction
(`16 40`), a noise blanker (`16 22`), an auto notch (`16 41`) and a manual notch
(`16 48`) — and nothing resembling NRL, ANFL or ANFT, which are WDSP LMS/FFT
filters. Declaring `hasRadioSideDsp` on it therefore lit up three buttons whose
intents reach no register on that radio: the HERMES §17 shape, where the control
moves, the setting persists and the audio never changes.

So the claim is now tiered, and each tier has its own disconnected rule:

| Flag | Means | Disconnected |
|---|---|---|
| `hasRadioSideDsp` | the radio runs its own RX DSP at all | permissive (assume present) |
| `hasLmsNoiseFilters` | ...and it has the WDSP LMS/FFT family | permissive — NRL/ANFL/ANFT predate the flag, and hiding them on a Flex the moment it disconnects would be a regression, not an honesty gain |
| `hasManualNotch` | it has one operator-placed in-passband notch | **not** permissive — MN is a new button, and a permissive default would show it on every radio in the window before a backend reports, including the Flexes that notch with TNFs instead |
| `hasHostNoiseBlanker` | **this host** blanks impulses in the radio's IQ, whatever the radio's own DSP does | **not** permissive — it only ever ADDs the NB button, so a permissive default would show NB on a radio claiming neither capability |

`hasExtendedDsp` is a fourth, orthogonal tier (the 8000-series NRS/RNN/NRF).

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

**NB is now the same correction applied to the receive side.** The button used
to be gated on `hasRadioSideDsp` alongside NR and ANF, on the reasoning that
`SliceModel::setNb` emits `slice set N nb=` and that wire text reaches nothing
without a Flex command plane. True of the COMMAND; wrong about the CONTROL. On a
direct-sampling backend the blanker the operator is asking for runs on **this
host** — WDSP's ANB on the raw IQ, ahead of the demodulator, which is the only
place an impulse can still be blanked before the bandpass smears it — so hiding
NB removed a working control. `hasHostNoiseBlanker` is what a backend claims to
say so, and `VfoWidget::applyRadioSideDspVisibility()` ORs it with
`hasRadioSideDsp` rather than replacing it, because on a Flex the blanker really
is the radio's.

NR and ANF stay hidden on such a radio, and the asymmetry is honest rather than
an oversight: WDSP has the stages (`anr`, `emnr`, `anf`) and nothing wires them
up yet. They become visible when they do something, not when they could.

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

### The status-bar row: hidden, not dimmed — and what stays

`CWX`, `DVK` and `FDX` are three labels in the status bar whose entire
implementation is a verb the radio's firmware executes: `cwx …`, `dvk …`,
`radio set full_duplex_enabled=`. They pass the `hasRadioSideDsp` test above,
but each got its own flag rather than riding that one — a family could plausibly
have a voice keyer without full duplex, and merging them would make the first
such backend a rewrite.

They are **hidden**, not disabled. A greyed-out control says "not right now";
these are "not on this radio, ever", and permanently dim labels read as a fault
the operator can go looking for.

Three things do **not** move with them:

- **ASR (Copy Assist)** sits in the same row and is host-side. `AsrAudioTap`
  subscribes to `AudioEngine::receivePresentationPostDspAudioReady` and whisper
  runs on this machine, so it works on every family. Gating it would remove a
  working control — the `EQ`-applet mistake above, one row over.
- **TNF**, and its `+TNF` sibling in the pan overlay menu. `tnf create/remove/
  set` and `sub tnf all` are Flex command-plane verbs, so by the test above TNF
  looks like a fourth member of this group — and it was written as one before
  being pulled back out. It stays ungated because the control is about to stop
  being empty: a host-side notch is landing and these are the surfaces it will
  drive. Gating it now would mean deleting the control and putting it straight
  back, which is precisely the round trip the `EQ` applet already made. If that
  notch does not land, reconsider this — but reconsider it as "is the control
  still empty", not as "is this a Flex feature".
- **CW itself.** A radio with `hasRadioSideCwKeyer = false` still transmits CW
  from a key, a paddle or the host keying path. What it lacks is a text buffer.

The **shortcuts** need the flag too, not just the buttons. The keyer F1-F12 keys
are `ApplicationShortcut`s that stay armed whether or not their button is on
screen, so `updateKeyerAvailability()` ANDs both capabilities into the same
availability that drives the enabled state and the panel auto-hide. Without
that, an HL2 in CW keeps F1-F12 firing `cwx send` into a backend with no such
verb.

And the buttons are not the last of it. `cwx` has four entry points that never
touch the status bar at all, so both keyer capabilities are read through
`RadioModel::hasRadioSideCwKeyer()` / `hasVoiceKeyer()` — which carry the
permissive disconnected rule themselves — rather than inline at each site:

| Surface | Where | On a radio that declares false |
|---|---|---|
| FlexControl / Ulanzi `CwxF1`..`CwxF12` macro action | `MainWindow::applyFlexControlAction` | Ignored, logged under `aether.cw`. The binding stays assignable — it is operator-scoped and outlives any one radio |
| MQTT `aethersdr/cw/transmit` | `MainWindow::wireSpotSubsystem` | Ignored, `qCWarning(lcMqtt)` |
| TCI `cw_msg`, `cw_macros`, `cw_macros_stop` | `TciProtocol` | Ignored, `qCWarning(lcCat)`. Checked inside the queued lambda, on the model's thread — the TCI socket thread must not read `RadioModel` |
| Automation bridge `cwx send\|speed\|stop` | `AutomationServer::doCwx` | Returns an error rather than `ok:true`, so a caller polling `get_state cwx` has something to blame |
| rigctl `send_morse` / `b`, `stop_morse` | `RigctlProtocol` | `RPRT -11` (RIG_ENAVAIL) instead of `RPRT 0` — Not1MM/N1MM must not be told a contest exchange went out |
| SmartCAT (Kenwood) `KY` | `SmartCatProtocol::cmdKY` | `?;` for both set and query; the query would otherwise answer `KY0;` "buffer empty" forever |

The two CAT surfaces read the accessor SYNCHRONOUSLY, on their own socket
thread — the same direct-read posture those files already take for
`isConnected()` / `cwxActive()`, and the only way to answer a protocol that
wants a return code. Only the mutation takes the queued hop to the model thread.

An `ok` for work that never happens is the same defect as a permanently dim
button, one plane over.

`hasVoiceKeyer` is evaluated **ahead of** `DvkAvailabilityGate`'s SmartSDR+
entitlement check, and the ordering is load-bearing. That gate fails *open* when
the entitlement is unknown (#4210 — the radio must say no before the UI does),
which is right for a Flex mid-handshake and would otherwise leave a live DVK
button on every radio that never reports a license at all. "Is this radio
licensed for the feature" is a question only a radio that *has* the feature can
be asked.

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

## Previously bypassed, now reconciled

`maxSlices` / `maxPanadapters` sat here for weeks: declared by every backend
and read by nothing — `RadioModel::maxSlices()` resolved the model-**name**
table, so an HL2 declaring `maxSlices = 1` was ignored and its limit came
from whatever the string `"Hermes-Lite 2"` happened to resolve to (the same
bypass `hasExtendedDsp` once had). **#4545 reconciled them**: both accessors
now prefer the connected non-Flex backend's declaration (`RadioModel.h`,
`maxSlices()` / `maxPanadapters()`). Their fallback paths differ:
`maxSlices()` returns `m_maxSlices`, seeded from the FlexLib model table,
revisable by live Flex `slices=N` status, and retained across disconnect
(#4854); `maxPanadapters()` reads the model table directly. The three
enforcement sites — `RigctlProtocol`, `TciServer`, `AutomationServer` — all
resolve through the accessors. HL2's figure is genuinely dynamic (discovery
receiver count, capped by the link budget at the running span).

The lesson this section keeps: a declared capability needs its consumers
**converted**, not merely present — these two fields looked wired from the
backend side the whole time.

## Declared, but nothing reads them at all

| Field | Flex | HL2 | Sim | Note |
|---|:--:|:--:|:--:|---|
| `sampleRatesHz` | — | 4 rates | `{}` | HL2 populates it honestly; no consumer exists |
| `txPowerMaxWatts` | — (0.0) | 0.0 | 0.0 | Global fallback ceiling; Flex still omits it despite transmitting, which remains wrong but inert while `txPowerBands` is empty |
| `hasAmplifier` | — (❌) | ❌ | ❌ | The AMP applet is driven by `TunerModel::presenceChanged`, not by this |
| `extensions` | — | — | — | The namespaced vendor bag; never populated |

`txPowerBands` is the consumed exception to this section: `RadioModel` reads it
to update `TransmitModel::maxPowerLevel` when the transmit slice crosses into a
range with a different PA rating. Flex, HL2 and Sim explicitly leave it empty;
the IC-9700 declares 144–148 MHz at 100 W, 430–450 MHz at 75 W and 1240–1300
MHz at 10 W. An empty list preserves the prior global/radio-reported behaviour.

On the Icom side these ratings are not written into `capabilities()` by hand.
They are read from `bandsFor()` in `IcomModels.cpp` — the same table the tune
guard (`supportsFrequency` / `nearestSupportedFrequency`) uses to refuse the
two holes in the IC-9700's envelope, because the ceilings and the tunable
ranges describe the same three RF decks and must not be able to disagree. An
empty table is also the predicate the tune path keys on, so a model with one
continuous range keeps its untouched command path.

What `maxPowerLevel` actually drives on an Icom is the **meter and gauge full
scale** (`MainWindow_Wiring.cpp`, `VfoWidget::txPowerFullScaleW`). RF power
itself is a 0–100 % CI-V level, so the radio's own PA governs the watts. This
field makes a 10 W 23 cm transmission read against a 10 W scale instead of a
100 W one; it does not clamp the request.

These are the ones to check first when something "should have worked". Note the
pattern in the Flex column: all four fields in the table above are left at
their defaults, and every one of them is correct only by accident or inert
only by luck. That is the trap in rule 1 above, sitting in the tree.

## Not a capability field, but the same contract

`IRadioBackend::linkStats()` / `linkStatsUpdated()` — the transport counters
behind the title-bar heartbeat, the status-bar `Network:` field and the whole
Network Diagnostics pane. Not in `RadioCapabilities` because it carries live
values rather than a yes/no, but it follows rule 1 in the same shape and belongs
on the same checklist when you add a backend.

| | Flex | HL2 | Sim |
|---|:--:|:--:|:--:|
| overrides `linkStats()` | ❌ | ✅ | ❌ |

`LinkStats::reported` defaults to **false**, and that default is the compatible
one for once: a backend that says nothing leaves every consumer on the source it
already had (the Flex `RadioConnection` + `PanadapterStream`). A backend that
owns its own socket must override it, or its operator gets a connected radio
reporting 0 kbps.

Within the struct, individual figures a transport cannot measure are `-1`, not
`0` — `RadioModel::hasLinkRtt()` and `hasStreamCategoryStats()` are the
predicates the readouts ask before printing. See [`HERMES.md`](../HERMES.md)
§21.3 for why a zero there is a claim the app cannot support.

## Where the values come from

- **FlexBackend** seeds `maxSlices`, `maxPanadapters` and `hasExtendedDsp` from
  `ModelCapabilities` — the FlexLib-sourced platform table keyed by model name
  (Principle I). That is derived-from-name truth being used to *seed*
  reported-by-backend truth; the two remain distinct concepts.
- **Hl2Backend** reports `canTransmit` from its own TX gate (`m_txAllowed`) so a
  build with transmit disabled looks RX-only from above the seam.
- **IcomCivBackend** derives `receiveOnlyModes` from `modeListFor()` filtered by
  `modeIsReceiveOnly()` rather than listing it a third time, so a mode cannot be
  offered in the combo without a consistent transmit answer for it. It also
  keeps a wire-level backstop in `setKeying`/`setTune` — silent apart from a log
  line, since the operator-facing refusal belongs to the guard above.
- **SimBackend** must never look like something that can key a transmitter
  (Principle VI).

## Tests

[`tests/radio_capability_gating_test.cpp`](../../tests/radio_capability_gating_test.cpp)
asserts each backend's declared flags, the relay firing on both edges, and the
permissive-on-disconnect rule. Every assertion reads a **capability** — never
`caps.family`, never a backend type. A test that asserted the family would pass
just as happily against the anti-pattern the struct exists to prevent.
