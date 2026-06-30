#!/usr/bin/env python3
"""
Live capture-file test for local CW/CWX sidetone recording (#2539).

Drives the AetherSDR automation bridge against a *running* automation build
connected to a *real radio on ANT2 (dummy load)* and records a single
Client-Side QSO WAV that interleaves, in order:

  1. phone (SSB)  — client TX test tone @ 1200 Hz, keyed via PTT
  2. CW (CWX)     — CWX macro sends morse; AetherSDR's local sidetone is captured
  3. phone (SSB)  — client TX test tone @ 1800 Hz

…proving the recorder captures BOTH sources and survives SSB<->CW mode switches
mid-recording. Verify the WAV with cw_recording_analyze.py.

Preconditions (the bridge precondition — we do NOT launch the GUI app):
  * App running with AETHER_AUTOMATION=1 AETHER_AUTOMATION_ALLOW_TX=1, connected.
  * Radio TX antenna == ANT2 (asserted below; aborts otherwise).

Run ON the machine with the app (so the AF_UNIX socket is local):
  python3 cw_recording_test.py
Emits one JSON line of results (incl. the WAV path + segment offsets).
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from automation_probe import Bridge, discover_socket  # noqa: E402

REC_DIR = "/tmp/aether-rec"        # TCC-safe, SSH-readable (not ~/Documents)
PHONE1_HZ = 1200.0
PHONE2_HZ = 1800.0
CW_WPM = 20
CW_TEXT = "TEST TEST DE PARIS"


def main():
    sock = discover_socket()
    if not sock:
        print(json.dumps({"ok": False,
                          "error": "no bridge socket — launch the automation "
                                   "build (AETHER_AUTOMATION=1 "
                                   "AETHER_AUTOMATION_ALLOW_TX=1) and connect first"}))
        return 2

    b = Bridge(sock)
    results = {"ok": False, "socket": sock, "steps": []}

    def req(**kw):
        r = b.request(kw)
        results["steps"].append({"cmd": kw, "resp": r})
        return r

    if not req(cmd="ping").get("ok"):
        print(json.dumps({"ok": False, "error": "ping failed", **results}))
        return 2

    # ── SAFETY GATE: TX antenna must be the ANT2 dummy load ──────────────────
    ants = set()

    def walk(n):
        if n.get("accessibleName") == "TX antenna":
            ants.add(n.get("value"))
        for ch in n.get("children", []):
            walk(ch)
    for r in req(cmd="dumpTree").get("roots", []):
        walk(r)
    if ants != {"ANT2"}:
        print(json.dumps({"ok": False,
                          "error": f"ABORT: TX antenna not ANT2: {sorted(ants)}"}))
        return 3
    results["txAntenna"] = "ANT2"

    # Original mode to restore at the end.
    orig_mode = (req(cmd="get", model="slice", selector="active",
                     property="mode").get("value") or "USB")

    os.makedirs(REC_DIR, exist_ok=True)
    req(cmd="record", action="dir", value=REC_DIR)

    def set_mode(m):
        req(cmd="invoke", target="Operating mode", action="setCurrentText", value=m)
        time.sleep(0.4)

    def ptt(on):
        req(cmd="key", action="ptt", value=("on" if on else "off"))

    def tone(on, hz=None, db=-10):
        if on:
            req(cmd="testtone", action="on", value=f"{hz} {db}")
        else:
            req(cmd="testtone", action="off")

    seg = []
    t0 = time.time()

    def mark(label):
        seg.append({"label": label, "tSec": round(time.time() - t0, 2)})

    try:
        req(cmd="record", action="start")
        mark("record_start")

        # ── 1. phone (SSB) — test tone keyed via PTT ──────────────────────────
        set_mode("USB")
        mark("phone1_USB")
        ptt(True)
        tone(True, PHONE1_HZ)
        time.sleep(2.5)
        tone(False)
        ptt(False)
        time.sleep(0.6)

        # ── 2. CW (CWX) — local sidetone capture (the new feature) ────────────
        set_mode("CW")
        mark("cw_CWX")
        req(cmd="cwx", action="speed", value=str(CW_WPM))
        req(cmd="cwx", action="send", value=CW_TEXT)
        # Let the CWX message key out fully (PARIS-length text at 20 wpm ~ 6-8s).
        time.sleep(9.0)
        ptt(False)  # belt-and-suspenders unkey
        time.sleep(0.6)

        # ── 3. phone (SSB) again — proves SSB<->CW<->SSB all captured ─────────
        set_mode("USB")
        mark("phone2_USB")
        ptt(True)
        tone(True, PHONE2_HZ)
        time.sleep(2.5)
        tone(False)
        ptt(False)
        time.sleep(0.4)

        mark("record_stop")
        stop = req(cmd="record", action="stop")
        results["wav"] = stop.get("path")
        results["durationSecs"] = stop.get("durationSecs")
        results["ok"] = True
    finally:
        # ── Always leave the radio safe ──────────────────────────────────────
        tone(False)
        ptt(False)
        set_mode(orig_mode)
        tx = req(cmd="get", model="slice", selector="active", property="mode")
        results["restoredMode"] = tx.get("value")

    results["segments"] = seg
    results["expected"] = {
        "phone1": {"hz": PHONE1_HZ},
        "cw": {"approxSidetoneHzFromSettings": "read from cwSidetone pitch (~600)"},
        "phone2": {"hz": PHONE2_HZ},
    }
    print(json.dumps(results))
    b.close()
    return 0 if results["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
