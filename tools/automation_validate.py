#!/usr/bin/env python3
"""
AetherSDR control-surface validation sweep — part of the agent automation bridge.

Drives the in-app agent automation bridge (launch with AETHER_AUTOMATION=1) to validate
every value-bearing applet control: records its original value, probes its scale,
classifies it as linear or circular/wrapping, cross-checks the radio model where
exposed, and restores the original — then prints a findings table and timing.

It is the reusable form of the ad-hoc QA sweep that found the AM-carrier and
TX-monitor/CW-sidetone desyncs, hardened with the lessons from that run:

  * Circular/wrapping controls (e.g. the 0-360 phase slider where step 72 == 0)
    are classified as `wrapping`, not flagged as broken — the linear-clamp
    assumption is wrong for angular controls.
  * Disabled controls are skipped and reported as such; the bridge now refuses
    invoke() on them, so driving one is surfaced rather than a silent no-op.
  * Duplicate accessibleNames (e.g. "AF gain" in both RxApplet and
    PanadapterApplet) are addressed with applet-scoped targets ("RxApplet/AF
    gain") so each is driven deterministically.
  * Model cross-checks use the expanded `get` coverage (slice squelch/AGC/APF,
    `equalizer`) to catch GUI<->radio desyncs beyond the widget level.

No transmission: keying controls are skipped (and guarded by the bridge).

Usage:
    AETHER_AUTOMATION=1 ./build/AetherSDR.app/Contents/MacOS/AetherSDR &
    python3 tools/automation_validate.py            # full sweep + table
    python3 tools/automation_validate.py --json out.json
"""

import argparse
import json
import sys
import time

sys.path.insert(0, "tools")
from automation_probe import Bridge, discover_socket  # noqa: E402

# accessibleName -> (model, selector, property) for model-level cross-checks.
MODEL = {
    "AF gain": ("slice", "active", "audioGain"),
    "Audio pan": ("slice", "active", "audioPan"),
    "AGC threshold": ("slice", "active", "agcThreshold"),
    # "Squelch threshold" is deliberately NOT cross-checked: its value is cached
    # by RxApplet's Manual/Auto branching and only pushed to slice.squelchLevel
    # once squelch is enabled in Manual mode, so the model legitimately lags the
    # widget when squelch is off — a mode-gated control, not a desync.
    "APF bandwidth": ("slice", "active", "apfLevel"),
    "RF power": ("transmit", None, "rfPower"),
    "Tune power": ("transmit", None, "tunePower"),
    "AM carrier level": ("transmit", None, "amCarrierLevel"),
    "DEXP threshold": ("transmit", None, "dexpLevel"),
    "VOX level": ("transmit", None, "voxLevel"),
    "VOX delay": ("transmit", None, "voxDelay"),
    "CW speed": ("transmit", None, "cwSpeed"),
    "Microphone gain": ("transmit", None, "micLevel"),
    "Processor level": ("transmit", None, "speechProcLevel"),
    "Monitor volume": ("transmit", None, "monGainSb"),
    "CW delay": ("transmit", None, "cwDelay"),
    "CW audio pan": ("transmit", None, "monPanCw"),
}
NUMERIC = ("QSlider", "QDial", "QSpinBox", "QDoubleSpinBox")


class Validator:
    def __init__(self, bridge):
        self.b = bridge

    def req(self, o):
        return self.b.request(o)

    def model_value(self, name):
        if name not in MODEL:
            return None
        m, sel, prop = MODEL[name]
        o = {"cmd": "get", "model": m, "property": prop}
        if sel:
            o["selector"] = sel
        return self.req(o).get("value")

    def model_poll(self, name, want, timeout=0.4):
        """Wait for the mapped model field to reach `want`. (matched, ms, last)"""
        if name not in MODEL:
            return None, None, None
        t0 = time.monotonic()
        last = None
        while time.monotonic() - t0 < timeout:
            last = self.model_value(name)
            if str(last) == str(want):
                return True, round((time.monotonic() - t0) * 1000), last
            time.sleep(0.02)
        return False, round((time.monotonic() - t0) * 1000), last

    def set(self, target, value):
        return self.req({"cmd": "invoke", "target": target,
                         "action": "setValue", "value": str(value)})

    def inventory(self):
        """Walk the tree, returning value-bearing controls with their applet
        scope, range, enabled/keying flags. Duplicate accessibleNames get an
        applet-scoped target so each is addressable."""
        tree = self.req({"cmd": "dumpTree"})
        items = []
        seen = {}

        def walk(n, applet=None):
            cls = n.get("class", "").split("::")[-1]
            if cls.endswith("Applet") or cls.endswith("Panel"):
                applet = cls
            if applet and cls in NUMERIC + ("QComboBox",):
                nm = n.get("accessibleName")
                if nm:
                    items.append({
                        "applet": applet, "kind": cls, "name": nm,
                        "value": n.get("value"), "enabled": n.get("enabled", True),
                        "keying": bool(n.get("keying")), "range": n.get("range"),
                    })
                    seen[nm] = seen.get(nm, 0) + 1
            for c in n.get("children", []):
                walk(c, applet)
        for r in tree["roots"]:
            walk(r)

        # disambiguate duplicates with applet-scoped targets
        for it in items:
            it["target"] = (f"{it['applet']}/{it['name']}"
                            if seen[it["name"]] > 1 else it["name"])
            it["duplicate"] = seen[it["name"]] > 1
        return items

    def classify_numeric(self, it):
        """3-value scale probe with wrapping detection. Returns a result dict."""
        target, rng = it["target"], it["range"] or {}
        lo, hi = rng.get("min"), rng.get("max")
        res = {"kind_class": None, "min": lo, "max": hi}
        if lo is None or hi is None:
            res["kind_class"] = "unknown-range"
            return res
        if lo == hi:
            res["kind_class"] = "fixed"          # genuinely single-valued
            return res

        # Probe min, max, then mid LAST, so the control is left at a known mid
        # value for the model cross-check that follows.
        mid = int((float(lo) + float(hi)) / 2)
        at_min = self.set(target, lo).get("newValue")
        at_max = self.set(target, hi).get("newValue")
        at_mid = self.set(target, mid).get("newValue")

        linear = str(at_max) == str(hi)
        mid_sticks = at_mid is not None and abs(float(at_mid) - mid) <= 1
        if linear and str(at_min) == str(lo):
            res["kind_class"] = "linear"
        elif not linear and mid_sticks:
            # max didn't stick but mid-values do -> circular/wrapping control
            res["kind_class"] = "wrapping"
            res["wrap_to"] = at_max
        else:
            res["kind_class"] = "stuck"          # genuinely unresponsive
        res.update(at_min=at_min, at_mid=at_mid, at_max=at_max, mid=mid)
        return res


def main():
    ap = argparse.ArgumentParser(description="AetherSDR control-surface validation sweep")
    ap.add_argument("--socket", help="override bridge socket path")
    ap.add_argument("--json", help="write full results to this path")
    args = ap.parse_args()

    sock = args.socket or discover_socket()
    if not sock:
        sys.exit("error: no bridge socket; launch with AETHER_AUTOMATION=1")
    v = Validator(Bridge(sock))

    items = v.inventory()
    results, issues = [], []
    t_start = time.monotonic()

    for it in items:
        nm, target = it["name"], it["target"]
        r = {"applet": it["applet"], "control": nm, "target": target,
             "kind": it["kind"], "duplicate": it["duplicate"]}
        # disabled -> skip driving; the bridge refuses it, report as suspect
        if not it["enabled"]:
            r["status"] = "skipped-disabled"
            issues.append((it["applet"], nm, "disabled control",
                           "enabled:false — not driven (bridge refuses invoke)"))
            results.append(r)
            continue
        if it["keying"]:
            r["status"] = "skipped-keying"
            results.append(r)
            continue
        if it["kind"] not in NUMERIC:
            r["status"] = "combo-skip"   # combos validated elsewhere; not scale-probed
            results.append(r)
            continue

        orig = it["value"]
        cls = v.classify_numeric(it)
        r.update(cls)
        r["status"] = "tested"

        # validity findings
        if cls["kind_class"] == "stuck":
            issues.append((it["applet"], nm, "stuck/unresponsive",
                           f"range [{cls['min']},{cls['max']}] but values don't stick"))
        if cls["kind_class"] == "unknown-range":
            issues.append((it["applet"], nm, "no range exposed", ""))

        # model cross-check on the mid value (catches GUI<->model desync)
        if nm in MODEL and cls.get("at_mid") is not None:
            conv, lat, after = v.model_poll(nm, cls["at_mid"])
            r.update(model_converged=conv, model_lat_ms=lat, model_after=after)
            if conv is False:
                issues.append((it["applet"], nm, "GUI<->model desync",
                               f"widget={cls['at_mid']} model={after} (no converge {lat}ms)"))

        # restore
        if orig is not None:
            back = v.set(target, orig).get("newValue")
            r["restored"] = str(back) == str(orig)
            if r["restored"] is False:
                issues.append((it["applet"], nm, "restore failed", f"orig={orig} got {back}"))
        results.append(r)

    total = time.monotonic() - t_start
    txing = v.req({"cmd": "get", "model": "radio", "property": "transmitting"}).get("value")

    # ---- report ----
    tested = [r for r in results if r["status"] == "tested"]
    print(f"\nvalidated {len(tested)} numeric controls "
          f"({len(items)} total surfaces) in {round(total, 2)}s; transmitting={txing}\n")

    print(f"{'APPLET':16} {'CONTROL':22} {'CLASS':9} {'RANGE':>12} {'MODEL':8}")
    for r in results:
        if r["status"] != "tested":
            print(f"{r['applet']:16} {r['control'][:22]:22} {'· '+r['status']}")
            continue
        rng = f"[{r.get('min')},{r.get('max')}]"
        mdl = ("—" if r.get("model_converged") is None
               else "OK" if r["model_converged"] else "DESYNC")
        dup = " (scoped)" if r["duplicate"] else ""
        print(f"{r['applet']:16} {r['control'][:22]:22} {r['kind_class']:9} {rng:>12} {mdl:8}{dup}")

    print(f"\nISSUES: {len(issues)}")
    for ap_, nm, typ, det in issues:
        print(f"  [{ap_}/{nm}] {typ}: {det}")

    if args.json:
        json.dump({"results": results, "issues": issues, "total_s": round(total, 3),
                   "transmitting": txing}, open(args.json, "w"), indent=2)
        print(f"\nfull results -> {args.json}")


if __name__ == "__main__":
    main()
