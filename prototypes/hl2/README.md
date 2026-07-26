# HL2 backend spike (aetherd)

Throwaway Phase‑0 prototype to de-risk a **Hermes‑Lite 2** `IRadioBackend` before
committing to an in-tree `Hl2Backend`. Lives in `prototypes/` on purpose: an
in-tree backend is a "new radio family" and (per `AGENTS.md`) needs an approved
design doc first. This spike proves the **data plane** — the biggest unknown —
cheaply in Python, so the design note + C++ port are demand-driven.

## Why HL2 is different from Flex

Flex does DSP in hardware and ships **cooked audio + spectrum** (DAX/VITA‑49).
HL2 ships **raw IQ** — the client does *all* DSP (tune / decimate / demodulate /
FFT). The spike is about proving we can ingest that IQ and turn it into a
spectrum; the eventual `Hl2Backend` owns that RX DSP chain internally and emits
the same normalized `IRadioBackend` signals as `FlexBackend`.

## Protocol authority (Principle I)

HPSDR **Protocol 1 ("Metis")** over UDP:1024. Build from the spec, don't guess.
All protocol facts are consulted **clean-room** and re-expressed in original code
(see `../../THIRD_PARTY_LICENSES`):
- openHPSDR Protocol 1 Programmer's guide (discovery, EP2/EP6 frames, C&C bytes)
- Hermes‑Lite 2 wiki + gateware repo (board id `0x06`, register map, sample rates,
  the `0x0a` extended-range LNA gain register)
- pihpsdr `src/old_protocol.c` (the round-robin C&C init sequence — also where
  the **`CONFIG_MERCURY`** bit came from, which later turned out to be a no-op on
  HL2; see the correction below)

## Phases

| Step | File | Proves |
|---|---|---|
| ✅ 0.1 discovery | `discover.py` | HL2 reachable + identified (Metis handshake) |
| ✅ 0.2 stream | `stream.py` | start → EP6 IQ ingest, framing verified (see below) |
| ✅ 0.3 tune | `tune.py` | set RX1 NCO frequency via EP2 C&C — **WWV lands exactly at DC** |
| ✅ 0.4 spectrum | `spectrum.py` | FFT the IQ → panadapter, **data plane proven end-to-end** |
| ⬜ 1 in-tree | design note → `src/core/backends/hl2/Hl2Backend` | port proven logic behind the seam |

### The one non-obvious fact (phase 0.3/0.4) — **corrected 2026-07-24**

> **This section's original diagnosis was wrong and is kept for the record.** It
> claimed the flat-ADC-floor-noise symptom was caused by a missing
> **`CONFIG_MERCURY` (C1 bit 6, 0x40)** "ADC-select" bit. Inspection of the
> Hermes-Lite 2 gateware RTL shows the HL2 never decodes that bit
> (`cmd_data[30]` appears in no module), so it cannot have been the fix. The real
> cause is **ordering**: a stream started before any Command & Control frame has
> landed emits ADC-idle samples. Priming with C&C frames *before* `metis-start`
> is the cure — which is what the in-tree `MetisClient` now does. The same bit is
> still sent for openHPSDR compatibility, and is harmless.

A receiver that streams IQ but shows only **flat ADC-floor noise** at every
frequency needs C&C to land before the stream starts. The minimal working RX is
a round-robin of **three** registers:
`config` (speed + `#RX` in C4; the `CONFIG_MERCURY` and `0x04` duplex bits are
sent for openHPSDR compatibility but ignored by HL2),
**RX1 freq** (`C0=0x04`, 32-bit Hz BE — confirmed, *not* the `0x02` TX register),
and **LNA gain** (`C0=0x14`, `C4 = 0x40 | (dB+12)`). Verified live: tuning WWV
(10 MHz) puts its carrier at baseband DC ~50 dB over a clean noise floor.
IQ sample-parse note: the ADC DC offset lives on **I only** (Q mean ≈ 0), so a
`peak|I|` metric reads the DC spur, not signal — use a DC-removed FFT.

## Running

```bash
python3 discover.py                      # smoke test — is the HL2 on the LAN?
python3 discover.py --bcast 169.254.255.255   # if 255.255.255.255 is filtered
```

Verified device (2026‑07): HL2, MAC `00:1C:C0:A2:13:DD`, board `0x06`, gateware
7.4, idle. **Dual-homed** — LAN address `192.168.50.99` (reached over WiFi here),
and also answers on a link-local `169.254.x` direct-Ethernet interface. Discovery
replies from whichever interface the broadcast arrived on. Use `192.168.50.99`
for the protocol spike; prefer the **wired** interface for sustained IQ streaming
(HL2 pushes a continuous UDP torrent — WiFi will drop/jitter).

Phase-0.2 result (2026‑07, over WiFi to `192.168.50.99`, 3 s): 1146 EP6 packets,
144 396 IQ samples ≈ **48.1 k/s** (matches the 48 kHz default → frame parse is
correct), **0.00% packet loss**, 0 sync errors. (The −48 dBFS "peak" reported
at this stage was later found to be the ADC **DC offset on I**, not signal — the
receiver had no ADC source selected yet; see the phase-0.3/0.4 note above.)

Phase-0.3/0.4 result (2026‑07, over WiFi): with the corrected 3-register init,
`spectrum.py --freq 10000000` resolves the **WWV 10 MHz carrier at baseband DC,
~50 dB over a clean floor**, and `tune.py` streams 0-drop after tuning. The
raw-IQ data plane is proven end-to-end (discover → stream → tune → gain → FFT).

## Notes / gaps this spike will surface for the seam

- **Connect flow** isn't behind the seam yet (`FlexBackend::connectRadio` is a
  stub); HL2's discovery/start needs a home in `IRadioBackend::connectRadio`.
- **Data-plane frame formats** (`spectrumFrameReady`/`audioFrameReady`) are
  step‑4 "declared but not final" — the spike shows what HL2 needs to emit.
- **Encode path** (`invokeExtension`) is a stub — HL2 tuning/keying needs it.
