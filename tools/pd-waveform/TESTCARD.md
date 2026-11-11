# AetherPD transmit test card

A session at the radio, into a dummy load. The operator keys every
transmission; the agent changes only the settings listed, records each one
first, and restores it at the end. Stop at any step if anything looks wrong.

## Hardware (operator)

- Dummy load on ANT1, rated for the test power with margin.
- Sampler in line between ANT1 and the dummy load, its sample port into
  **RX A** (not XVTR, which also transmits).
- RX A must see **-20 dBm or less** at the highest test power, because full
  duplex leaves it unprotected. Work it out from the sampler's measured
  coupling, and check it on the RX A panadapter at 5 W before going higher.
- Start at **5 W**.

On the AetherSDR bench (FLEX-8600, 2026-11-11) the sampler is a CleanRF
RF-S2K+ with no pad, knob fully counter-clockwise. Its measured coupling
(aether-fabric `bench/rf-s2k/records/2026-11-11-coupling.md`) is -67.8 dB on
40 m, -61.4 dB on 20 m and -56.5 dB on 10 m, so RX A sees about -31 dBm at
5 W and -24 dBm at 25 W on 40 m, and -24 dBm at 5 W on 20 m. The runs below
are on **40 m (7.150 MHz) at 5, 10 and 25 W**, with **20 m at 5 W** as a
cross-check; 10 m needs a pad. The waveform is USB only, which is fine into
a dummy load on 40 m.

## Settings (agent records, changes, restores)

| Setting | Command | Restore |
|---|---|---|
| APD off | `apd enable=0` | `apd enable=<recorded>` |
| Full duplex on | `radio set full_duplex_enabled=1` | `radio set full_duplex_enabled=<recorded>` |
| TX audio from DAX | `transmit set dax=1` | `transmit set dax=<recorded>` |
| Transmit filter | not changed | recorded only |

The operator's AetherSDR does the rest, so nothing else on the radio
changes behind it:

- a panadapter on **RX A**, centred on the transmit frequency (7.150 MHz),
  with **DAX IQ channel 1**;
- the transmit slice on ANT1, USB at first, at the transmit frequency.

AetherWSPR keeps its own slice; it only receives.

## Before the runs

1. **Leakage:** terminate the RX A cable instead of the sampler. Full duplex
   on; key 5 W on 7.150 MHz for a few seconds and read RX A at that
   frequency. That is the radio's own leakage into RX A.
2. **Sample level:** reconnect the sampler, key 5 W again and read RX A
   (expect about -31 dBm). Go on only if it sits more than about 30 dB above
   the leakage.

## Runs

The agent installs one package at a time and reads the waveform's status
from `http://<radio>:48996` (one JSON line: `aligned`, `delay_samples`,
`delay_corr`, `coherence`, `offset_hz`, `fits`, `correcting`).

1. **Baseline, USB, no waveform.** The operator plays `two-tone.wav`
   (700 Hz + 1900 Hz) into DAX TX and keys for ~10 s. Note IMD3 and IMD5 on
   the RX A panadapter.
2. **AetherPD, correction off** (`AetherPD-correction-off.ssdr_waveform`).
   Set the slice to PD. Key for ~10 s with the two-tone. Check:
   - the transmitted signal looks like run 1 (same tones, same level);
   - status shows `feedback_ok`, `aligned`, a plausible `delay_samples`,
     `delay_corr` near 1;
   - how wide the transmit path is: whether anything appears outside the
     transmit filter (50-4000 Hz now) and below the carrier.
3. **AetherPD, correction on** (`AetherPD.ssdr_waveform`). Same as run 2,
   keyed ~20 s. Status should show `fits` climbing and `correcting: true`.
   Compare IMD3 and IMD5 with run 2.
4. Repeat run 3 at 10 W and 25 W on 40 m if run 3 helped, then once at
   5 W on 20 m (14.200 MHz, with the RX A pan moved there).

## Stop if

- SWR rises, or the dummy load heats faster than expected.
- The transmitted spectrum in PD looks wider or dirtier than in run 1.
- `correcting` is true but IMD gets worse.

## Afterwards (agent)

- Uninstall AetherPD.
- Restore every recorded setting; confirm each one by status.
- Write the results into README.md: the IMD figures, the loop delay, and
  what the transmit path did outside its filter.
