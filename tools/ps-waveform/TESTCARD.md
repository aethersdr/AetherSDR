# AetherPS transmit test card

A session at the radio, into a dummy load. The operator keys every
transmission; the agent changes only the settings listed, records each one
first, and restores it at the end. Stop at any step if anything looks wrong.

## Hardware (operator)

- Dummy load on ANT1, rated for the test power with margin.
- Sampler in line between ANT1 and the dummy load (e.g. a CleanRF RF-S2K),
  its sample port through an attenuator into **RX A**.
- Attenuation so RX A sees **-20 dBm or less** at the highest test power.
  Work it out from the sampler's coupling before keying; check it on the
  RX A panadapter at the first, lowest power.
- Start at **5 W**.

## Settings (agent records, changes, restores)

| Setting | Command | Restore |
|---|---|---|
| APD off | `apd enable=0` | `apd enable=<recorded>` |
| Full duplex on | `radio set full_duplex_enabled=1` | `radio set full_duplex_enabled=<recorded>` |
| TX audio from DAX | `transmit set dax=1` | `transmit set dax=<recorded>` |
| Transmit filter | not changed | recorded only |

The operator's AetherSDR does the rest, so nothing else on the radio
changes behind it:

- a panadapter on **RX A**, centred on the transmit frequency (e.g. 14.200
  MHz), with **DAX IQ channel 1**;
- the transmit slice on ANT1, USB at first, at the transmit frequency.

AetherWSPR keeps its own slice; it only receives.

## Runs

The agent installs one package at a time and reads the waveform's status
from `http://<radio>:48996` (one JSON line: `aligned`, `delay_samples`,
`delay_corr`, `coherence`, `offset_hz`, `fits`, `correcting`).

1. **Baseline, USB, no waveform.** The operator plays `two-tone.wav`
   (700 Hz + 1900 Hz) into DAX TX and keys for ~10 s. Note IMD3 and IMD5 on
   the RX A panadapter.
2. **AetherPS, correction off** (`AetherPS-correction-off.ssdr_waveform`).
   Set the slice to PS. Key for ~10 s with the two-tone. Check:
   - the transmitted signal looks like run 1 (same tones, same level);
   - status shows `feedback_ok`, `aligned`, a plausible `delay_samples`,
     `delay_corr` near 1;
   - how wide the transmit path is: whether anything appears outside the
     transmit filter (50-4000 Hz now) and below the carrier.
3. **AetherPS, correction on** (`AetherPS.ssdr_waveform`). Same as run 2,
   keyed ~20 s. Status should show `fits` climbing and `correcting: true`.
   Compare IMD3 and IMD5 with run 2.
4. Repeat run 3 at 25 W and 50 W if run 3 helped and RX A stays at or
   below -20 dBm.

## Stop if

- SWR rises, or the dummy load heats faster than expected.
- The transmitted spectrum in PS looks wider or dirtier than in run 1.
- `correcting` is true but IMD gets worse.

## Afterwards (agent)

- Uninstall AetherPS.
- Restore every recorded setting; confirm each one by status.
- Write the results into README.md: the IMD figures, the loop delay, and
  what the transmit path did outside its filter.
