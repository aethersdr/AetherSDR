# AetherFT8 — an FT8 receiver that runs on the radio

A receive-only waveform and container implementing [RFC #7114](https://github.com/aethersdr/AetherSDR/issues/7114).

Built from `tools/wspr-agent` (AetherWSPR, #7019). The shim is the same; the cycle and
the decoder differ.

| | AetherWSPR | AetherFT8 |
|---|---|---|
| cycle | 120 s | **15 s** |
| capture | 114 s | **13 s** |
| nominal decode target | ~6 s | **2 s** |
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

Needs **zig 0.17.0** exactly (it bundles the linker and musl for both targets, and 0.17.1
builds different bytes); `build.py` checks the version and refuses any other. `--armv7-only` skips the arm64 container, which needs
`../flex-tailnet-shim/mkimage.py`.

Produces `AetherFT8.ssdr_waveform` for a FLEX-6000 and `AetherFT8.tar.gz`, an arm64
container image, for a FLEX-8000 or Aurora. Both are bounded at 512 KB by `build.py`, and
both are static, stripped binaries; `agent.c` builds clean with `-Wall -Wextra -Werror`.
The legacy waveform runs on a FLEX-6300. The container has not run on a radio.

⚠ Build on **Linux**. `tools/wspr-agent/build.py` is known to produce a package the radio
silently refuses when run on Windows, and this script inherits its packaging.

### The ARM build under emulation

```
qemu-arm-static ./aether-ft8-armv7 selftest
  -> 20 spot(s) in 445 ms (budget 2000 ms)   selftest PASS
```

The ARM binary decodes the reference recording to **exactly the same 20 spots, with the
same callsigns, grids and frequencies** as the x86 build, so the cross-compile is
behaviourally correct and not merely syntactically valid.

⛔ **The 445 ms is NOT the radio's speed.** It is qemu-arm emulation on an i5-6402P, where
the same source runs natively in 27-42 ms. A Cortex-A8 at 700 MHz-1 GHz is far slower than
either number represents. The figure that counts is the one measured on the radio: on a
FLEX-6300, 40 m in the evening, decodes took a median of **2.6 s** and at most **2.9 s**
against the **2 s** a cycle leaves after its 13-second capture. If a decode is still running one second into the next cycle,
the receiver skips that capture and increments `skipped_cycles` in `/v1/status`.

## Known gaps

- **Band hopping is not offered.** The agent carries AetherWSPR's hop schedule
  (`band_for_cycle`) and accepts it in `/v1/config`, but AetherSDR's Configure window sets
  one band and sends no hop list. #7114 leaves an FT8 hopping scheme as an open question.
- **`"score"`, not `"snr"`.** ft8_lib reports a candidate correlation score, which is not a
  dB signal-to-noise ratio. The status JSON names it `score` so a consumer cannot plot it
  beside a WSPR spot's real dB figure by mistake.
- **The arm64 container has not run on a radio.** The ARMv7 build has, on a FLEX-6300.
- It decodes 77.3% of the reference recordings at the default depth, with ordered-statistics
  decoding and no signal subtraction (`osd/PROVENANCE.md`; 73.4% with OSD off).
