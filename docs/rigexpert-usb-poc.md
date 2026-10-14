# AetherLab Stick Pro USB proof of concept

Implementation for [RFC #6471](https://github.com/aethersdr/AetherSDR/issues/6471).
The operator reported verbal maintainer approval on 2026-10-14; this branch
implements the USB bring-up step toward the quick-check design. Initial macOS
USB acquisition is demonstrated below; full hardware qualification is pending. This is an independent instrument, not an
`IRadioBackend` or a replacement transceiver session.

## Scope and authority

The first surface is **Tools → AetherLab USB Proof of Concept...**, an
independent canon workspace window with an SWR plot, keyboard-operated marker,
R/X and return-loss readouts, and a JSON session export. A station tab, Smith
chart, comparison/reference traces, bandwidth/resonance analysis, Touchstone,
BLE, serial models, stored device sweeps, calibration controls, firmware update,
and a station-wide TX interlock are deferred.

Protocol authority is RigExpert's MIT-licensed
[AntScope2 at 901923c3029e661385fdf88dc7b089cc2263f609](https://github.com/rigexpert/AntScope2/tree/901923c3029e661385fdf88dc7b089cc2263f609),
especially [hid_analyzer.cpp](https://github.com/rigexpert/AntScope2/blob/901923c3029e661385fdf88dc7b089cc2263f609/analyzer/hid_analyzer.cpp)
and `baseanalyzer.cpp`. The implementation is independently written from that
wire behavior; no AntScope code is vendored. The
[research oracle](https://github.com/jensenpat/AetherSDR/blob/caf751cf00d9b845d62abc1effc95c42004bb6f5/docs/rfc/aetherlab/rigexpert-oracle.md)
contains the source index, vectors, transport differences and open questions.
Stick Pro range comes from the
[manufacturer manual](https://rigexpert.com/files/product_documentation/Stick-Pro/Manual-en.pdf).

## Ownership and transport

`AntennaAnalyzerModel` owns the protocol state, deadlines, trace, and measured
samples. GUI and bridge consume that model. `RigExpertHidTransport` is the
injectable I/O boundary; its production implementation owns hidapi calls on one
I/O thread. No radio objects, settings persistence, transceiver commands, audio,
or spectrum subscriptions are changed.

Explicit discovery enumerates normal-mode VID `0483`, PID `A1DE`, serial prefix
`4600` or `4601`, preserving the complete serial and interface path. Bootloader
PID `A1DA` and other model prefixes are excluded. The operator selects an
interface and connects explicitly. A per-path `QLockFile` excludes another
AetherSDR instrument session on the same host. It cannot detect AntScope or
other applications: close those before testing. Multiple HID interfaces are
shown independently so descriptor assumptions can be investigated.

At most 64 received reports may await delivery to the model; exceeding that
backlog fails and closes the instrument instead of growing the event queue.
The worker drains at most 64 existing reports before issuing a fresh identity
query. Every normal send uses the source API's 64-byte buffer: `07`, payload
count, up to 62 ASCII bytes, zero padding. The read buffer is 64 bytes; the
parser accepts a 2–64 byte source-API report and rejects a payload count beyond
the bytes actually returned, with one identity-only compatibility exception:
`VER` **Stick Pro 108** sends full 64-byte FULLINFO reports declaring 63 text
bytes, although only 62 bytes follow the header. AntScope bounds that count to
the received capacity. AetherLab does so only during identification/idle identity
metadata, flags each affected raw report, and exposes
`identityLengthQuirkReports`. Measurement/setup reports retain strict lengths.

Live macOS qualification on 2026-10-14 confirmed this convention: the device
report descriptor declares report IDs 1/7/8 with 63 data bytes plus report ID
(64 total); a separate 256-byte HID read buffer still returned 64-byte reports.
VER identity succeeded before the old parser rejected the first FULLINFO packet.
The captured public metadata is replayed in `rigexpert_usb_poc_test`. Optional
FULLINFO text may be incomplete; it never establishes measurement capabilities
or calibration state. No 65th byte is invented and no numeric prefix is accepted
from an overclaimed measurement report.

## State machine

1. Connect → `identifying`; send `VER\r\n`.
2. A full `Stick Pro <version> ...` identity → send `FULLINFO\r\n`.
3. After 500 ms without another identity/info line → `ready`. The identity
   must exist; this quiet period is a host heuristic for optional FULLINFO,
   not a protocol-defined completion ACK. Identification has a 3 s total
   deadline. Full firmware text and up to 32 info lines are retained.
4. Explicit sweep → `FQ<centerHz>\r`; require an exact `OK` after that command's
   HID write receipt before sending `SW<fullWidthHz>\r`.
5. Require the next exact `OK` after the SW write receipt → `FRX<N>\r`.
6. Parse strict finite CSV `frequencyMHz,Rohm,Xohm` after the FRX write receipt.
   One initial FRX `OK` is tolerated before rows, without extending deadlines.
7. The AntScope normal-HID hypothesis is `N + 1` rows, observed on the tested
   Stick Pro 108 at the four counts below. Other tuples remain unqualified. Require
   strictly increasing returned frequency, requested first and last endpoints,
   and that row count. Completion means this **host cutoff**, not an observed
   firmware end-of-sweep marker. Extra data after cutoff is discarded locally;
   the bounded report trace may retain it while USB closes.
8. Completion, Stop, Disconnect, window close, malformed data or timeout
   cancels queued sends, attempts `off\r`, closes USB, and retains data until the next Connect. A new
   sweep requires an explicit new Connect. No automatic sweep, retry or
   reconnect occurs.

No wire acquisition ID exists. Connection identity and write receipts fence
callbacks and obvious premature ACKs; they cannot prove an unlabelled late
firmware reply belongs to the current command. Fresh USB sessions, initial
drain and fail-closed setup mitigate that ambiguity. Hardware qualification
must include late FULLINFO, multiple ACKs in one report, and stop/reopen races.

## Host limits and outputs

| Surface | Bound / behavior | Evidence |
|---|---|---|
| Sweep frequency | 100,000–600,000,000 Hz inclusive | Manufacturer range; USB stepping unqualified |
| Span | Positive, even integer Hz; start < stop | Host requirement to represent exact integer center/endpoints |
| FRX N | 1–1000; span at least N Hz | Host cap, **not** a discovered firmware maximum |
| Expected rows | N + 1; at most 1001 samples | Observed for N=1, 2, 10, 200 at 14.000–14.350 MHz on the tested Stick Pro 108; other settings unqualified |
| Setup ACK | 3 s per command | Host deadline, not a firmware timing specification |
| Sweep | 15 s without a valid row; 60 s total, starting at FQ | Host deadlines; partial data never labelled complete |
| Input line | 512 bytes, LF framing, optional single CR before LF | Host parser bound |
| Trace | Last 128 input/output reports, actual length, UTC, first 64 bytes in hex | Bounded diagnostics; a long sweep overwrites earlier reports |
| Full info | First 32 lines | Identity retained separately without 3-character truncation |
| Export | Atomic JSON: metadata, all accepted raw CSV rows, R/X, derived values, trace | No settings persistence; serial/path may be personal data |

Returned frequency is retained as integer Hz rounded from its MHz text, with
the complete original CSV row also retained. No fabricated frequency grid,
substituted sample or R floor is used. Raw R/X is the device's output, already
subject to device calibration. No host OSL or correction is applied. Negative
R and singular/non-passive reflection values are retained and flagged; invalid
CSV or non-finite input fails acquisition.

At 50 Ω, `Γ = (Z−50)/(Z+50)`, `SWR = (1+|Γ|)/(1−|Γ|)`,
`RL = −20 log10 |Γ|`. JSON uses null for infinite/undefined derived values and
a quality string (`perfect-match`, `total-reflection`,
`non-passive-or-singular`, `valid`). The plot clips to SWR 1–5; exported
measurements and marker readouts are not clamped. A partial/cancelled trace
remains visibly labelled by its terminal state.

## RF and automation

The Stick Pro emits measurement RF. Physical isolation from transceiver TX is
required. The manual UI requires **Analyzer RF port is isolated from
transmitters**, reset on each connection. There is **no station TX interlock**
in this PoC. `off`, a HID write receipt and USB close do not establish physical
RF silence: snapshots always say `rfOffConfirmed: false`.

Bridge/MCP commands use the `lab` verb. They stay within the same model, with
no unrestricted raw-command escape hatch:

```text
lab status
lab devices
lab connect 0
lab sweep 14000000 14350000 200 isolated
lab stop
lab disconnect
```

Remote acquisition requires **both** an explicit `isolated` token on the
request and exact environment value `AETHER_AUTOMATION_ALLOW_LAB_RF=1` on the
app process. This grants analyzer RF only, separate from radio TX permission.
Generic invoke, pointer and coordinate automation refuse the Sweep button
(`aetherLabRf`) even with permission: automation must use the typed `lab sweep`
request with its explicit isolation token. Manual UI activation still uses
the isolation checkbox.
Observe-only mode permits status, enumeration and stop/disconnect, but blocks
connect/acquisition. Stop remains available when permission is absent.
Bridge responses also report `automationRfAllowed` without starting a sweep.
Authentication uses the existing automation token. The existing MCP
`bridge_command` tool accepts `{"request":{"cmd":"lab","action":"status"}}`
and the corresponding action/value fields for the other commands.

## Observed macOS bench result

On 2026-10-14, clean app commit `433d91400b9a9c5eafae6ce53b62f602a73b7d63`
ran on macOS 27.0.1 arm64 with one USB Stick Pro identifying as **Stick Pro 108**.
The operator explicitly authorized sweeps into a connected dummy load. An
isolated app profile had no connected transceiver, authenticated MCP, analyzer
RF permission enabled, and transceiver TX automation disabled.

All acquisitions used the real MCP `bridge_command` → `lab` → model → USB path,
with range 14,000,000–14,350,000 Hz:

| FRX intervals | Accepted rows | Result |
|---:|---:|---|
| 1 | 2 | Complete; exact endpoints and every grid frequency |
| 2 | 3 | Complete; exact endpoints and every grid frequency |
| 10 | 11 | Complete; exact endpoints and every grid frequency |
| 200 | 201 | Complete; exact endpoints and every grid frequency |

The 201-row sweep returned `14.175000,51.46,-0.99` at its center. Calculated
at 50 Ω, that is **SWR 1.03539**, **return loss 35.1963 dB**, and
**|Z| 51.4695 Ω**. This agrees with the operator's rounded front-panel reference
(SWR 1.04, RL 35.2 dB, |Z| 51.5 Ω, R 51.4 Ω, X −1.02 Ω). It establishes
protocol/readout agreement on this load, not independent metrology accuracy.
Each connection recorded three identity-length compatibility reports;
measurement report lengths remained strict. The actual window and its marker
were inspected with the completed trace.

The snapshot's `countQualifiedOnHardware: false` remains conservative: this
bench does not certify all host-supported settings. Calibration provenance,
full range/count limits, fault/cancellation timing, physical RF silence,
Windows/Linux, other models and the radio-tab workflow remain unqualified.
The written `off` and USB close are observable host actions, not RF-off proof.

## macOS hardware qualification

Start with a normal-mode USB Stick Pro, firmware/model/serial recorded, a
known 50 Ω load or an isolated antenna, and AntScope closed. Inspect the active
radio's use/lock state and honor the live-radio procedure before touching it;
this test does not need a transceiver connection. Initial read-only discovery
and identity do not emit measurement commands. Capture each stage separately:

| Test | Required evidence |
|---|---|
| Discovery/open | VID/PID, all matching interfaces, complete serial, descriptor/report format; distinguish permission failure from wrong interface |
| Identity | Actual VER string, FULLINFO contents/terminator, timing and coalescing; preserve full firmware/revision |
| First finite sweep | FQ/SW/FRX bytes and write receipts; each OK in order; N=1, 2, 10 and 200 row counts/endpoints |
| Numerical check | 50 Ω, open, short, known reactive/impedance standards; compare same span against AntScope with the same device calibration |
| Cancellation | Stop during each setup phase, mid-sweep and just before last row; USB close; no new acquisition on reopen |
| Recovery | Unplug during identify/setup/data; duplicate/missing/out-of-order rows; stalled capture; partial data retained |
| Bounds | Device/host minimum and maximum frequency, supported stepping, largest qualified FRX N and measured timing, explicitly avoiding claims from host caps |
| RF silence | Independently observe analyzer RF after natural completion, off, close and unplug; a protocol flag is insufficient |
| Automation | Identity/status/samples/trace via MCP, explicit RF permission and isolation; negative guards remain injected tests |

Local injected tests establish framing, input bounds, math, ordering,
count/endpoints, cancellation, old-session fencing, timeouts and bridge gates.
They do **not** prove USB descriptor compatibility, firmware behavior, absolute
accuracy, calibration application, RF-off, Windows/Linux portability, or the
full station-tab workflow. Qualify those before promoting this to supported
hardware. The full tab design needs all local TX producers gated through the
real arbiter, safe session switching and measured return-to-radio behavior.
