# HL2 auto RF gain replay inputs

The recorded on-air legs behind `tests/Hl2AutoGainReplayD168.h` and
`tests/Hl2AutoGainReplayD245.h`. Each header is generated from its directory
here, and `ctest` checks that it still is:

```bash
python3 tools/gen_hl2_autogain_replay_d168.py           # rewrite the header
python3 tools/gen_hl2_autogain_replay_d168.py --check   # exit 1 if it differs
python3 tools/gen_hl2_autogain_replay_d245.py           # rewrite the header
python3 tools/gen_hl2_autogain_replay_d245.py --check   # exit 1 if it differs
```

Both runs were recorded receive only, from one Hermes-Lite 2 on the air, by a
runner that armed the auto RF gain law and polled the backend's health 4 times
a second. Per leg there are two inputs:

- **the poll series**, `series-<leg>.json.gz`: one record per poll, Unix time
  `t` and the five fields the generators read — `totO` (overload-bit
  observations so far), `blocks` (bandscope blocks so far), `blockAgeMs`,
  `headroom` (dB, two decimals, `null` when there is no reading) and `railN`.
  The recording held about twenty fields per poll; the others are dropped here,
  every kept value is exactly as recorded, and gzip only keeps the files small.
- **the app log's auto-gain lines**, which carry the attack and release times,
  the offsets and each attack's `window K / N` overload count.

| dir | recorded | legs | log |
|---|---|---|---|
| `d168/` | 2026-10-01 22:32-22:50 CEST, LNA +20 dB | 80 m, 30 m | `hl2-auto-rf-gain.txt`: the `aether.hl2: HL2 auto RF gain:` lines of that session's log |
| `d245/` | 2026-11-12/13 23:02-00:26 CET, AetherSDR main `50ed5fff2` | 40 / 30 / 80 m at +48 dB in two rounds, 80 m at +20 dB, the dummy load at +48 dB | `hl2log-<leg>.json`: the runner's capture of the leg's `aether.hl2` lines, one JSON object per line |

The log times are the recording host's wall clock (CEST, UTC+2, for d168; CET,
UTC+1, for d245). The generators fix those offsets, so their output does not
depend on the time zone they run in.

The rest of each session log (audio devices, network, unrelated categories)
and the dropped poll fields are not here. They stay with the contributor's lab
records of runs d168 (`run-20261001-222013`) and d245
(`run-20261112-225954`), from which these files were cut. The headers' data
come out byte for byte the same from those full records as from these files.
