#!/usr/bin/env python3
"""Non-keying persistence diagnostic supervised outside AetherSDR (macOS/Linux).

No radio is contacted by `plan`. `run` launches its own isolated client; it never
attaches to or kills an existing application. JSON evidence is the primary output.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from automation_probe import Bridge
from radiocert_persist_applets import AppletRun, ALL_CONTROLS, CW_CONTROLS, flatten

SCHEMA = 1
# Owner and scope are contracts, not conclusions inferred from a successful set.
CONTROLS = {
    "fftAverage": ("displayFftAvgSlider", "setValue", "radio", "pan/band", "average"),
    "fftFps": ("displayFftFpsSlider", "setValue", "radio", "pan/band", "fps"),
    "showGrid": ("displayShowGridBtn", "setChecked", "client", "display slot", None),
    "wfColorScheme": ("displayColorSchemeCombo", "setCurrentIndex", "client", "display slot", None),
}
BANDS = ("20m", "40m")


class StopRun(RuntimeError):
    """A prerequisite, ambiguity or safety condition prevents further mutations."""


def atomic_json(path, data):
    """Write-ahead evidence: old complete document or new complete document."""
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(data, stream, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Journal:
    def __init__(self, path, metadata):
        self.path = Path(path)
        self.data = {"schemaVersion": SCHEMA, "metadata": metadata, "status": "running",
                     "events": [], "results": [], "cleanup": []}
        self.save()

    def save(self):
        atomic_json(self.path, self.data)

    def report(self):
        lines = ["# RadioCert Persist", "", f"Run status: **{self.data['status']}**", "",
                 "| Scenario | Outcome | Evidence |", "|---|---|---|"]
        for result in self.data["results"]:
            lines.append(f"| {result['scenario']} | {result['outcome']} | client model and presentation |")
        lines += ["", "The JSON journal contains every action intent and observed snapshot. "
                  "ESTABLISHED applies only to the observed fields and sampled interval; "
                  "independent wire readback, disk commits and RF behavior remain unverified.", "",
                  "Cleanup covers seeded fields only. Band-stack, frequency and span side effects "
                  "remain recorded for operator review. Conflicting state is left intact.", "",
                  "Not run: " + ", ".join(self.data["metadata"].get("plan", plan())["notRun"]) + ".", ""]
        if self.data.get("appletCoverage"):
            lines += ["", "| Applet setting | UI action | Restart | Cleanup |",
                      "|---|---|---|---|"]
            for field, row in self.data["appletCoverage"].items():
                lines.append(f"| {field} | {row.get('actionExercised', False)} | "
                             f"{row.get('transitions', {}).get('full client restart', 'NOT RUN')} | "
                             f"{row.get('cleanup', 'NOT RUN')} |")
            lines += ["", "Radio EQ and TX values are setpoint observations only; no RF was requested.", ""]
        if self.data["status"] == "interrupted":
            lines += ["Stopped: " + self.data["events"][-1].get("reason", "unknown"), ""]
        self.path.with_suffix(".md").write_text("\n".join(lines), encoding="utf-8")

    def event(self, kind, **values):
        self.data["events"].append({"kind": kind, "at": time.time(), **values})
        self.save()

    def result(self, scenario, expected, samples, prerequisites=None):
        result = compare(expected, samples)
        if prerequisites:
            result["seedConcerns"] = prerequisites.copy()
            result["limitation"] = result.get("limitation", "") + " Earlier seeds had model/presentation disagreements; later agreement alone is not a complete retention proof."
            if result["outcome"] == "ESTABLISHED":
                result["observationOutcome"] = "ESTABLISHED"
                result["outcome"] = "INCONCLUSIVE"
        self.data["results"].append({"scenario": scenario, **result})
        self.save()
        print(f"{scenario}: {result['outcome']}", flush=True)
        return result


def compare(expected, samples):
    """No vacuous success, no default-coercion of missing/false/zero values."""
    if not expected or not samples:
        return {"outcome": "INCONCLUSIVE", "reason": "no comparable evidence"}
    missing, differences = [], []
    for index, sample in enumerate(samples):
        for key, want in expected.items():
            if key not in sample or sample[key] is None:
                missing.append({"sample": index, "field": key})
            elif type(sample[key]) is not type(want) or sample[key] != want:
                differences.append({"sample": index, "field": key,
                                    "expected": want, "observed": sample[key]})
    return {"outcome": "CONCERN" if differences else "INCONCLUSIVE" if missing else "ESTABLISHED",
            "evidenceLevel": "client-model-and-presentation", "samples": len(samples), "expected": expected,
            "missing": missing, "differences": differences,
            "limitation": "No independent wire, disk-commit or RF proof; no whole-radio verdict."}


def band_of(frequency):
    # Identification of the two operator-selected contexts, not tuning edges or
    # regional band-plan authority. Band selection itself uses the production UI.
    if isinstance(frequency, (int, float)) and math.isfinite(frequency):
        if 14 <= frequency < 15:
            return "20m"
        if 7 <= frequency < 8:
            return "40m"
    return None


def ready(snapshot, serial, *, slice_count=1, tx_permission=False):
    if snapshot.get("schemaVersion") != SCHEMA:
        raise StopRun("unsupported persist snapshot schema")
    identity, radio = snapshot["identity"], snapshot["radio"]
    if type(tx_permission) is not bool or identity.get("txAllowed") is not tx_permission or identity.get("readOnly") is not False:
        raise StopRun("bridge permission does not match the explicit scenario")
    if not radio.get("connected") or radio.get("serial") != serial:
        raise StopRun("target radio is disconnected or identity changed")
    if snapshot.get("family") != "flex":
        raise StopRun("v1 mutation plan is Flex-only; other families support inventory")
    if snapshot.get("clientSettingsDomains") != 0:
        raise StopRun("Flex unexpectedly declares client-owned operating state")
    if radio.get("transmitting") is not False:
        raise StopRun("radio is transmitting or TX state is unknown")
    tx = snapshot.get("transmit", {})
    if any(tx.get(field) is not False for field in ("transmitting", "tuning", "mox", "voxEnable")):
        raise StopRun("TX/VOX is active or unknown; receive-only run cannot proceed")
    clients = snapshot.get("clients", {}).get("clients", [])
    if not clients or not any(c.get("isUs") for c in clients):
        raise StopRun("client ownership has not been published")
    if any(not c.get("isUs") for c in clients):
        raise StopRun("another client is present; MultiFlex is a separate, not-yet-implemented stage")
    slices, pans = snapshot.get("slices", []), snapshot.get("pans", [])
    if slice_count not in (1, 2) or len(slices) != slice_count or len(pans) != 1:
        raise StopRun(f"scenario requires exactly {slice_count} slices and one panadapter")
    active = [s for s in slices if s.get("active") is True]
    if len(active) != 1:
        raise StopRun("active slice is missing or ambiguous")
    s, p = active[0], pans[0]
    if not p.get("ownedByUs") or not p.get("clientHandle"):
        raise StopRun("pan ownership is unknown or foreign")
    for item in slices:
        if item.get("locked") or item.get("diversity") or item.get("externalReceiveReplacement") or item.get("linkedTo", -1) != -1:
            raise StopRun("locked, linked, diversity or external-source slice needs its own scenario")
        if item.get("panId") != p.get("panId"):
            raise StopRun("slice/pan mapping has not converged")
    if slice_count == 2:
        ids = [item.get("sliceId") for item in slices]
        letters = [item.get("letter") for item in slices]
        slots = {slot.get("id"): slot.get("state") for slot in radio.get("slots", [])}
        if (radio.get("maxSlices", 0) < 2 or len(set(ids)) != 2
                or any(type(i) is not int or i < 0 or slots.get(i) != "ours" for i in ids)
                or len(set(letters)) != 2 or any(x not in tuple("ABCDEFGH") for x in letters)):
            raise StopRun("two-slice capability, identity or ownership is incomplete")
    displays = snapshot.get("display", {}).get("pans", [])
    if len(displays) != 1 or displays[0].get("panId") != p.get("panId"):
        raise StopRun("display-to-pan mapping is missing or ambiguous")
    if not p.get("centerKnown") or p.get("average", -1) < 0 or p.get("fps", -1) < 0:
        raise StopRun("radio display publication is incomplete")
    return s, p, displays[0]


def values(snapshot, serial):
    s, p, display = ready(snapshot, serial)
    result = {"context.band": band_of(s["frequency"]), "slice.mode": s["mode"], "slice.filterLow": s["filterLow"],
              "slice.filterHigh": s["filterHigh"], "slice.rxAntenna": s["rxAntenna"],
              "slice.txAntenna": s["txAntenna"], "slice.audioMute": s["audioMute"],
              "pan.rxAntenna": p.get("rxAntenna")}
    for name in ("radioReportedAverage", "radioReportedFps", "averageIsRequest", "fpsIsRequest"):
        result[f"pan.{name}"] = p.get(name)
    for name, (_, _, _, _, model_field) in CONTROLS.items():
        result[f"display.{name}"] = display.get(name)
        if model_field:
            value = p.get(model_field)
            result[f"pan.{model_field}"] = value if value is not None and value >= 0 else None
    result.update(flatten({"transmit": snapshot.get("transmit", {}),
                           "equalizer": snapshot.get("equalizer", {})}))
    return result


def alternate(value, first, second):
    return second if value == first else first


def plan(rx_antennas=None):
    return {"schemaVersion": SCHEMA, "phase": "persist", "family": "flex", "bands": BANDS,
            "scope": "one isolated client, one slice, one panadapter; receive-only",
            "controls": [{"field": field, "target": c[0], "owner": c[2], "scope": c[3]}
                         for field, c in CONTROLS.items()],
            "scenarios": ["distinct band mode/filter and FFT settings", "mode round trip",
                          "20m/40m band round trips", "normal Quit and same-profile relaunch",
                          "post-restart band revisit", "guarded restoration of seeded fields"],
            "appletControls": [dict(field=c.field, target=c.target, owner=c.owner, scope=c.scope)
                               for c in ALL_CONTROLS],
            "rxAntennas": list(rx_antennas or []),
            "antennaPolicy": "Optional explicit ANT1/ANT2 RX-only changes; TX antenna is an untouched sentinel. "
                             "Signal/audio loss on a disconnected RX port is expected, not a persistence failure.",
            "notRun": ([] if rx_antennas else ["antenna switching"]) + ["slice create/delete", "MultiFlex external authority",
                       "Icom mutation plan", "radio power cycle", "crash/kill recovery",
                       "memory banks", "client DSP", "layout and audio-device persistence"],
            "cleanupLimit": "Restores seeded fields only when still equal to our last expected values. "
                            "Full before/action/after snapshots retain band-stack, frequency and span "
                            "side effects for operator review; those side effects are not automatically undone."}


class Supervisor:
    def __init__(self, app, profile, output, journal, headless=False):
        self.app, self.profile, self.output, self.journal = app, profile, output, journal
        self.identity = "persist-" + uuid.uuid4().hex
        self.process = None
        self.bridge = None
        self.gui_id = None
        self.launches = 0
        self.headless = headless

    def request(self, request):
        response = self.bridge.request(request, timeout_seconds=10)
        if response.get("ok") is not True:
            raise StopRun(f"bridge refused {request}: {response}")
        return response

    def initialize_profile(self):
        # Setup happens only before the FIRST GUI launch, never between quit
        # and relaunch (which would mask missing production flush behavior).
        env = os.environ.copy()
        env["AETHER_SETTINGS_DIR"] = str(self.profile)
        result = subprocess.run([str(self.app), "--config", "set", "AutoConnectToLastRadio", "False"],
                                env=env, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise StopRun("could not disable auto-connect in the isolated profile")
        check = subprocess.run([str(self.app), "--config", "get", "AutoConnectToLastRadio"],
                               env=env, capture_output=True, text=True, timeout=30)
        if check.returncode != 0 or check.stdout.strip() != "False":
            raise StopRun("isolated auto-connect setting did not persist")
        self.journal.event("profile-initialized", autoConnectToLastRadio=False)

    def launch(self):
        if self.process and self.process.poll() is None:
            raise StopRun("old process is still alive; refusing duplicate client")
        self.launches += 1
        # Different endpoints prove that a stale socket cannot answer the new run.
        endpoint = str(Path(tempfile.gettempdir()) / ("aether-persist-" + uuid.uuid4().hex))
        env = os.environ.copy()
        env.pop("AETHER_AUTOMATION_ALLOW_TX", None)
        env.update(AETHER_AUTOMATION="1", AETHER_AUTOMATION_SOCKET=endpoint,
                   AETHER_AUTOMATION_IDENTITY=self.identity,
                   AETHER_AUTOMATION_LABEL="RadioCert Persist", AETHER_AUTOMATION_STATION="RadioCert Persist",
                   AETHER_SETTINGS_DIR=str(self.profile))
        if self.headless:
            env["QT_QPA_PLATFORM"] = "offscreen"
        with (self.output / f"client-{self.launches}.log").open("wb") as log:
            self.process = subprocess.Popen([str(self.app)], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.journal.event("launch", pid=self.process.pid, endpoint=endpoint, profile=str(self.profile))
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise StopRun(f"client exited during startup ({self.process.returncode})")
            try:
                self.bridge = Bridge(endpoint)
                identity = self.request({"cmd": "whoami"})
                if identity["pid"] != self.process.pid:
                    raise StopRun("bridge PID does not match owned process")
                if self.gui_id and identity["guiClientId"] != self.gui_id:
                    raise StopRun("GUIClientID changed across process restart")
                if identity["txAllowed"] or identity["readOnly"]:
                    raise StopRun("client did not start with the required non-keying controls")
                self.gui_id = identity["guiClientId"]
                snapshot = self.request({"cmd": "radiocert", "action": "persist"})
                if Path(snapshot["settingsDirectory"]).resolve() != self.profile.resolve():
                    raise StopRun("client did not use the isolated settings profile")
                self.journal.event("identity", identity=identity, snapshot=snapshot)
                return
            except (OSError, ConnectionError):
                if self.bridge:
                    self.bridge.close()
                    self.bridge = None
                time.sleep(0.25)
        raise StopRun("client bridge did not become ready within 60 seconds")

    def quit(self):
        pid = self.process.pid
        self.journal.event("quit-pending", pid=pid, route="File/Quit QAction; no forced settings save")
        try:
            self.request({"cmd": "invoke", "target": "Quit", "action": "trigger"})
        except (ConnectionError, OSError):
            # A normal quit may close its own socket before the response flushes.
            pass
        self.bridge.close()
        self.bridge = None
        try:
            code = self.process.wait(timeout=30)
        except subprocess.TimeoutExpired as exc:
            raise StopRun("normal Quit did not terminate the owned process; no forced kill or relaunch") from exc
        self.journal.event("quit-complete", pid=pid, exitCode=code)
        if code != 0:
            raise StopRun(f"client exited abnormally ({code}); restart is not established")


def named_widget(tree, name):
    matches = []
    def visit(node):
        if isinstance(node, dict):
            if node.get("objectName") == name:
                matches.append(node)
            for child in node.values():
                if isinstance(child, (dict, list)):
                    visit(child)
        elif isinstance(node, list):
            for child in node:
                visit(child)
    visit(tree)
    if len(matches) != 1:
        raise StopRun(f"expected one {name} widget, found {len(matches)}")
    return matches[0]


def show_control(read, act, target):
    tree = read({"cmd": "dumpTree"})
    if not named_widget(tree, target).get("visible"):
        if not named_widget(tree, "panMenuDisplayBtn").get("visible"):
            act({"cmd": "invoke", "target": "SpectrumOverlayMenu/→", "action": "click"},
                "expand panadapter menu")
        act({"cmd": "invoke", "target": "panMenuDisplayBtn", "action": "click"},
            "open Display panel")
    act({"cmd": "scrollTo", "target": target}, "scroll Display control into view")
    widget = named_widget(read({"cmd": "dumpTree"}), target)
    if not widget.get("visible") or not widget.get("enabled"):
        raise StopRun(f"{target} did not become visible and enabled")


class Run:
    def __init__(self, supervisor, serial, journal, rx_antennas=None):
        self.supervisor, self.serial, self.journal = supervisor, serial, journal
        self.rx_antennas = rx_antennas
        self.baselines, self.expected = {}, {}
        self.seed_concerns = {}
        self.trace_pid = None
        self.trace_seq = 0
        self.applets = AppletRun(self)

    def assert_ready(self, snapshot):
        return ready(snapshot, self.serial)

    def snapshot(self):
        snapshot = self.supervisor.request({"cmd": "radiocert", "action": "persist"})
        self.journal.event("snapshot", snapshot=snapshot)
        if self.trace_pid is not None and self.trace_pid == snapshot["identity"]["pid"]:
            trace = self.supervisor.request({"cmd": "log", "action": "tail",
                                            "value": f"5000 since={self.trace_seq}"})
            if trace.get("events"):
                first = trace["events"][0]["seq"]
                if first > self.trace_seq + 1:
                    self.journal.event("wire-log-gap", pid=self.trace_pid,
                                       after=self.trace_seq, firstAvailable=first,
                                       limitation="A missing interval cannot exclude an intervening wrong-value write.")
                self.journal.event("wire-log", pid=self.trace_pid, trace=trace)
            self.trace_seq = trace.get("seq", self.trace_seq)
        return snapshot

    def wait_ready(self, band=None, mode=None):
        deadline, stable_since = time.monotonic() + 30, None
        last_error = "radio not ready"
        while time.monotonic() < deadline:
            snapshot = self.snapshot()
            try:
                s, _, _ = ready(snapshot, self.serial)
                if band and band_of(s["frequency"]) != band:
                    raise StopRun(f"waiting for {band} context")
                if mode and s["mode"] != mode:
                    raise StopRun(f"waiting for {mode} publication")
                stable_since = stable_since or time.monotonic()
                if time.monotonic() - stable_since >= 2:
                    return snapshot
            except StopRun as exc:
                last_error = str(exc)
                stable_since = None
            time.sleep(0.5)
        raise StopRun(last_error)

    def action(self, request, reason):
        before = self.snapshot()
        ready(before, self.serial)  # Ownership/TX checked before every mutation.
        self.journal.event("action-pending", request=request, reason=reason, before=before)
        response = self.supervisor.request(request)
        self.journal.event("action-response", request=request, response=response)

    def band(self, name):
        self.action({"cmd": "shortcut", "target": f"band_{name}"}, f"production BAND selection: {name}")
        return self.wait_ready(name)

    def connect(self):
        # Runtime-only categories, restarted with each owned process. Preserve
        # command responses alongside snapshots: some Flex display setters ACK
        # without immediately re-publishing the sender's model value.
        identity = self.supervisor.request({"cmd": "whoami"})
        if self.trace_pid != identity["pid"]:
            self.trace_pid, self.trace_seq = identity["pid"], 0
            for category in ("aether.connection", "aether.protocol"):
                self.supervisor.request({"cmd": "log", "action": "set", "value": category + " on"})
        existing = self.snapshot()
        if existing["radio"].get("connected"):
            return self.wait_ready()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            radios = self.supervisor.request({"cmd": "connect", "action": "list"}).get("radios", [])
            matches = [r for r in radios if r.get("serial") == self.serial]
            if len(matches) == 1:
                radio = matches[0]
                # Discovery is a prerequisite, never a reason to evict a client.
                if str(radio.get("status", "")).lower() != "available":
                    raise StopRun(f"radio is not advertised Available: {radio}")
                self.journal.event("connect-pending", radio=radio)
                self.supervisor.request({"cmd": "connect", "action": "local", "value": "serial " + self.serial})
                return self.wait_ready()
            time.sleep(0.5)
        raise StopRun("exact serial was not discovered; no fallback to first radio")

    def observe(self, name, expected):
        expected = dict(expected)
        if "seed convergence" not in name:
            for field, reported in (("average", "radioReportedAverage"), ("fps", "radioReportedFps")):
                if f"pan.{field}" in expected:
                    expected[f"pan.{reported}"] = expected[f"pan.{field}"]
        samples = []
        for _ in range(11):
            samples.append(values(self.snapshot(), self.serial))
            time.sleep(0.5)
        prerequisites = self.seed_concerns if "seed convergence" not in name else None
        return self.journal.result(name, expected, samples, prerequisites)

    def control(self, field, value):
        target, action, _, _, _ = CONTROLS[field]
        show_control(self.supervisor.request, self.action, target)
        self.action({"cmd": "invoke", "target": target, "action": action, "value": value}, f"Display panel: {field}")

    def seed(self, band, index):
        baseline = self.band(band)
        self.baselines[band] = baseline
        self.journal.event("band-baseline", band=band, snapshot=baseline)
        _, _, display = ready(baseline, self.serial)
        mode, low, high = ("USB", 250, 2750) if index == 0 else ("LSB", -2600, -200)
        self.action({"cmd": "slice", "action": "mode", "value": mode}, "seed mode")
        # Capture the destination mode's filter before editing that context too.
        mode_before = self.wait_ready(band, mode)
        self.journal.event("mode-baseline", band=band, snapshot=mode_before)
        old_filter = mode_before["slices"][0]
        if (low, high) == (old_filter["filterLow"], old_filter["filterHigh"]):
            low, high = (300, 2650) if index == 0 else (-2550, -250)
        self.action({"cmd": "slice", "action": "filter", "value": f"{low} {high}"}, "seed passband")
        settings = {"fftAverage": alternate(display["fftAverage"], 17, 19) if index == 0 else alternate(display["fftAverage"], 43, 47),
                    "fftFps": alternate(display["fftFps"], 15, 18) if index == 0 else alternate(display["fftFps"], 25, 30)}
        # Client appearance belongs to the display slot, not to each band.
        if index == 0:
            settings.update(showGrid=not display["showGrid"],
                            wfColorScheme=alternate(display["wfColorScheme"], 1, 2))
        for field, value in settings.items():
            self.control(field, value)
        if self.rx_antennas:
            self.rx_antenna(self.rx_antennas[index])
        expected = {"context.band": band, "slice.mode": mode, "slice.filterLow": low, "slice.filterHigh": high}
        for field, value in settings.items():
            expected[f"display.{field}"] = value
            model_field = CONTROLS[field][4]
            if model_field:
                expected[f"pan.{model_field}"] = value
        if index:
            for field in ("showGrid", "wfColorScheme"):
                expected[f"display.{field}"] = self.expected[BANDS[0]][f"display.{field}"]
        # Untouched setpoints are sentinels, not proof of a negative wire event.
        original = values(baseline, self.serial)
        expected.update({key: original[key] for key in ("slice.rxAntenna", "slice.txAntenna", "slice.audioMute")})
        if self.rx_antennas:
            expected.update({"slice.rxAntenna": self.rx_antennas[index],
                             "pan.rxAntenna": self.rx_antennas[index]})
        self.expected[band] = expected
        self.wait_ready(band)
        result = self.observe(f"{band}: seed convergence", expected)
        if result["outcome"] != "ESTABLISHED":
            # A diagnostic must retain the concern and continue independent
            # transitions. Ownership/TX/topology failures still stop the run.
            self.seed_concerns[band] = sorted({item["field"] for key in ("differences", "missing")
                                              for item in result.get(key, [])})
            self.journal.event("seed-concern", band=band, fields=self.seed_concerns[band])

    def rx_antenna(self, port):
        if not self.rx_antennas or port not in self.rx_antennas:
            raise StopRun("RX antenna port was not explicitly selected for this run")
        _, pan, _ = ready(self.snapshot(), self.serial)
        if port not in pan.get("antennas", []):
            raise StopRun("RX antenna port is not in the radio-published antenna list")
        self.action({"cmd": "slice", "action": "rxant", "value": port}, "RX antenna setpoint only")

    def antenna_roundtrip(self):
        if not self.rx_antennas:
            return
        expected = self.expected[BANDS[0]]
        alternate_port = self.rx_antennas[1]
        self.rx_antenna(alternate_port)
        self.wait_ready(BANDS[0])
        changed = {**expected, "slice.rxAntenna": alternate_port, "pan.rxAntenna": alternate_port}
        self.observe("20m: RX antenna alternate port", changed)
        self.rx_antenna(self.rx_antennas[0])
        self.wait_ready(BANDS[0])
        self.observe("20m: RX antenna round trip", expected)

    def cleanup(self):
        # Do not guess whether a mismatch is our bug or a newer operator action.
        for band in reversed(BANDS):
            if band not in self.baselines:
                continue
            now = self.band(band)
            actual = values(now, self.serial)
            expected = self.expected.get(band, {})
            if compare(expected, [actual])["outcome"] != "ESTABLISHED":
                self.journal.data["cleanup"].append({"band": band, "outcome": "INCONCLUSIVE",
                    "reason": "state differs from the last test intent; left intact for operator review"})
                self.journal.save()
                continue
            baseline = self.baselines[band]
            s, _, display = ready(baseline, self.serial)
            if self.rx_antennas and s["rxAntenna"] not in self.rx_antennas:
                self.journal.data["cleanup"].append({"band": band, "outcome": "INCONCLUSIVE",
                    "reason": "original RX port is outside the authorized pair; left for operator recovery"})
                self.journal.save()
                continue
            # Restore the filter in the mode whose memory we changed before
            # returning to the original mode. Never write it into another mode.
            mode_events = [e for e in self.journal.data["events"] if e["kind"] == "mode-baseline" and e["band"] == band]
            old_mode = mode_events[-1]["snapshot"]["slices"][0]
            self.action({"cmd": "slice", "action": "filter", "value": f"{old_mode['filterLow']} {old_mode['filterHigh']}"}, "restore seeded mode filter")
            self.action({"cmd": "slice", "action": "mode", "value": s["mode"]}, "restore original band mode")
            # Global client appearance is restored once, at the first band.
            fields = CONTROLS if band == BANDS[0] else ("fftAverage", "fftFps")
            restore = {}
            if self.rx_antennas:
                self.rx_antenna(s["rxAntenna"])
                restore.update({"slice.rxAntenna": s["rxAntenna"], "pan.rxAntenna": s["rxAntenna"]})
            for field in fields:
                self.control(field, display[field])
                restore[f"display.{field}"] = display[field]
                if CONTROLS[field][4]:
                    restore[f"pan.{CONTROLS[field][4]}"] = display[field]
            restore.update({"slice.mode": s["mode"], "slice.filterLow": s["filterLow"],
                            "slice.filterHigh": s["filterHigh"]})
            # An ACK-only setter is dispatch evidence. Revisit the context to
            # get fresh radio state before claiming restored FFT persistence.
            self.band(BANDS[1] if band == BANDS[0] else BANDS[0])
            self.band(band)
            result = self.observe(f"{band}: seeded-field cleanup", restore)
            self.journal.data["cleanup"].append({"band": band, **result,
                "limit": "band-stack/span/frequency side effects retained in journal for manual review"})
            self.journal.save()

    def execute(self):
        self.connect()
        for index, band in enumerate(BANDS):
            self.seed(band, index)
        for band in BANDS:
            self.band(band)
            self.observe(f"{band}: band round trip", self.expected[band])
        # Change mode through the actual mode shortcut. No filter is written in
        # the intermediate CW context; capture that context before leaving it.
        self.band(BANDS[0])
        self.applets.seed()
        for expected in self.expected.values():
            expected.pop("slice.audioMute", None)
        self.applets.observe("seeded applet settings")
        self.applets.bypass_roundtrips()
        self.band(BANDS[1])
        self.band(BANDS[0])
        self.applets.observe("band round trip")
        self.action({"cmd": "shortcut", "target": "mode_cw"}, "mode round trip")
        self.wait_ready(BANDS[0], "CW")
        self.journal.event("intermediate-mode", snapshot=self.snapshot())
        self.applets.seed(CW_CONTROLS)
        self.action({"cmd": "shortcut", "target": "mode_usb"}, "return seeded mode")
        self.wait_ready(BANDS[0], "USB")
        self.observe("20m: mode round trip", self.expected[BANDS[0]])
        self.antenna_roundtrip()
        self.applets.observe("mode and antenna round trips")
        self.supervisor.quit()
        self.supervisor.launch()
        self.connect()
        # Observe restored entry context BEFORE a band action could repair it.
        self.observe("20m: full client restart", self.expected[BANDS[0]])
        self.applets.observe("full client restart")
        for band in reversed(BANDS):
            self.band(band)
            self.observe(f"{band}: post-restart revisit", self.expected[band])
        self.applets.observe("post-restart band revisit")
        self.action({"cmd": "shortcut", "target": "mode_cw"}, "restore CW applet settings")
        self.wait_ready(BANDS[0], "CW")
        self.applets.restore(CW_CONTROLS)
        self.action({"cmd": "shortcut", "target": "mode_usb"}, "restore phone context")
        self.wait_ready(BANDS[0], "USB")
        self.applets.restore()
        # Muted slice was an intentional applet seed; restore it before the
        # display runner compares its untouched sentinels.
        self.cleanup()
        self.supervisor.request({"cmd": "log", "action": "reset"})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("plan", "run", "smoke"))
    parser.add_argument("--app", type=Path, help="AetherSDR executable or .app bundle")
    parser.add_argument("--profile", type=Path, help="new, dedicated directory; must not exist")
    parser.add_argument("--output", type=Path, help="new evidence directory; must not exist")
    parser.add_argument("--rx-antennas", nargs=2, choices=("ANT1", "ANT2"),
                        help="explicitly authorized RX ports for 20m and 40m; no TX actions")
    parser.add_argument("--serial", help="exact discovery serial; run never selects first")
    args = parser.parse_args(argv)
    if args.command == "plan":
        print(json.dumps(plan(args.rx_antennas), indent=2))
        return 0
    if sys.platform == "win32":
        parser.error("v1 process supervision supports macOS/Linux; Windows named-pipe timeout work is pending")
    if not args.app or not args.profile or not args.output or (args.command == "run" and not args.serial):
        parser.error("--app, --profile, --output and (for run) --serial are required")
    app = args.app.resolve()
    if app.suffix == ".app":
        app /= "Contents/MacOS/AetherSDR"
    if not app.is_file() or not os.access(app, os.X_OK):
        parser.error("app executable is missing or not executable")
    if args.rx_antennas and len(set(args.rx_antennas)) != 2:
        parser.error("RX antenna pair must contain two different authorized ports")
    profile, output = args.profile.resolve(), args.output.resolve()
    if profile == output or profile in output.parents or output in profile.parents:
        parser.error("profile and output must be separate, non-nested directories")
    if profile.exists() or output.exists():
        parser.error("use new profile/output directories; existing operator data is never reused")
    profile.mkdir(parents=True, mode=0o700)
    output.mkdir(parents=True, mode=0o700)
    digest = hashlib.sha256()
    with app.open("rb") as binary:
        for chunk in iter(lambda: binary.read(1024 * 1024), b""):
            digest.update(chunk)
    binary_hash = digest.hexdigest()
    journal = Journal(output / "persist.json", {"plan": plan(args.rx_antennas), "app": str(app), "sha256": binary_hash,
                      "profile": str(profile), "serial": args.serial, "evidence": "not live hardware" if args.command == "smoke" else "live diagnostic"})
    runner_sources = {}
    for name in ("radiocert_persist.py", "radiocert_persist_applets.py"):
        source = Path(__file__).with_name(name).read_bytes()
        runner_sources[name] = hashlib.sha256(source).hexdigest()
        (output / name).write_bytes(source)
    journal.data["metadata"]["runnerSources"] = runner_sources
    journal.save()
    supervisor = Supervisor(app, profile, output, journal, headless=args.command == "smoke")
    try:
        supervisor.initialize_profile()
        supervisor.launch()
        if args.command == "smoke":
            first = supervisor.request({"cmd": "radiocert", "action": "persist"})
            supervisor.quit()
            supervisor.launch()
            second = supervisor.request({"cmd": "radiocert", "action": "persist"})
            if first["identity"]["pid"] == second["identity"]["pid"]:
                raise StopRun("restart did not replace the process")
            supervisor.quit()
            journal.event("smoke-complete", scope="real bridge snapshot and normal process restart; no radio connected")
        else:
            Run(supervisor, args.serial, journal, args.rx_antennas).execute()
            # Leave the client open for operator inspection, including concerns.
            journal.event("client-left-open", pid=supervisor.process.pid)
        journal.data["status"] = "completed"
        journal.save()
    except (StopRun, OSError, ConnectionError, KeyError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        journal.data["status"] = "interrupted"
        journal.event("stopped", reason=str(exc),
                      recovery="Inspect action-pending/band-baseline/mode-baseline before further changes. "
                               "Owned client may still be running; no automatic kill or blind restore.")
        journal.report()
        print(f"Stopped: {exc}\nEvidence: {journal.path}", file=sys.stderr)
        return 2
    journal.report()
    print(f"Evidence: {journal.path}")
    return 0  # Diagnostic findings are in the report, not a radio pass/fail code.


if __name__ == "__main__":
    raise SystemExit(main())
