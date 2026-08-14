# Radio certification — lessons and roadmap

Lessons 1.1–1.18 came from the Hermes-Lite 2 bring-up. **1.19–1.31 came from the
Icom IC-705**, the first radio brought up with `radiocert` in hand rather than
after the fact — which is the point of the tool, and a useful check on it: some
of what follows is a defect radiocert found, and some is a gap in radiocert
itself that only a second radio could expose.

**1.37–1.40 came from returning to the Hermes-Lite 2** on 2026-08-10 and running
`radiocert meters` against it after the Icom work had reshaped the tool. All
four are defects in the *tool*, not the radio, and all four are shapes this
document already warns about, aimed back at the instrument: a negative finding
from a stage that never transmitted, a false positive that a sibling table had
already been fixed to prevent, a staleness probe reading a stale value, and a
hardcoded finding that outlived the bug it described. **Two of the concerns in
that run were wrong and none of the meters were** — which is the ratio at which
an operator stops reading concerns. Turning the tool on a radio it has already
certified is cheap, and it is the only way these were ever going to surface.

Why `radiocert` is shaped the way it is, and what it still cannot do.

The reference tables live in [`radio-certification.md`](radio-certification.md);
the Hermes-Lite 2 bring-up narrative lives in `HERMES.md`. This file is the
part that generalises — the reasoning a future agent needs before it trusts a
clean report, and the work queued behind it.

---

## 1. The lessons, in the order they cost us

### 1.1 A convention error is invisible to any test that shares the convention

Transmit ran on the **wrong sideband** for a fortnight. Every internal
instrument agreed it was right, because the panadapter reads the same wire order
as the transmitter and therefore cannot disagree with it.

| Check | Result at the time |
|---|---|
| Modulator sideband assertion | 85 dB suppression, "correct" side — passed |
| Simulator loopback | tone at the expected bin — passed |
| Live panadapter | clean single sideband, correct side of centre |
| USB vs LSB forward power | 3875 vs 3876 — identical |
| TX FIFO depth | healthy, refuting the starvation theory |

Found in one sentence by an operator with a second receiver: *"I heard the LSB
side of AetherSDR on the USB side of the Yaesu."*

**Consequence for certification.** Self-consistency is not correctness, and
adding more internal tests increases confidence without increasing truth. Every
phase must state which of its checks are independent and which merely agree with
the implementation. The report ends with what it cannot determine for this
reason, not as a disclaimer.

### 1.2 Two compensating errors cancel everywhere except one geometry

The receive path handed the demodulator the conjugate and the spectrum the raw
wire — each wired to the other's convention. At normal off-centre tuning the two
errors cancelled and the audio was correct.

**Consequence.** Measure at **zero shift** first. Compensating errors cancel at
every other geometry, so a measurement taken at normal tuning proves nothing.
Force it by tuning far enough that the DDC must re-centre, then landing on the
target.

### 1.3 A single-mode test proves nothing about handedness

`hl2_shift_test` validated in LSB — the one mode the inversion made correct —
and passed throughout.

**Consequence.** Sideband checks run in **all four** SSB-family modes, and never
with the test signal at the pan centre: a mirror is invisible on its own axis.

### 1.4 Test stimulus must use the wire's convention, not the textbook's

Both receive unit tests generated `exp(+jwt)`. The wire sends `exp(-jwt)`.
Correct expectations against the wrong stimulus, so a mirrored panadapter *and*
an inverted demodulator both passed.

**Consequence.** Synthesise stimulus in the convention the hardware actually
uses. This is also the cheapest check available — it needs no radio.

### 1.5 The bridge is not the UI

Three separate bugs reached the operator because the automation bridge drives
`RadioModel` and the GUI's transmit controls drive `TransmitModel`, which emits
Flex command strings that never reach a backend without a command channel.
Keying worked perfectly under test and did nothing when MOX was pressed.

**Consequence.** `radiocert` keys through `TransmitModel::requestPttOn`, the
same path as the MOX button. A diagnostic that keyed the way only the bridge can
would inherit the blindness it exists to remove.

### 1.6 Readback proves nothing

The mode map passed for twelve modes while the backend mapped nine. `RTTY` still
reads back perfectly and is demodulated as USB.

**Consequence.** Controls are certified by **effect**. Halving a linear gain is
−6.02 dB; that is arithmetic rather than a property of any radio, which is what
makes it a threshold that transfers to hardware nobody has characterised.
Measured on the HL2: 6.023 dB.

### 1.7 Validate at the rate the UI actually produces

A filter-pipeline reset was validated with seven writes two seconds apart and
shipped. A pan drag issues a centre command every 33 ms, so the real path fired
~30 resets per second; the board halted its stream and stopped answering
discovery until power-cycled.

**Consequence.** Continuous gestures, not discrete actions. And if a commit
message needs the sentence "this has not been verified against X", verify X or
do not ship that line.

### 1.8 Meters are not trustworthy until something has checked them

The transmit stages originally inferred "no RF" from a missing SWR reading —
which is really "no SWR reading". The two are the same statement only after the
meters have been validated.

**Consequence.** Phases run in dependency order — `tune → rx → tx → meters` —
and meters come **last** precisely because nothing earlier may lean on them.

### 1.9 A measurement that looks in the wrong place reads as absence

The sideband stage hardcoded 24 kHz while the capture tap ran at the device's
48 kHz. It reported −80 to −109 dB for every mode while the RMS plainly showed a
25 dB signal — i.e. "no signal" rather than "I am misconfigured".

**Consequence.** Derive measurement parameters from the data (each capture
chunk reports its own rate) and **report them**, so the number is auditable.

### 1.10 An overloaded instrument must decline to answer

Monitoring our own transmitter saturates the receiver. Both sidebands then read
alike, and the stage confidently reported `SIDEBAND LOOKS INVERTED` — a false
alarm produced by the tool built to prevent them.

**Consequence.** Detect saturation and return INCONCLUSIVE. A diagnostic's worst
failure is a confident wrong answer, because it sends the next agent somewhere
specific.

### 1.11 Stale values answer a different question

`MeterModel` keeps last-known readings and never clears them, so "is there
forward power now" was answered by the SWR left over from the previous keyed
stage — reported as a carrier while the transmitter sent silence.

**Consequence.** Every meter read carries an age, and stages ignore anything
older than 3 s.

### 1.12 "No audio" has to mean it

The carrier-suppression stage keyed without enabling the test tone — but the
microphone was live, and the ALC's whole job is to lift a quiet room to full
modulation.

**Consequence.** Silence the source, do not merely stop driving it.

### 1.13 Fixing a bug once does not fix it everywhere it lives

§1.9 was found and fixed in `stage-rx-sidebands`. Review found the *same* bug
still sitting in `stage-sideband` — the flagship stage, the one this tool is
named for — reading the *same* `output` tap through the *same* wrong constant.
The fix had been applied to the site where the symptom appeared rather than to
the class of defect.

It survived because of a second-order effect worth remembering on its own: the
saturation guard (§1.10) returned INCONCLUSIVE before the tone comparison was
ever reached, so the stage never got far enough to be wrong on the bench. A
guard that suppresses a symptom also suppresses the evidence. The first person
to add attenuation and get a usable measurement would have received a confident
`SIDEBAND LOOKS INVERTED` computed from noise.

**Consequence.** When a defect is found, grep for its *shape* — here, every
`DEFAULT_SAMPLE_RATE` against a device-rate tap — not just its site. The
measurement primitives were moved to `RadioCertificationMath.h` and given a test
that asserts the failure directly: a 48 kHz tone probed at an assumed 24 kHz
reads 228 dB down, while `rms()` reads *identically* at both rates. That last
number is the whole lesson — the rate-independent statistic is exactly the one
that let the bug hide.

### 1.14 A generic tool that hardcodes one radio's facts reports false defects

`radiocert` asserted "`setRfGain` has no runtime path" and applied a
Hermes-Lite-2 meter-expectation table to whatever backend was connected. On a
Flex, the first is a working control reported as broken and the second is a
clean bill of health for meters that were never checked for.

A hardcoded false finding is worse than a missing one: nothing distinguishes it
from a measured result, and the report's own docs tell readers to read
`concern` first.

**Consequence.** Radio-specific claims are gated on `RadioModel::family()` and
say so in the report when they are skipped. This is a stopgap — the real answer
is the radio profile in §2.1.

### 1.15 A diagnostic must leave the radio where it found it

Every stage that moved the dial restored it to the *option* frequency rather
than the operator's. `radiocert tune` — the one phase safe enough to need no TX
permission — silently relocated the VFO to 20 m and left it there. Mic gain and
drive level were both carefully saved and restored, which is what marked this as
an oversight rather than a decision.

Worse, in an `all` run the receive stages parked the dial on WWV and the
transmit stages never re-tuned, so **every keyed stage transmitted a few hundred
Hz off a standards station, out of band** — while the report's `radio` block
faithfully said 14.2 MHz throughout. That is §1.11 again: a value that answers a
different question than the one being asked.

**Consequence.** `run()` saves the operator's frequency and mode at entry and
restores both at exit; the transmit block re-establishes the dial before
anything keys. Restoration is RAII where a throw could strand hardware state.
The `meters` phase keys too, and needed the same treatment — a fix applied to
one keying block is not applied to the class of keying blocks (§1.13 again).

### 1.16 §1.1 recurs one level up: the check shared its subject's convention

The sideband stage was the answer to §1.1 — *a convention error is invisible to
any test that shares the convention* — and it shared one.

It compared the transmitted tone demodulated on the matching sideband against
the opposite one. But `Hl2Backend::setSliceMode` drives the transmit chain and
the receive chain together ("the transmit sideband follows the slice"), so both
legs were *matched pairs*: TX-USB/RX-USB and TX-LSB/RX-LSB. With the
transmitter correct, both legs recover the tone. With it inverted, both go
silent. **The difference between them was noise in either case**, so the
comparison could not discriminate anything — and the stage emitted a confident
`SIDEBAND LOOKS INVERTED` verdict from it.

What *is* observable is the absolute level in each leg, which answers a real
and narrower question: do the transmitter and the demodulator agree about which
side of the carrier a sideband is on. A transmit-only inversion silences both
legs. A **shared** inversion — both chains wrong in the same direction — remains
invisible by construction, which is precisely §1.1 restated one level up.

**Consequence.** The stage was rewritten to measure agreement, not correctness,
and renamed to say so. Its `observation` states what it cannot see, and the
external-receiver check stays in `manualChecks` as the only thing that settles
the absolute question. Writing a check for a class of error does not exempt the
check from that class.

### 1.17 A refused action reads as a broken subject

`TransmitModel::requestPttOn` returns `void` and silently does nothing when
`runPttPreflight` refuses — a band limit, an interlock. `radiocert` never
confirmed the radio actually keyed, so every downstream stage measured an
unkeyed radio and blamed whatever it happened to be testing: "audio never
reached the modulator", "the transmitter is not producing RF".

The same shape as §1.8: a diagnostic reporting a defect in the nearest
subsystem rather than the responsible one.

**Consequence.** `keyViaOperatorPath` returns whether the radio reached the
requested state, refusals are counted, and `keyRefusals` is a top-level report
field — a non-zero count invalidates the transmit stages rather than annotating
them.

Related, and found with it: with Quindar enabled, `requestPttOff` does not
unkey. It starts an outro tone and defers the real unkey behind a timer, so
"the call returned" and "the radio stopped transmitting" are different moments —
the watchdog was being disarmed while the radio still transmitted. And the
Quindar *intro* tone is transmitted as audio, landing inside
`stage-carrier-suppression`'s assertion that nothing is being sent (§1.12). The
diagnostic now silences Quindar for the run and waits for the actual unkey.

### 1.18 A probe needs a band when you do not control the target

`tonePower()` integrates coherently over the whole buffer, so a 1.5 s capture is
a ~0.67 Hz bin. That is right for our own test tone, whose frequency we set. It
is wrong for an off-air reference: WWV is exact, but *our dial* is not, and a
1 ppm oscillator error at 10 MHz moves the carrier ~10 Hz — fifteen bins away.

Measured in `radio_certification_math_test`: a 10 Hz drift reads **−240 dB** on
an exact-bin probe and **−12 dB** on a ±25 Hz band search. The exact-bin result
is indistinguishable from a deaf receiver, and — being the same symptom — would
have been read as the §1.9 wrong-rate bug all over again.

**Consequence.** `tonePowerNear()` searches a band whenever the tone's exact
frequency is not under our control, and the span is reported alongside the
result.

---

### 1.19 An undocumented constant in a reference implementation is load-bearing

kappanhang sets its tracked sequence to `1` in one line, with no comment. Ours
started at `0` — the obvious choice — and the IC-705 read `0` as one *before*
the start of the space, inferred a wrap, and answered our login with a stream of
retransmit requests for a window that never existed. It never processed the
login. Every visible indicator was healthy: the handshake completed, pings
answered, RTT 30 ms.

**Consequence.** When porting from a reference, an unexplained initial value is
a fact about the *radio*, not a stylistic choice. Diverge only with evidence.
Recorded in `icom-oracle.md` §2.6 so the next model does not pay for it.

---

### 1.20 A failed session must actually tear down

`fail()` emitted `disconnected()` and left the streams running, so a failed
connect leaked three UDP sockets. An Icom serves **one client**, so those held
the radio's session slot and every later attempt — from any program — timed out
with "no answer". A radio that worked once and then refused to talk to anything.

**Consequence.** Reporting a failure and *ending* it are different jobs, and on
a single-client radio the second one is what the next connect depends on.
Confirmed with `lsof` against the app's own pid, which is the check worth
running when a radio goes unreachable after a failure.

---

### 1.21 The disconnect packet does not end the session

Closing each stream (type `0x05`) closes the **streams**. The **session** stays
authenticated on the radio until it times out, and the next login is refused —
presenting as "auth error on reconnect", which sounds like a credential problem
and is not. The protocol has a separate deauthentication (`auth 0x01`), and
teardown order matters: it must be the last thing the radio hears.

**Consequence.** Certification should include a **reconnect** stage:
connect, disconnect, immediately reconnect. Nothing else in the suite exercises
teardown, and a leaked session is invisible until the second connect.

---

### 1.22 One packet shape, two meanings, decided by session phase

On reconnect the radio sends a status packet carrying an auth-failure sentinel
*after* the new session is fully established — login accepted, capabilities
read, streams granted, token accepted, both media streams handshaking. It is
reporting the **previous** session's teardown. Read as fatal, it killed a
working connection every time, and §1.20's fix (making failures real) turned the
misreading into a hard failure instead of a harmless log line.

Found by reading the log: every stage reported success, and then the session
died.

**Consequence.** A packet's meaning can depend on where the session is, not only
on its bytes. Classify against session phase, and be suspicious of any fatal
verdict that arrives after a complete success sequence.

---

### 1.23 A conservative default applied at the wrong moment is a false negative

The Icom backend answers capabilities from a model record that starts as
"unknown" — deliberately no scope, **no transmit** — until the CI-V address
query identifies the radio. That default is right for a radio we cannot
characterise. But the address query needs a stream that does not exist on the
connect edge, so anything reading capabilities *then* saw `canTransmit=false`
and refused to key a radio that transmits perfectly well. `radiocert meters` and
`radiocert tx` were both blocked by it, and the message pointed at TX permission
— which was granted.

**Consequence.** A safe default needs a defined *resolution point*, not just a
safe value. Here the radio's name arrives during the handshake, early enough to
resolve the model before capabilities are first read; the address query still
runs and still wins.

---

### 1.24 Inherited limits carry their source's assumptions

Two constants taken from reference implementations were correct *there* and
wrong for us, with the same signature: **commands work perfectly and the
panadapter is black.**

- The CI-V frame cap of 80 bytes is the longest *command* frame. Hamlib and
  kappanhang both use it; neither decodes spectrum. A scope sweep is ~496 bytes.
- The serial payload length is a 16-bit field. kappanhang reads 8 bits, which is
  byte-identical below 256 and all it ever needs — again, no spectrum.

**Consequence.** When adopting a constant, ask what the source *does* with it.
A limit that has never met your use case has never been tested against it.

---

### 1.25 Gate the stages that key, not the phase that contains them

`radiocert meters` refused outright without TX permission, but only two of its
stages transmit — the **inventory** reads the meter model and keys nothing. That
put the single most useful early question ("are the meters wired up at all?")
behind a permission nobody grants on day one, and it is a *receive* question.

Running it immediately found that every Icom meter was published under a source
and id nothing looks up — §1.8's orphaned-meter seam, reached by a different
route, on a backend whose S-meter had been decoding correctly for days.

**Consequence.** Phase-level gating is too coarse. Gate the keying stages; let
the rest report, and let `keyRefusals` say what did not run. Applied — `meters`
without TX now reports the inventory and a non-zero refusal count.

---

### 1.26 Transmit state must be polled, not inferred from your own commands

The operator keyed the IC-705 from its own front-panel PTT, watched the radio's
meters move, and saw nothing at all in AetherSDR. Every transmit meter was
defined, calibrated against Icom's own guide, and published under the right
source — and none of them was ever *requested*.

Two independent causes, either one sufficient:

* `m_keyed` was set only by our own `setKeying()` and by an unsolicited
  `1C 00` frame — and that frame arrives only if CI-V Transceive is enabled on
  the radio. A backend that learns it is transmitting **only from its own
  outbound commands** is blind to every other way the radio can be keyed:
  front-panel PTT, a foot switch, VOX, a second client.
* The five TX meters were never added to the poller's *visible* set, so even
  with the keyed flag correct nothing would have asked for them.

The failure mode is the nastiest kind — a meter that reads zero looks
identical to a meter that is working and measuring nothing. The RX/TX split
that suppresses transmit meters while receiving is right (they read zero and
look like a fault), which is exactly why the state driving it has to come from
the radio.

**Consequence.** Poll `1C 00` on a slow cadence — 250 ms is plenty; it only has
to *notice* a transmission, and it shares the CI-V stream with tuning. Fixed in
`2d5ed841`. **Confirmed on a live IC-705 into a dummy load**: all four transmit
meters now feed (`TX:FWDPWR`, `TX:SWR`, `TX:ALC`, `TX:COMPPEAK`, ages ~1.1 s),
with `keyRefusals: 0` and SWR reading 1.0:1 — the one number in that set with an
independent right answer.

That run keyed through radiocert's own path, so it proves the *visible-set* half.
The *polling* half — a front-panel PTT, which is how the operator hit this — is
still unproven, and it can only be proven by a human pressing the button.

---

### 1.27 "Fed" is not "rendered" — the seam is not the consumer

`radiocert meters` reported every Icom transmit meter as `defined: true,
everFed: true`, with ages around a second. It was **completely correct**, and
five meters were dead on the operator's screen at that moment.

The phase measures where the value ARRIVES. Nothing measured where it is
CONSUMED. Between the two sits `MeterModel`, which interprets a meter by NAME
and applies a unit it ASSUMES rather than the one the meter was declared with:

| Meter | Declared | Assumed | What the operator saw |
|---|---|---|---|
| `TX:FWDPWR` | Watts | dBm, so `10^(v/10)/1000` | 5 W rendered as 0.003 W — motionless |
| `TX:ALC` | Percent 0–100 | dBFS on a −20…0 gauge | pinned at the top, always |
| `TX:SWR` | SWR | SWR ✓ | correct, and SUPPRESSED — its consumers gate on forward power, which was reading zero |

The `unit` field was carried across the seam, printed in the inventory, and then
ignored by the only two consumers that convert. A certification phase that
prints a field nobody downstream honours is reporting the wire, not the product.

Note the third row especially: SWR was never broken. It was **collateral** —
one mis-scaled meter silenced a correct one through a dependency the inventory
does not draw.

**Consequence.** The meters phase must read back through the consumer. For every
meter it can, assert on `MeterModel`'s converted output — `fwdPowerInstant()`,
`swr()`, `swAlc()` — not on "a value arrived". And it must report the declared
unit next to the unit the consumer will apply, because when those disagree the
meter is already wrong and no amount of freshness will show it.

### 1.28 Three ways a meter can be absent, and only one is a defect

The same run raised concerns about `TX:MICPEAK` and `RAD:PATEMP`. Neither is a
defect: the IC-705's CI-V meter set is `15 02/11/12/13/14/15/16` and contains no
microphone-level meter and no temperature meter **at all**. There is nothing to
wire.

So "absent" needs to be three states, not one:

* **unsupported** — the radio's protocol has no such meter. The UI should hide
  the face; a permanent floor reading is indistinguishable from a fault.
* **unmapped** — the protocol has it and this backend does not send for it.
  A real gap, and the only one worth a warning.
* **unfed** — mapped, requested, and nothing came back. The §1.8 orphan.

Today all three render the same way, so a structural fact about the radio
generates a warning on every run forever, and warnings that never go away stop
being read. That is how the genuinely-unmapped ones hide.

**Consequence.** A meter's absence must be reported with its REASON, and the
reason has to come from the backend, which is the only thing that knows.

### 1.29 A control that cannot be DRIVEN cannot be certified

Certifying the Icom control surface stalled before it began, twice, and neither
cause was a defect in the radio or the backend:

* **The RX applet's DSP toggles carry no `objectName` and no
  `accessibleName`.** In a bridge tree dump they appear as anonymous buttons
  reading `"checked"` / `"unchecked"`. `invoke()` addresses controls by name, so
  NR, NB and ANF were unreachable — not broken, unreachable.
* **The NR keyboard shortcut does not toggle NR.** `nr2_toggle` cycles
  off → NR → NR2 → NR4, where NR2 and NR4 are HOST-side. Fired four times
  against a live radio it emitted nothing on the wire, because the host chain's
  enable is applied through a queued invocation and the cycle re-entered its
  first branch every time. The one control it appears to name is the one it can
  fail to reach.

Both were found by trying to drive them, and neither would have appeared in any
amount of source reading — the second in particular looks correct at every line.

**Consequence.** Addressability is a TESTABILITY requirement, not a nicety.
Anything a certification stage must exercise needs either a stable identifier or
a model-level verb that bypasses the widget entirely. `slice dsp <control>
<on|off> [level]` was added for exactly this, and it is the pattern: when a
control is hard to address, add the verb rather than the accessible name, because
the verb also survives the applet being redesigned.

### 1.30 A frame proves the route; only its VALUE proves the translation

The Icom control run reported 8 of 8 passing, and the check behind each was
"did a frame containing `16 40` appear". That is a test of wiring, and it would
have passed just as cleanly with every level wrong.

The check that means something compares the bytes against the encoding computed
independently:

```
NR level 40 %  ->  14 06 01 02      because 40 * 255 / 100 == 102 == BCD 0102
NB level 55 %  ->  14 12 01 40      because 55 * 255 / 100 == 140
squelch  40 %  ->  14 03 01 02
```

This is §1.6 (controls are certified by effect) applied one layer lower: the
arithmetic is doable on paper, so it transfers to any radio and any register
without a bench.

**Consequence.** A control stage asserts the DECODED value, never the presence
of a command. And the expected value is computed in the test from the percentage
rather than pasted from an observed capture — a captured constant only proves
the implementation still agrees with itself (§1.1).

### 1.31 When one control is two registers, the ORDER is on the wire

AetherSDR's NR is a single control. The IC-705 splits it: `16 40` enables,
`14 06` sets depth. So the intent carries both, and "NR on at level 60" reached
the backend as a level change followed by an enable — putting `16 40 00` on the
wire immediately before `16 40 01`.

A brief disable of the operator's noise reduction, and two frames on a CI-V
stream that metering already shares. Every individual command was correct.

**Consequence.** A backend that fans one control into several registers must
suppress the ones that did not change, and must forget what it believes on
disconnect — the radio keeps its own state across sessions and we have not read
it back, so carrying the last session's belief would suppress the first command
that matters. And a certification stage should assert the SEQUENCE a control
produces, not the set: correct commands in the wrong order are a defect that
set-membership cannot see.

### 1.32 Match the radio's display quantisation, not mathematical nearest

Icom `14 xx` controls store 0000–0255 but show integer percentages on the front
panel. Decoding with nearest rounding made RF Power, Mic Gain, Monitor Level,
and every sibling percentage appear one point ahead for roughly half the range.
The implementation and its round-trip test agreed; the radio did not.

The display contract is asymmetric:

```
radio raw -> ASDR percent : floor(raw * 100 / 255)
ASDR percent -> radio raw : ceil(percent * 255 / 100)
```

The ceiling on write chooses the first raw value in the radio's requested
display bucket; the floor on read reproduces the number the radio shows. Keep
this conversion central and test all 0–100 values. Do not apply it to `15 xx`
meters merely because their payload can also reach 255: meter breakpoints are
physical, non-linear, and often model-specific.

**Consequence.** For a scaled control, certify three values independently: the
requested UI value, the exact wire value computed from the official guide, and
the value shown by the radio after its reply. A shared encode/decode round trip
cannot catch a convention both sides share incorrectly.

### 1.33 Transceive is a hint, not a subscription contract

CI-V Transceive did not reliably announce NR and NB changes on the
IC-7300MK2. It also cannot be assumed to cover RF gain, RF power, mic gain,
monitor, VOX, notches, preamp, attenuator, or tuner state on another model.
Connect-time reads made the UI correct at launch and then allowed it to drift.

Reliable convergence needs all three paths:

1. take a complete model-supported snapshot at connect;
2. consume unsolicited frames for low-latency updates; and
3. periodically poll every readable state that matters to the visible UI.

The radio reply is authority. Adopt it into the model without reflecting a new
setter command, and do not replay client defaults over it on reconnect. For a
register whose documented read does not work on the bench, mark the control
send-only and keep any optimistic state session-local rather than pretending it
is subscribed or replaying it at reconnect.

**Consequence.** Test an external change, not only an AetherSDR setter: change
the radio from its panel or a TX-gated raw protocol command, wait at least two
poll periods, and verify the model and the actual widget both follow.

### 1.34 A transmit meter value expires at unkey

The backend can legitimately retain the last forward-power sample for logs, but
the operator's gauge answers a different question: "what is the transmitter
doing now?" A non-zero value at startup or after unkey is wrong even when its
wire value was once correct. A response already in flight at the unkey edge can
otherwise repaint the stale value after the UI clears it.

**Consequence.** Gate the visible TX meter on radio-authoritative transmit
state, clear it synchronously on unkey, and ignore late replies for presentation.
Certify startup idle, active key, immediate unkey, and one full response timeout
after unkey. Inspect the live gauge as well as the backend/model value.

### 1.35 A percentage ceiling is not a watt ceiling

`AETHER_AUTOMATION_TX_MAX_POWER=10` limits an RF/Tune Power control to ten
percent. It does not promise ten watts. On the IC-7300MK2, a ten-percent Tune
Power setting produced a CI-V forward-power indication of 16 W. Two-tone uses
Tune Power rather than RF Power, and an ATU cycle may choose its own tune drive,
so clamping the RF Power slider alone is not a physical safety limit.

**Consequence.** Require both an explicit control-percentage ceiling and an
explicit physical-watt ceiling. Start at a conservative percentage, require a
fresh calibrated forward-power meter, and unkey on the first sample over the
watt limit or when fresh telemetry does not arrive promptly. This measured
watchdog is reactive and cannot prevent the first transient; where exceeding
the limit even briefly is unacceptable, use an external wattmeter/interlock or
do not run unattended. Never start an ATU cycle under a watt-limited test unless
its tune-drive behaviour is separately authorized.

### 1.36 Restart is a distinct certification phase

A set/readback in one process proves neither reconnect authority nor periodic
subscription. The IC-7300MK2 controls were therefore tested across two complete
AetherSDR launches: set a distinctive safe value, close the application,
restart and reconnect, then verify the radio-reported value in both the model
and the visible widget. The periodic external-change test ran again after the
restart.

**Consequence.** Every stateful control gets a four-part record: set path,
same-session radio reply, post-restart adoption, and later external-change
convergence. Mark a control partial if any one is missing. Restore the
operator's original state at the end; if restoration itself would transmit,
leave the radio in the safer state and say so.

### 1.37 A stage that transmits must check that it transmitted

`radiocert meters` was run twice against the same Hermes-Lite 2, minutes apart,
with one difference: the operator's RF power slider sat at **0** for the first
run. It reported

> DEFINED BUT NEVER FED … `TX:SWR` … Either publish them with a documented scale
> or stop defining them

and `meter-scale` recorded `swr: -1` beside it. The second run, at 50 % drive,
read `TX:SWR` = 1.0 with an age of 531 ms. Nothing had been fixed in between.

The verb keyed correctly, the audio reached the modulator, `keyRefusals` was 0
and every precondition passed — the radio simply radiated nothing, because zero
drive is zero drive. `run()` clamps RF power **down** to the automation ceiling
and has no floor, and no keyed stage asks whether forward power actually
appeared before reporting on the meters that depend on it.

This is §1.8 arriving from the other side. That lesson says a transmit stage
must not infer "no RF" from a missing meter reading; this one says a **meter**
stage must not infer "no meter" from missing RF. Both are the same confusion
between the instrument and its subject, and the meters phase is where it is most
dangerous — a false `NEVER FED` is a standing invitation to delete a working
meter, which is precisely what the concern text recommends.

**Consequence.** Any stage whose measurement requires a transmission must
establish that the transmission happened, from a quantity independent of the
thing being measured, and downgrade to **inconclusive** rather than negative
when it did not. A drive floor would help and is not sufficient: an interlock, a
band limit or a PA that never enabled produce the same silence at any slider
setting. `NOT-TESTED` is already a first-class outcome in the control scrub
(§1.29); the meters phase needs it too.

### 1.38 A lesson learned in one table is not learned in its sibling

Every HL2 meters run reports

> UNIT MISMATCH — these are defined, fed and MISREAD … `TX:ALC` (declared dBFS,
> expected dB)

on a meter that is correct. `MeterSurfaces.h` holds `TX:ALC` as accepting
`"dBFS,Percent"` — a **set** — and its comment says exactly why:

> The first version of this field held the one unit the consumer applied, and
> running it against a live radio flagged both FWDPWR and ALC as mismatched —
> after they had been fixed.

`kMeterTable` in `RadioCertification.cpp` is still the first version: one
expected unit, compared by equality. So the fix was made, written down, and
applied to one of the two tables that needed it — and the tool that reads the
other one has emitted a permanent false positive on healthy hardware ever since.

It ranks the finding first, too, above every real concern, because §1.28's
ordering puts a fed-and-misread meter ahead of an absent one. The one class of
finding designed to be impossible to overlook is the one that is always wrong.

**Consequence.** When a check is duplicated — a document and its executable
copy, two tables over the same domain — fixing one and not the other is worse
than never having fixed either, because the surviving copy now contradicts a
correction someone made deliberately. Grep for the other copy at the moment of
the fix. And a concern that fires on every run of healthy hardware must be
treated as a defect in the checker with the same urgency as a missed
detection: §1.28 warns that a concern which never goes away stops being read,
and this one is loud enough to take the real findings with it.

### 1.39 The "is it rendered" probe needs the same guards as the gauge

§1.27 added `asRendered` so a stage could report what the operator's gauge will
actually show, rather than only what crossed the seam. On the HL2 it reports:

```json
"asRendered": { "fwdPowerWatts": 0.001, "swr": 1.0, "alcDbfs": -1.43 }
```

All three numbers are wrong in a different way, and the meters are fine:

- **`fwdPowerWatts: 0.001`** in a run whose own keyed stage measured 2.0 W.
  `stageMeterInventory` runs after `stageControlEffect` has unkeyed and settled
  for 700 ms, so for a transmit-only quantity it samples the one moment the
  value is guaranteed absent. It can never show a keyed reading.
- **`swr: 1.0`** in the same run that reported `TX:SWR everFed: false` — a
  stage contradicting itself inside one JSON object. `MeterModel::swr()` returns
  `m_swr{1.0f}` with no measured companion, so "never fed" and "a perfect match"
  are the same float. Reflected power has `reflectedPowerMeasured`; SWR has
  `swrSampleLive()`, which `RigctlProtocol` and the SWR sweep both use and
  `asRendered` does not.
- **`alcDbfs: -1.43`** is a real value from the previous key, reported unlabelled
  and unaged — §1.11's stale reading, in the probe added to catch staleness.

**Consequence.** A probe that reads a consumer must read it the way the consumer
does, including its freshness gate, and must sample it when the quantity can
exist. Reading a typed accessor raw makes the probe a *third* convention
alongside the seam and the gauge, and §1.1 applies to it as much as to anything
else: it will agree with nothing and be believed anyway. Where the model already
exposes a liveness predicate, the probe's failure to call it is the bug.

### 1.40 A hardcoded finding outlives the bug it describes

`stageControlEffect` emits, on every Hermes-Lite 2 run:

> `SliceModel::setRfGain` has no runtime path on this backend — LNA gain is
> connect-parameters only, so the preamp/attenuator control does nothing after
> connect

The first clause is true and the conclusion is false. `SliceModel::setRfGain` is
a dead end — its body is `sendCommand("slice set N rfgain=X")`, Flex wire text
no seam backend can receive — but the operator's RF Gain slider does not call
it. It routes through `RadioModel::setPanRfGainFor` to
`Hl2Backend::setPanRfGain`, which writes the AD9866 LNA register at runtime.
`setPanRfGainFor` exists *because of this bug* and says so:

> Without this the HL2's RF Gain slider moved, persisted, and changed nothing:
> `lnaGainDb` was applied once at connect and never again.

The fix landed, the control works, and the finding stayed — because it is not a
measurement. It is a `problems <<` string behind an `if (family == "hl2")`,
asserted rather than observed, so nothing about the repair could reach it.

§1.14 is the spatial version of this: a hardcoded fact about one radio, reported
against another. This is the temporal version, and it is worse in one specific
way — §1.14's false finding is visibly about the wrong radio, while this one is
about the right radio and was simply true last year. Nothing distinguishes it
from a live result, which is the same complaint §1.28 makes about a concern that
never clears.

It is also the **second** permanent false positive `radiocert` emits on healthy
HL2 hardware, next to §1.38's unit mismatch. Two of the concerns in a clean run
are wrong, which is the ratio at which an operator stops reading them.

**Consequence.** A hardcoded finding needs an expiry mechanism, and the cheapest
one is to stop hardcoding: assert the wiring the way the tool asserts everything
else, by effect. RF gain has an unusually good one available — an 8 dB LNA step
must move `SLC:LEVEL` by 8 dB, a known answer that needs no calibration and no
transmission. Where a finding genuinely cannot be measured, it belongs in the
radio profile (§2.1) as declared data with a date on it, not in a stage as a
string. And when a defect is fixed, grep the certification tool for its
description — the tool is the last place anyone looks for a stale claim about a
bug they just repaired.

**The rule, stated so it can be enforced at review time.** Everything above is
the cautionary tale; this is the thing to check against the next `problems <<`
string:

> A certification finding must either be **produced by a measurement taken
> during the run**, or **carry the date and the tree it was verified against**.

A finding expressed as a measurement re-derives its own truth on every run and
cannot go stale. A finding expressed as a literal behind a family gate is only
ever as current as the day someone typed it — and nothing in the output
distinguishes the two, which is what makes the second kind so expensive. §1.14
retired the *spatial* form of this failure (a gate asserting something about the
wrong radio); this is the *temporal* form, and it is harder to catch precisely
because the assertion was true when it was written and is about the right radio.

## 2. Next steps

### 2.1 The radio profile — highest leverage

Have `radiocert` emit the invariants it **established**, as a machine-readable
block, and verify them on later runs:

```
wire handedness      : conjugate-of-analytic
demod consumes       : raw wire
spectrum consumes    : conjugated
slice shift sign     : slice - NCO
TX IQ                : conjugated for wire
audio rate / IQ rate : 24000 / 48000
mode map             : {usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty} → all mapped
```

**Why this matters more than another stage.** Right now every one of those facts
is prose spread across `HERMES.md`. As a profile they become a checklist a new
backend fills in, and a regression in any of them is a **diff** rather than a
debugging session. Bring-up stops being exploration and becomes "determine these
seven facts, then run the cert".

Design notes:
- The profile is per-backend and lives with the backend, not the tool.
- `radiocert` reports **measured** against **declared** and flags disagreement.
  A backend that declares the wrong handedness and behaves consistently with its
  declaration is still wrong, but it is wrong *visibly*.
- Include the rates: the 24/48 kHz mismatch cost a whole measurement (§1.9).

### 2.2 Automate `consumer-agreement`

**The most valuable stage in the tool, and the one that cannot run.** It compares
where the panadapter draws a signal against which sideband recovers it — the
check that actually found the receive inversion, because the panadapter was the
only consumer with no compensating error.

Blocked on: the panadapter's bins are not reachable through the seam. The
spectrum arrives as `spectrumFrameReady(int, QByteArray)` into `RadioModel`, but
nothing exposes a snapshot a diagnostic can correlate against demodulated audio.

Sketch:
- A read-only spectrum snapshot accessor (last frame, with its centre and span).
- Park a known carrier **off-centre**, capture both the bin index of the peak and
  the demodulated audio frequency, and assert they describe the same side.
- Report the two independently, so a disagreement names which consumer is
  compensating rather than just failing.

Until then it is emitted as an **operator check** in `manualChecks`, deliberately
visible rather than quietly skipped.

### 2.3 Rebuild the meters phase around the consumer

§1.27 and §1.28 are the same request seen twice: the phase reports the seam, and
the operator lives at the consumer. Concretely, and in the order that pays:

**Report the unit twice.** Declared, and applied. One line per meter:
`TX:FWDPWR  declared=Watts  applied=Watts  raw=5.0  rendered=5.0 W`. A mismatch
between columns two and three is a defect with no other symptom, and it is
free to detect — both values are already in the process.

**Assert through `MeterModel`, not the seam.** `everFed` proves delivery. The
question the operator is asking is whether the gauge moved, and the value the
gauge reads is the converted one. Every meter that has a typed accessor should
be checked through it.

**Plausibility, not just liveness.** Two checks with known answers, both
available on any dummy load:
  * forward power must be **non-zero while keyed** — a fed meter reading zero
    under key is the exact signature of a scale error;
  * a meter sitting **exactly on a rail** for a whole keyed window is
    suspicious, and pinned-at-full is how a unit mismatch presents.

**Name the meters nothing reads.** `RAD:PACURRENT` and `RAD:OVF` are published
by the Icom backend and consumed by nobody. That is not a bug, but it is
invisible today, and the inverse — a consumer with no producer — is exactly the
§1.8 orphan. The phase already knows both sides; it should print the join.

**Draw the dependency.** SWR was suppressed by forward power, and no output in
the run said so. When a meter is gated on another meter, a suppressed reading
must name its gate rather than reporting as merely stale.

**Finish the family table.** `expectationsApplied: false` on anything that is
not an HL2 (§2.1). Until that lands, every per-meter reading is measured and
valid and NOTHING renders a verdict on it — which is how a run can be green and
useless at the same time.

**Confirmed on hardware, 2026-08-10.** The 2026-08-10 Hermes-Lite 2 run turned
three of the items above from anticipated into observed, and the "forward power
must be non-zero while keyed" check is now the highest-priority one: it is the
single check that would have caught §1.37. Four concrete changes fall out, all
small and all in `RadioCertification.cpp`:

1. **A keyed-RF precondition on the meters phase.** If no keyed stage produced
   forward power, every transmit-meter verdict in that run is `INCONCLUSIVE`,
   not `NEVER FED` (§1.37). This must not be a drive floor alone — an interlock
   or a disabled PA is silent at any slider setting.
2. **`acceptedUnits` as a set in `kMeterTable`,** matching `kMeterSurfaces`,
   which ends a permanent false positive on healthy hardware (§1.38).
3. **`asRendered` through the liveness gates and sampled while keyed** —
   `swrSampleLive()` already exists and two other consumers call it (§1.39).
4. **Refresh `kMeterTable`'s notes and `expectedOnHl2` column.** Four HL2 meters
   are marked not-expected while being defined and fed, so a regression in any
   of them cannot be reported by `expectedButMissing`.

**All four are implemented in #4917**, which lands directly on top of this
document. Naming it here is not bookkeeping — it is §1.40's rule applied to this
file: a list of open work is a finding like any other, and one that outlives its
fix is exactly the failure §1.37–1.40 exist to record. If a fifth item is ever
added here, it needs the same treatment when it closes.

### 2.4 What the bridge needs for any of this to be automatable

The five defects above were each found by hand — reading source, decoding wire
dumps, and screenshotting a panadapter that turned out to say "Connecting to
radio…". None of that is repeatable by an agent, and all of it could have been
a verb.

* **`meters` should join producer to consumer.** Today `get model=meters` lists
  what the model holds. It cannot answer "what will the gauge show", which is
  the only question that matters. The join — backend def, declared unit, raw
  value, converted value, consuming accessor — exists entirely inside the
  process and is not exposed anywhere.
* **A session-liveness verb.** Several measurements in this bring-up were taken
  against a session the radio had already revoked; `get model=pan` answered
  cheerfully from stale state while the panadapter rendered a connecting
  spinner. There is no cheap way to ask "is data still arriving". Something
  like `health` reporting per-stream last-packet age would have caught it in
  one call instead of a screenshot.
* **Wire logging must be reachable at runtime.** `log set aether.icom.stream on`
  returned `ok: true, enabled: false`, so every wire-level diagnosis needed a
  process restart — and on a single-client radio each restart costs a ~60 s
  session timeout. The decisive evidence for the zoom bug was a single CI-V
  echo; it took three relaunches to see it.
* **A raw command-injection verb, TX-gated.** Confirming a wire format
  empirically meant editing C++, rebuilding, relaunching and reconnecting. For
  the span command that loop ran twice. `civ send 27 15 00 00 00 25 00 00`
  against a live session would have answered it in one call — and the FB/NG
  reply is the ground truth that no internal test can produce (§1.1).
* **A measured-watt transmit rail.** `AETHER_AUTOMATION_TX_MAX_POWER` is a
  percentage clamp, not a physical limit (§1.35). A TX harness needs a fresh,
  calibrated forward-power ceiling that force-unkeys on over-limit or missing
  telemetry and reports which control actually supplied drive (RF Power, Tune
  Power, ATU, or a backend-specific path).
* **A presentation snapshot for gauges.** A retained backend value and a
  correctly cleared visible gauge are intentionally different after unkey
  (§1.34). Expose the rendered/consumer value directly so the certification
  stage can assert startup, keyed, and idle presentation without inferring it
  from producer state.

### 2.5 A control-certification phase

There isn't one. `radiocert` proves meters, tuning, sideband and keying, and
says nothing about whether the operator's switches reach the radio — which on
this bring-up was where most of the defects were. §1.29–1.31 are what it would
need:

* **Drive through model verbs, not widgets.** The applet toggles are not
  addressable and the shortcut for NR does not reach the slice. Verbs are also
  the only route that survives an applet redesign.
* **Assert the decoded value against arithmetic**, computed in the stage from
  the requested percentage — never a constant pasted from a capture.
* **Assert the sequence**, so a fan-out that emits its registers in a harmful
  order is caught. `civ trace` already provides the ordered frames.
* **Report unreachable controls as a distinct outcome.** "This control has no
  backend verb" and "this control was driven and the radio ignored it" are
  different findings and currently look identical, which is §1.28's shape again.
* **Cover the inventory, not a sample.** The per-family control map in
  `aetherd-icom-civ-backend-design.md` Appendix D is the list; a stage that
  walks it can report coverage instead of leaving it to be counted by hand.

### 2.6 Smaller, already identified

| Item | Note |
|---|---|
| `TX:FWDPWR` / `TX:REFPWR` defined but never fed | two power meters that can never move; publish with a documented scale or stop defining them |
| `SliceModel::setRfGain` has no runtime path | LNA gain is connect-parameters only; the preamp control does nothing after connect |
| `TX:ALC` computed and discarded | `Hl2TxDsp` emits `alcGain`; nothing consumes it |
| Tune power not separable from RF power | TUNE keys at full drive on a fresh connect |
| `RTTY` unmapped | silently demodulated as USB; conventionally lower-sideband on HF, so it wants a decision rather than a default |
| Sideband stage saturates | even at 5 % drive into a dummy load a few inches away; needs inline attenuation or a second receiver |
| Wire-convention stimulus harness (§1.4) | inject synthetic IQ at the backend boundary; needs no radio and would have caught the receive inversion |
| **Reconnect stage** (§1.21) | connect / disconnect / immediately reconnect; nothing in the suite exercises teardown today, and a leaked session is invisible until the second connect |
| ~~**Icom `TX:SWR` / `TX:FWDPWR` / `TX:ALC` / `TX:COMPPEAK` defined but never fed**~~ | **Fixed and confirmed on hardware** (`2d5ed841`, §1.26). Remaining: the front-panel-PTT path needs a human to key it. |
| **The mic stages assume a radio that has a mic meter** | `meter-scale` and `control-effect` both certify through `TX:MICPEAK`, and the IC-705 publishes no mic-level meter at all — its set is `15 02/11/12/13/14/15/16`. On a radio that owns its own microphone (`hostModulates: false`) mic gain is not ours to set and mic peak is not ours to read, so both stages report a concern for something that is not a defect. Gate them on the capability, per §1.25 |
| ~~**Icom `micSelection` still reports `MIC`**~~ | **Fixed** — a forced selection is now adopted into TransmitModel without emitting a command nobody issued |
| ~~**Icom pan/waterfall agreement unverified**~~ | **Confirmed fixed by the operator.** Never reproduced here — it went away with the slice/pan registration fix, which is itself worth noting: a symptom that resolves as a side effect was never understood, and §2.2 is what would have told us which |
| ~~**Icom RX filter steps are not capability-driven**~~ | **Fixed** — `rxFilterWidthsHz` publishes the three reachable filters and the applet narrows to them, restoring the operator's own list on disconnect. NOT yet confirmed on screen with the applet open |
| ~~**Icom RIT/XIT has no control**~~ | **Fixed and verified on the wire** — the VfoWidget control already existed and drove SliceModel; only the last hop to the seam was missing. `21 01 01` then `21 00 00 00 00` observed. The radio still shares ONE offset register between RIT and XIT, so a UI offering two independent offsets would be lying |
| **The meter-unit fix is only partly verified** (§1.27) | ALC and forward power both move under key on an IC-705, so the unit fix is real. NOT established: whether forward power is trustworthy at low drive — at ~0.8 W of 10 W the Po meter sits in the bottom few counts of a `0=0% / 143=50%` scale and reads zero intermittently. Re-measure at higher drive before believing either the meter or the dropout it appears to show |
| **Transmit cuts out roughly once a second on FT8** | operator-visible on the radio's own panadapter. Established: the radio does NOT unkey (`mox` holds), ALC stays continuously active, and the send rate is correct (100 kB/s against 96 kB/s needed, the rest being packet headers). So audio reaches the modulator without gaps and only forward power cycles. Unresolved whether that is real RF or the low-drive meter quantisation above |
| **A revoked session looks healthy** | the radio can withdraw a session after the streams are granted; the models keep their last values and the panadapter renders a "Connecting…" spinner while `get model=pan` answers cheerfully. `liveness` now exposes this, but nothing ACTS on it — the backend should disconnect rather than pretend |
| **`liveness` meterAgeMs reads null on a live link** | spectrum and audio ages are correct on the same call. Either honest timing or `updateValueByName` not stamping `m_valueUpdatedMs` the way `updateValues` does; a liveness field that reads "never" on a working link is exactly the false signal this work exists to remove |

---

## 3. Using this on a new radio

1. **Build a model command table from its official guide.** Separate controls
   from meters even when both use byte-sized values; select model-specific
   curves and capabilities before touching hardware.
2. **Connect twice without transmitting.** On each launch, capture `liveness`,
   `controls map`, `controls meters`, and the visible widgets. A connect snapshot
   is not a subscription test.
3. **`radiocert tune`** — no permission needed. Dial goes where told; every mode
   the app can emit survives a round trip.
4. **`radiocert rx`** — no permission needed. Establish wire handedness at
   **zero shift**, off-centre, in all four SSB modes, against a known carrier.
   WWV is the default reference: free, always on, exactly known, and not us.
5. **Certify every control in both directions.** Drive UI → wire → radio → UI,
   then make an external radio-side change and allow two poll periods for
   wire → model → widget convergence. Repeat representative values after a full
   application restart.
6. **Establish the TX safety envelope before keying.** Assert the exact live
   dummy-load port, tuner bypass, frequency/mode, Tune Power and RF Power, a
   percentage ceiling, a physical-watt ceiling, fresh calibrated telemetry,
   and an unconditional unkey watchdog.
7. **`radiocert tx`** — keys. Modulation, sideband, lifecycle. Begin at the
   lowest authorized drive and stop on the first safety exception.
8. **`radiocert meters`** — keys. Check the instruments against known stimuli,
   including the actual gauge at startup, while keyed, immediately after unkey,
   and after late replies could arrive.

Read the `concern` fields first, then the measurements. The tool does not pass
or fail: thresholds meaningful for one radio are guesses for the next, and a
tool that prints PASS for hardware nobody has characterised is worse than one
that prints the numbers.

**Then do the manual checks it lists.** They are the ones no amount of internal
measurement can replace, and skipping them is how this project shipped a
transmitter on the wrong sideband with every test green.
