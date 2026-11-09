# AetherFT8 — an FT8 receiver that runs on the radio

Status: **prototype, proposed in RFC #7114 and not yet approved.** No PR until it is,
per `GOVERNANCE.md` and the rule recorded in #7112.

Built from `tools/wspr-agent` (AetherWSPR, #7019). The shim is the same; the cycle and
the decoder differ.

| | AetherWSPR | AetherFT8 |
|---|---|---|
| cycle | 120 s | **15 s** |
| capture | 114 s | **13 s** |
| decode budget | ~6 s | **2 s** |
| audio to the decoder | complex 375 Hz baseband (mixed down 1500 Hz) | **real 12 kHz** |
| decoder | `wsprd` from WSJT-X, forked child, stdout parsed | **`ft8_lib`, in-process** |
| status port | 48995 | **48997** |
| uploads | wsprnet.org | **none** (see below) |
| 20 m dial | 14.0956 MHz | **14.074 MHz** |

## Transmit

**It does not transmit.** It sends no TX, PTT or keying command, sets `slice set <n> tx=0`,
and the vendored decoder has its transmit-side encoder **deleted** (`ft8_lib/PROVENANCE.md`).
Phase 1 is decode-only by construction, not by configuration: there is no TX code path here
to disable. A transmitting version is a separate proposal with its own Principle VI
analysis — see #7114.

## No uploads

AetherWSPR posts spots to wsprnet.org from the radio. There is no equivalent here: PSK
Reporter uses a different protocol, and an outbound connection from the radio is its own new
public surface under #7112. The uploader thread, its queue and the `upload` setting are
**absent**, not disabled. Spots leave the agent only when a LAN caller reads `/v1/status`.

## Build and test

Needs no radio. On a Linux host:

```sh
for f in ft8_lib/ft8/*.c ft8_lib/common/monitor.c ft8_lib/common/wave.c ft8_lib/fft/*.c; do
  gcc -O2 -std=gnu11 -w -I. -Ift8_lib -c "$f" -o "$(basename ${f%.c}).o"
done
gcc -O2 -std=gnu11 -Wall -Wextra -I. -Ift8_lib -c agent.c -o agent.o
gcc -O2 -std=gnu11 -w -I. -Ift8_lib -c grayline.c -o grayline.o
gcc -O2 -o aether-ft8 *.o -lm -lpthread
./aether-ft8 selftest
```

The selftest decodes one of ft8_lib's own off-air reference recordings, so it exercises
`run_decode()` exactly as a live cycle does. Last run: **20 spots in 188 ms** against the
2000 ms budget, and `selftest PASS`.

## Two traps found while building this, recorded so they are not rediscovered

1. **`ftx_message_decode_std()` returns OK with garbage for a non-standard message.**
   A compound or portable callsign (`PA0SKT/2`) travels as a type-4 "nonstandard call"
   message. Calling `decode_std` on one returns `FTX_MESSAGE_RC_OK` and fills the fields
   with nonsense — measured at ft8_lib `9fec6ca`, `CQ PA0SKT/2 JO33` came back as
   `de="C19PFD" extra="PL77"`. So `spot_from_message()` dispatches on
   `ftx_message_get_type()` and never on the return code. Trusting the return code would
   publish fictitious callsigns for every portable operator on the band.

2. **A type-4 message has no grid field.** 58 of its bits go on the callsign itself, so
   `CQ PA0SKT/2 JO33` renders as `CQ PA0SKT/2`. A missing grid there is the protocol, not
   a defect.

## Build for the radio

```sh
PATH=$HOME/zig-x86_64-linux-0.17.0:$PATH python3 -I build.py --output ./out --host
```

Needs **zig 0.17** (it bundles the linker and musl for both targets); `build.py` checks
the version and refuses otherwise. `--armv7-only` skips the arm64 container, which needs
`../flex-tailnet-shim/mkimage.py`.

Produces `AetherFT8.ssdr_waveform` for a FLEX-6000. Measured 2026-11-08:

| | |
|---|---|
| package | **93,947 bytes** (bound 512 KB; AetherWSPR is ~142 KB) |
| ARM binary | 346,952 bytes, ELF 32-bit ARM EABI5, statically linked, stripped |
| `agent.c` flags | `-Wall -Wextra -Werror` — clean |

⚠ Build on **Linux**. `tools/wspr-agent/build.py` is known to produce a package the radio
silently refuses when run on Windows, and this script inherits its packaging.

### ARM verified under emulation, not on a radio

```
qemu-arm-static ./aether-ft8-armv7 selftest
  -> 20 spot(s) in 445 ms (budget 2000 ms)   selftest PASS
```

The ARM binary decodes the reference recording to **exactly the same 20 spots, with the
same callsigns, grids and frequencies** as the x86 build, so the cross-compile is
behaviourally correct and not merely syntactically valid.

⛔ **The 445 ms is NOT the radio's speed.** It is qemu-arm emulation on an i5-6402P, where
the same source runs natively in 27-42 ms. A Cortex-A8 at 700 MHz-1 GHz is far slower than
either number represents. The only figure that settles the budget question is one measured
on the radio, and the closest available anchor is an earlier timing harness on the same
FLEX-6300, which took **~1.75 s** for one FT8 decode -- about **73% of the 2.4 s budget**,
with a **5.2 s outlier while the radio was busy**. Treat the budget as open until this
agent is timed on real hardware.

## Known gaps

- **Band hopping is unconverted.** The config still carries WSPR's gray-line hop schedule,
  which has no FT8 meaning (WSPR's scheme is WSJT-X's two-minute slot rotation). v1 should
  run a fixed band; #7114 lists this as an open question rather than inventing a scheme.
- **`"score"`, not `"snr"`.** ft8_lib reports a candidate correlation score, which is not a
  dB signal-to-noise ratio. The status JSON names it `score` so a consumer cannot plot it
  beside a WSPR spot's real dB figure by mistake.
- **Never run on a radio.** Built for ARMv7 and verified under qemu (identical decodes), but no
  run on real hardware, no live capture, and therefore **no trustworthy decode-time figure**.
- Sensitivity is ~73% of WSJT-X with no OSD and no signal subtraction — a property of the
  dependency, documented in `ft8_lib/PROVENANCE.md`.
