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
- **Decoder:** wsprd from WSJT-X (`wsprd/`, GPL v3 or later; identical in
  2.7.0 and 3.0.2), built in and run in a child process at nice 19. `main`
  is `wsprd_main`, FFTW is replaced by `fftw_compat.c` (the decode path needs
  only a 512-point FFT), and the Fortran OSD pass is translated to C
  (`osdwspr.c`). With the same input it prints the same decodes as stock
  wsprd.
- **Depth:** WSJT-X's Fast (`-q -B`), Normal (`-C 500 -o 4`) and Deep
  (`-C 500 -o 4 -d`), and `standard`, wsprd's own defaults (the Fano decoder
  uncapped, no OSD). `standard` is the default: on 30 simulated signals at
  -29 to -33 dB it decoded 17, Deep 16, Normal 16 and Fast 6.
- **Band hopping:** WSJT-X's. Each two-minute slot `(minute % 20) / 2` names
  one of 160 m to 10 m, used if it is ticked for the time of day (sunrise
  gray line, day, sunset gray line or night, at the station's grid);
  otherwise a band from a random permutation of that period's ticked bands.
  `grayline.c` is a translation of WSJT-X's Fortran.
- **Callsign and grid:** from the radio (`info`, and the GPS grid) unless set.
- **Upload:** to wsprnet.org the way WSJT-X does, when `upload` is on.

## Status and settings: TCP 48995, LAN callers only

| Request | Does |
|---|---|
| `GET /v1/status[?since=<unix>]` | State, clock, band, upload counters, recent cycles and spots |
| `GET /v1/config` | Settings, and whether an admin token was issued |
| `POST /v1/config` | Any subset of `{callsign, grid, band, hopping, gray_minutes, hop_sunrise[], hop_day[], hop_sunset[], hop_night[], depth, rxant, upload, dax_channel}`. The first POST is open and returns `admin_token`; later ones need `Authorization: Bearer <token>` |

When the tailnet shim runs on the radio, the agent publishes 48995 through
it (`POST /v1/local/expose`), so a remote AetherSDR can read it too.

## Build and test

```sh
python3 -I build.py --output OUT --host     # needs zig 0.17
OUT/aether-wspr-host selftest -27           # synthetic signal through DSP + decoder
./test_osd.sh /path/to/wsjtx                # C ports vs WSJT-X's Fortran (needs gfortran)
```

`AETHER_WSPR_RADIO=<ip>` runs the host build against
a radio over the network.
