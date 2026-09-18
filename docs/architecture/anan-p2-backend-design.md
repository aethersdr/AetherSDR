> **Status note (updated after RFC #4970 approval for the Phase 1b RX
> skeleton):** this file is
> the original RFC #4970 proposal, kept as the historical design record and
> NOT rewritten to track the shipped implementation in every detail
> (`AnanDsp` below has been corrected to its shipped name, `AnanRxDsp`, but
> the surrounding narrative and section 5's open questions are otherwise
> preserved as originally written). What's actually settled since:
> - Open question 4 (advertised-DDC-count clamping) shipped as recommended:
>   clamp to what Discovery reports, never probe past it.
> - Open question 3 (connect-shape) shipped as a dedicated ANAN section in
>   `ConnectionPanel`, not a `RadioConnectRequest::params` negotiation.
> - The maintainer approved RFC #4970 for this receive-only experimental
>   phase. The original questions below remain part of the historical record,
>   but they are no longer unresolved landing gates for Phase 1b.
>
> For current behavior, use the backend sources and registered ANAN tests;
> this archived proposal is not the implementation authority.

### Preflight

- [x] I have read [GOVERNANCE.md](https://github.com/aethersdr/AetherSDR/blob/main/GOVERNANCE.md) and confirmed this change requires an RFC
- [x] I have searched existing issues and this RFC has not been proposed before
- [x] I have not opened a PR for this change yet

### Problem

Continues #78, which asked for openHPSDR Protocol 1 *and* Protocol 2 and was
closed once the Protocol 1 / Hermes-Lite 2 work landed. This proposes the
Protocol 2 half: a fifth `IRadioBackend` implementor, `AnanBackend`, for Apache
Labs ANAN transceivers, built clean-room in the same manner as the HL2 backend.
Structurally follows `docs/architecture/aetherd-hl2-backend-design.md`, because
the two sit on the same branch of the seam.

I have an ANAN-G2 on the bench and will run the full `radiocert` bring-up against
it once this RFC is approved.

---

## 1. Problem

### 1.1 An ANAN cannot be reached by relaxing a constant

The P1 layer that exists is HL2-shaped in the places that would matter:

- `Hl2Discovery.cpp:213` rejects any discovery reply whose board ID is not `0x06`
- `ocFilterByteForHz()` (`MetisProtocol.cpp:45`, declared `MetisProtocol.h:284`)
  encodes the N2ADR companion board's one-hot filter map; an ANAN has Alex filter
  banks instead. This is not just a table of wrong values — the function's
  contract *is* that accessory board. `tests/hl2_metis_protocol_test.cpp:144-166`
  asserts the mapping against specific band edges (1.8/2.0, 3.5/4.0, 7.0/7.3,
  14.0/14.35, 28.0/29.7 MHz) and asserts `kOcNone` at 600 kHz and 50.15 MHz for
  "no filter fitted". An ANAN would fail those assertions by design.
- `kC0AdcGain` is the AD9866 LNA register; ANAN has step attenuators
- Register `0x0e` is sent as zeros — `MetisProtocol.h` already documents that
  this address means per-DDC ADC assignment on generic openHPSDR and TX LNA gain
  on the HL2, and that the generic meaning is the one a multi-ADC device needs
- `MetisProtocol.cpp` records that ATU tune, Alex filters and VNA all stay zero

None of that is wrong — the backend was built for the radio in front of it. It
does mean an ANAN needs its own backend.

### 1.2 The radio, measured

Everything below about wire behaviour is grounded in a live discovery capture
rather than in the spec alone. The probe sent discovery only — no General packet,
no `run` bit, no PTT.

**ANAN-G2, Protocol 2 discovery reply, 60 bytes** (MAC redacted as `xx`):

```
byte:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
      00 00 00 00 02 xx xx xx xx xx xx 0A 2B 1B 00 00

byte: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
      00 00 00 00 04 01 00 2E 00 00 00 00 00 00 00 00

byte  raw  decodes as
----  ---  --------------------------------------------
   4   02  reply; radio not currently streaming
5-10   xx  radio MAC
  11   0A  board type 10 = SATURN (ANAN-G2)
  12   2B  protocol version 4.3 (tenths)
  13   1B  firmware/gateware version 27 (integer)
  20   04  4 DDCs advertised
  21   01  frequencies as phase word, not Hz
  22   00  little endian
  23   2E  46 - p2app build number (see 2.2)
```

The three that matter most downstream: **4 DDCs**, **phase words not Hz**, and
**firmware 27** — each load-bearing in 2.3, 2.4 and Gap C respectively.

### 1.3 Protocol 1 is out of scope, and this is measured

The bring-up radio runs the Saturn stack: the XDMA PCIe driver plus `p2app`, no
piHPSDR installed. `p2app` filters its port-1024 receive path by length —

> `p2app.c:910` — *only process packets of length 60 bytes on this port, to
> exclude protocol 1 discovery for example.*

— and P1 discovery is 63 bytes. `sw_projects/P1_app/` is a **separate binary**
with its own reply template; it is not loaded here. **Protocol 1 discovery to the
same address drew no reply**, confirming the source reading.

Generalising the existing P1 layer past board ID `0x06` remains worth doing, as a
separate RFC by whoever has a P1-capable ANAN on the bench. This project does not
ship radio code that has not been certified against the radio, and I cannot
certify P1.

### Proposal

## 2. Proposal

### 2.1 Protocol 2 is a different wire stack, not an extension of Protocol 1

The most important thing for a reviewer to internalise, because "add ANAN" sounds
like it should reuse `MetisProtocol`/`MetisClient` and it reuses none of it.

| | Protocol 1 (existing) | Protocol 2 (this RFC) |
| --- | --- | --- |
| Sockets | one, UDP :1024 | ~10 ports, distinct packet types |
| Control | C0 address + C1–C4, round-robin banks | typed packets: General, DDC-Specific, DUC-Specific, High Priority |
| Keying | C0 bit 0 (MOX) on every frame | `run`/PTT bits in the High Priority packet |
| Receivers | up to 12, sharing one EP6 frame in rounds | up to 80 DDCs, each on its own port |
| Link | 100BASE-T (`MetisProtocol.h:107`) | higher rate; exact link requirement **to be measured**, not asserted |
| Rates | 48/96/192/384 kHz | 48/96/192/384/768/1536 ksps, settable per DDC |

Default port map, from the openHPSDR Ethernet Protocol v4.4 spec (all overridable
via the General packet):

```
1024  discovery, General packet
1025  DDC-Specific (to HW) / High Priority (to PC)
1026  DUC-Specific (to HW) / mic samples (to PC)
1027  High Priority (to HW) / wideband ADC0 base (to PC, +1 per ADC, 8 max)
1028  DDC audio
1029  DUC I&Q
1035  DDC0 data base (+1 per DDC, up to DDC79)
```

The `run` bit and PTT live at High Priority byte 4 (`[0] = run`, `[1] = PTT0`).
A session is Discovery, then a General packet, then High Priority with `run` set.

### 2.2 Where the spec and the hardware disagree

Two field decodings in 1.2 were wrong when taken from the spec, and were
corrected against the radio-side source and the live capture. Both surfaced on
the very first packet this project ever decoded from an ANAN — which is the
argument for the provenance model in 2.9 rather than an aside.

- **Byte 13 is an integer, not tenths.** The spec's worked example implies a
  decimal reading. `p2app` writes `GetFirmwareVersion()` there, and its template
  comment reads *"this SDR firmware version. >17 to enable QSK"* — a `>17`
  threshold is only meaningful against an integer. On the G2, 27 is the FPGA
  bitstream number, matching `saturnprimary2024V27.bin`.
- **Byte 23 is "beta version" per spec, but `p2app` overwrites it** with
  `P2APPVERSION` (`p2app.c:69`, currently 46). A client must not read it as a
  beta flag on a Saturn board.

Both belong in the backend's inline field citations. Section 5 asks whether this
project or I should report them upstream.

### 2.3 Firmware version is a capability gate

`p2app.c:854` spawns the wideband data threads only `if (Version >= 18)`, and a
comment at the firmware-read site notes TX scaling changed at V13. Byte 13 is
therefore not decoration — `AnanBackend` must read it and branch, exactly as the
radio-side application does. This is the first place a naive client silently
misbehaves on older gateware.

### 2.4 Frequencies are phase words on this board

Byte 21 = 1. Not a menu option — the radio's stated expectation. The conversion
is `delta = 2^32 × F / Fs` with `Fs = 122.88 MHz` (`saturnregisters.c:719–741`,
`VSAMPLERATE`). `setSliceFrequency` and `setTxFrequency` need this path.

### 2.5 What `AnanBackend` owns

Mirrors `Hl2Backend`'s ownership shape. The backend owns its wire objects and
their worker threads, and lives entirely below the seam under
`src/core/backends/anan/`.

```
AnanBackend : IRadioBackend
├── P2Client        (the multi-socket UDP wire — discovery, General/DDC/DUC/HP
│                    egress, DDC IQ + audio + mic ingest; owns its sockets and
│                    RX thread)
│      emits: iqBlockReady(ddcIndex, block), linkUp/linkDown, dropStats
│      accepts: setDdcFrequency(), setDdcRate(), setDucFrequency(),
│               setDrive(), setAlex(), setAttenuator(), setRun(), setPtt()
└── AnanRxDsp       (engine-side fixed-block worker; one WdspChannel RX per
                     active DDC, plus the TX chain)
       out: demodulated PCM, FFT bins, meter levels
```

Like HL2 and unlike Flex, this is the **"owns a DSP chain"** branch of the seam:
the radio ships raw IQ and the client does all tune/decimate/demodulate/FFT work.
That branch is already proven by `Hl2Backend`, which is the main reason this is a
tractable RFC rather than an architecture project.

`P2Client` and `AnanRxDsp` meet through a bounded SPSC queue per DDC, with
starvation counted as a zero-IQ gap and overflow dropping a whole oldest block —
the same contract `Hl2Backend` documents, for the same reason.

**No new external dependencies.** Qt networking and the already-vendored WDSP
only. (Noted because new dependencies are independently RFC-triggering under
GOVERNANCE.md.)

### 2.6 Seam mapping

**Intents down**

| Interface verb | P2 realization |
|---|---|
| `connectRadio(req)` | Discovery to `req.host:1024`, then General packet establishing ports and rates, then High Priority with `run` set. `connected()` on first DDC data. |
| `disconnectRadio()` | High Priority with `run` clear; stop threads in reverse order. |
| `setSliceFrequency(id, hz)` | DDC-Specific packet, NCO for the bound DDC. **Phase word** per 2.4. |
| `setSliceMode(id, mode)` | Engine DSP — `WdspChannel` mode, not a wire command. |
| `setKeying(on)` | High Priority PTT bit. **Gated** — see 2.10. |
| `setTxFrequency(hz)` | DUC-Specific packet. Phase word. |
| `setRfPower(pct)` | High Priority drive level. |
| band change | Alex filter data + open collectors in the High Priority packet. |

**State up.** DDC IQ → `AnanRxDsp` → `audioFrameReady`, spectrum bins,
`meterUpdate`. High Priority *status* packets carry forward and reverse power,
supply volts, ADC overflow and the 10 MHz reference PLL lock (spec v4.4 change
log and Appendix A; **not yet observed on this radio** — the 1.2 capture was
discovery only). Those map onto existing `MeterDef` rows wherever the definitions
already exist; which ones actually arrive is a phase 2 finding.

### 2.7 Capabilities

Initial `RadioCapabilities` for a G2-class board, driven by what discovery
reports rather than hardcoded per model:

```
canTransmit            true   (gated, phase 3 — see 2.11)
hostModulates          true   (client-side WDSP TX, like HL2)
takesTxAudioOverSeam   true
hasRadioSideDsp        false
radioOwnsDbmScale      false  (client computes it from raw IQ)
persistsMemories       false  (host-side, like HL2 design note §4a)
maxSlices / maxPanadapters  from discovery byte 20, clamped — see Gap C
hasTuner               ?      (see below)
hasAmplifier           ?      (see below)
hasSupplyVoltageTelemetry  true
hasSelectableMicInputs ?      (see below)
hasProfiles            false
hasMultiClientSessions false
```

Every line is a claim to **verify at bring-up**, not to assert. HL2 lesson §1.14
(`docs/CERTIFICATION.md:171`) — a generic tool that hardcodes one radio's facts
reports false defects — applies to capability tables at least as strongly as to
diagnostics.

**Unverified lines, flagged rather than asserted.** I have not fetched the Apache
Labs G2 manual. `hasSelectableMicInputs`, `hasTuner` and the PA telemetry lines
are inferred from `p2app`'s `AriesATU.c` and `GanymedePAControl.c` existing,
which shows the *software* supports those accessories, not that this radio has
them fitted. All are phase 2/4 findings.

**Board identity is a runtime option on this platform.** `p2app.c:588` accepts
`-i saturn` / `-i orionmk2` and changes the reported board type accordingly.
`AnanBackend` must therefore not key behaviour off board type alone; the
capability bytes and firmware version are the load-bearing fields.

### 2.8 Structural gaps Protocol 2 forces

Named up front, in the spirit of the HL2 note's "two structural gaps" section.

**Gap A — multiple sockets per session.** Every existing backend opens one
connection. `ConnectionPanel`'s manual-IP path and the family-selection flow
assume a host and a port. P2 needs a host and a negotiated port *set*. Probably
small, but it should be designed rather than discovered.

**Gap B — the wideband ADC stream has no consumer.** P2 offers up to 8 wideband
(bandscope) streams, and on this radio they exist only at firmware ≥ 18 (2.3).
Nothing in AetherSDR consumes a full-ADC-width spectrum today. Out of scope for
v1; flagged so nobody assumes it comes for free.

**Gap C — DDC count: three numbers, and they disagree.** The spec allows 80. The
UI supports 8 panadapters. Discovery advertises **4**. The Saturn register layer
declares `VNUMDDC 10` (`saturnregisters.h:22`), so the gateware carries more DDC
registers than `p2app` advertises. The backend must clamp to the **advertised**
count and record which limit bound it. `MetisProtocol.h`'s rule — never assume
the receiver count, always clamp against the board's own report — applies
verbatim, and this radio is a live example of why.

**Gap D — per-DDC sample rates, plus interleave.** `SetP2SampleRate()`
(`saturnregisters.c:561`) takes a per-DDC rate from 48 to 1536 ksps **and** an
`InterleaveWithNext` flag, with all DDCs committed in one register write. Where a
global sample rate forces every receiver to be rebuilt on a bandwidth change,
per-DDC rates need not inherit that — but the batched register write means a rate
change is still a multi-DDC transaction, not an independent per-slice one.

**Gap E — the abrupt-exit hazard, unverified for P2.** `Hl2EmergencyStop.h`
exists because an HL2 that is not told to stop keeps streaming at a host that is
gone, then stops answering discovery — alive at the network layer, invisible to
every client, needing a physical power cycle, reproduced three times with
`kill` on the client process. Whether `p2app` behaves the same way when its peer vanishes is
**not yet established**. Phase 1a should determine it deliberately rather than by
accident, and if it does, P2 needs an async-signal-safe stop path of its own — a
High Priority packet with `run` clear, sent from a signal handler.

### 2.9 Clean-room provenance

Following `THIRD_PARTY_LICENSES`' Protocol References model, `CONTRIBUTING.md`'s
clean-room requirement, and CONSTITUTION Principle I's spirit as applied to
non-Flex protocols by the HL2 backend.

**Licence status — checked, clear.** `laurencebarker/Saturn` carries a full GPLv3
`LICENSE`, and per-file headers agree (e.g. `OutDDCIQ.c`: *"copyright Laurence
Barker November 2021 / licenced under GNU GPL3"*). `p2app` lives inside that same
repository under the same licence, as does the FPGA RTL — 125 Verilog/SystemVerilog
sources outside the testbenches, plus the Vivado project script, not merely the
`.bin` bitstreams.

Allowed inputs, in precedence order:

1. **openHPSDR Ethernet Protocol v4.4** (Phil Harman VK6PH) — primary authority
   for packet layouts, port map, discovery board table. Demonstrably not
   sufficient on its own (2.2).
2. **Saturn FPGA RTL** — the authority for what the hardware actually decodes,
   which is where the HL2 work found the spec's gaps.
3. **`p2app`** — the radio-side Protocol 2 server, and **the counterparty on the
   wire**: it is the program this backend will be talking to. Reading it to learn
   what the hardware does with a field has the same standing pihpsdr had for the
   HL2 layer. Worth naming separately from item 5, since the two sit differently
   with respect to this backend.
4. **Apache Labs G2 user manual** for hardware behaviour, mic and PA specifics.
5. **Other host-side clients** — NereusSDR, Thetis, piHPSDR, deskHPSDR, Zeus SDR
   station-engine — consulted for *what to check*, never copied. The same
   standing they had during the HL2 work.

**Disclosure, stated up front.** Before writing this RFC I read NereusSDR's
`P1RadioConnection` and `P2RadioConnection`, while working out whether porting
them was the better route (see 4, alternative A). NereusSDR is a host-side
client, so that is a closer relationship to what this backend would be than item
3 above, and a weaker starting position than the HL2 work had. It seems better to
say so than to leave it for someone to notice later. Mitigations proposed, for
maintainers to accept or reject:

- No NereusSDR checkout on the development machine or in any AI-assisted session
  while the backend is written.
- Implementation written against the spec, the RTL and `p2app`, with source and
  section cited inline at each non-obvious field — the practice `MetisProtocol.h`
  already follows, which also makes provenance auditable after the fact.
- If a maintainer would rather this work be done by someone who has not read that
  code, that is a legitimate call and better made now than at review.

### 2.10 Bring-up, hardware and certification

`radiocert` exists and has already brought up a second family (Icom CI-V), so
this backend starts with an executable procedure rather than an exploration.
Phases run in dependency order — `tune`, `rx`, `tx`, `meters` — and nothing
earlier may lean on a meter.

Available here: the ANAN-G2 itself, a dummy load, and an SDRplay RSP1B with SDR++
as a genuinely independent receiver. Not available: any other ANAN model, or a
Protocol 1 radio (1.3).

Bench conditions for the G2, which transmits at a power level no previous
bring-up on this project has involved:

- **Dummy load for every keyed stage.** No antenna.
- **External PA out of line.** The station amplifier disconnected or in standby
  for the entire bring-up; nothing in `radiocert` should reach it.
- `AETHER_AUTOMATION_TX_MAX_POWER` clamped low. Per the PR #4487 review thread
  this is honoured for the whole run with a scope-guard restore; I have read the
  thread but not the implementation.
- `tune` and `rx` first — neither keys, and neither needs TX permission.
- Confirm no other host holds a session. Discovery byte 4 reads `3` instead of
  `2` when the radio is already streaming; the 1.2 capture read `2`.

**The one check `radiocert` structurally cannot make, this bench can.**
`stageSideband`'s own observation text records that a shared RX/TX inversion is
invisible by construction and that only an unrelated receiver settles it;
`consumer-agreement` remains an operator check pending a spectrum tap through the
seam (`RadioCertification.cpp:604`). The RSP1B is that unrelated receiver. Both
checks will be run manually and the results reported, because on a new wire
protocol IQ handedness is a fresh decision rather than an inherited one — exactly
the condition under which the HL2 inversion survived a fortnight.

### 2.11 Out of scope for v1, and phasing

Out of scope: **PureSignal** (WDSP's `calcc.c` is vendored, but the feedback-DDC
plumbing is not, and it is the largest scope multiplier available — separate
RFC); **wideband/bandscope streams** (Gap B); **diversity and synchronous DDCs**;
**Protocol 1 generalisation** (1.3); and **P2 boards other than the bring-up
radio** — board-specific behaviour that cannot be certified against hardware is
not shipped.

| Phase | Establishes | Keys? |
|---|---|---|
| 1a | Discovery, General packet, `run`, DDC0 IQ ingest at one rate. Throwaway spike, not in-tree, proving the wire before any C++ backend exists. Also settles Gap E deliberately. 1.2 is the first half, already done. | no |
| 1b | `P2Client` + `AnanRxDsp` in-tree; one DDC through `WdspChannel`; panadapter and audio; `radiocert tune` + `rx` clean. | no |
| 2 | Multi-DDC, band switching with Alex, step attenuator, meters from High Priority status. `radiocert meters`. Buffer sizing measured (see 3). | no |
| 3 | TX: DUC, drive, PTT, the WDSP TX chain. `radiocert tx`, plus the manual external-receiver sideband check. | yes |
| 4 | ATU, PA telemetry, operating-state restore per MAC. | — |

Phase 3 is deliberately last and deliberately separate. Nothing keys until
receive handedness is settled, because transmit handedness is downstream of it —
HERMES.md §15.6.


### Cross-platform impact

## 3. Cross-platform impact

AetherSDR targets Linux, macOS and Windows. P2 introduces no new UI, no new
dependencies, and no changes to shared code paths — the backend is additive below
the seam. The platform-sensitive surface is networking, and it is wider than P1's
because there are ~10 sockets instead of one.

- **Broadcast discovery.** `QUdpSocket` does not enable `SO_BROADCAST` itself.
  `Hl2Discovery.cpp` already sets it on the native handle with a `Q_OS_WIN` branch
  for the `winsock2` signature and a `NOMINMAX` guard against `windows.h`'s
  `min`/`max` macros. `P2Client` needs the same treatment. Where the radio's IP is
  known, unicast discovery avoids the issue entirely and should be the default
  path.
- **Socket receive buffers.** At 1536 ksps across multiple DDCs the ingest rate is
  well above anything the HL2 backend drives. Default `SO_RCVBUF` differs
  materially across the three platforms, and Linux additionally caps it at
  `net.core.rmem_max`. The backend should request an explicit buffer size, log
  what it actually got, and surface drops through `dropStats` rather than failing
  silently. **Unverified:** what buffer size is sufficient — a phase 2
  measurement.
- **Timer granularity and thread scheduling.** P2 expects a steady High Priority
  cadence. Windows' default timer resolution is coarser than Linux's, and macOS
  applies its own scheduling policies. This is a known-risk area rather than a
  measured one; the mitigation is to drive cadence from the data stream where
  possible rather than from a wall-clock timer.
- **Fixed port numbers.** 1024–1035 are above the privileged range on all three
  platforms and below every default ephemeral range, so no elevation is needed. A
  port already in use — a second AetherSDR instance, another HPSDR client — must
  produce a clear error rather than a silent partial session.
- **Signal handling for Gap E**, if it proves necessary, is POSIX-only in the form
  `Hl2EmergencyStop` uses; the existing file already brackets its POSIX path with
  `#ifndef Q_OS_WIN`. Any P2 equivalent should follow that structure.

**Honest limitation: I can only test Linux.** The Windows and macOS paths would be
written to match the existing HL2 precedents and would need someone else to
exercise them. If the project would rather not land a backend whose two other
platforms are untested, that is a reasonable objection and I would rather hear it
now — see question 5.


### Alternatives considered

## 4. Alternatives considered

**A — port an existing GPLv3 implementation.** NereusSDR (GPL-3.0) already
carries P1 and P2 with ANAN board coverage, ported from Thetis with per-file
provenance headers and its own attribution audit. Licence-compatible, and on
paper much faster. Not proposed as the primary path for two reasons: the
first-order dependency set is roughly 17k lines before any `IRadioBackend`
adapter is written, and those classes are written against NereusSDR's own object
graph rather than the seam; and taking source from another client would be a
change to how this project sources protocol code, which is a maintainer decision
rather than a contributor one. Question 2 leaves it open if maintainers prefer
it.

**B — generalise the existing P1 layer to cover ANAN as well.** Not proposed
here because I cannot certify P1 against hardware (1.3), not because it is a bad
idea. It is a good idea for someone with the right radio.

**C — do nothing; use a different client for ANAN.** Entirely reasonable, and the
cheapest answer. Good OpenHPSDR clients already exist, and there is no obligation
on AetherSDR to cover every radio family. If this is the call, worth saying so on
#78 so the next person does not re-derive the analysis.

**D — Protocol 2 for the G2 only, clean-room.** This proposal. Slowest of the
options that produce code, and the only one that both preserves the existing
provenance model and can be certified against hardware I actually have.

## 5. Open questions

1. **Is the disclosure in 2.9 acceptable**, with the mitigations proposed, or
   would you rather this work were done by someone who has not read NereusSDR?
2. **Is alternative A preferred after all?** If the project would rather take a
   port of existing GPLv3 code than a clean-room build, that changes everything
   downstream and is worth saying before any code exists.
3. **Gap A** — is a negotiated port set better handled in `RadioConnectRequest`'s
   `params`, or does `ConnectionPanel` need a per-family connect shape?
4. **Gap C** — advertised DDCs (4) versus gateware registers (10). Clamp to
   advertised and never probe past it is my recommendation; confirm.
5. **Is a Linux-tested-only backend landable as `experimental`** (section 3), or
   does the project want Windows and macOS exercised first?

### Implementation scope

_No response_
