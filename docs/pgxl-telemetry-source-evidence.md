# PGXL telemetry sources: hardware evidence

Captured on 2026-09-29 on a FLEX-8600 running SmartSDR 4.2.20.41343 with a
PowerGeniusXL on firmware 3.9.8 (the PGXL greeted with `V3.9.8 AUTH`). A
TunerGeniusXL was also on the station. The operator keyed each transmit, a
TUNE in OPERATE. Private addresses and serial numbers are redacted below, and
the PGXL authorization code is not recorded anywhere.

The questions were:

1. Which PGXL readings does the radio relay, and by which route?
2. Where a reading comes from both the radio and the PGXL, which source
   refreshes it faster?

## 1. The radio's amplifier status carries no PGXL telemetry

A read-only API session subscribed to `amplifier all`. With the PGXL idle, then
through a 16-second TUNE, the radio sent this for the PGXL:

```text
17:37:18 amplifier 0x7BFE73CC ip=<PGXL_HOST> model=PowerGeniusXL serial_num=<SERIAL> ant=ANT1:PORTA,ANT2:NONE state=IDLE
17:37:39 interlock ... state=PTT_REQUESTED reason=AMP:PG-XL source=TUNE ...
17:37:39 amplifier 0x7BFE73CC state=TRANSMIT_A
17:37:39 interlock ... state=TRANSMITTING ... amplifier=0x7BFE73CC,0x76DECDDB
17:37:55 interlock ... state=UNKEY_REQUESTED ...
17:37:55 amplifier 0x7BFE73CC state=IDLE
```

The status carries `ip`, `model`, `serial_num`, `ant` and `state` only. No
`temp`, `hltemp`, `tempb`, `id`, `vdd`, `vac` or `meffa` arrived at any point.

The readings the radio does relay arrive as meters. Subscribing to
`meter all` returned five for the PGXL's handle (the `desc=External Meter` field is omitted):

```text
meter 49.src=AMP#49.num=0x7BFE73CC#49.nam=FWD#49.low=30.0#49.hi=63.0#49.unit=dBm#49.fps=0#
meter 50.src=AMP#50.num=0x7BFE73CC#50.nam=RL#50.low=0.4#50.hi=60.0#50.unit=dB#50.fps=0#
meter 51.src=AMP#51.num=0x7BFE73CC#51.nam=DRV#51.low=10.0#51.hi=50.0#51.unit=dBm#51.fps=0#
meter 52.src=AMP#52.num=0x7BFE73CC#52.nam=ID#52.low=0.0#52.hi=70.0#52.unit=Amps#52.fps=0#
meter 53.src=AMP#53.num=0x7BFE73CC#53.nam=TEMP#53.low=0.0#53.hi=100.0#53.unit=degC#53.fps=0#
```

`TEMP` is the only temperature. The Harmonic Load heatsink temperature, Vdd,
Vac, fan mode and MEffA have no radio source.

Before this change AetherSDR decoded `FWD`, `RL`, `DRV` and `TEMP` but not
`ID`, and read drain current from the amplifier status instead, where it never
arrives. With the direct connection down, the Id gauge did not move during a
transmit.

## 2. Refresh rates, radio and PGXL side by side

[`tools/probe_pgxl_telemetry_sources.py`](../tools/probe_pgxl_telemetry_sources.py)
ran both captures at once for four minutes, with one TUNE of about 10 seconds:

- **Radio:** registered a UDP port with `client udpport`, subscribed to
  `meter all`, and logged every sample of the PGXL's meters.
- **PGXL:** authenticated on port 9008 and sent `status` every 50 ms, twice
  the rate AetherSDR polls while transmitting, logging every reply.

```text
python3 tools/probe_pgxl_telemetry_sources.py --radio-host <RADIO_HOST> --pgxl-host <PGXL_HOST>
python3 tools/probe_pgxl_telemetry_sources.py --analyze
```

Output of `--analyze` for the transmit:

```text
Radio: transmit 47.11-57.54 s (10.4 s)
  FWD    19.7 samples/s,  13.3 changes/s
  RL     19.7 samples/s,  18.7 changes/s
  DRV    19.7 samples/s,  17.8 changes/s
  ID     19.7 samples/s,   0.4 changes/s
  TEMP   19.7 samples/s,   0.6 changes/s
PGXL: transmit 47.09-57.45 s (10.4 s)
  fwd     18.0 replies/s,   2.3 changes/s
  swr     18.0 replies/s,  10.8 changes/s
  id      18.0 replies/s,   0.4 changes/s
  temp    18.0 replies/s,   0.6 changes/s
  hltemp  18.0 replies/s,   0.0 changes/s
  vdd     18.0 replies/s,   0.1 changes/s
```

"Samples" and "replies" count how often a source delivers a value. "Changes"
counts how often the value is new. The radio sends every meter about 20 times a
second, but only the changes carry information.

Drain current and PA heatsink temperature change at the same moments from both
sources. Times are seconds into the capture; the radio values are the raw
meter values scaled by the radio's units (amps = raw / 256, degrees Celsius =
raw / 64):

| Id, PGXL | Id, radio | PA temp, PGXL | PA temp, radio |
|---|---|---|---|
| 47.54 s: 18.1 A | 47.58 s: 18.1 A | 51.68 s: 44.1 °C | 51.73 s: 44.1 °C |
| 48.38 s: 19.0 A | 48.42 s: 19.0 A | 52.96 s: 44.8 °C | 53.00 s: 44.8 °C |
| 51.31 s: 19.1 A | 51.31 s: 19.1 A | 55.49 s: 46.8 °C | 55.45 s: 46.8 °C |
| 54.21 s: 19.2 A | 54.22 s: 19.2 A | 56.80 s: 47.9 °C | 56.78 s: 47.9 °C |
| 57.91 s: 5.0 A (unkeyed) | 57.93 s: 5.0 A | 58.02 s: 48.9 °C | 58.09 s: 48.9 °C |

The PGXL itself updates drain current and PA heatsink temperature slowly, and
the radio passes each change on within about 40 ms.

## What AetherSDR does with this

| Reading | Sources | Faster | Used |
|---|---|---|---|
| Forward power | radio, PGXL | radio (13.3 vs 2.3 changes/s) | radio while fresh, else PGXL |
| SWR | radio, PGXL | radio (18.7 vs 10.8 changes/s) | radio while fresh, else PGXL |
| Drain current | radio, PGXL | tie | radio while fresh, else PGXL |
| PA heatsink temperature | radio, PGXL | tie | radio while fresh, else PGXL |
| Drive | radio only | | radio |
| Harmonic Load heatsink temperature, Vdd, Vac, fan mode, MEffA | PGXL only | | PGXL; blank when the direct connection is down |

A tie goes to the radio, so every shared reading follows one rule. "Fresh"
means a sample within the last 1500 ms (`kRelayMeterFreshnessMs`); a station
without the radio relay uses the PGXL's values.

## Verified in the app

With this change, during a TUNE in OPERATE:

- **RADIO** (direct connection down): Id read 19 A and moved with the
  transmit, PA read 110.0 °F with no HL value, and Vdd and Vac read "—". Before
  this change Id did not move on RADIO.
- **DIRECT** (built together with the PGXL authentication change, #6008, which
  the PGXL on firmware 3.9.8 requires): Id read 19 A, Vdd 52.0 V and Vac 117 V,
  and the temperature showed both heatsinks. Neither Id nor PA temperature
  flickered between the two sources.

## Limits

These results establish behavior for this radio and this PGXL at the firmware
versions shown, with the PGXL idle and during a TUNE transmit. They do not
establish behavior for other SmartSDR or PGXL firmware, or for voice or CW
transmit.
