#!/usr/bin/env python3
"""
AetherSDR slice-0 RX verification harness (aetherd migration regression check).

Drives a running AetherSDR instance through the automation bridge and asserts
the core connect -> slice-0 RX pipeline is intact, plus the TX-safety guard.
Built for the aetherd RFC (docs/aetherd-headless-engine-design.md): run it
before and after a structural change (the libaethercore split, the
IRadioBackend seam, a touchpoint conversion) to prove behaviour is preserved.

It exercises exactly the surface the migration puts at risk:
  * connect verbs resolve through IConnectionAutomation (step 1 inversion)
  * RadioModel / SliceModel / PanadapterModel / TransmitModel resolve through
    the (possibly split) engine library
  * the TX-keying guard still refuses a keying invoke without ALLOW_TX

It does NOT key the transmitter and does NOT require ALLOW_TX. The one place
it touches TX is to assert a keying control is *refused*.

Usage:
    # Against an already-running bridge (auto-discovers the socket):
    python3 tools/verify_slice0_rx.py

    # Launch a throwaway instance, verify, tear it down:
    python3 tools/verify_slice0_rx.py --launch ./build/AetherSDR

    # Emit a JSON snapshot for before/after diffing across a refactor:
    python3 tools/verify_slice0_rx.py --json > before.json
    #   ...apply the change, rebuild...
    python3 tools/verify_slice0_rx.py --json > after.json
    #   the "assertions" block should be identical; model values may drift
    #   with live band conditions, so diff assertions, not frequencies.

Exit 0 iff every assertion passes. Requires a radio reachable by the running
instance (real or simulator); with no radio, connect is skipped and the run
reports SKIPPED (exit 0) rather than failing — a headless CI box has no radio.

Reuses tools/automation_probe.py as the transport (no new socket code).
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROBE = REPO / "tools" / "automation_probe.py"


def probe(*args, socket=None):
    """Run one automation_probe verb, return parsed JSON (or {} on non-JSON)."""
    cmd = [sys.executable, str(PROBE)]
    if socket:
        cmd += ["--socket", socket]
    cmd += list(args)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "probe timeout"}
    try:
        return json.loads(out)
    except (json.JSONDecodeError, ValueError):
        return {"ok": False, "error": "non-json", "raw": out[:200]}


class Report:
    def __init__(self):
        self.assertions = []
        self.snapshot = {}

    def check(self, name, ok, detail=""):
        self.assertions.append({"name": name, "pass": bool(ok), "detail": detail})
        mark = "PASS" if ok else "FAIL"
        # Progress goes to stderr so --json keeps stdout pure JSON.
        print(f"  [{mark}] {name}" + (f" — {detail}" if detail else ""),
              file=sys.stderr)
        return ok

    @property
    def passed(self):
        return all(a["pass"] for a in self.assertions)


def run(socket, want_grab, grab_path):
    r = Report()

    # 1. Bridge reachable + model-facing get resolves (engine lib linked).
    radio = probe("get", "radio", socket=socket)
    if not r.check("bridge reachable + get radio ok", radio.get("ok"),
                   radio.get("error", "")):
        return r, "bridge unreachable"
    rm = radio.get("radio", radio)

    connected = bool(rm.get("connected"))
    r.snapshot["radio"] = {"connected": connected, "model": rm.get("model")}

    # 2. If not already connected, drive the inverted connect path.
    if not connected:
        radios = probe("connect", "list", socket=socket)
        n = radios.get("count", 0)
        if n == 0:
            r.check("connect list resolves (IConnectionAutomation)",
                    radios.get("ok"), "no radios discovered — RX checks SKIPPED")
            print("  [SKIP] no radio reachable; connect/RX assertions skipped", file=sys.stderr)
            return r, "skipped-no-radio"
        r.check("connect list resolves (IConnectionAutomation)", radios.get("ok"),
                f"{n} radio(s)")
        probe("connect", "local", "first", socket=socket)
        # Wait for the connect + slice-0 status to arrive.
        for _ in range(20):
            time.sleep(1)
            if probe("get", "radio", socket=socket).get("radio", {}).get("connected"):
                break
        rm = probe("get", "radio", socket=socket).get("radio", {})
        connected = bool(rm.get("connected"))

    r.check("radio connected", connected, rm.get("model", ""))
    if not connected:
        return r, "connect failed"

    # 3. Slice-0 RX state present and well-formed (SliceModel through engine).
    slices = probe("get", "slices", socket=socket).get("slices", [])
    r.check("slice-0 exists", len(slices) >= 1, f"{len(slices)} slice(s)")
    if slices:
        s0 = slices[0]
        mode = s0.get("mode")
        flo, fhi = s0.get("filterLow"), s0.get("filterHigh")
        r.check("slice-0 has a mode", bool(mode), str(mode))
        r.check("slice-0 has a valid RX filter",
                isinstance(flo, (int, float)) and isinstance(fhi, (int, float))
                and flo < fhi, f"[{flo}, {fhi}]")
        r.snapshot["slice0"] = {"mode": mode, "filterLow": flo, "filterHigh": fhi,
                                "frequency": s0.get("frequency")}

    # 4. PanadapterModel resolves (RX data pipeline through engine).
    pans = probe("get", "pans", socket=socket).get("pans", [])
    r.check("panadapter model present", len(pans) >= 1, f"{len(pans)} pan(s)")

    # 5. TransmitModel resolves (does not key).
    tx = probe("get", "transmit", socket=socket)
    r.check("transmit model resolves", tx.get("ok"), "")

    # 6. TX-safety guard: a keying invoke must be REFUSED *by the guard*
    #    without ALLOW_TX. (This asserts the guard; it does not key.)
    #    Match the specific guard phrasing, not a bare "TX" substring — an
    #    unrelated failure like "unknown control TX_MOX" would false-pass and
    #    the guard would never have run.
    mox = probe("invoke", "MOX", "toggle", socket=socket)
    err = str(mox.get("error", "")).lower()
    refused_by_guard = (not mox.get("ok")) and (
        "transmit-keying" in err or "tx-safety" in err or "allow_tx" in err)
    r.check("TX-keying guard refuses MOX (no ALLOW_TX)", refused_by_guard,
            mox.get("error", "")[:70])

    # 7. Optional: prove the GPU panadapter renders (needs a real display;
    #    offscreen returns an empty image, which we report as SKIP not FAIL).
    if want_grab:
        g = probe("grab", "SpectrumWidget", grab_path, socket=socket)
        if g.get("bytes", 0) > 0:
            r.check("panadapter GPU grab non-empty", True, f"{g['bytes']} bytes")
        else:
            print("  [SKIP] panadapter grab empty (offscreen / no GPU context)", file=sys.stderr)

    return r, "ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", help="bridge socket override (else auto-discover)")
    ap.add_argument("--launch", metavar="BINARY",
                    help="launch this AetherSDR binary (offscreen) for the run "
                         "and tear it down after")
    ap.add_argument("--grab", action="store_true",
                    help="also attempt a panadapter GPU framebuffer grab "
                         "(needs a real display; offscreen -> SKIP)")
    ap.add_argument("--grab-path", default="/tmp/aether_verify_pan.png")
    ap.add_argument("--json", action="store_true",
                    help="emit the assertions+snapshot as JSON to stdout")
    args = ap.parse_args()

    app = None
    env = dict(os.environ)
    socket = args.socket
    if args.launch:
        env["AETHER_AUTOMATION"] = "1"
        env.setdefault("QT_QPA_PLATFORM", "offscreen")
        app = subprocess.Popen([args.launch], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(10)  # let the bridge come up + auto-connect

    try:
        if not args.json:
            print("slice-0 RX verification (aetherd migration regression check)", file=sys.stderr)
        report, status = run(socket, args.grab, args.grab_path)
    finally:
        if app:
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()

    if args.json:
        print(json.dumps({"status": status,
                          "passed": report.passed,
                          "assertions": report.assertions,
                          "snapshot": report.snapshot}, indent=1))
    else:
        print(f"\nRESULT: {'PASS' if report.passed else 'FAIL'} ({status})")

    # A skipped run (no radio) is not a failure.
    if status == "skipped-no-radio":
        return 0
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
