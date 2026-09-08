# Research for `radiocert persist`

**For:** AetherSDR maintainers and radio bring-up operators. **Research date:** September 7, 2026 Pacific; GitHub refreshes extend into September 8 UTC. **Status:** findings and design discussion, not an approved implementation specification.

**Source baseline:** `ab03be0629a498603ff02ef68a7dbfbd0de91d10`, refreshed `origin/main` when this worktree was created. GitHub issue/PR dispositions were queried separately. RTL PR #5473 merged during the research, after this source baseline; its newly merged contract is identified below without claiming its implementation was audited in this checkout.

## The finding

`persist` should establish whether **the right state reaches the right owner and returns to the right user-visible context after a transition**. “The setting was saved” is only one part of that claim.

The history contains lost writes, missing restore/application paths, stale client replay over valid radio state, wrong scope, temporary state becoming permanent, replaced objects with stale UI subscriptions, and software that agrees with itself while the radio does something else. Several reports describe lost settings that still existed on disk. Others describe correctly persisted values that should never have been saved.

The first useful Flex run is a small, non-keying matrix around Display/ANT controls, band and profile recall, slice recreation, and a real application restart. Icom should run the same mechanics with independently evidenced mode/DATA/filter semantics and explicit unreadable-control outcomes. HL2 supplies especially valuable regression cases for client-owned storage, same-value operator intent, and startup ordering.

This research is a curated project-lifetime sample spanning March–September 2026, not an exhaustive issue count. Closed issues and merged fixes are historical evidence, not a fresh certification of current hardware. Some rows below describe explicit contracts or review-discovered defects rather than separate user-filed bugs.

## Coverage across families

| Family / surface | Current ownership evidence | What `persist` should establish | Important boundary |
|---|---|---|---|
| FlexRadio, including Aurora reports | Operating-state client domains are empty. Adopt radio state and radio/profile recall; client appearance has its own owner. | Radio → model → controls/rendering convergence after band/profile/session transitions, including changes made outside AetherSDR. | Do not add client replay to satisfy an old UX report. Active/TX slice replay was explicitly rejected. |
| Hermes-Lite 2 and devices using its backend | Declared client domains include tuning, passband, rate, RF gain, TX setpoints, AGC and CW. Per-radio `OperatingState` is the sanctioned owner. | Capture, durable commit, startup application and effective DSP/hardware behavior, with per-band and receiver rules kept distinct. | Hardware-wide gain/drive, per-receiver live AGC and flat AGC restart memory have different scopes. |
| Icom | Operating-state client domains are empty. The working memory bank is nevertheless a durable shared client store; native channels are explicit import sources. | Fresh CI-V state and all UI consumers agree; DATA/filter tuples and imported recall metadata survive their intended transitions. | Model-specific readback and topology matter. A bare ACK or optimistic label cannot prove restoration. |
| ANAN-G2 | Baseline explicitly declares no client operating-state domains and no restore implementation yet. | First document uncharacterized retention and exercise startup/application behavior that exists. | Empty domains here are an implementation gap, not proof that firmware persists everything. |
| RTL-SDR | Baseline declares tuning, passband, rate, RF gain and memories. Newly merged #5473 separates connection locator from reported serial. | Restore using the declared identity, flush the old scope before swap, and report actual filter/DSP effectiveness separately. | Anonymous and duplicate-serial devices intentionally share state under the approved contract. `RtlSlices` runtime cutover is still staged. |
| KiwiSDR receive replacement | Local memories and client source preferences; integration can change Flex state underneath. | Source changes, slice recreation and band recall preserve the intended audio/binding state without contaminating other contexts. | Some settings intentionally reset on full restart even though they survive a band change. |
| Sim/demo | Operating-state restore deliberately empty; shared local memories still available. | Runner, report, UI and store workflow testing. | A Sim result is never radio-firmware persistence evidence. |
| Shared client UI and integration settings | AppSettings/scoped feature documents; credentials use the credential abstraction and Keychain. | Layout, display, DSP, audio-device and integration preferences survive their documented lifetime. | A desktop/package/platform regression can affect several families without being a radio protocol bug. |

Source: [capability declarations](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/core/backends/RadioCapabilities.h#L261), the family implementations at the same commit, [RadioStateMemory](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/core/RadioStateMemory.cpp), [active/TX selection decision](https://github.com/aethersdr/AetherSDR/pull/4759#issuecomment-5294212984), and [RTL identity contract, PR #5473](https://github.com/aethersdr/AetherSDR/pull/5473).

## Historical issue and change table

Dispositions are GitHub snapshots at research time. “Reported” means the issue describes the symptom; it does not promote speculative developer notes into a verified root cause. Dates identify the incident or relevant merge, not the date of a new test. Each row's last column is a proposed certification case.

### FlexRadio

| Case | Sources and disposition | What happened / evidence | Transition to cover |
|---|---|---|---|
| F01 | [#5111 Display/ANT persistence audit](https://github.com/aethersdr/AetherSDR/issues/5111), open, Aug 20 | FFT AVG/FPS/RF Gain restart complaints; source audit identifies competing WNB replay and Grid reset mismatch. Current baseline still reads saved WNB and calls the radio setters. AVG/FPS complaints need live reproduction. | Distinctive values → restart → radio/model/menu/rendering comparison; inspect delayed writes and a second pan. |
| F02 | [#2465 rate reassertion](https://github.com/aethersdr/AetherSDR/pull/2465), merged May 8; [#4126 display decoding/reconcile](https://github.com/aethersdr/AetherSDR/pull/4126), merged Jul 9; [#4261 honor radio display state](https://github.com/aethersdr/AetherSDR/pull/4261), merged Jul 18 | Earlier fixes reasserted client display values after radio recall. Later repair removed those copies; adaptive throttling also needed to preserve newer radio/profile values when its cap lifted. | Profile/band recall while throttled → clear throttle → retain the authoritative requested rate, distinguishing it from effective render rate. |
| F03 | [#3263 CW→SSB profile enables hidden squelch](https://github.com/aethersdr/AetherSDR/issues/3263), closed May 29 | Report includes paired AetherSDR/SmartSDR wire transcripts: profile says SQL off, stale applet state sends SQL on, UI remains off. The old attachment's flag survived slice teardown. | Contrasting profiles that recreate the slice; compare SQL state and positive RX behavior after recreation. |
| F04 | [#649 per-band zoom](https://github.com/aethersdr/AetherSDR/issues/649), closed Apr 5; [#1886 band antenna recall](https://github.com/aethersdr/AetherSDR/issues/1886), closed May 1 | Different band spans reverted; bands inherited the previous antenna. These are user symptoms; historical proposed client-copy fixes are not today's authority contract. | Two bands with different span and valid RX/TX port selections, A→B→A and restart. |
| F05 | [#3543 CAT/TCI band recall](https://github.com/aethersdr/AetherSDR/issues/3543), open; [#4876 CAT recentering](https://github.com/aethersdr/AetherSDR/pull/4876), merged Aug 15 | CAT could leave the pan behind. The merged fix recenters out-of-span tunes but explicitly does not issue GUI band-stack recall. | Keep raw-frequency intent separate from band-selection intent; observe documented radio-side recall rather than impose identical outcomes. |
| F06 | [#4543 MIDI BAND](https://github.com/aethersdr/AetherSDR/issues/4543), open; [#4967 native route fix](https://github.com/aethersdr/AetherSDR/pull/4967), merged Aug 27; [#5279 TCI BAND plugins](https://github.com/aethersdr/AetherSDR/issues/5279), open | Same named BAND operation returned a default frequency through one route and the remembered frequency through another. TCI plugins using fixed `vfo:` commands remain a separate case. | Drive the actual UI, native shortcut/MIDI and applicable plugin route. A passing internal bridge action does not cover an external plugin. |
| F07 | [#5081 active/TX slice selection](https://github.com/aethersdr/AetherSDR/issues/5081), open; [#4759 rejected persistence](https://github.com/aethersdr/AetherSDR/pull/4759), closed unmerged Aug 14; [#4985 initial enumeration fix](https://github.com/aethersdr/AetherSDR/pull/4985), merged Aug 27 | User expects the previous selection. Maintainer rejected local save/replay over radio state; the accepted narrow change stops first enumeration from asserting active. | Vary enumeration order and reconnect; each client's selection converges to its own radio-reported state. Record disputed UX expectations separately. |
| F08 | [#3977 stale MultiFlex session adjusts reclaimed pan](https://github.com/aethersdr/AetherSDR/issues/3977), closed Jul 4 | Support evidence attributes changing floor to the earlier session rather than the new client. This is ownership/lifetime failure, not missing disk storage. | Session replacement and ownership transfer; observe current owner's positive changes and stable presentation. |
| F09 | [#3243 CW/SSB filter restart](https://github.com/aethersdr/AetherSDR/issues/3243), closed Jun 10; [#2985 NRS cross-session level](https://github.com/aethersdr/AetherSDR/issues/2985), open | Filter settings reportedly revert; an NRS field reportedly needs different firmware retention treatment. The latter remains field/firmware-specific evidence to verify. | Mode-specific low/high edges and levels, same-session recall versus full restart; do not infer one retention rule for all DSP fields. |
| F10 | [#1560 mute restart](https://github.com/aethersdr/AetherSDR/issues/1560), closed Apr 30; [#336 profile changes output destination](https://github.com/aethersdr/AetherSDR/issues/336), closed Mar 29 | Mute lost on restart; hardware-speaker choice changed to PC output on profile load. Root causes not established here. | Separate local, slice and radio mute from host-device selection and actual audible routing. |
| F11 | [#2043 power after upgrade](https://github.com/aethersdr/AetherSDR/issues/2043), closed May 5; [#3535 profile/PGXL start ordering](https://github.com/aethersdr/AetherSDR/issues/3535), open | Reports of lower power returning to 100 W after update, or incorrect profile power depending on app/radio/amplifier startup order. | Setpoint readback after upgrade, profile load and readiness order. No RF transmission is needed for this comparison. |
| F12 | [#2696 tune-mode lifetime](https://github.com/aethersdr/AetherSDR/issues/2696), closed May 16 | App restart and radio power cycle had different results; operation cleanup could force single-tone instead of the prior choice. | Explicit lifetime per field, including temporary operation entry/exit; radio power-cycle coverage is a separate run. |

### HL2 and devices using that backend

| Case | Sources and disposition | What happened / evidence | Transition to cover |
|---|---|---|---|
| H01 | [#5400 per-band gain overwritten](https://github.com/aethersdr/AetherSDR/issues/5400), closed Sep 6; [#5402 fix](https://github.com/aethersdr/AetherSDR/pull/5402), merged Sep 6 | Band-agnostic startup replay replaced the saved band gain. An explicit connection override could also overwrite the durable start-band entry during capture/departure. Fix has socket-free evidence, not live HL2 validation. | Different legacy/A/B gain values → normal startup → band roundtrip → restart; separate temporary connection-override case. |
| H02 | [#5466 same-value operator intent](https://github.com/aethersdr/AetherSDR/pull/5466), merged Sep 7 | Stored −12, temporary live +20, explicit operator +20: equality return kept the old stored value. Production test failed before repair. | Deliberately confirm an already-live override; durable intent must change even if a hardware write is unnecessary. |
| H03 | [#4909 AGC reset](https://github.com/aethersdr/AetherSDR/issues/4909), closed Aug 15; [#4911 repair](https://github.com/aethersdr/AetherSDR/pull/4911), merged Aug 15 | Missing domain/fields/notification/echo returned AGC to med/65. Reconnect reseeding could flatten different live receiver settings. | Slow/40 and valid zero; distinguish independent live RX states on reconnect from documented flat restart seeding. Applied WDSP evidence must be separate from model echo. |
| H04 | [#5256 CW Mode B](https://github.com/aethersdr/AetherSDR/issues/5256), closed Sep 3; [#5380 repair](https://github.com/aethersdr/AetherSDR/pull/5380), merged Sep 3 | Radio Setup used raw Flex commands, bypassing host model/keyer state. Reopening the dialog already lost the choice. | Real control → dialog reopen → mode/band transitions → restart; iambic enable, A/B, paddle swap and CWU/CWL. No keying required. |
| H05 | [#4619 scoped operating memory](https://github.com/aethersdr/AetherSDR/pull/4619), merged Jul 31 | Defines per-radio restore, reset on an unseen radio, override precedence and per-band hardware gain/drive following the TX-owning slice. Contract evidence. | Radio A→unseen B→A; browsing RX2's band must respect the documented hardware-wide ownership rule. |
| H06 | [#4484 initial mode/passband ordering](https://github.com/aethersdr/AetherSDR/pull/4484), merged Jul 30 | Startup publication preceded correct passband derivation; displayed USB and applied filter disagreed. | Reopen DSP, switch mode and restore a custom filter; ensure mode defaults cannot overwrite the final intended passband. |
| H07 | [#5370 non-Flex TX Delay](https://github.com/aethersdr/AetherSDR/issues/5370), open | Control sent unsupported Flex command; a number that sticks would still not establish amplifier timing. | Capability and functional-application prerequisite. Report unavailable/unfinished control rather than certify storage alone. |

### Icom

| Case | Sources and disposition | What happened / evidence | Transition to cover |
|---|---|---|---|
| I01 | [#5327 memories disappear](https://github.com/aethersdr/AetherSDR/issues/5327), closed Sep 6; [#5328 durable ownership repair](https://github.com/aethersdr/AetherSDR/pull/5328), merged Sep 6 | 151 entries remained on disk but were hidden by the wrong store/capability selection; Import and Tune also broke. Repair explicitly lacks live Icom proof. | Populated bank remains visible, writable and recallable connected/disconnected and after restart. |
| I02 | [#5328 imported memory contract](https://github.com/aethersdr/AetherSDR/pull/5328), merged Sep 6 | GUID + native group/channel provenance, DATA/filter/recallability metadata, annotation preservation and flush-before-success all matter. Some native modes are intentionally display-only. | Sync twice, edit including empty annotation, restart, sync again; preserve manual/CSV rows and source identity. Record unsupported recall honestly. |
| I03 | [#4931 DATA never sent](https://github.com/aethersdr/AetherSDR/issues/4931), closed Aug 19 | IC-9700 trace and front-panel observation showed DIGU/USB and DFM/FM could produce identical writes while the app cached the intended label and received ACKs. | Mode + DATA + filter-slot tuple before/after filter changes, external mode changes and restart; independent readback, not cached neutral mode. |
| I04 | [#5222 IC-705 TX cut controls swapped](https://github.com/aethersdr/AetherSDR/issues/5222), open | Reporter checked the radio menu: low cut changed high cut and vice versa. This establishes the symptom; codec diagnosis is not independently bench-tested here. | Two distinguishable cuts; independently verify target and non-target values before claiming either is persistable. |
| I05 | [#5219 RX Controls stale after reconnect](https://github.com/aethersdr/AetherSDR/issues/5219), closed Aug 25 | Same logical slice ID, replacement model object; one display followed the old object while radio/VFO followed TCI. IC-705 reproduction; shared-path IC-7300MK2 coverage requested. | Reconnect, then external tune; all visible consumers must follow the new session object. |
| I06 | [#4799 connect adoption, credentials and diagnostic scrub](https://github.com/aethersdr/AetherSDR/pull/4799), merged Aug 9 | Saving credentials before connect success could replace known-good credentials. A diagnostic scrub also cleared NR before capturing its old state. | Failed connection preserves usable credentials through the credential API; capture before mutation; connect adopts radio-owned values. |

### Other families and shared client surfaces

| Case | Sources and disposition | What happened / evidence | Transition to cover |
|---|---|---|---|
| K01 | [#4158 Kiwi source on band change](https://github.com/aethersdr/AetherSDR/issues/4158), closed Jul 17; [#4204 repair](https://github.com/aethersdr/AetherSDR/pull/4204), merged Jul 17 | Band recall recreates the Flex slice and drops Kiwi binding. Band-change retention differs from intentional full-restart return to local Flex audio. | Source binding across slice recreation versus full restart; explicit different expected outcomes. |
| K02 | [#4209 Kiwi mute contaminates bands](https://github.com/aethersdr/AetherSDR/issues/4209), closed Jul 23 | Temporary radio-side mute was saved in every visited Flex band. Shutdown cleanup repaired only the current band. Live FLEX-8600 log evidence. | Visit A/B/C, exit replacement source, restart and revisit every touched band; preserve intentionally muted control bands too. |
| K03 | [#4300 Kiwi/diversity volume](https://github.com/aethersdr/AetherSDR/issues/4300), closed Jul 22 | FLEX-6600 report: replacement source change reset the new diversity slice volume to zero. | Source A→B→A with distinct volume/pan/mute values and a second-slice sentinel. |
| R01 | [#4862 RTL baseline](https://github.com/aethersdr/AetherSDR/pull/4862), merged Sep 2 | Restore/reset exists, but the PR explicitly limits what stored passband numbers prove about effective filtering. | Separate stored/readback passband from actual DSP response. |
| R02 | [#5473 RTL device identity](https://github.com/aethersdr/AetherSDR/pull/5473), **merged Sep 8 UTC during research** | Index locator differs from reported serial; approved anonymous/duplicate sharing and unclaimed old locator rows are explicit. Owner/schema preparation does not activate the future slice-store cutover. | USB reorder/retry and same-family swap under the approved identity/fallback rules; report non-migration rather than silently infer identity. |
| A01 | [#5143 ANAN asynchronous DSP startup](https://github.com/aethersdr/AetherSDR/pull/5143), merged Aug 31 | Preserve requested mode/filter/AGC through DSP construction. No dedicated ANAN persistence incident found in scoped research. | Connect and asynchronous readiness preserve requested state; inventory absent durability contracts as gaps. |
| U01 | [#4590 local memories](https://github.com/aethersdr/AetherSDR/pull/4590), merged Jul 30; [#4623 shared bank](https://github.com/aethersdr/AetherSDR/pull/4623), merged Jul 31 | HL2/Kiwi/Sim memory saves originally used dead Flex commands. The resulting local bank is deliberately shared across radios; empty store suppresses reimport. | Add/edit/remove/recall/restart; cross-family sharing; delete-all stays empty; migration does not resurrect deleted data. |
| U02 | [#733 palette persistence](https://github.com/aethersdr/AetherSDR/issues/733), closed Apr 5 | FLEX-8400 report: palette changed visibly but reverted on restart. This is client appearance, not radio state. | Palette/appearance change, render, save, restart and render again, with pan/source scope checked. |
| U03 | [#605 band-plan upgrade](https://github.com/aethersdr/AetherSDR/issues/605), closed Apr 5; [#3358 button bar/band plan](https://github.com/aethersdr/AetherSDR/issues/3358), closed Jun 5 | Region/labels, hidden buttons and Band Plan Off were lost. #605 reporter confirmed resolution; the atomic-save explanation remained a hypothesis. | Off/false values, toolbar visibility/order, region/font preferences across normal quit and upgrade. |
| U04 | [#2896 dock-side restore](https://github.com/aethersdr/AetherSDR/issues/2896), closed May 22; [#4980 minimal/canvas handoff](https://github.com/aethersdr/AetherSDR/pull/4980), merged Aug 14 | Saved left-dock choice was not applied on redock. Review found temporary canvas disable erased restart intent; final review records restart/bridge repair evidence. | Float/redock, temporary layout transition, quit while temporary, restart, queued re-entry; geometry and indicator agree. |
| U05 | [#4592 manual squelch memory](https://github.com/aethersdr/AetherSDR/issues/4592), closed; [#4049 adaptive filter old-station restore](https://github.com/aethersdr/AetherSDR/issues/4049), closed Jul 5 | Auto values can overwrite manual intent; same frequency does not mean same station. These are transient-state scope failures. | Manual→auto→manual; switch slices; invalidate derived state when its source/lifetime changes. Deterministic signal cases belong offline. |
| U06 | [#3639 packaged Keychain access](https://github.com/aethersdr/AetherSDR/issues/3639), closed Jun 19; [#1610 serial parameters](https://github.com/aethersdr/AetherSDR/issues/1610), closed Jun 21 | Source build could use saved credential while Linux AppImage could not; serial parameters reverted on reopen/reconnect. | Packaged application + credential availability, device/port reopen; no plaintext credential comparison or unintended DTR/RTS keying. |
| U07 | [#4273 process survives exit, VFO/EQ reset](https://github.com/aethersdr/AetherSDR/issues/4273), open; [#5190 persistent ASR crash configuration](https://github.com/aethersdr/AetherSDR/issues/5190), open | Intermittent incomplete shutdown report and a separate crash-on-every-launch recovery proposal. Neither body proves a new general persistence root cause. | Confirm process exit; restart/recovery are distinct. Fault injection and crash recovery need an isolated lane, not the first live radio run. |

## What existing radiocert covers, and what is missing

At the source baseline, `RadioCertification::Phase` is `Tune, Rx, Tx, Meters, All`. There is no persist phase. `stageLifecycle()` measures RX audio before/during/after transmission; it does not disconnect or restart the client. The current epilogue captures selected runtime values but is not a multi-context persistence journal. [Source: phase and options](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/core/RadioCertification.h#L69), [lifecycle](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/core/RadioCertification.cpp#L1752), [run cleanup](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/core/RadioCertification.cpp#L1818).

Reuse `get radio/slices/pans/transmit/display`, `dumpTree`, scoped `invoke`, connection actions and existing workspace controls. `get display` already distinguishes stored intent from effective capability-masked appearance. Use model verbs for protocol diagnosis, but drive at least one actual operator-facing entry point for certification; H04 and F06 would otherwise escape. [Bridge documentation](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/docs/automation-bridge.md#get-display).

The existing storage seams and socket-free tests already cover substantial policy: `RadioStateMemory`, scoped exact/fallback reads, version guards, and `radio_state_memory_test`, `hl2_gain_restore_test`, `bandstack_scoped_test`, memory import/recall tests, and squelch-memory tests. Their presence is not evidence they ran in this research. Client operating-state capture uses a 2-second trailing timer and a 10-second maximum wait, plus explicit quit/disconnect paths. A restart probe should exercise those paths, not force an extra save that masks a missing production flush. [Capture/flush code](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/models/RadioModel.cpp#L443), [timer setup](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/src/models/RadioModel.cpp#L2552).

The prior [universal radiocert RFC #5342](https://github.com/aethersdr/AetherSDR/issues/5342) remains open. Its discussion specifically separates lifecycle/restart/ledger work from the smaller meter changes. This document supplies evidence for that follow-up; it does not assume the whole earlier RFC is implemented or approved.

## Proposed design direction for discussion

### 1. Define an expectation before changing the setting

Use universal transition runners with a small, versioned per-setting contract. Capabilities decide whether a control exists; they do not by themselves prove how it should persist. Do not derive the expected result solely from the production persistence implementation under test.

Each contract needs:

- **Setting and semantic identity:** family, model/firmware applicability, radio identity, client/station identity where relevant, receiver/VFO, pan/display source, band/mode/filter slot/profile as needed. Record logical identity separately from session object IDs.
- **Owner and scope:** radio, client feature document, intentionally shared local document, or transient runtime state. Empty capability domains must not be misread as proof of on-radio storage.
- **Transition meaning:** keep value; recall destination context; accept newer radio/external state; reset temporary state; intentionally share; or unknown.
- **Evidence basis:** accepted product decision, model-specific official guide, current source contract, or characterized hardware observation with firmware and date. A user report awaiting a decision is a separate class.
- **Observability:** independent radio readback, store revision/value, model, widget, renderer/DSP or physical effect; list unavailable layers explicitly.
- **Normalization and timing:** legal range, discrete choices, quantization/tolerance, expected completion evidence and bounded settling budget.

For Flex, linked mode/microphone and antenna/transmit-profile relationships are documented historically in the [FlexRadio Profiles How-To Guide](https://edge.flexradio.com/www/uploads/20200818184953/SmartSDR-Profiles-How-to-Guide.pdf). That guide is from 2016: use it to identify interactions, not as a complete 2026 firmware oracle. Confirm the actual test station's profile associations and current firmware semantics before approving expected values.

For Icom, existing certification documentation requires mode/DATA/filter readback together and warns that IC-7300MK2 RX-ANT has no usable state readback on the observed firmware. A restored-looking dropdown cannot establish antenna persistence. Use model-specific guide evidence and, where needed, operator/front-panel observation. [Icom certification notes](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/docs/radio-certification.md#L405).

### 2. Separate the four questions

For each setting, report separately:

1. **Recorded:** did the intended durable owner capture it, where that owner is observable?
2. **Reacquired/applied:** after the transition, did fresh authoritative state or the actual backend/DSP configuration reach the expected value?
3. **Presented:** do the visible controls and effective rendering/audio state agree?
4. **Contained and restored:** did only the intended contexts change, and did cleanup restore every test-owned mutation?

For radio-owned values, a client database row is not required. For hardware without readback, a command plus local echo is weaker than an independent effect measurement. “Recorded and displayed” cannot be upgraded to “effective on hardware” without that evidence.

Use per-claim outcomes `ESTABLISHED`, `CONCERN`, `INCONCLUSIVE`, `NOT RUN`, and `NOT APPLICABLE`, compatible with the existing diagnostic direction. Unknown ownership/semantics should be explicit metadata, not a silent skip or a green result. Export coverage denominators, including applicable-but-unexercised settings. Avoid one blanket “radio certified” badge.

### 3. Use discriminating transitions

| Scenario | Sequence | Main failure classes |
|---|---|---|
| P01 Control reconstruction | Change → close/reopen applet/dialog → inspect | UI-only state; disconnected save/apply signal |
| P02 Mode round trip | Mode A/custom filter → B/distinct state → A | Default ordering, mode groups, DATA loss, filter slots, RTTY/CW edges |
| P03 Band round trip | Band A → B → A; repeat in reverse order | Wrong band scope, stale replay, antenna/gain/drive/span recall |
| P04 Display and source | Pan/source A and B with distinct preferences; toggle view, throttle and clone/reset when applicable | Primary-pan-only restore, mixed radio/client fields, intent versus effective value |
| P05 Antenna/profile association | Approved RX/TX port or existing profile A → B → A | Legitimate linked changes versus collateral overwrite; port labels versus actual state |
| P06 Slice/VFO lifecycle | Distinct A/B slices → change focus → recreate one → reconnect | Reused numeric IDs, stale widget attachment, active versus TX owner |
| P07 Graceful restart | Set → ordinary user quit immediately; repeat after normal settling → verify exit → same-profile relaunch | Missing save/flush; late startup defaults; duplicate old process |
| P08 Disconnect/reconnect | Set distinct receiver states → disconnect → immediate reconnect | Session teardown, reseeding live state, subscriptions and readiness |
| P09 External newer state | Disconnect client → change via authorized front panel/second client → reconnect | Stale client copy winning over fresher radio state |
| P10 Identity/scope | Radio A → unseen B → A; optional device reorder | Scope changes before flush, unstable locator identity, approved sharing/fallback |
| P11 Memory workflows | Add/import/sync → edit → restart → recall → repeat Sync | Hidden store, provenance, annotations, recall metadata, deletion resurrection |
| P12 Temporary automation | Manual value → automatic/temporary override → exit → revisit all touched contexts | Auto values stored as intent, stale cleanup overriding newer action |
| P13 Store/platform lane | Previous-release data → upgrade; future/corrupt/read-only state; interrupted commit | Atomicity, compatibility, failure reporting, package-specific storage access |
| P14 Radio power-cycle lane | Verified baseline → controlled radio reboot → reacquire | Firmware durability; distinct from client restart and reconnect |

P13 belongs mainly in isolated storage/offline/platform tests. P14 and multi-client/physical-device cases require their own run setup. Neither should silently enter the first Flex sweep.

Use distinct non-default A/B values and an unchanged sentinel context. Include valid zero, false/off, empty annotations, and explicit same-value intent. Avoid unnecessary limit values on live hardware. Run directed historical cases first, then capability-valid pairwise combinations; save the seed and order for reproducibility. Different action orders matter: mode→filter and filter→mode need not have the same intended result.

### 4. Restart must be supervised outside the app

A small external runner should retain the plan, expected values and journal while the app exits. It should launch the selected binary with the same isolated settings profile and intended station/client identity, discover the fresh bridge endpoint, and verify both old-process termination and a new session before sampling. A new empty profile on every launch would invalidate the persistence test; a hidden surviving process could invalidate it in the opposite direction.

Capture the first fresh state and a bounded later timeline. Require readiness/freshness evidence rather than one fixed sleep; include relevant radio echoes, object replacement and DSP readiness. Measure production save timing without injecting a special pre-restart save. A later, separate crash lane can test recovery and last-committed-state semantics; a crash before an acknowledged commit is not automatically loss of promised durability.

This is a concrete new orchestration gap. Reuse existing bridge reads/actions first; add only missing read-only provenance (owner/scope, fresh-versus-optimistic state, commit outcome, effective DSP state) demonstrated necessary by these cases. Do not create a second generic settings writer just for certification.

### 5. Treat cleanup as part of the experiment

Before each mutation, journal the original value, owning context and dependency group. Persist the runner journal atomically outside the process being restarted. Track every band/profile/slice/source touched, including side effects of linked profile loads. Capture the failing evidence **before** cleanup.

Restore in a dependency-aware order through authorized production controls and verify every touched context. Do not restore an entire stale database over newer user changes. If another operator or client changes state, stop the conflicting scenario and preserve that newer intent; report cleanup as incomplete instead of forcing the old baseline back. Unreadable original state needs a manual baseline/return procedure or a non-mutating case.

The first run is non-keying. Power/antenna/profile/CW/VOX-related cases are setpoint or readback cases only, with station readiness and linked side effects considered in the plan. Restoration must never replay PTT, TUNE, an ATU cycle, queued CW/packet audio, or other transmit intent. Existing locks and live-radio authorization still apply. Live quiet observations do not prove a universal absence of unwanted commands: command-refusal/non-event guarantees remain socket-free injected-transport tests, as required by the [project test-layer boundary](https://github.com/aethersdr/AetherSDR/blob/ab03be0629a498603ff02ef68a7dbfbd0de91d10/AGENTS.md#test-layer-boundary--where-an-assertion-lives).

### 6. Make failures useful for the next fix

Every concern should carry: app commit and package/OS, radio model/firmware and privacy-safe identity, contract revision, exact action route, starting context, expected versus observed values at each layer, timestamps/session identity, collateral changes, and cleanup outcome. Attach a minimal replay sequence; do not automatically file issues.

Useful diagnosis categories are `never recorded`, `wrong scope`, `not applied`, `overwritten later`, `stale consumer`, `uncharacterized expectation`, and `insufficient independent observation`. These describe evidence, not automatically proven root causes. A failed prerequisite should mark its dependent persistence claims inconclusive instead of producing twenty redundant bugs.

Compare runs by stable setting/scenario identity and contract version. Report newly established coverage, regressions, newly missing observations and intentionally changed expectations separately. Keep historical bug sequences as reusable regressions, with a socket-free counterpart for the causal policy/encoding where appropriate.

## First two runs

**Flex first:** start with one radio, one existing station identity, two valid bands and one pan. Prioritize F01/F02: AVG, FPS, weighted average, waterfall duration, RF gain, WNB and client-only Grid/palette. Add a second pan to expose scope mistakes; then contrasting mode/filter/SQL and existing profile transitions. Follow with graceful immediate/settled restart and ordinary reconnect. Finally add a separately prepared MultiFlex/external-change case. Keep raw CAT frequency moves and explicit BAND commands as different contracts.

**Icom second:** retain the same runner. Start with one characterized model and verify mode/DATA/filter tuple, front-panel convergence, fresh consumer attachment after reconnect, and shared memory visibility/recall. Distinguish IC-705, IC-7300MK2 and IC-9700 profile facts without branching the whole test program by model. RX-ANT without readable state stays inconclusive for automated retention; unsupported native memory recalls remain inapplicable. Do not use the first model's observed semantics as another model's certification.

**Next HL2 pass:** use the existing incident-derived cases for per-band gain, temporary override/same-value intent, AGC reconnect versus restart, CW dialog controls and custom passband order. This is the strongest test of whether the runner truly supports client-owned persistence as well as Flex/Icom radio authority.

## Decisions to make after reviewing these findings

1. Approve the initial setting contracts and expected transitions, especially disputed Flex active/TX selection and raw-QSY versus BAND behavior. The existing authority decisions remain the baseline.
2. Choose the smallest initial matrix: the proposed Flex Display/ANT + mode/filter + restart cases would directly target the open audit without turning this into every integration test at once.
3. Agree on evidence strength and how inconclusive/uncharacterized rows appear. A surviving slider value should never imply effective RF/DSP behavior.
4. Approve the external restart supervisor and journal as separate implementation scope, then expand through incident-driven gaps.

## Research method, unresolved evidence and verification

Discovery used live GitHub issue/PR searches across all states for persistence/restore/remember/reset/restart/profile/band terms plus family inventories, followed by issue bodies, resolution/review comments, related changes and current source reads. Two bounded research lanes covered Flex and the other families while the coordinator examined shared UI/storage and radiocert. Search lists were capped (typically 100); related records were deduplicated by failure class. Repeated test-failure tracker issues were not counted as independent persistence incidents.

The highest-impact source claims were independently re-read: #5111, #3263, #4759's final decision, #4876, #5402/#5466, #5328, #4931, #5222, #4209 and the live #5473 disposition. Historical AI-generated suggestions were not treated as policy. Web research identified official Flex documentation; the current large user-guide PDF exceeded the browser fetch limit, so the older readable profiles guide is explicitly dated and is not used to assert current per-field firmware retention.

| Material question | Evidence and confidence | Remaining gap / next action |
|---|---|---|
| Is there already a persistence phase? | High: enum, dispatcher, lifecycle and bridge docs inspected at pinned SHA. | No live execution needed to establish this source fact. |
| Are current reported Flex failures reproduced? | WNB competing replay path is present in baseline source; historical field reports support prioritization. | Fresh radio/model/UI/command timeline, especially AVG/FPS and Grid. |
| Can current Icom readback independently prove every setting? | High: DATA incident and explicit RX-ANT readback limitation show it cannot. | Per-model guide/front-panel evidence and observation metadata. |
| Are closed/merged cases now correct on hardware? | Historical validation varies; several PRs explicitly lack live-radio proof. | Run the proposed cases on exact app/firmware combinations. |
| Are all devices uniquely isolated? | No universal rule: shared MemoryBank and approved RTL identity sharing are explicit counterexamples. | Enumerate contract scope and identity quality in each run. |
| Is the proposed matrix exhaustive? | No; it spans the recurring failure classes and all current families. | Expand from uncovered settings and new failures, not issue-count claims. |

Stop point: additional broad searches were yielding variations of the same failure classes; each proposed major scenario has source support or an explicit evidence gap. The next useful evidence is agreement on expected semantics and a bounded live Flex run.

**Validation of this deliverable:** documentation structure, source links, case identifiers and Git diff checked. No application code changed; no compile, CTest, simulation, CI dispatch, or live-radio/TX run was performed. No GitHub issue, review, comment or PR was posted.
