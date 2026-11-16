# Correction selection, signed upgrade and matched RF qualification

On the FLEX-8600 (firmware 4.3.16.42034), the ordinary Linux AetherSDR
workflow installed the signed correction-selection successor, retained its
identity across reboot, and measured correction improvement using **local
DAX IQ with FDX disabled**. This is bounded development evidence, not a
production release claim. The complete P1–P10 objective remains open.

Native source: `82cf771d8eb4b453060f778f325fa482e8dedf5f`.
Desktop source: `2245f5aaa6dcf941c365a4166cf2ee4c9510ba13`.
Release SHA-256:
`176f8c7a9ae68562c280a84b8b1dfc88516b5857811afdf37bac68fb24459b49`.

## Installation and privilege lifetime

One approved update through the GUI committed and confirmed temporary
installer cleanup in 48.189 seconds. The predecessor's private signing key
was not required: the package independently pins and verifies the previous
and candidate public keys and signed artifacts. The update preserved the
existing provision file byte for byte. The broad diagnostic bridge did not
execute the upgrade or the RF measurements; it was used separately to audit
the resulting host state.

The audit matched all runtime executable hashes to the built candidate.
Temporary upgrade directories, units, carrier container and carrier image
were absent. After Stop, DSP/IQ workers, private packet sockets and the fixed
ARP entry were absent; `accept_local` remained zero. The primary shim and
retained private control peer/supervisor ran as UID 78666 with all capability
sets zero and `NoNewPrivs=1`. The retained control peer supports stopped
status, so Stop is not described as leaving only one process.

One commanded reboot changed the kernel boot ID. The same exact committed
release and identity were ready after 64.265 seconds. The ordinary GUI then
started a new session and repeated idle bypass → enable → bypass without
arming a source or transmitting. This is not a physical power-loss or
interrupted-transaction test, and is not FLEX-6300 qualification.

## Matched bench measurements

Each pair used 7.150 MHz, ANT1 → dummy load/sampler → attenuated RXA,
preamp off, stock APD off and FDX off. Correction selection used the ordinary
GUI. Each transmission was a separate explicit bridge action, with cooldown
between runs. The measurement source retained its eight-second bound and
independent 9.5-second unkey cutoff. No automatic rearm or RF retry occurred.

IMD3 values below are **dBc per tone**, lower/upper sidebands:

| Nominal power setting | Correction off | Correction on | Improvement |
|---|---:|---:|---:|
| 5 W | −28.02 / −27.49 | −44.29 / −54.04 | 16.27 / 26.55 dB |
| 10 W | −27.47 / −26.93 | −44.67 / −50.59 | 17.20 / 23.65 dB |

Each result averages four 16,384-sample complex IQ frames at 48 kHz.
The 10 W pair followed the reboot. Fundamental-derived power differed by
0.318 dB at the 5 W setting and 0.314 dB at the 10 W setting, within the
checker's 0.5 dB matching bound. Both sidebands at both powers were reported
above the analyzer's noise-limited threshold.

All four exact exports, including raw float32 IQ and per-frame hashes, are
retained in [evidence/20261116-correction](evidence/20261116-correction/).
The independent NumPy FFT/interpolation implementation reproduced the
spectra within 0.000004 dB and reproduced the IMD ratios. Recheck from the
repository root:

```sh
python3 tools/pd-waveform/recheck_iq_export.py \
  tools/pd-waveform/evidence/20261116-correction/5w-on-01.json \
  --baseline tools/pd-waveform/evidence/20261116-correction/5w-off-01.json
python3 tools/pd-waveform/recheck_iq_export.py \
  tools/pd-waveform/evidence/20261116-correction/10w-on-01.json \
  --baseline tools/pd-waveform/evidence/20261116-correction/10w-off-01.json
```

Numerical agreement checks the exported samples and computation; it does
not independently validate the RF measurement chain. Nominal settings are
not calibrated output watts: the maximum sampled radio forward readings
were about 3.34 W at the 5 W setting and 6.95 W at the 10 W setting. ADC headroom and absolute IQ scaling remain
unknown, and LP-100A integration remains deferred.

## Remaining qualification

One finite pair at each setting establishes improvement under these bench
conditions. It does not establish sustained voice performance, long-run
scheduling reliability, all-band behavior or parity with stock APD. The
natural 128-sample slip and historical all-bypass cold start still need a
root cause; successful acquisitions do not close those investigations.
Cold capture still completes close to the existing source deadline.

Final process-failure and interrupted-transaction gates, production package
trust/distribution, Windows/macOS qualification, actual FLEX-6300 installation
and RF testing, stock-APD comparison and independent release review remain.

The [machine-readable report](CORRECTION-SELECTION-LIVE-QUALIFICATION.json)
retains exact run telemetry, authenticated installation observations, the
host audit, hashes and test-harness limitations, including the initial
terminal-text and DAX-panel visibility mismatches. Earlier evidence is
preserved in its original reports.
