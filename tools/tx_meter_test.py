#!/usr/bin/env python3
"""
TX meter test harness for the agent automation bridge.

Hardware-in-the-loop transmit + meter validation, built from the lessons of the
manual TX runs:

  * Single-instant meter reads are unreliable — meters update at different rates
    (PACURRENT is only reported ~1 s into TX). This harness SAMPLES OVER THE
    KEYED WINDOW and uses the per-meter `age_ms` the bridge now reports to reject
    stale values (a 0 with a stale/absent age is not a real reading).
  * The tuner must already be BYPASSED. Restoring a tuned state requires RF, so
    the harness refuses to change it behind the operator's back.
  * A radio power control is a percentage, not watts. The harness requires an
    explicit measured-watt ceiling and unkeys on the first fresh sample above
    it. This is a reactive backstop; begin at a conservative percentage.
  * Two-tone uses Tune Power, not RF Power. Stage and verify Tune Power before
    starting it instead of inheriting the last sweep value.
  * It never leaves the radio keyed: every burst is short, unkey-verified, and
    the bridge's TX watchdog is a final backstop.

SAFETY: requires AETHER_AUTOMATION_ALLOW_TX=1 on the app. Requires the exact TX
antenna and a calibrated live forward-power meter, gates every burst on measured
watts and SWR, and aborts on missing telemetry or any unkey failure. Use into a
dummy load.

Usage:
    AETHER_AUTOMATION=1 AETHER_AUTOMATION_ALLOW_TX=1 ./build/.../AetherSDR &
    python3 tools/tx_meter_test.py --ant ANT1 --max-watts 10 --levels 2,5
"""

import argparse
import json
import statistics
import sys
import time

sys.path.insert(0, "tools")
from automation_probe import Bridge, discover_socket  # noqa: E402

FRESH_MS = 1500          # reporting freshness
SAFETY_FRESH_MS = 500    # only a current keyed sample may police physical watts
POWER_SAMPLE_DEADLINE_S = 0.9


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
            if self.tuning():
                self.inv("Tune", "click")
            # Semantic unkey is always allowed and survives applet redesigns;
            # do not depend only on finding a particular MOX widget.
            self.cmd(cmd="key", action="ptt", value="off")
            self.cmd(cmd="txtest", action="off")
            time.sleep(0.25)
        return False

    def meters(self):
        return self.g("meters")

    def liveness(self):
        return self.cmd(cmd="liveness").get("liveness", {})

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

    def atu_bypassed(self):
        st = self.g("transmit", "atuStatus")
        return st in ("bypass", "manual_bypass"), st


def sample_window(tx, dur=1.4, settle=0.2, max_watts=None, max_swr=2.5):
    """Key-down meter sampling. Collect fresh fwd/swr/temp/volts and the freshest
    PACURRENT/ALC over the window. Returns a dict of aggregates + freshness."""
    fwd, swr, temp, volts, alc = [], [], [], [], []
    pacur = None  # (value, age) freshest
    pacur_reliable = True
    micp = []
    stop_reason = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < dur:
        elapsed = time.monotonic() - t0
        if elapsed > settle:
            m = tx.meters()
            link_alive = tx.liveness().get("linkAlive")
            if link_alive is False:
                stop_reason = "radio link is not alive"
            fwd_age = m.get("fwdPowerAgeMs", 1e9)
            fwd_val = m.get("fwdPower", 0)
            if 0 <= fwd_age < FRESH_MS and fwd_val > 0.3:
                fwd.append(fwd_val)
            if (max_watts is not None and 0 <= fwd_age < SAFETY_FRESH_MS
                    and fwd_val > max_watts):
                stop_reason = (f"measured forward power {fwd_val:.1f} W exceeds "
                               f"{max_watts:.1f} W ceiling")
            # swr is null when no live sample exists, and swrAgeMs is -1
            # when no sample EVER arrived -- both mean "no data", not 0.0,
            # and -1 must not pass a < FRESH_MS check (#4536).
            swr_age = m.get("swrAgeMs", 1e9)
            swr_val = m.get("swr")
            if 0 <= swr_age < FRESH_MS and swr_val is not None:
                swr.append(swr_val)
                if swr_val > max_swr:
                    stop_reason = f"SWR {swr_val:.2f} exceeds {max_swr:.2f} ceiling"
            temp.append(m.get("paTemp", 0)); volts.append(m.get("supplyVolts", 0))
            alc.append(m.get("swAlc", -150))
            pv, pa, prel = Tx.meter_from_all(m, "PACURRENT")
            if not prel:
                pacur_reliable = False
            if pv is not None and pa is not None and pa < FRESH_MS:
                if pacur is None or pv > pacur[0]:
                    pacur = (pv, pa)
            micp.append(m.get("micPeak", -150))
            if elapsed >= POWER_SAMPLE_DEADLINE_S and not any(
                    0 <= x.get("age_ms", -1) < SAFETY_FRESH_MS
                    and x.get("name") == "FWDPWR" and x.get("has_value")
                    for x in m.get("all", [])):
                stop_reason = "no fresh calibrated FWDPWR sample before safety deadline"
            if stop_reason:
                tx.cmd(cmd="txtest", action="off")
                tx.ensure_unkeyed()
                break
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
        "stopReason": stop_reason,
    }


def tx_antennas(tree):
    """Return every live TX-antenna value exposed by dumpTree.

    The caller must compare this set for exact equality with the one authorized
    value. Empty or ambiguous discovery is deliberately not accepted.
    """
    values = set()

    def walk(node):
        if node.get("accessibleName") == "TX antenna":
            values.add(node.get("value"))
        for child in node.get("children", []):
            walk(child)

    for root in tree.get("roots", []):
        walk(root)
    return values


def main():
    ap = argparse.ArgumentParser(description="TX meter test harness (agent automation bridge)")
    ap.add_argument("--ant", required=True,
                    help="exact live TX antenna connected to the dummy load")
    ap.add_argument("--max-watts", required=True, type=float,
                    help="maximum calibrated forward power permitted during this run")
    ap.add_argument("--levels", default="2,5",
                    help="Tune Power control percentages, low to high (NOT watts)")
    ap.add_argument("--two-tone-percent", type=int,
                    help="Tune Power percentage for two-tone (default: first --levels value)")
    ap.add_argument("--max-swr", type=float, default=2.5,
                    help="immediate unkey threshold (default: 2.5)")
    ap.add_argument("--json", help="write full results here")
    ap.add_argument("--socket")
    args = ap.parse_args()
    levels = [int(x) for x in args.levels.split(",")]
    if args.max_watts <= 0:
        ap.error("--max-watts must be positive")
    if not levels or any(x < 0 or x > 100 for x in levels):
        ap.error("--levels must contain percentages from 0 through 100")
    tone_percent = args.two_tone_percent if args.two_tone_percent is not None else levels[0]
    if tone_percent < 0 or tone_percent > 100:
        ap.error("--two-tone-percent must be from 0 through 100")

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
    tx_ants = tx_antennas(tree)
    print(f"  TX antenna(s): {tx_ants or '?'}   expected: {args.ant}")
    if tx_ants != {args.ant}:
        sys.exit(f"ABORT: TX antenna {tx_ants} != expected {args.ant} — refusing to key")
    orig_tp = tx.g("transmit", "tunePower")
    print(f"  idle, tunePower={orig_tp}, transmitting={tx.txing()}")

    ok, st = tx.atu_bypassed()
    print(f"  ATU bypass: {ok} (status={st})")
    if not ok:
        sys.exit("ABORT: tuner is not bypassed; bypass it before the run so the harness "
                 "does not change a state it cannot safely restore")

    # --- tune-power sweep ---
    print("\n=== TUNE-POWER SWEEP (windowed, freshness-gated) ===")
    print(f"{'set%':>5} {'fwdW':>6} {'swr':>5} {'PAcur':>7} {'paTemp':>7} {'V':>6} {'ALC':>7} {'n':>3}")
    rows = []
    abort = False
    try:
        for tp in levels:
            tx.inv("Tune power", "setValue", tp); time.sleep(0.1)
            actual_tp = tx.g("transmit", "tunePower")
            if actual_tp != tp:
                print(f"  *** Tune Power readback {actual_tp} != requested {tp} — ABORT ***")
                abort = True
                break
            tx.inv("Tune", "click")
            t0 = time.monotonic()
            while time.monotonic() - t0 < 1.5 and not tx.tuning():
                time.sleep(0.05)
            agg = sample_window(tx, max_watts=args.max_watts, max_swr=args.max_swr)
            if not agg["stopReason"]:
                tx.inv("Tune", "click")
            ok_unkey = tx.ensure_unkeyed()
            agg.update(set=tp, unkeyed=ok_unkey, atu=tx.g("transmit", "atuStatus"))
            rows.append(agg)
            print(f"{tp:>5} {str(agg['fwd']):>6} {str(agg['swr']):>5} {str(agg['paCurrent']):>7} "
                  f"{str(agg['paTemp']):>7} {str(agg['volts']):>6} {str(agg['alc']):>7} {agg['n_fwd']:>3}")
            if not ok_unkey:
                print("  *** UNKEY FAILED — ABORT ***"); abort = True; break
            if agg["stopReason"]:
                print(f"  *** {agg['stopReason']} — ABORT ***"); abort = True; break
            if agg["paTemp"] and agg["paTemp"] > 75:
                print("  *** PA temp — ABORT ***"); abort = True; break
            time.sleep(1.6)

        # --- two-tone ALC test ---
        if not abort:
            print("\n=== TWO-TONE (ALC / PEP — needs modulation) ===")
            tx.inv("Tune power", "setValue", tone_percent); time.sleep(0.1)
            if tx.g("transmit", "tunePower") != tone_percent:
                print("  two-tone unavailable: Tune Power did not reach the requested safe value")
                abort = True
            if abort:
                return
            r = tx.cmd(cmd="txtest", action="twotone")
            if r.get("ok"):
                t0 = time.monotonic()
                while time.monotonic() - t0 < 1.5 and not tx.txing() and not tx.tuning():
                    time.sleep(0.05)
                agg = sample_window(tx, dur=1.2, max_watts=args.max_watts,
                                    max_swr=args.max_swr)
                tx.cmd(cmd="txtest", action="off"); ok = tx.ensure_unkeyed()
                print(f"  two-tone: fwd={agg['fwd']}W swr={agg['swr']} ALC={agg['alc']}dBFS "
                      f"PAcur={agg['paCurrent']} unkeyed={ok}")
                rows.append({"set": "two-tone", **agg, "unkeyed": ok})
                if agg["stopReason"]:
                    print(f"  *** {agg['stopReason']} — ABORT ***")
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
