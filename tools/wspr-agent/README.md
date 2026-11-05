# AetherWSPR (prototype)

A receive-only WSPR receiver that runs on a FlexRadio as a legacy waveform
and serves its spots as JSON. It needs no PC, no GUI client slot and no DAX
IQ, which the radio delivers only to a separate computer on its LAN.

- **Radio side:** a non-GUI client on 127.0.0.1:4992 makes one headless DIGU
  slice (`tx=0`) and takes its DAX audio (`dax_rx`, 24 kHz). The radio
  removes headless slices when its last GUI client disconnects; the agent
  remakes the slice at once and loses that one cycle.
- **Capture:** each even UTC minute, 114 s, timed by the system clock. It
  waits until the kernel reports the clock synchronized; on an 8000-series
  radio the clock comes from its GPS-disciplined stratum-1 NTP. Lost VITA
  packets become silence so the timing holds.
- **DSP:** mix 1500 Hz to 0, then two FIR decimators: 24 kHz to 1500 Hz to
  375 Hz complex, in the scale and sign wsprd's own `.wav` reader produces.
- **Decoder:** wsprd from WSJT-X 2.7.0 (`wsprd/`, GPL v3 or later), built in
  and run in a child process at nice 19. Two changes from upstream: `main`
  is `wsprd_main`, and FFTW is replaced by `fftw_compat.c`, since the decode
  path needs only a 512-point FFT. The Fortran OSD pass, used only with
  `-o`, is left out (`osd_stub.c`). With the same input it prints the same
  decodes as stock wsprd.
- **Callsign and grid:** from the radio (`info`, and the GPS grid) unless set.
- **Upload:** to wsprnet.org the way WSJT-X does, when `upload` is on.

## Status and settings: TCP 48995, LAN callers only

| Request | Does |
|---|---|
| `GET /v1/status[?since=<unix>]` | State, clock, band, upload counters, recent cycles and spots |
| `GET /v1/config` | Settings, and whether an admin token was issued |
| `POST /v1/config` | `{callsign, grid, bands[], cycles, rxant, upload, dax_channel}`, any subset. The first POST is open and returns `admin_token`; later ones need `Authorization: Bearer <token>` |

When the tailnet shim runs on the radio, the agent publishes 48995 through
it (`POST /v1/local/expose`), so a remote AetherSDR can read it too.

## Build and test

```sh
python3 -I build.py --output OUT --host     # needs zig 0.17
OUT/aether-wspr-host selftest -27           # synthetic signal through DSP + decoder
```

`AETHER_WSPR_TEST_REPORT=1` builds a test variant that also shows each
cycle's summary as a GUI client's station name, for testing a radio reached
only through the shim. `AETHER_WSPR_RADIO=<ip>` runs the host build against
a radio over the network.
