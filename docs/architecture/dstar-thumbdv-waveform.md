# D-STAR Local Waveform

AetherSDR starts the D-STAR waveform as a local helper process. The helper
talks to the radio over the Flex waveform network API and uses a ThumbDV or
DV3000U over the operating system serial device for AMBE voice encode/decode.
The ThumbDV stays attached to the computer running AetherSDR; it does not need
to be plugged into the radio.

Software AMBE encode/decode is intentionally not bundled at this time because
of patent review concerns.

## App-To-Helper Contract

The default helper executable name is `aether-dstar-waveform`. On macOS and
Linux, AetherSDR builds this helper from the bundled
`third_party/smartsdr-dsp` ThumbDV waveform sources and places it beside the
main executable. Windows can still run an externally configured helper, but the
bundled helper is currently POSIX/termios based.

AetherSDR starts the helper with the radio address and configured ThumbDV
serial device:

```text
--host <radio-ip> --vocoder thumbdv --serial <thumbdv-device> --mode DSTR --underlying-mode DFM
```

AetherSDR also runs the helper from its executable directory so it can read the
bundled `ThumbDV.cfg`, and sets these environment variables for helpers that
follow the Waveform Processor convention or prefer environment configuration:

```text
SSDR_RADIO_ADDRESS=<radio-ip>
AETHER_DSTAR_VOCODER=thumbdv
AETHER_DSTAR_THUMBDV_SERIAL=<thumbdv-device>
AETHER_DSTAR_FAIL_FAST=1
AETHER_DSTAR_MODE=DSTR
AETHER_DSTAR_UNDERLYING_MODE=DFM
```

The helper registers the mode with the radio through the Flex waveform API. The
bundled `ThumbDV.cfg` uses this shape:

```text
waveform create name=ThumbDV mode=DSTR underlying_mode=DFM version=1.1.0
waveform set ThumbDV tx=1
waveform set ThumbDV rx_filter low_cut=-3500
waveform set ThumbDV rx_filter high_cut=3500
waveform set ThumbDV tx_filter low_cut=0
waveform set ThumbDV tx_filter high_cut=4800
waveform set ThumbDV udpport=5000
```

## Data Path

With `underlying_mode=DFM`, the radio performs the FM discriminator stage and
the helper receives demodulated L/R sample packets. The helper then performs
D-STAR symbol/frame recovery, passes AMBE voice frames to the ThumbDV, and
sends decoded PCM back to the radio as speaker data.

Using `RAW` would move FM/GMSK demodulation into the helper and require I/Q
data. That is not required for the DFM D-STAR path above.

On transmit, the radio supplies microphone sample packets when the operator
intentionally keys the DSTR slice. The helper sends speech to the ThumbDV for
voice encoding, builds D-STAR/GMSK transmitter samples, and sends transmitter
data packets back to the radio. AetherSDR does not key the transmitter to start
the helper.

## Dependency Boundary

The helper is a separate executable, not part of the main Qt GUI binary. Flex's
current `waveform-sdk` is LGPL-3.0 and normally comes from their waveform
development container. The bundled helper instead uses the public GPL-3.0
SmartSDR-DSP ThumbDV waveform sources listed in `THIRD_PARTY_LICENSES`.

The historical SmartSDR-DSP branch carried legacy FTDI D2XX headers and a
static library; those are intentionally not bundled. `aether_serial_compat.c`
provides the small D2XX call surface that the ThumbDV code uses on top of
normal OS serial ports.

## Trademark Note

D-STAR is a registered trademark of Icom Inc. AetherSDR uses the term only to
identify compatibility with the D-STAR amateur-radio protocol and is not
affiliated with or endorsed by Icom Inc.
