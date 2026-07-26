#!/usr/bin/env python3
"""Pan / waterfall tuning conformance suite.

Drives every way an operator moves the VFO and asserts, after each, that the
four numbers which must agree actually agree:

    pan model centre == spectrum view centre == waterfall row centre

Any drift between those is the "waterfall is off / the pan won't follow" class
of bug: they all describe the RECEIVER'S WINDOW, so at rest they must be
identical and the tolerance is deliberately tight.

The slice is deliberately NOT in that equality. On a single-DDC backend the
slice tunes inside the passband while the NCO — and therefore the pan centre —
holds still, so a slice offset from centre is correct and expected. What is
asserted about the slice is that it lands where it was asked to.

Why this exists as a tool rather than a one-off: the failure it catches is
invisible to unit tests (it needs a live data plane and real gesture handling)
and nearly invisible by eye (a slow drift looks like propagation). It is also
exactly the failure a NEW radio backend is most likely to reintroduce, because
it comes from an unstated assumption — that the radio re-echoes pan geometry
continuously. A backend that publishes geometry only when it changes breaks it.
Run this against any new backend before calling receive "done".

Usage:
    python3 tools/tune_conformance.py [--socket NAME] [--tolerance-hz N]

The app must be running with AETHER_AUTOMATION=1 and connected to a radio
(hpsdrsim -hermeslite2 -P1 is ideal: deterministic and harmless).
"""
import argparse
import json
import os
import socket
import sys
import tempfile
import time


class Bridge:
    def __init__(self, name):
        self.path = name if os.path.isabs(name) else os.path.join(
            tempfile.gettempdir(), name)

    def call(self, obj, timeout=8.0):
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.settimeout(timeout)
        c.connect(self.path)
        c.sendall(json.dumps(obj).encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = c.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
        c.close()
        return json.loads(buf.decode().splitlines()[0])


def find_spectrum(node, out):
    if isinstance(node, dict):
        if str(node.get("class", "")).endswith("SpectrumWidget"):
            out.append(node)
        for key in ("children", "roots"):
            for ch in node.get(key, []) or []:
                find_spectrum(ch, out)
    elif isinstance(node, list):
        for n in node:
            find_spectrum(n, out)
    return out


class Suite:
    def __init__(self, bridge, tol_hz):
        self.b = bridge
        self.tol = tol_hz
        self.results = []

    def snapshot(self):
        sl = self.b.call({"cmd": "get", "model": "slice",
                          "selector": "active"}).get("slice", {})
        pan = self.b.call({"cmd": "get", "model": "pan",
                           "selector": "active"}).get("pan", {})
        sws = find_spectrum(self.b.call({"cmd": "dumpTree",
                                         "filter": "spectrum"}), [])
        return sl, pan, (sws[0] if sws else {})

    def check(self, name, expect_slice=None):
        sl, pan, sw = self.snapshot()
        f, pc = sl.get("frequency"), pan.get("centerMhz")
        vc, wf = sw.get("centerMhz"), sw.get("wfRowCenterMhz")
        errs = []
        if None in (f, pc, vc):
            errs.append("incomplete state (is a radio connected?)")
        else:
            if abs((vc - pc) * 1e6) > self.tol:
                errs.append(f"view vs pan model {(vc - pc) * 1e6:+.1f} Hz")
            if wf is not None and abs((wf - vc) * 1e6) > self.tol:
                errs.append(f"waterfall vs view {(wf - vc) * 1e6:+.1f} Hz")
        if expect_slice is not None and f is not None \
                and abs((f - expect_slice) * 1e6) > 200.0:
            errs.append(f"slice {f} != expected ~{expect_slice}")
        # The slice must stay inside the window the pan is showing, or it is
        # tuned somewhere the operator cannot see.
        if None not in (f, vc, sw.get("bandwidthMhz")):
            half = sw["bandwidthMhz"] / 2.0
            if abs(f - vc) > half:
                errs.append(f"slice {f} outside the displayed span "
                            f"({vc} +/- {half})")
        ok = not errs
        self.results.append((name, ok, "; ".join(errs)))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:<32} "
              f"slice={f} view={vc} wf={wf}"
              + ("" if ok else "\n         " + "; ".join(errs)))
        return ok

    def geometry(self):
        sws = find_spectrum(self.b.call({"cmd": "dumpTree",
                                         "filter": "spectrum"}), [])
        return sws[0]["geometry"]

    def run(self):
        b, settle = self.b, lambda s=1.2: time.sleep(s)

        b.call({"cmd": "tune", "value": "10.000"}); settle()
        if not self.check("baseline", 10.000):
            print("\nBaseline already misaligned — fix that before trusting "
                  "the rest of this run.")

        g = self.geometry()
        b.call({"cmd": "clickAt",
                "value": f"{g['x'] + int(g['w'] * 0.70)} "
                         f"{g['y'] + int(g['h'] * 0.30)}"})
        settle(); self.check("click-to-tune")

        b.call({"cmd": "tune", "value": "10.020"}); settle()
        b.call({"cmd": "dragAt", "target": "SpectrumWidget",
                "value": "700 250 -180 0"})
        settle(); self.check("drag-to-tune")
        b.call({"cmd": "dragAt", "target": "SpectrumWidget",
                "value": "700 250 220 0"})
        settle(); self.check("drag-to-tune (reverse)")

        b.call({"cmd": "tune", "value": "10.000"}); settle()
        for _ in range(15):
            b.call({"cmd": "shortcut", "target": "tune_up_1"})
        settle(); self.check("arrow keys x15 up")
        for _ in range(15):
            b.call({"cmd": "shortcut", "target": "tune_down_1"})
        settle(); self.check("arrow keys x15 down", 10.000)

        # SpectrumWidget clamps the wheel to +-1 step per event and debounces
        # within 50 ms (#504/#556), so notches must be spaced like a real mouse.
        b.call({"cmd": "tune", "value": "10.000"}); settle()
        for _ in range(5):
            r = b.call({"cmd": "wheel", "target": "SpectrumWidget",
                        "value": "700 250 1"})
            if not r.get("ok"):
                print("    wheel verb error:", r.get("error"))
                break
            time.sleep(0.07)
        settle(); self.check("wheel x5 notches up", 10.0005)
        for _ in range(5):
            b.call({"cmd": "wheel", "target": "SpectrumWidget",
                    "value": "700 250 -1"})
            time.sleep(0.07)
        settle(); self.check("wheel x5 notches down", 10.000)

        b.call({"cmd": "tune", "value": "10.000"}); settle()
        for _ in range(10):
            b.call({"cmd": "shortcut", "target": "tune_up_1"})
            b.call({"cmd": "wheel", "target": "SpectrumWidget",
                    "value": "700 250 1"})
            time.sleep(0.07)
        settle(1.6); self.check("rapid mixed arrows + wheel")

        bad = [r for r in self.results if not r[1]]
        print(f"\n{len(self.results) - len(bad)}/{len(self.results)} passed")
        for name, _, why in bad:
            print(f"  FAIL {name}: {why}")
        return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default="aethersdr-automation",
                    help="bridge socket name (AETHER_AUTOMATION_SOCKET) or path")
    ap.add_argument("--tolerance-hz", type=float, default=1.0)
    args = ap.parse_args()
    bridge = Bridge(args.socket)
    try:
        bridge.call({"cmd": "ping"})
    except OSError as e:
        print(f"cannot reach bridge at {bridge.path}: {e}")
        print("start the app with AETHER_AUTOMATION=1 and pass --socket")
        return 2
    print(f"=== pan/waterfall tuning conformance ({bridge.path}) ===")
    return Suite(bridge, args.tolerance_hz).run()


if __name__ == "__main__":
    sys.exit(main())
