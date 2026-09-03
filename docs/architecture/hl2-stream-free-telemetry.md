# HL2 stream-free telemetry — Design Note

**Status:** Draft. The decode half is built and tested
(`MetisProtocol.cpp::parseDiscoveryReply`); this note is the plan for the
*poller* that uses it, and exists chiefly to settle **when** to poll rather
than leave that to a timer somebody picks later.

**Scope:** reading the radio's own state — PA temperature, forward and reverse
power, PTT, ADC clip, TX FIFO, PTT hang time — **without an IQ stream**, and
therefore while another client holds the radio or while our own stream is
broken. Roadmap item #15. Transmit behaviour, register *writes*, and the
RQST/ACK command plane (item #13) are out of scope; §6 says why the last of
those is not a prerequisite.

Every claim below is labelled **R** read from source, **M** measured, **I**
inferred, or **A** assumed/unestablished, following the convention `HERMES.md`
§2 argues for. Gateware citations are `softerhardware/Hermes-Lite2` at
`883a338`, the SHA in this radio's discovery string `20231230_74p2_883a338`;
board variant `hl2b5up_main`.

---

## 1. What is already true, and what is missing

AetherSDR already ingests the full telemetry set **while it holds the stream**.
The EP6 C&C bytes carry a four-slot round-robin that free-runs with no request
from us; `MetisClient` applies it from both frames of every packet and publishes
at 10 Hz; `Hl2Backend::publishTelemetry` turns it into SWR, forward watts, PA
temperature and an ADC-overload edge. That path is done. **R**

What does not exist is any way to read the radio when we are *not* streaming:

| situation | in-band (EP6) | today |
|---|---|---|
| we hold the stream | works | fine |
| another client holds the radio | no stream to read | **nothing** |
| our stream has stalled or dropped | the packets that carry telemetry are the packets that stopped | **nothing** |
| connected to nothing yet | no stream | **nothing** |

The last three are exactly where an operator most wants the radio's own state,
and the third is the one that matters most: **the telemetry rides the very
packets whose absence is the fault being diagnosed.** A transport cannot report
its own silence, which is the same lesson `HERMES.md` §21.2 already paid for
with `LinkStats`.

## 2. The route, and why it costs nothing to ask

The 60-byte discovery reply already carries all of it, at offsets `0x17`–`0x29`.
We have been receiving those bytes at every discovery and discarding them since
the parser was written; `parseDiscoveryReply` now decodes them. **R**

The request is the discovery packet we already send: `EF FE 02` + 57 zeros. The
gateware accepts it on ports **1024 and 1025** with no `run` term anywhere in
the decode path (`dsopenhpsdr1.v:185-207`), and builds the same reply either way
(`usopenhpsdr1.v:261-307`). **R**

### 2.1 Port 1025, and why it is not optional

Which port asked decides only where the reply is *addressed*, and that is the
whole reason this design exists. `network.v:686-698`:

```verilog
if (to_port[0]) begin                    // 1025: always update
  udp_destination_ip_sync   <= udp_destination_ip;
end else if (~run) begin                 // 1024: frozen while streaming
  run_destination_ip <= udp_destination_ip;
```

Port 1024 replies go to `run_destination_*`, which is **frozen while `run` is
asserted**. So a port-1024 poll issued while somebody else is streaming is
answered *to that somebody else*. Port 1025 keeps its own destination and always
answers the asker. **R**

Consequence for this design: **the poller uses 1025.** Port 1024 would work only
in the one case where we are the streaming host — which is precisely the case
where we do not need it, because the in-band path is already running.

### 2.2 It is a read, and stays one

The poller sends exactly one packet type: `EF FE 02`. It never sends
`metis-start`/`metis-stop` on 1024, never issues a port-1025 *command*
(`EF FE 05`), and never writes a register. That is not a convention — it is the
guard that makes it safe to poll a radio another operator is using. `hl2-diag`'s
`hl2_monitor.py` states the same rule for the same reason.

## 3. When to poll — the part that must not be a timer

Discovery **preempts the IQ path**. In `usopenhpsdr1.v`'s transmit state
machine, `START` tests the discovery branch (`:234`) *before* the EP6 branch
(`:238`), so each poll inserts a 60-byte datagram ahead of a queued IQ packet.
**R** How much that costs in practice is **not measured** here; the reply is
~1/17 the size of an EP6 packet, so the delay is small, but "small" is an
inference and not a budget. **I/A**

Three facts bound the cadence, and only one of them is a measurement of the
thing that matters:

- Port 1025 sustains **~86 Hz with zero losses**, round trip 0.6 ms median /
  9.1 ms worst. **M** (`hl2-lab` FACTS, `reference-tx/docs/telemetry-bandwidth.md`)
- **How fast the radio refreshes those fields is NOT known.** With no RF the
  ADC fields dither across 3–4 codes, so timing their changes measures the
  dither. The data is consistent with anything from ~10 Hz upward. **A**
- The in-band path already publishes at 10 Hz (`kTelemetryMinIntervalMs`). **R**

So "how fast *can* we poll" is answered and irrelevant. The question is how fast
is *useful*, and above the refresh rate the answer is: not at all — extra polls
return the same reading and buy nothing but wire contention. Since the refresh
rate is unestablished from ~10 Hz down, **any cadence above 10 Hz is spending
EP6 slots for information the radio may not have regenerated.**

### The rule

**Poll only when the in-band path is not delivering, and poll slowly.**

| state | cadence | why |
|---|---|---|
| we hold the stream and EP6 is arriving | **do not poll at all** | the in-band path already delivers the same fields, in the same units, at 10 Hz. A poll adds contention for zero new information |
| we hold the stream and EP6 has stopped | **2 Hz** | this is the case item #15 exists for. The instrument must keep reading exactly when the thing it shares a socket with has failed |
| another client holds the radio | **1 Hz** | a status display, not a meter. Nobody acts on sub-second PA temperature, and this poll lands in someone else's session |
| idle / not connected | **1 Hz while a telemetry surface is visible, never otherwise** | polling a radio nobody is looking at is pure cost |

Two things this rule gets right that a single timer would not. It makes the
poller's *duty* the complement of the in-band path's, so the two never compete
for the same wire at the same time. And it puts the highest cadence in the
failure case rather than the healthy one — which is the opposite of what a
"refresh every N ms" timer does, and the whole point of §1's last row.

**Not established, and it bounds all of the above:** the radio's actual refresh
rate. If it is measured and turns out to be well above 10 Hz, the stalled-stream
row is worth revisiting. Nothing else changes, because nothing else is limited
by the radio.

## 4. Which source wins, and the state that must not collapse

Two paths now produce the same fields in the same raw units — deliberately, so
they cross-check rather than being two unrelated numbers. When both have spoken,
**the in-band reading wins**: it is fresher, it costs nothing extra, and it is
the one whose cadence we control.

But a consumer must be able to tell these apart, and they are three states, not
two:

| reading shown as | actually |
|---|---|
| absent | **we have never asked** · **we asked and got no reply** · we asked, got a reply, and the field was not in it |

Those want different actions — start the poller, check the network, accept that
this gateware has no `EXTENDED_RESP` — so the model carries the *source and the
age* alongside the value, not a bare optional. The pattern is `HERMES.md`
§21.3's: a measurement of nothing and the absence of a measurement are different
claims, and a readout that renders both as a dash has picked one.

The specific traps already known:

- **`adcClipCount` means two different things depending on `streaming`.**
  Only an EP6 packet clears it (`control.v:465`), so while streaming it is
  "clip windows in the last EP6 interval, 0–3 saturating" and while idle it is
  "clipped at least once since the last stream ended" — stuck at 3, and a
  poller cannot clear it. It is not a count either: `rxclip` is a sticky rail
  latch added as a *level*, so a few clock edges saturate it. **R** Any surface
  showing it must pair it with the streaming state, and **must never derive a
  rate** — the window length in wall-clock terms is unestablished. **A**
- **`0x03` in the status byte is `run` OR a gateware flash erase having just
  finished** (`usopenhpsdr1.v:266`), and `0x04` is a flash write in progress.
  **R** Harmless while nobody flashes over Ethernet; named so it is not
  rediscovered.
- **A gateware without `EXTENDED_RESP` sends hard zeros in these bytes**
  (`control.v:826, :914`), which this layer cannot distinguish from genuine
  zeros. Our board sets `EXTENDED_RESP(1)`
  (`variants/hl2b5up_main/hermeslite.v:110`). **R** A caller needing certainty
  must compare across polls; the decode says so at the site rather than
  pretending.

## 5. Shape of the change

Additive, and outside the region `Hl2Backend`'s transmit drive path occupies.

1. **Done.** `parseDiscoveryReply` decodes `0x17`–`0x29` into optional fields on
   `DiscoveryReply`, with the gateware's offsets pinned by test against
   `usopenhpsdr1.v`'s own down-counter.
2. **Next.** A small poller owning one UDP socket to `<radio>:1025`, sending
   `EF FE 02` on the §3 schedule and emitting the parsed reply. It belongs
   beside `Hl2Discovery` — same packet, same parser, different port and
   lifetime — and not inside `MetisClient`, whose socket and thread belong to
   the stream this is supposed to outlive.
3. **Then.** A seam field carrying source and age, and the surface that renders
   it, including the "another client holds the radio" case which no existing
   readout has ever had to express.

## 6. Why item #13 is not a prerequisite

The roadmap calls RQST/ACK (item #13) "the gate for everything below it" and
also calls item #15 "pollable without a stream, the cheapest first increment".
Settled in the lab's `streams/hl2-telemetry/docs/gating.md`: they are about
different channels, and the contradiction is in the word "below" in a table, not
in the radio.

RQST/ACK is the **command-echo** path — `iresp <= {1'b1, resp_cmd_addr, ...}`
with the gateware's own `// Queue size is 1` (`control.v:467-468`) — and it
gates the *register* plane: PA bias, config EEPROM, AD9866 registers, the IO
board. **R** None of temperature, power, PTT or clip is a command response.
They ride the discovery reply unconditionally, and the EP6 slots automatically.
Nothing in this design sends `EF FE 05`, and nothing in it needs to.

## 7. What this note does not establish

- **Nothing here is measured** except the two figures explicitly marked **M**,
  both from `reference-tx`'s bandwidth run rather than from this code.
- The radio's telemetry **refresh rate** (§3) — the one number that would let
  the cadence be derived rather than argued.
- The **wire cost of a poll during streaming** (§3) — bounded by inference from
  packet sizes, not measured.
- The **`rxclip` window** in wall-clock terms (§4), without which no clip rate
  exists.
- Whether a port-1024 poll from the *streaming* host is answered to itself. The
  gateware says the reply goes to `run_destination_*`, which is that host — but
  it has not been run, and this design does not depend on it, because it uses
  1025.
