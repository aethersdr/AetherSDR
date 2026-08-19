# TX control testing — standard authorization and method

Use this prompt and method for live automation-bridge tests against a real
radio. Replace every bracketed value; do not infer them from an earlier run.

## Authorization prompt

> Run a control-and-transmit sweep against `[RADIO MODEL]` over the AetherSDR
> automation bridge. The dummy load is connected to `[EXACT TX ANTENNA VALUE]`.
> You are authorized to transmit up to `[MAX PHYSICAL WATTS] W` at
> `[FREQUENCY]` in `[MODE]`.
>
> Before keying, assert the exact live TX antenna, tuner bypass, frequency,
> mode, RF Power, and Tune Power. Treat an unreadable state as failure. Apply a
> conservative percentage ceiling and a separate measured-watt watchdog; a
> power-control percentage is not watts. Unkey immediately on over-limit power,
> missing/stale power telemetry, high SWR, timeout, disconnect, or any state
> disagreement.
>
> Cover meters, control set/reply/external-change paths, a short TX window, and
> a complete AetherSDR restart. Verify actual visible widgets, not only models.
> Restore all non-transmitting state, confirm the radio is unkeyed, and leave
> the tuner bypassed if restoring its prior state would require RF.

## Hard safety gates

All gates must pass before each keying stage:

1. Explicit authorization names the radio, frequency/mode, exact dummy-load
   antenna value, and maximum physical watts.
2. `dumpTree` reports exactly that TX antenna. Empty, ambiguous, or different
   values abort the run.
3. The tuner already reports bypass. `atu bypass` does not key, but changing a
   tuned state is not automatically reversible without a new RF tune cycle.
4. RF Power and Tune Power are both read and staged conservatively. Icom
   two-tone uses Tune Power; an ATU cycle may use backend/radio-specific drive.
5. `AETHER_AUTOMATION_TX_MAX_POWER` is treated only as a 0–100 control
   percentage ceiling. It is never reported as a watt limit.
6. A calibrated forward-power meter must produce a fresh sample promptly after
   key. Unkey on the first sample above the authorized watt limit. This is a
   reactive backstop and cannot prevent the first transient; use an external
   wattmeter/interlock when a transient would be unacceptable.
7. Every exit path sends unkey and confirms tuning, MOX, model transmitting,
   and radio transmitting are false.

Do not run unattended TX certification on a backend with no calibrated live
power meter unless an independent physical interlock enforces the watt limit.

## Evidence sequence

### 1. Baseline and liveness

- Confirm one automation instance and no competing radio session.
- Capture `whoami`, `liveness`, `health`, `controls map`, `controls meters`, and
  the relevant `dumpTree` widgets.
- At idle, require the visible forward-power gauge to read zero. A retained
  backend sample is diagnostic history, not current power.

### 2. Control convergence

For each control:

1. record the operator's original radio and UI value;
2. drive the actual widget or semantic bridge verb;
3. verify expected protocol bytes from the model's official guide;
4. wait for the radio reply and verify model plus actual widget;
5. change it externally at the radio or with a safe raw protocol command;
6. allow two periodic-poll intervals and verify model plus widget again; and
7. restore the original value when restoration does not transmit.

For Icom 0000–0255 percentage levels, independently calculate:

```
write raw = ceil(percent * 255 / 100)
read percent = floor(raw * 100 / 255)
```

Do not use that formula for `15 xx` meters; use the model-specific published
calibration curve. CI-V Transceive is helpful but is not accepted as the only
subscription path.

### 3. Short transmit window

- Begin with the lowest authorized Tune Power percentage.
- Sample forward power, SWR, ALC, compression, voltage, current, and thermal
  data that the radio actually supports. Mark unsupported meters as such.
- Reject stale ages and rail-pinned values.
- Verify the actual power gauge is live only while keyed.
- Unkey immediately, then verify the gauge is zero both at the edge and after a
  late in-flight response could arrive.

Use the guarded harness:

```bash
python3 tools/tx_meter_test.py \
  --ant ANT1 \
  --max-watts 10 \
  --levels 2,5
```

`--levels` and `--two-tone-percent` are Tune Power percentages, not watts.

### 4. Restart proof

Close AetherSDR completely and repeat the baseline with a fresh process. Verify
distinctive safe values are adopted from the radio rather than replayed from
client defaults. Repeat one external-change/poll test. A same-process
disconnect/reconnect is useful but does not replace an application restart.

## Deliverable

Report a table with one row per control/meter and separate columns for:

- set path;
- protocol value;
- same-session radio reply;
- visible widget;
- external-change convergence;
- post-restart adoption; and
- restoration.

Report peak measured watts, SWR, telemetry freshness, abort reason, and final
unkey state. Never call a percentage setting “watts.”
