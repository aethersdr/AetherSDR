# tools/hl2/ — bare-metal Hermes-Lite 2 probes

Standalone Python probes that speak HPSDR Protocol 1 ("Metis") directly over
UDP:1024, with no AetherSDR process involved. They started life as the Phase‑0
spike that de-risked the in-tree `Hl2Backend`; they are kept because a probe that
shares none of the app's code is the only tool that can contradict the app's own
conventions — see `docs/HERMES.md` §2 on the decisive bug that every internal
measurement agreed with, because they all shared the wrong convention.

| Script | What it answers |
|---|---|
| `discover.py` | Is an HL2 reachable, and what board/gateware does it report? |
| `stream.py` | Does EP6 IQ ingest frame correctly, and at what loss rate? |
| `tune.py` | Does an EP2 C&C frame move the RX1 NCO where we asked? |
| `spectrum.py` | FFT of the raw IQ — where does a known carrier actually land? |
| `hpsdr.py` | Shared frame encode/decode helpers imported by the other four. |

Run them from the repo root; the sibling `import hpsdr` resolves because Python
puts the script's own directory on `sys.path`:

```bash
python3 tools/hl2/discover.py
```

```bash
python3 tools/hl2/spectrum.py --host 192.168.1.21 --freq 10000000
```

`spectrum.py` needs `numpy`; the other three are standard library only.

## Where the HL2 documentation lives

- [`docs/HERMES.md`](../../docs/HERMES.md) — the bring-up field notes. Start at
  §15 (receive handedness and tuning) and §5 (sideband selection).
- [`docs/architecture/aetherd-hl2-backend-design.md`](../../docs/architecture/aetherd-hl2-backend-design.md)
  — the design note these scripts were ported into.
- [`docs/archive/hl2-phase0-spike.md`](../../docs/archive/hl2-phase0-spike.md) —
  the original spike write-up, including the corrected `CONFIG_MERCURY` finding
  and the live measurements each phase produced.
- [`docs/architecture/hl2-multi-ddc-test-matrix.md`](../../docs/architecture/hl2-multi-ddc-test-matrix.md)
  — the multi-DDC test matrix.
