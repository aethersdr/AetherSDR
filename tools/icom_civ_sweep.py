#!/usr/bin/env python3
"""CI-V capability sweep — black-box, READ-ONLY, against a live IC-705.

Clean-room by construction: every fact this produces is observed on the wire
(CONSTITUTION.md Principle IV explicitly blesses "capturing and studying the
protocol as it actually behaves"). Nothing here reads a firmware image.

SAFETY. Unknown CI-V command space is not inert. It contains "power the radio
off" (18 00 — a one-way trip over WiFi, since the WLAN interface goes with it),
transmit keying (1C 00), the antenna-tuner cycle (1C 01), scan and memory
writes. So this sends an explicit ALLOWLIST of read forms and nothing else.

A READ is `cmd + sub` with NO payload. Adding a payload is what makes it a set,
so every probe below is exactly two bytes (or four for the 1A 05 menu, whose
item number is part of the address rather than a value).
"""
import json
import socket
import sys
import time

SOCK = "/var/folders/gk/dff34svj3390yjvn15b486s00000gn/T/icomsweep"

# Commands that must NEVER be probed, with the reason each one is here.
FORBIDDEN = {
    0x00: "set frequency (transceive)",
    0x01: "set mode (transceive)",
    0x05: "set frequency",
    0x06: "set mode",
    0x07: "VFO select / swap",
    0x08: "memory channel select",
    0x09: "memory write",
    0x0A: "memory->VFO",
    0x0B: "memory clear",
    0x0E: "scan control",
    0x0F: "split / duplex",
    0x17: "send CW message — TRANSMITS",
    0x18: "power off/on — one-way trip over WiFi",
    0x1C: "PTT and antenna tuner — KEYS THE TRANSMITTER",
    0x28: "voice TX — TRANSMITS",
}


def call(req, timeout=20):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall(json.dumps(req).encode() + b"\n")
    buf = b""
    while b"\n" not in buf:
        d = s.recv(1 << 20)
        if not d:
            break
        buf += d
    s.close()
    return json.loads(buf.decode().strip())


def probe(hexcmd, cmd, sub_label):
    """Send one read form and return the radio's answer bytes, or None.

    MATCHES ONLY FRAMES THAT ARRIVED AFTER THE PROBE. The first version of this
    walked the whole ring newest-first, and the app's own metering — an S-meter
    poll every 100 ms and a transmit-state poll every 250 — meant it kept
    finding somebody else's reply. Commands we KNOW work (15 11 Po, 16 40 NR)
    came back "silent" while the sweep looked plausible, which is the exact
    shape of a measurement that reads the wrong place and reports absence
    (CERTIFICATION.md 1.9).
    """
    first = int(hexcmd.split()[0], 16)
    if first in FORBIDDEN:
        raise SystemExit(f"REFUSED {hexcmd}: {FORBIDDEN[first]}")

    # CORRELATE BY AGE, not by index. The ring is capped at 200 frames, so once
    # it is full its length stops growing and an index diff silently returns
    # nothing — which it did, turning the whole sweep blank while every probe
    # still reported success. Age is unaffected by eviction.
    r = call({"cmd": "civ", "action": "send", "value": hexcmd})
    if not r.get("ok"):
        return ("error", r.get("error"))
    time.sleep(0.22)
    tr = call({"cmd": "civ", "action": "trace", "value": "all"})["result"]
    fresh = [e for e in tr if e["ageMs"] <= 320]
    want = hexcmd.lower()
    saw_ng = False
    for e in fresh:
        if e["dir"] != "rx":
            continue
        h = e["hex"]
        if h == "fa":
            saw_ng = True
            continue
        if h.startswith(want):
            return ("ok", h[len(want):].strip())
    return ("NG", None) if saw_ng else ("silent", None)


def sweep(label, frames, out):
    print(f"\n=== {label} ===", flush=True)
    for hexcmd, note in frames:
        kind, data = probe(hexcmd, None, note)
        if kind == "ok":
            print(f"  {hexcmd:14s} -> {data or '(empty)':22s}  {note}", flush=True)
            out.append({"cmd": hexcmd, "result": "supported", "data": data, "note": note})
        elif kind == "NG":
            out.append({"cmd": hexcmd, "result": "rejected", "note": note})
        elif kind == "silent":
            out.append({"cmd": hexcmd, "result": "silent", "note": note})
        else:
            print(f"  {hexcmd:14s} !! {data}", flush=True)


def main():
    out = []
    lv = call({"cmd": "liveness"})["liveness"]
    if not lv.get("connected"):
        raise SystemExit("not connected to the radio")
    print(f"connected, spectrum age {lv.get('spectrumAgeMs')} ms")

    # 0x15 — meters and status flags.
    sweep("15 xx  meters / status",
          [(f"15 {i:02x}", "") for i in range(0x00, 0x20)], out)

    # 0x14 — levels.
    sweep("14 xx  levels",
          [(f"14 {i:02x}", "") for i in range(0x00, 0x20)], out)

    # 0x16 — function switches.
    sweep("16 xx  functions",
          [(f"16 {i:02x}", "") for i in range(0x00, 0x62)], out)

    # 0x21 — RIT / dTX.
    sweep("21 xx  RIT / dTX",
          [(f"21 {i:02x}", "") for i in range(0x00, 0x04)], out)

    # 0x27 — scope. 0x00 is the waveform push; skip it, it is already decoded
    # and asking for one just adds a sweep to the stream we already receive.
    sweep("27 xx  scope",
          [(f"27 {i:02x}", "") for i in range(0x10, 0x21)], out)

    with open(sys.argv[1], "w") as f:
        json.dump(out, f, indent=1)
    print(f"\nwrote {len(out)} probes to {sys.argv[1]}")


if __name__ == "__main__":
    main()
