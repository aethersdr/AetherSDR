#!/usr/bin/env python3
"""
TX meter test harness for the agent automation bridge.

Hardware-in-the-loop transmit + meter validation, built from the lessons of the
manual TX runs:

  * Single-instant meter reads are unreliable — meters update at different rates
    (PACURRENT is only reported ~1 s into TX). This harness SAMPLES OVER THE
    KEYED WINDOW and uses the per-meter `age_ms` the bridge now reports to reject
    stale values (a 0 with a stale/absent age is not a real reading).
  * The ATU recalls stored antenna matches that corrupt dummy-load readings, so
    the harness ensures the tuner is BYPASSED before measuring (`atu bypass`).
  * It never leaves the radio keyed: every burst is short, unkey-verified, and
    the bridge's TX watchdog is a final backstop.

SAFETY: requires AETHER_AUTOMATION_ALLOW_TX=1 on the app. Confirms the TX antenna
is the expected dummy-load port before keying, gates on SWR after the first
(lowest-power) burst, and aborts on any unkey failure. Use into a dummy load.

Usage:
    AETHER_AUTOMATION=1 AETHER_AUTOMATION_ALLOW_TX=1 ./build/.../AetherSDR &
    python3 tools/tx_meter_test.py                 # tune sweep + two-tone ALC
    python3 tools/tx_meter_test.py --ant ANT1 --levels 10,25,50,75,100
"""

import argparse
import json
import statistics
import sys
import time

sys.path.insert(0, "tools")
from automation_probe import Bridge, discover_socket  # noqa: E402

FRESH_MS = 1500          # a meter reading older than this is treated as stale


class Tx:
    def __init__(self, bridge):
        self.b = bridge

    def g(self, model, prop=None, sel=None):
        o = {"cmd": "get", "model": model}
        if sel: o["selector"] = sel
        if prop: o["property"] = prop
        r = self.b.request(o)
        return r.get("value") if prop else r.get(model, {})

    def inv(self, target, action, value=None):
        o = {"cmd": "invoke", "target": target, "action": action}
        if value is not None: o["value"] = str(value)
        return self.b.request(o)

    def cmd(self, **kw):
        return self.b.request(kw)

    def tuning(self): return bool(self.g("transmit", "tuning"))
    def mox(self): return bool(self.g("transmit", "mox"))
    def txing(self): return bool(self.g("radio", "transmitting"))

    def ensure_unkeyed(self):
        for _ in range(8):
            if not self.tuning() and not self.mox() and not self.txing():
                return True
            if self.tuning(): self.inv("Tune", "click")
            if self.mox() or self.txing(): self.inv("MOX transmit", "setChecked", "false")
            self.cmd(cmd="txtest", action="off")
            time.sleep(0.25)
        return False

    def meters(self):
        return self.g("meters")

    @staticmethod
    def meter_from_all(m, name):
        """Return (value, age_ms, reliable) for a named meter from the 'all'
        table, picking the freshest instance (disambiguates duplicate-named
        meters). `reliable` is False if the bridge flagged the meter known-bad
        for this radio (e.g. PACURRENT clipping on FLEX-8000, #3729)."""
        best = (None, None)
        reliable = True
        for x in m.get("all", []):
            if x.get("name") == name and x.get("has_value"):
                if x.get("reliable") is False:
                    reliable = False
                age = x.get("age_ms", -1)
                if age is not None and age >= 0 and (best[1] is None or age < best[1]):
                    best = (x.get("value"), age)
        return best[0], best[1], reliable

    def ensure_atu_bypass(self):
        st = self.g("transmit", "atuStatus")
        if st in ("bypass", "manual_bypass"):
            return True, st
        # disable auto-recall of stored matches, then bypass
        self.inv("ATU memories", "setChecked", "false")
        time.sleep(0.3)
        self.cmd(cmd="atu", action="bypass")
        time.sleep(0.4)
        st = self.g("transmit", "atuStatus")
        return st in ("bypass", "manual_bypass"), st


def sample_window(tx, dur=1.4, settle=0.2):
    """Key-down meter sampling. Collect fresh fwd/swr/temp/volts and the freshest
    PACURRENT/ALC over the window. Returns a dict of aggregates + freshness."""
    fwd, swr, temp, volts, alc = [], [], [], [], []
    pacur = None  # (value, age) freshest
    pacur_reliable = True
    micp = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < dur:
        if time.monotonic() - t0 > settle:
            m = tx.meters()
            if m.get("fwdPowerAgeMs", 1e9) < FRESH_MS and m.get("fwdPower", 0) > 0.3:
                fwd.append(m["fwdPower"])
            if m.get("swrAgeMs", 1e9) < FRESH_MS:
                swr.append(m.get("swr", 0))
            temp.append(m.get("paTemp", 0)); volts.append(m.get("supplyVolts", 0))
            alc.append(m.get("swAlc", -150))
            pv, pa, prel = Tx.meter_from_all(m, "PACURRENT")
            if not prel:
                pacur_reliable = False
            if pv is not None and pa is not None and pa < FRESH_MS:
                if pacur is None or pv > pacur[0]:
                    pacur = (pv, pa)
            micp.append(m.get("micPeak", -150))
        time.sleep(0.12)
    # Don't report a number the radio says is unreliable (e.g. clipped PACURRENT)
    pa_out = ("unreliable" if not pacur_reliable
              else round(pacur[0], 2) if pacur else "stale")
    return {
        "fwd": round(statistics.median(fwd), 1) if fwd else None,
        "swr": round(statistics.median(swr), 2) if swr else None,
        "paTemp": round(max(temp), 1) if temp else None,
        "volts": round(statistics.median(volts), 2) if volts else None,
        "alc": round(max(alc), 1) if alc else None,
        "paCurrent": pa_out,
        "n_fwd": len(fwd),
    }


def main():
    ap = argparse.ArgumentParser(description="TX meter test harness (agent automation bridge)")
    ap.add_argument("--ant", default="ANT1", help="expected TX antenna (dummy load)")
    ap.add_argument("--levels", default="10,25,50,75,100", help="tune-power levels W")
    ap.add_argument("--json", help="write full results here")
    ap.add_argument("--socket")
    args = ap.parse_args()
    levels = [int(x) for x in args.levels.split(",")]

    sock = args.socket or discover_socket()
    if not sock:
        sys.exit("error: no bridge socket; launch with AETHER_AUTOMATION=1 AETHER_AUTOMATION_ALLOW_TX=1")
    tx = Tx(Bridge(sock))

    # --- pre-flight ---
    print("=== PRE-FLIGHT ===")
    if tx.txing() or tx.tuning():
        tx.ensure_unkeyed()
    # confirm TX antenna via dumpTree
    tree = tx.b.request({"cmd": "dumpTree"})
    tx_ants = set()
    def walk(n):
        if n.get("accessibleName") == "TX antenna": tx_ants.add(n.get("value"))
        for c in n.get("children", []): walk(c)
    for r in tree["roots"]: walk(r)
    print(f"  TX antenna(s): {tx_ants or '?'}   expected: {args.ant}")
    if tx_ants and tx_ants != {args.ant}:
        sys.exit(f"ABORT: TX antenna {tx_ants} != expected {args.ant} — refusing to key")
    orig_tp = tx.g("transmit", "tunePower")
    print(f"  idle, tunePower={orig_tp}, transmitting={tx.txing()}")

    ok, st = tx.ensure_atu_bypass()
    print(f"  ATU bypass: {ok} (status={st})")
    if not ok:
        print("  WARNING: ATU not confirmed bypassed — readings may include a stored match")

    # --- tune-power sweep ---
    print("\n=== TUNE-POWER SWEEP (windowed, freshness-gated) ===")
    print(f"{'setW':>5} {'fwdW':>6} {'swr':>5} {'PAcur':>7} {'paTemp':>7} {'V':>6} {'ALC':>7} {'n':>3}")
    rows = []
    abort = False
    try:
        for tp in levels:
            tx.inv("Tune power", "setValue", tp); time.sleep(0.1)
            tx.inv("Tune", "click")
            t0 = time.monotonic()
            while time.monotonic() - t0 < 1.5 and not tx.tuning():
                time.sleep(0.05)
            agg = sample_window(tx)
            tx.inv("Tune", "click")
            ok_unkey = tx.ensure_unkeyed()
            agg.update(set=tp, unkeyed=ok_unkey, atu=tx.g("transmit", "atuStatus"))
            rows.append(agg)
            print(f"{tp:>5} {str(agg['fwd']):>6} {str(agg['swr']):>5} {str(agg['paCurrent']):>7} "
                  f"{str(agg['paTemp']):>7} {str(agg['volts']):>6} {str(agg['alc']):>7} {agg['n_fwd']:>3}")
            if not ok_unkey:
                print("  *** UNKEY FAILED — ABORT ***"); abort = True; break
            if tp == levels[0] and agg["swr"] and agg["swr"] > 2.5:
                print(f"  *** SWR {agg['swr']} high at lowest power — ABORT ***"); abort = True; break
            if agg["paTemp"] and agg["paTemp"] > 75:
                print("  *** PA temp — ABORT ***"); abort = True; break
            time.sleep(1.6)

        # --- two-tone ALC test ---
        if not abort:
            print("\n=== TWO-TONE (ALC / PEP — needs modulation) ===")
            r = tx.cmd(cmd="txtest", action="twotone")
            if r.get("ok"):
                t0 = time.monotonic()
                while time.monotonic() - t0 < 1.5 and not tx.txing() and not tx.tuning():
                    time.sleep(0.05)
                agg = sample_window(tx, dur=1.2)
                tx.cmd(cmd="txtest", action="off"); ok = tx.ensure_unkeyed()
                print(f"  two-tone: fwd={agg['fwd']}W swr={agg['swr']} ALC={agg['alc']}dBFS "
                      f"PAcur={agg['paCurrent']} unkeyed={ok}")
                rows.append({"set": "two-tone", **agg, "unkeyed": ok})
            else:
                print(f"  two-tone unavailable: {r.get('error')}")
    finally:
        tx.inv("Tune power", "setValue", orig_tp)
        safe = tx.ensure_unkeyed()
        print(f"\nrestored tunePower={tx.g('transmit','tunePower')} transmitting={tx.txing()} "
              f"atu={tx.g('transmit','atuStatus')} (unkeyed={safe})")
        if args.json:
            json.dump(rows, open(args.json, "w"), indent=2)


if __name__ == "__main__":
    main()
