#!/usr/bin/env python3
"""tools/hl2/spectrum.py puts a signal on the side of the tuned frequency it is on.

aethersdr/AetherSDR#4265: the probe built `I + jQ` straight off the wire. The
HPSDR wire is the conjugate of the analytic convention (docs/HERMES.md,
"Receive handedness and tuning"), so the probe drew every signal mirrored about
the tuned frequency. Its own validation could not see that: a carrier at zero
offset, through a magnitude FFT, is its own mirror image.

WHAT THIS RUNS. The real capture() from spectrum.py, with the socket replaced
by an object that hands back EP6 packets built here. Nothing is bound and no
radio is addressed. The packets carry one carrier 3 kHz ABOVE the tuned
frequency, written the way the wire carries it: I = cos(wt), Q = -sin(wt).

WHAT IT PROVES. That the samples capture() returns have that carrier at
+3 kHz, and, when numpy is installed, that panadapter() prints its strongest
row on the + side. A second carrier at zero offset shows why the old validation
was blind: its result is the same with and without the conversion.

WHAT IT DOES NOT PROVE. That a Hermes-Lite 2 sends the conjugate. The stimulus
is this repository's own statement of the wire convention, taken from
docs/HERMES.md and from Hl2RxDsp, which conjugates before its spectrum. A wrong
statement there would make this test agree with a wrong tool. Only an
off-centre capture from a radio settles that.

Stdlib only for the first two checks. The panadapter check needs numpy and says
so when it is skipped.
"""

import cmath
import contextlib
import io
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "hl2"))

import hpsdr      # noqa: E402
import spectrum   # noqa: E402

RATE = 48000
OFFSET_HZ = 3000
AMPLITUDE = 1 << 20
SAMPLES_PER_PACKET = 126     # two 512-byte frames, 63 one-receiver samples each

failures = 0


def check(ok, what):
    global failures
    if ok:
        print(f"[ OK ] {what}")
    else:
        print(f"FAIL: {what}")
        failures += 1


def wire_packet(seq, first_sample, offset_hz):
    """One EP6 packet carrying a carrier `offset_hz` above the tuned frequency,
    in the wire's convention: exp(-j*w*t)."""
    out = bytearray(bytes([0xEF, 0xFE, 0x01, 0x06]) + struct.pack(">I", seq))
    n = first_sample
    for _ in range(2):
        out += hpsdr.SYNC + bytes(5)
        for _ in range(63):
            phase = 2 * math.pi * offset_hz * n / RATE
            i = int(round(AMPLITUDE * math.cos(phase)))
            q = int(round(-AMPLITUDE * math.sin(phase)))
            out += i.to_bytes(3, "big", signed=True) + q.to_bytes(3, "big", signed=True) + bytes(2)
            n += 1
    return bytes(out)


class WireSocket:
    """Stands in for the UDP socket capture() is given. Not a peer: it answers
    every recvfrom() with the next packet of a fixed carrier and records what
    capture() sent."""

    def __init__(self, offset_hz):
        self.offset_hz = offset_hz
        self.seq = 0
        self.sent = []

    def sendto(self, data, dst):
        self.sent.append(bytes(data))

    def recvfrom(self, _size):
        pkt = wire_packet(self.seq, self.seq * SAMPLES_PER_PACKET, self.offset_hz)
        self.seq += 1
        return pkt, ("192.0.2.1", hpsdr.METIS_PORT)


def level(iq, hz):
    """Magnitude of the single DFT bin at `hz`, normalised to the carrier."""
    acc = 0j
    for n, x in enumerate(iq):
        acc += x * cmath.exp(-2j * math.pi * hz * n / RATE)
    return abs(acc) / (len(iq) * AMPLITUDE)


def captured(offset_hz, nsamp=4032):
    sock = WireSocket(offset_hz)
    # settle < 0: keep every packet. capture() discards until `settle` seconds
    # have passed, and a clock that has not advanced yet would drop the first.
    iq, drops = spectrum.capture(sock, ("192.0.2.1", hpsdr.METIS_PORT), 10_000_000,
                                 0, 20, nsamp, settle=-1.0)
    return sock, iq, drops


def main():
    sock, iq, drops = captured(OFFSET_HZ)
    check(len(iq) == 4032 and drops == 0, "capture() returns the samples it was fed, no drops")
    check(all(p[:4] in (bytes([0xEF, 0xFE, 0x04, 0x01]), bytes([0xEF, 0xFE, 0x04, 0x00]),
                        bytes([0xEF, 0xFE, 0x01, 0x02])) for p in sock.sent)
          and all(p[11] & 1 == 0 and p[523] & 1 == 0 for p in sock.sent if p[2] == 0x01),
          "capture() sent only start, stop and EP2 frames with MOX clear")

    above = level(iq, +OFFSET_HZ)
    below = level(iq, -OFFSET_HZ)
    print(f"       carrier fed {OFFSET_HZ} Hz above the tuned frequency: "
          f"level at +{OFFSET_HZ} Hz = {above:.3f}, at -{OFFSET_HZ} Hz = {below:.3f}")
    check(above > 0.9 and below < 0.01,
          "a carrier above the tuned frequency is at a positive baseband frequency")

    # The blind spot, stated as a measurement: at zero offset the conversion
    # changes nothing, which is why WWV at baseband DC validated a mirrored axis.
    _, centred, _ = captured(0)
    as_wire = [x.conjugate() for x in centred]
    check(abs(level(centred, 0) - level(as_wire, 0)) < 1e-9,
          "a carrier at zero offset reads the same with and without the conversion")

    if spectrum.np is None:
        print("[SKIP] panadapter() row check: numpy is not installed")
    else:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            spectrum.panadapter(iq, 10_000_000, RATE, 64)
        rows = []
        for line in buf.getvalue().splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "kHz":
                rows.append((float(parts[2]), float(parts[0])))
        peak_db, peak_khz = max(rows) if rows else (0.0, 0.0)
        print(f"       panadapter(): strongest row at {peak_khz:+.1f} kHz ({peak_db:.1f} dBFS)")
        check(len(rows) == 64 and 2.0 < peak_khz <= 3.0,
              "panadapter() prints the carrier on the + side, in the column that holds +3 kHz")

    if failures == 0:
        print("test_hl2_spectrum_handedness: all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
