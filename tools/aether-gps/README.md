# AetherGPS

A small relay that shares a FlexRadio's GPS on the local network (RFC #7267).
It is a static C binary, packaged two ways: a legacy waveform for the
FLEX-6000 (ARMv7) and a container image for the FLEX-8000 and Aurora (arm64),
whose containers share the radio's network. AetherSDR's AetherGPS window uses it for the Sky View, and any
gpsd or NMEA client can use it too.

## Why it is needed

The radio's API reports only satellite counts. Measured on a FLEX-8600
(firmware 4.3.16), the radio runs **gpsd 3.22**, which owns the GPSDO's serial
port (`/dev/ttyGPS`) and the PPS line (`/dev/pps0`). It listens on
`127.0.0.1:2947` only, so nothing off the radio can reach it.

## What it serves

| Where | What |
|---|---|
| TCP 2947 at the radio's LAN address | The gpsd protocol. Each client gets its own gpsd session, and gpsd's replies pass through unchanged (VERSION, DEVICES, WATCH, TPV, SKY, PPS and so on). Only `?WATCH`, `?POLL`, `?VERSION` and `?DEVICES` are passed to gpsd. Anything else, `?DEVICE` above all (it reconfigures the receiver), gets an `ERROR` and is never sent. Since gpsd runs a command that follows a WATCH's JSON on the same line, any command with a second `?` is refused too. At most 8 clients. A client that sends no allowed command within 30 s of connecting is closed. After that a client may stay connected without sending more, but one whose 64 KB backlog in the relay stays full for 30 s (it has stopped reading) is closed. |
| UDP 10110 broadcast on the LAN subnet | NMEA 0183 sentences, one per datagram, from gpsd's NMEA watch. 10110 is IANA's `nmea-0183` port. |
| The tailnet, through the shim | When the [tailnet shim](../flex-tailnet-shim/README.md) runs on the radio, TCP 2947 is published on the tailnet (`POST /v1/local/expose`, renewed every 5 minutes). The shim carries TCP only, so the NMEA broadcast stays on the LAN. |

The LAN address is the one on the default route's interface, in the gateway's
subnet. The radio's internal networks (172.30.x on an 8000) are never listened
on or broadcast into. The listener binds that address only, because gpsd holds
`127.0.0.1:2947` and a wildcard bind fails. The address is re-read every
30 seconds. Only callers on a private, link-local or loopback address are
served.

Everything gpsd reports is passed on, **including the station's position**
(TPV, and the GGA/RMC sentences), to anything on the LAN that asks or listens.
That was a deliberate ruling on #7267.

If gpsd isn't running, a client gets
`{"class":"ERROR","message":"AetherGPS: gpsd is not running on the radio"}`
and is closed. This is also how to find out whether a FLEX-6000 runs gpsd.

## Building and testing

```sh
python3 -I build.py --output OUT --host
python3 -I test_agent.py OUT/aether-gps-host
```

The build needs **zig 0.17.0**. The package is reproducible byte for byte
with the same zig; 0.17.1 is known to change ARMv7 output (it does for AetherWSPR). The tests run a stand-in
gpsd on loopback and pin the relay to `127.0.0.1` with the shim
registration off (`-s 0`).

AetherSDR ships both packages as Qt resources, pinned by SHA-256 and size in
`src/core/AetherGpsPackage.h`. **Waveforms → Install → AetherGPS** picks the one
the radio takes. Once installed it is an ordinary row in the installed list.
To change the relay, rebuild, copy both files to `resources/waveforms/`, and
update the pins and the version together.
