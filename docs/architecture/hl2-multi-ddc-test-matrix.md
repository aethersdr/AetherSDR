# Hermes-Lite 2 — multi-DDC test matrix

Branch `feat/hl2-multi-rx`. Covers running up to four independent DDCs
(receivers / slices / panadapters) on one Hermes-Lite 2.

**How to read the Status column**

| Mark | Meaning |
|---|---|
| **SIM** | Verified by me against `hpsdrsim -hermeslite2 -P1` |
| **UNIT** | Covered by an automated test in the suite |
| **TODO** | Not verified by anyone yet — your pass is the first |
| **HW** | Needs real hardware; the simulator cannot answer it |

**The operator has since run this end to end on a real Hermes-Lite 2, transmit
included.** The `HW` rows below are therefore covered — they are kept, with their
marks, because they say *which* checks only hardware can answer, and that is what
a re-test after any refactor needs to repeat. `TODO` rows remain genuinely
unexercised by anyone.

`hpsdrsim` is *necessary, never sufficient*. Its register decode is looser than
real gateware, and it generates a synthetic scene rather than real RF — so
anything about signal quality, band filtering in the antenna path, or transmit
is **HW** no matter how green it looks in the simulator.

---

## 0. Before you start

```bash
open /Users/patj/aether/AetherSDR/.worktrees/feat-hl2-multi-rx/build/AetherSDR.app
```

- **No settings change is needed.** Connect comes up with **one** receiver;
  `Hl2Settings::receiverCount` is retired. If you previously set it, it is
  ignored.
- Radio: HL2 at `192.168.1.21`. Simulator: `./hpsdrsim -hermeslite2 -P1`.
- Running the simulator on this Mac alongside the real radio is now safe.
  `hl2_tx_loopback_test` used to probe a hardcoded `192.168.1.12` — this
  machine — so a local simulator made it run when it should have skipped. It now
  defaults to loopback and refuses to key anything that does not answer discovery
  with hpsdrsim's synthetic `AA:BB:CC:DD:xx:FF` MAC, so it cannot reach the
  radio. It also skips a simulator that is already streaming to another client,
  so running the suite cannot take a session out from under an app you have
  driving it. Point it elsewhere with `AETHER_HL2_SIM_HOST` (an IP literal —
  names are not resolved) if the simulator is on another box.
- Only one client per radio. Check nothing else is connected first.
- Turn on the HL2 log: `QT_LOGGING_RULES="aether.hl2*=true"`. Most rows below
  are confirmed from a log line, not from the screen.

---

## 1. Lifecycle — add and close receivers

| # | Test | Expected | Status |
|---|---|---|---|
| 1.1 | Connect | Exactly **1** panadapter, 1 slice. No settings file involved | SIM |
| 1.2 | Add Panadapter ×3 | DDC 1, 2, 3 appear; log `added receiver — DDC n … running n+1 of 4` | SIM |
| 1.3 | Add a 5th | Refused at the board's reported limit | SIM |
| 1.4 | Close pane 2 of 3 | Pane 2 goes; panes 1 and 3 **keep their identity** — the third is still the third | UNIT |
| 1.5 | Close the last pane | **Refused.** A radio with no receivers is not reachable | SIM |
| 1.6 | Close the TX pane | Transmit moves to a surviving receiver, log `transmit moves from DDC n to m` | UNIT |
| 1.7 | Add/close 10× in a row | No WDSP channel leak — receivers still open on the 10th | TODO |
| 1.8 | Close a pane, then add | New pane gets a **fresh** number, never a retired one | UNIT |
| 1.9 | **Pop out** a pane | Pops out, keeps streaming, controls still address its own DDC | **TODO** |
| 1.10 | **Maximize** a pane | Maximizes and restores; other DDCs keep running | **TODO** |
| 1.11 | Disconnect / reconnect at 4 DDCs | Comes back at 1 (by design). No orphaned panes | TODO |

Rows 1.9 and 1.10 are the ones I did **not** verify.

---

## 2. Link budget — the rate × receiver-count product

Wire rate in Mbit/s, including UDP/IP/Ethernet headers. The HL2's ethernet is
**100BASE-T**; the policy admits 70% of it.

| Rate | 1 RX | 2 RX | 3 RX | 4 RX | Max RX |
|---|---|---|---|---|---|
| 48 kHz | 3.3 | 5.9 | 8.4 | 11.1 | 4 |
| 96 kHz | 6.7 | 11.7 | 16.9 | 22.2 | 4 |
| 192 kHz | 13.4 | 23.4 | 33.7 | 44.4 | 4 |
| 384 kHz | 26.8 | 46.8 | 67.5 | **88.8** | **3** |

| # | Test | Expected | Status |
|---|---|---|---|
| 2.1 | 4 DDCs, zoom out to 384 kHz | **Refused**; span snaps back, log names the Mbit/s | UNIT |
| 2.2 | 3 DDCs at 384 kHz | Allowed (67.5 Mbit/s) | TODO |
| 2.3 | At 384 kHz, try to add a 4th | Refused — ceiling falls with the span | TODO |
| 2.4 | 4 DDCs at 192 kHz, watch dropped-EP6 counter | Stays at zero over several minutes | **HW** |
| 2.5 | Zoom limits shown in the UI | Upper limit shrinks as receivers are added, so you cannot reach a refused span | TODO |

Row 2.4 is the one that matters most on real hardware — it is the difference
between the budget being right and being merely plausible.

---

## 3. Shared hardware — one AD9866 behind every DDC

| # | Test | Expected | Status |
|---|---|---|---|
| 3.1 | 2 DDCs, **same** band | Band filter stays engaged; **no WIDE** indicator | SIM |
| 3.2 | 2 DDCs, **different** bands | Filter **bypasses**; **WIDE** shows on *both* panes; log names the frequencies and says the AM-broadcast HPF is out | SIM |
| 3.3 | Bring them back to one band | Filter re-engages, WIDE clears | SIM |
| 3.4 | Preamp change on one pane | **Every** pane's RF-gain indicator follows — one register, one answer | SIM |
| 3.5 | Preamp change, watch the trace | Noise floor does **not** jump — the dB reference moves with the gain | **HW** |
| 3.6 | Spanning bands near a broadcast transmitter | Noise floor **rises** on every receiver. This is the real cost of bypass | **HW** |
| 3.7 | Span bands, then key | While keyed the **TX receiver's** filter wins — harmonics stay legal | **HW** |

Row 3.7 is a legality check, not a convenience one. Worth doing deliberately.

Bridge helper for 3.4:
```bash
pan rfgain 0xe1000000 6
```

---

## 4. Per-slice controls

Each row: change it on **one** slice, confirm the others are untouched.

| # | Control | Expected | Status |
|---|---|---|---|
| 4.1 | Frequency | Only that DDC's NCO moves; its pan centre follows, others still | SIM |
| 4.2 | Mode | Only that slice; the TX chain follows **only** if it is the TX slice | UNIT |
| 4.3 | Filter / passband | Per slice | UNIT |
| 4.4 | AGC mode + threshold | Per slice | UNIT |
| 4.5 | **Mute** | That slice drops out of the mix; others keep playing | SIM |
| 4.6 | **AF gain** | That slice's level only | TODO |
| 4.7 | **Balance** | That slice moves L/R; centre is unity, not a 3 dB dip | TODO |
| 4.8 | Mute the TX slice | Muting receive must not affect transmit | TODO |
| 4.9 | S-meter per pane | A strong signal on one does **not** move another's needle | TODO |
| 4.10 | Frame rate per pane | Background panes can run slower; span is shared, frame rate is not | TODO |

---

## 5. Transmit

**Dummy load only.** Suggested: 7.200 LSB, per the standing authorisation.

| # | Test | Expected | Status |
|---|---|---|---|
| 5.1 | TX indicator on VFO panel | Exactly **one** slice shows TX | SIM |
| 5.2 | Click TX on another slice | Transmit moves; old indicator clears, new one lights | SIM |
| 5.3 | After moving, check TX frequency | TX NCO follows the **new** slice. Nothing reads it back, so verify by transmitting | **HW** |
| 5.4 | After moving, check sideband | TX mode/passband follow the new slice | **HW** |
| 5.5 | Tune a non-TX slice while keyed | Transmit frequency does **not** follow it | **HW** |
| 5.6 | Key with 4 DDCs running | All four mute during TX; none plays your own carrier back | TODO |
| 5.7 | Unkey | Audio returns on every unmuted slice, no stale tail | TODO |
| 5.8 | Forward power / SWR at 4 DDCs | Unchanged from single-DDC operation | **HW** |

Rows 5.3 and 5.4 cannot be answered from the app — nothing reads the TX NCO
back. That is exactly how the original wrong-band and wrong-sideband bugs
survived (`docs/HERMES.md` §14, §16). Confirm with a receiver or the wspr.live oracle.

---

## 6. TCI (WSJT-X)

Enable *Settings → Autostart TCI*.

| # | Test | Expected | Status |
|---|---|---|---|
| 6.1 | Connect WSJT-X, single VFO | Works as before | TODO |
| 6.2 | Split = Rig | Second DDC is **created**; `split_enable:…,true` confirmed | SIM |
| 6.3 | Split, then check the radio | 2 DDCs; transmit on the **second** | SIM |
| 6.4 | Disconnect WSJT-X | Transmit returns to DDC 0 | SIM |
| 6.5 | Split when already at the receiver limit | Refused cleanly; WSJT-X falls back to single-VFO, does **not** hang | UNIT |
| 6.6 | Decode FT8 on both DDCs, different bands | Both decode. Note WIDE is on — see 3.6 | **HW** |
| 6.7 | Transmit FT8 via TCI split | Correct band **and** sideband | **HW** |

---

## 7. Reliability / soak

| # | Test | Expected | Status |
|---|---|---|---|
| 7.1 | 4 DDCs for 1 hour | No dropped EP6, no audio dropouts, no drift | **HW** |
| 7.2 | Add/close during heavy RF | No wedge, no stuck stream | **HW** |
| 7.3 | Kill the app at 4 DDCs (SIGTERM) | Radio released; reconnect works without a power cycle | **HW** |
| 7.4 | Pull the ethernet at 4 DDCs | Clean disconnect, no hang | **HW** |
| 7.5 | CPU at 4 DDCs | Four WDSP channels + four FFTs — record the number | TODO |
| 7.6 | Add/close on a LOSSY link (wifi, or a shaped path) | Link stays up. The stream restart's metis-start is retried on loss; unretried, one dropped datagram ended the session ~2 s later on the silence watchdog | `hl2_receiver_count_restart_test` |
| 7.7 | ~10 add/close cycles, then check every survivor | Spectrum and audio still arrive for each. Closing the middle renumbers every later DDC, and a chain wired to an index goes quiet with nothing logged | `hl2_receiver_churn_test` |
| 7.8 | 7.7 under `-fsanitize=thread` | No race naming the receiver vector. Read the DIFFERENTIAL, not the count — QtCore is uninstrumented, so every `BlockingQueuedConnection` reports as a race (`hl2_backend_test` alone: 57). See HERMES §20.15.1 | TODO |

Row 7.3 matters: the SIGTERM wedge was fixed by `Hl2EmergencyStop` (#4503,
now in main). `kill -9` still wedges the radio — that is expected, not a bug.

Rows 7.6–7.8 came out of review rather than operation, and 7.8 is the only one
here that needs a special build:

```bash
CXXFLAGS="-fsanitize=thread -g -fno-omit-frame-pointer -O1" LDFLAGS="-fsanitize=thread" \
  cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

---

## Wire-format reference

Numbers a failure can be checked against. Payload geometry is
`6N + 2` bytes per round, rounds never straddle a 512-byte frame, and the tail
is zero-padded.

| Receivers | Round bytes | Rounds/frame | Samples per receiver per packet |
|---|---|---|---|
| 1 | 8 | 63 | 126 |
| 2 | 14 | 36 | 72 |
| 3 | 20 | 25 | 50 |
| 4 | 26 | 19 | 38 |

Adding or closing a receiver **restarts the EP6 stream** on purpose — the
payload layout changes and the packet carries no marker for where. A brief
audio gap there is correct behaviour, not a fault.

---

## Known-failing, not caused by this work — now fixed

`hl2_tx_loopback_test` used to fail against the simulator on transmit-sideband
checks, and commit `256142a6` (pre-multi-DDC) failed identically, so the "not
caused by this work" call was right. It was not a transmit fault: the test
expected the fed-back tone BELOW centre, which was correct only against the
pre-#4471 panadapter that drew the raw wire. #4471 added the receive-side
conjugation and the expectation went stale. The run-to-run variation was the
hardcoded `192.168.1.12` reaching a simulator on another machine.

No exclusion is needed any more — run the whole suite:

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build -j22
```

It SKIPS cleanly when no simulator is running, so it is safe in CI and on a
machine connected to real hardware — and the skip is honest rather than silent:
it exits 77, and `SKIP_RETURN_CODE` on the `add_test` makes ctest report
`***Skipped`. A run that measured nothing cannot be mistaken for one that keyed
and passed.
