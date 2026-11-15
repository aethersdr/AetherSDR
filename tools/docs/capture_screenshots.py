#!/usr/bin/env python3
"""Capture the user-docs screenshots from scripted AetherSDR sessions.

Every shot is declared in ``docs/user/screens.json``: the page and heading it
belongs under, the automation-bridge steps that put the app into the right
state, the widget to grab, an optional crop, a caption and (mandatory) alt
text. This script launches a private AetherSDR instance, drives it over the
automation bridge's raw unix-socket JSON protocol (docs/automation-bridge.md
-- no MCP client needed), grabs each widget to PNG, blacks out anything that
identifies the station, and writes the results to
``docs/user/static/img/screens/<id>.png``.

Three kinds of shot, three passes (Linux; see docs/user/README.md):

* Real radio -- every shot not tagged otherwise. ``--radio-serial <serial>
  --no-netns``: the app must reach the LAN, and the script connects only to
  that serial and aborts (after disconnecting) if it lands on any other radio.
* Demo -- shots tagged ``"radio": "demo"`` (the pages that teach the
  simulator). Without ``--radio-serial`` the script connects to DEMO-0001, in a
  private network + UTS namespace (``unshare -n -u --map-current-user``) so no
  real radio can be discovered or reached.
* Popups -- shots tagged ``"offscreen": true``. On Wayland a popup menu needs
  a focused window, which a background capture never has, so menus are shot in
  a ``--platform offscreen --dpr <ratio>`` pass (with or without
  ``--radio-serial``, for the radio or the demo ones).

On-screen passes use a real compositor: ``--platform wayland`` (or xcb) with
``--allow-gpu-build``, so the GPU panadapter renders. ``--hypr-workspace N``
(Hyprland) opens the app's windows silently on a background workspace and
removes that rule on exit. ``--only id1,id2`` re-shoots a subset;
``--socket``/``--token`` attach to an instance you launched yourself (with
``--keep``), ``--skip-setup`` skips replaying the setup steps.

Rendering. Under ``QT_QPA_PLATFORM=offscreen`` there is no GL context, so the
default QRhi (GPU) panadapter shows "Spectrum renderer unavailable"; only the
popup pass runs there. The script refuses a GPU build without
``--allow-gpu-build``.

Safety. The app is launched with a fresh settings directory,
``AETHER_AUTOMATION_NO_TX=1``, a random bridge token and an explicit socket;
the script refuses transmit verbs and never presses a keying control. Steps
that change the radio undo themselves (``remove_extra_slices``,
``notch_snapshot``/``notch_remove_new``, which never touches a notch that was
there before). On exit it disconnects and kills only the PID it launched.

Redaction. After the crop, every visible widget in the grab whose text holds
an IP or MAC address, the serial, a tailnet name, a GPS position, the bridge
token, the scratch paths or the host name is filled black; text no widget
holds (list items, painted labels) is found by OCR (``tesseract``) and filled
too, as are a shot's manual ``redact`` boxes (logical pixels, relative to the
cropped image). The result is OCR'd again and anything that still reads like
an address is reported. Look at every image before committing it all the same.

Requirements: Python 3.8+, an AetherSDR binary, ImageMagick (``magick``) and
``tesseract``; ``oxipng``/``pngquant`` are used when present.
"""

import argparse
import json
import math
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_MANIFEST = os.path.join(REPO, "docs", "user", "screens.json")
DEFAULT_OUT = os.path.join(REPO, "docs", "user", "static", "img", "screens")
DEMO_SERIAL = "DEMO-0001"

# Text that identifies the operator's radio or machine and must never appear
# in a published image (see Capture.redact).
IPV4_RE = re.compile(r"(?<![\d.])\d{1,3}(?:\.\d{1,3}){3}(?![\d.])")
IPV6_RE = re.compile(r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}(?![0-9a-f:])")
MAC_RE = re.compile(r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])")
# A GPS-equipped radio reports the station's position: decimal coordinates
# and a 6-character Maidenhead locator are as identifying as an address.
LATLON_RE = re.compile(r"-?\d{1,3}\.\d{3,}\s*[,/ ]\s*-?\d{1,3}\.\d{3,}")
# A Tailscale MagicDNS name carries the tailnet's own identifier.
TAILNET_RE = re.compile(r"(?i)\b[\w-]+\.ts\.net\b|\btail[0-9a-f]{4,}\b")
GRID6_RE = re.compile(r"(?<![A-Za-z0-9])[A-R]{2}\d{2}[a-x]{2}(?![A-Za-z0-9])")


# Documentation and loopback addresses identify nobody; leave them readable.
SAFE_IPV4 = re.compile(r"^(?:127\.|0\.0\.0\.0$|192\.0\.2\.|198\.51\.100\.|203\.0\.113\.)")


OCR_PRIVATE_PREFIX = re.compile(
    r"(?<![\d.])(?:192\.168|10\.\d{1,3}\.|172\.(?:1[6-9]|2\d|3[01])\.|169\.254|"
    r"100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.)")


def has_private_ipv4(text):
    return any(not SAFE_IPV4.match(m.group(0)) for m in IPV4_RE.finditer(text))


def looks_like_ipv6(text):
    for m in IPV6_RE.finditer(text):
        tok = m.group(0)
        # "12:34:56" is a clock, not an address: require '::' or 5+ groups,
        # and at least one hex digit.
        if ("::" in tok or tok.count(":") >= 4) and re.search(r"[0-9a-fA-F]", tok):
            return True
    return False


class BridgeError(RuntimeError):
    pass


class Bridge:
    """Newline-delimited JSON over the AetherSDR automation unix socket."""

    def __init__(self, path, token, timeout=60):
        self.path = path
        self.token = token
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.rfile = self.sock.makefile("rb")

    def request(self, req, allow_fail=False):
        req = dict(req)
        if self.token:
            req["token"] = self.token
        self.sock.sendall((json.dumps(req) + "\n").encode())
        line = self.rfile.readline()
        if not line:
            raise BridgeError("bridge closed the connection")
        resp = json.loads(line)
        if not resp.get("ok") and not allow_fail:
            shown = {k: v for k, v in req.items() if k != "token"}
            raise BridgeError("%s -> %s" % (json.dumps(shown), resp.get("error", resp)))
        return resp

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------- tree helpers

def iter_nodes(node, ancestors=()):
    yield node, ancestors
    for child in node.get("children") or []:
        yield from iter_nodes(child, ancestors + (node,))


def short_class(node):
    return (node.get("class") or "").split("::")[-1]


def node_matches(node, name):
    return name in (node.get("objectName"), node.get("accessibleName"),
                    node.get("class"), short_class(node))


def find_node(tree, name, scope=None, visible=True, predicate=None):
    """First node matching ``name`` (objectName/accessibleName/class), optionally
    under an ancestor matching ``scope``. Visible nodes win."""
    hits = []
    for root in tree.get("roots", []):
        for node, anc in iter_nodes(root):
            if not node_matches(node, name):
                continue
            if scope and not any(node_matches(a, scope) for a in anc + (node,)):
                continue
            if predicate and not predicate(node):
                continue
            hits.append(node)
    if visible:
        vis = [n for n in hits if n.get("visible")]
        if vis:
            return vis[0]
    return hits[0] if hits else None


def node_text(node):
    return node.get("text") if node.get("text") is not None else node.get("value")


def resolve_spec(tree, spec, scope=None):
    """A crop/redaction widget spec: an objectName, accessibleName or class
    (as find_node); "text:<label>" for the first visible widget whose text or
    value is <label>; or "group:<label>" for the QGroupBox (or other frame)
    that holds such a widget -- group boxes carry no name of their own."""
    if spec.startswith(("text:", "group:")):
        kind, _, label = spec.partition(":")
        for root in tree.get("roots", []):
            for node, anc in iter_nodes(root):
                if not node.get("visible") or any(not a.get("visible") for a in anc):
                    continue
                if node_text(node) != label:
                    continue
                if scope and not any(node_matches(a, scope) for a in anc):
                    continue
                if kind == "text":
                    return node
                for a in reversed(anc):
                    if short_class(a) in ("QGroupBox", "QFrame"):
                        return a
        return None
    return find_node(tree, spec, scope=scope)


def find_button_by_text(tree, text, scope=None):
    for root in tree.get("roots", []):
        for node, anc in iter_nodes(root):
            if not node.get("visible") or node_text(node) != text:
                continue
            if "Button" not in (node.get("class") or ""):
                continue
            if scope and not any(node_matches(a, scope) for a in anc):
                continue
            return node, anc
    return None, None


def visible_windows(tree):
    return [r for r in tree.get("roots", []) if r.get("visible")]


# ---------------------------------------------------------------- image helpers

def magick_bin():
    return shutil.which("magick")


def crop_png(src, dst, x, y, w, h):
    tool = magick_bin()
    if not tool:
        print("    ! ImageMagick not found; leaving %s uncropped" % os.path.basename(dst))
        shutil.copyfile(src, dst)
        return
    subprocess.run([tool, src, "-crop", "%dx%d+%d+%d" % (w, h, x, y), "+repage", dst],
                   check=True)


def optimise_png(path, quantize=False):
    """Lossless recompression (and optional palette quantization)."""
    if quantize and shutil.which("pngquant"):
        subprocess.run(["pngquant", "--force", "--skip-if-larger", "--strip",
                        "--quality", "70-95", "--output", path, path], check=False)
    if shutil.which("oxipng"):
        subprocess.run(["oxipng", "-q", "-o", "4", "--strip", "safe", path], check=False)
        return
    tool = magick_bin()
    if not tool:
        return
    tmp = path + ".opt.png"
    args = [tool, path, "-strip"]
    if quantize and not shutil.which("pngquant"):
        args += ["-dither", "None", "-colors", "256"]
    args += ["-define", "png:compression-level=9", "-define", "png:compression-filter=5",
             "-define", "png:compression-strategy=1", "-define", "png:exclude-chunks=date,time",
             tmp]
    subprocess.run(args, check=True)
    if os.path.getsize(tmp) < os.path.getsize(path):
        os.replace(tmp, path)
    else:
        os.remove(tmp)


# ---------------------------------------------------------------- runner

class Capture:
    def __init__(self, args, manifest):
        self.args = args
        self.manifest = manifest
        # Shots tagged "radio": "demo" teach the simulator and run against
        # DEMO-0001; every other shot runs against the --radio-serial radio.
        self.serial = args.radio_serial or DEMO_SERIAL
        self.demo = self.serial == DEMO_SERIAL
        self.secrets = []           # literal strings to black out (see redact)
        self.pii_hits = []
        self.proc = None
        self.pid = None
        self.bridge = None
        self.workdir = None
        self.token = None

    def key(self, name):
        """Manifest key for this run: "setup", "setup_demo", and for the
        offscreen popup pass "setup_offscreen" / "setup_demo_offscreen"
        when the manifest has one."""
        key = name + "_demo" if self.demo else name
        if self.args.platform == "offscreen" and key + "_offscreen" in self.manifest:
            key += "_offscreen"
        return key

    # -- launch / attach ---------------------------------------------------

    def binary(self):
        if self.args.binary:
            return os.path.abspath(self.args.binary)
        build = os.path.abspath(self.args.build_dir)
        for cand in ("AetherSDR", os.path.join("AetherSDR.app", "Contents", "MacOS", "AetherSDR")):
            p = os.path.join(build, cand)
            if os.path.isfile(p):
                return p
        sys.exit("error: no AetherSDR binary under %s (pass --binary)" % build)

    def check_build(self):
        if self.args.allow_gpu_build or self.args.binary:
            return
        cache = os.path.join(os.path.abspath(self.args.build_dir), "CMakeCache.txt")
        try:
            with open(cache) as f:
                for line in f:
                    if line.startswith("AETHER_GPU_SPECTRUM:") and line.strip().endswith("=ON"):
                        sys.exit("error: %s was configured with AETHER_GPU_SPECTRUM=ON; offscreen "
                                 "captures need -DAETHER_GPU_SPECTRUM=OFF (or pass "
                                 "--allow-gpu-build with a real display platform)" % self.args.build_dir)
        except OSError:
            pass

    def launch(self):
        self.check_build()
        if not self.demo and not self.args.no_netns:
            sys.exit("error: --radio-serial needs --no-netns (the private network namespace "
                     "cannot reach a radio on the LAN)")
        exe = self.binary()
        tmp = tempfile.gettempdir()
        self.workdir = tempfile.mkdtemp(prefix="ads-", dir=tmp)
        settings = os.path.join(self.workdir, "settings")
        home = os.path.join(self.workdir, "home")
        for d in (settings, home, os.path.join(home, ".config")):
            os.makedirs(d, exist_ok=True)
        sock_name = "ads-%d" % os.getpid()
        self.socket_path = os.path.join(tmp, sock_name)
        if len(self.socket_path) > 100:
            sys.exit("error: socket path %s is too long for AF_UNIX; shorten TMPDIR"
                     % self.socket_path)
        # --token fixes the bridge token so a --keep instance can be driven
        # again later with --socket/--token; otherwise it is random.
        self.token = self.args.token or secrets.token_hex(16)
        env = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "AETHER_AUTOMATION_ALLOW_TX"):
            env.pop(k, None)
        env.update({
            "HOME": home,
            "XDG_CONFIG_HOME": os.path.join(home, ".config"),
            "AETHER_SETTINGS_DIR": settings,
            "QT_QPA_PLATFORM": self.args.platform,
            "AETHER_AUTOMATION": "1",
            "AETHER_AUTOMATION_SOCKET": sock_name,
            "AETHER_AUTOMATION_NO_TX": "1",
            "AETHER_AUTOMATION_IDENTITY": "docs-screens",
            "AETHER_MCP_TOKEN": self.token,
            "TZ": "UTC",
            "LANG": "en_US.UTF-8",
            "LC_ALL": "en_US.UTF-8",
        })
        if self.args.platform == "offscreen":
            # A large virtual screen, so popup menus do not wrap into
            # columns, scaled to the on-screen pass's pixel ratio.
            conf = os.path.join(self.workdir, "offscreen.json")
            with open(conf, "w") as f:
                json.dump({"screens": [{"name": "docs", "x": 0, "y": 0,
                                        "width": 5120, "height": 3200,
                                        "logicalDpi": 96}]}, f)
            env["QT_QPA_PLATFORM"] = "offscreen:configfile=" + conf
            if self.args.dpr != 1.0:
                env["QT_SCALE_FACTOR"] = "%g" % self.args.dpr
        if self.args.platform != "offscreen" and os.environ.get("DISPLAY"):
            env["DISPLAY"] = os.environ["DISPLAY"]
        if self.args.platform.startswith("wayland"):
            # The Wayland socket is a filesystem path under XDG_RUNTIME_DIR, so
            # it stays reachable from inside the private network namespace.
            env["WAYLAND_DISPLAY"] = self.args.wayland_display or os.environ.get(
                "WAYLAND_DISPLAY", "wayland-0")
        self.hypr_rule_on()
        cmd = [exe]
        if sys.platform.startswith("linux") and not self.args.no_netns and shutil.which("unshare"):
            # --keep-caps lets the shell set the namespace's hostname (the
            # Radio Setup "Station Name" defaults to it); setpriv then drops
            # every capability before exec'ing the app, which keeps the PID.
            drop = ('setpriv --ambient-caps=-all --inh-caps=-all '
                    if shutil.which("setpriv") else "")
            inner = ("ip link set lo up 2>/dev/null; hostname %s 2>/dev/null; "
                     'exec %s"$0"' % (self.manifest.get("hostname", "aethersdr-demo"), drop))
            cmd = ["unshare", "-n", "-u", "--map-current-user", "--keep-caps",
                   "sh", "-c", inner, exe]
        log_path = os.path.join(self.workdir, "app.log")
        print("launching %s\n  settings %s\n  socket   %s\n  log      %s"
              % (" ".join(cmd[:4]) + (" ... " + exe if len(cmd) > 1 else ""),
                 settings, self.socket_path, log_path))
        self.proc = subprocess.Popen(cmd, env=env, cwd=self.workdir,
                                     stdin=subprocess.DEVNULL,
                                     stdout=open(log_path, "wb"), stderr=subprocess.STDOUT,
                                     start_new_session=True)
        self.pid = self.proc.pid
        print("  pid      %d" % self.pid)
        deadline = time.time() + 60
        while not os.path.exists(self.socket_path):
            if self.proc.poll() is not None:
                sys.exit("error: AetherSDR exited early (rc=%s); see %s"
                         % (self.proc.returncode, log_path))
            if time.time() > deadline:
                sys.exit("error: bridge socket never appeared; see %s" % log_path)
            time.sleep(0.25)
        time.sleep(0.5)
        self.bridge = Bridge(self.socket_path, self.token)

    # -- Hyprland placement ------------------------------------------------

    def hyprctl(self, *argv):
        env = dict(os.environ)
        if not env.get("HYPRLAND_INSTANCE_SIGNATURE"):
            d = os.path.join(os.environ.get("XDG_RUNTIME_DIR", ""), "hypr")
            if os.path.isdir(d):
                sigs = sorted(os.listdir(d), key=lambda n: os.path.getmtime(os.path.join(d, n)))
                if sigs:
                    env["HYPRLAND_INSTANCE_SIGNATURE"] = sigs[-1]
        return subprocess.run(["hyprctl", *argv], env=env, capture_output=True, text=True)

    def hypr_rule_on(self):
        """Open every AetherSDR window on a background workspace without
        switching the operator's view (Hyprland Lua config)."""
        ws = self.args.hypr_workspace
        if not ws:
            return
        r = self.hyprctl("eval", 'o.window("^AetherSDR$", { workspace = "%d silent" })' % ws)
        if r.returncode != 0 or "error" in (r.stdout + r.stderr).lower():
            sys.exit("error: could not add the Hyprland workspace rule: %s" % (r.stdout + r.stderr))
        self.hypr_rule = True
        print("  hyprland  AetherSDR windows -> workspace %d (silent) until exit" % ws)

    def hypr_rule_off(self):
        if getattr(self, "hypr_rule", False):
            self.hyprctl("reload")   # drops the runtime rule; the config is re-read as-is
            self.hypr_rule = False
            print("  hyprland  config reloaded; workspace rule removed")

    def attach(self):
        token = self.args.token or os.environ.get("AETHER_MCP_TOKEN")
        self.socket_path = self.args.socket
        self.workdir = tempfile.mkdtemp(prefix="ads-", dir=tempfile.gettempdir())
        self.token = token
        self.bridge = Bridge(self.socket_path, token)

    def shutdown(self):
        if self.bridge:
            try:
                if self.proc is not None and not self.args.keep:
                    self.bridge.request({"cmd": "disconnect"}, allow_fail=True)
            except Exception:
                pass
            self.bridge.close()
        if self.proc is not None and not self.args.keep:
            # Kill exactly the process we launched -- never by name or pattern.
            try:
                os.kill(self.pid, signal.SIGTERM)
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.kill(self.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
            except ProcessLookupError:
                pass
            print("stopped AetherSDR pid %d" % self.pid)
            try:
                os.remove(self.socket_path)  # SIGTERM skips the server's cleanup
            except OSError:
                pass
        elif self.proc is not None:
            print("left AetherSDR pid %d running (--keep); socket %s"
                  % (self.pid, self.socket_path))
        if not self.args.keep:
            self.hypr_rule_off()
        if self.workdir and not self.args.keep_workdir and not self.args.keep:
            shutil.rmtree(self.workdir, ignore_errors=True)

    # -- radio -------------------------------------------------------------

    def serial_label(self):
        # A real radio's serial identifies the operator: never print it.
        return self.serial if self.serial == DEMO_SERIAL else "the --radio-serial radio"

    def assert_radio(self):
        """Abort (after disconnecting) if the app is on any radio other than
        the one this run is for."""
        radio = self.bridge.request({"cmd": "get", "model": "radio"})["radio"]
        if radio.get("connected") and radio.get("serial") != self.serial:
            self.bridge.request({"cmd": "disconnect"}, allow_fail=True)
            raise SystemExit("ABORT: connected to a radio (%s) that is not %s -- disconnected"
                             % (radio.get("model"), self.serial_label()))
        return radio

    def connect_radio(self):
        radio = self.assert_radio()
        if radio.get("connected"):
            return
        deadline = time.time() + 20
        while True:
            listing = self.bridge.request({"cmd": "connect", "action": "list"})
            serials = [r.get("serial") for r in listing.get("radios", [])]
            if self.serial in serials or time.time() > deadline:
                break
            time.sleep(1)
        if self.serial not in serials:
            raise SystemExit("ABORT: %s is not in the discovery list (%d radios); %s"
                             % (self.serial_label(), len(serials),
                                "is the demo simulator hidden in this settings profile?"
                                if self.serial == DEMO_SERIAL else
                                "is the radio on, and was --no-netns given?"))
        # Raw wire form: selector "serial" is honoured (never "first").
        self.bridge.request({"cmd": "connect", "action": "local",
                             "value": "serial " + self.serial})
        self.bridge.request({"cmd": "connect", "action": "wait", "value": "30000"})
        radio = self.assert_radio()
        if not radio.get("connected"):
            raise SystemExit("ABORT: connect to %s did not complete" % self.serial_label())
        print("connected to %s (%s)" % (self.serial_label(), radio.get("model")))

    # -- steps -------------------------------------------------------------

    def tree(self):
        return self.bridge.request({"cmd": "dumpTree"})

    def run_step(self, step):
        if "cmd" in step:
            req = {k: v for k, v in step.items() if not k.startswith("_")}
            if req["cmd"] in ("key", "cwx", "txtest", "atu", "transmit", "testtone"):
                raise BridgeError("refusing transmit verb %r" % req["cmd"])
            self.bridge.request(req, allow_fail=step.get("_allow_fail", False))
            if step.get("_sleep"):
                time.sleep(step["_sleep"])
            return
        if "sleep" in step:
            time.sleep(step["sleep"])
            return
        if "click_text" in step:
            self.click_text(step)
            return
        if "wait_visible" in step:
            self.wait_visible(step["wait_visible"], step.get("timeout", 8))
            return
        if "wait_floors" in step:
            self.wait_floors(step.get("timeout", 20))
            return
        if "wait_slice" in step:
            self.wait_slice(step.get("timeout", 20))
            return
        if "close_windows" in step:
            self.close_windows()
            return
        if "remove_extra_slices" in step:
            self.remove_extra_slices()
            return
        if "notch_snapshot" in step:
            self.notches_before = {n["id"] for n in self.notches()}
            return
        if "notch_remove_new" in step:
            self.remove_new_notches()
            return
        raise BridgeError("unknown step %r" % step)

    def remove_extra_slices(self):
        """Undo a shot's extra slices (split, +RX): keep the lowest-numbered
        slice this client owns, remove the rest."""
        slices = self.bridge.request({"cmd": "get", "model": "slices"}).get("slices", [])
        ids = sorted(sl["sliceId"] for sl in slices)
        for sid in ids[1:]:
            self.bridge.request({"cmd": "slice", "action": "remove", "value": str(sid)},
                                allow_fail=True)
            time.sleep(1.0)

    def notches(self):
        return self.bridge.request({"cmd": "notch", "action": "list"}).get("notches", [])

    def remove_new_notches(self):
        """Remove only the notches a shot added -- never one that was there
        before (the operator's own, permanent ones included)."""
        before = getattr(self, "notches_before", None)
        if before is None:
            raise BridgeError("notch_remove_new without notch_snapshot")
        for n in self.notches():
            if n["id"] not in before and not n.get("permanent"):
                self.bridge.request({"cmd": "notch", "action": "remove", "value": str(n["id"])})
                time.sleep(0.5)

    def click_text(self, step):
        """Click a visible push button by its text, located through dumpTree and
        clicked target-locally (global clickAt has no screen offscreen)."""
        text, scope = step["click_text"], step.get("scope")
        tree = self.tree()
        node, anc = find_button_by_text(tree, text, scope)
        if node is None:
            if step.get("optional"):
                return
            raise BridgeError("no visible button with text %r under %r" % (text, scope))
        if "checked" in step and bool(node.get("checked")) == bool(step["checked"]):
            return
        frame = anc[0]
        for a in anc:
            if scope and node_matches(a, scope):
                frame = a
        g, fg = node["geometry"], frame["geometry"]
        lx = g["x"] - fg["x"] + g["w"] // 2
        ly = g["y"] - fg["y"] + g["h"] // 2
        target = frame.get("objectName") or short_class(frame)
        self.bridge.request({"cmd": "clickAt", "target": target, "value": "%d %d" % (lx, ly)})
        time.sleep(step.get("_sleep", 0.6))

    def wait_visible(self, name, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_node(self.tree(), name, predicate=lambda n: n.get("visible")):
                return
            time.sleep(0.3)
        raise BridgeError("%r never became visible" % name)

    def wait_slice(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            radio = self.bridge.request({"cmd": "get", "model": "radio"})["radio"]
            if radio.get("sliceCount", 0) >= 1 and radio.get("panCount", 0) >= 1:
                return
            time.sleep(0.5)
        raise BridgeError("the demo never created its slice and panadapter")

    def wait_floors(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = self.bridge.request({"cmd": "floors"}, allow_fail=True)
            for f in r.get("floors", []):
                v = f.get("noiseFloorDbm")
                if isinstance(v, (int, float)) and math.isfinite(v) and v < 0:
                    return
            time.sleep(0.5)
        raise BridgeError("panadapter never produced FFT frames")

    def close_windows(self):
        tree = self.tree()
        for win in visible_windows(tree):
            cls = short_class(win)
            if cls in ("MainWindow", "QComboBoxPrivateContainer"):
                continue
            if cls == "ConnectionPanel":
                self.bridge.request({"cmd": "connect", "action": "hide"}, allow_fail=True)
                continue
            if cls == "QMenu":
                continue
            target = win.get("objectName") or cls
            self.bridge.request({"cmd": "close", "target": target}, allow_fail=True)
        time.sleep(0.5)

    # -- shots -------------------------------------------------------------

    def grab_frame(self, tree, shot):
        """The dumpTree node whose rectangle the grab covers: crop/redact
        coordinates are relative to it (logical pixels)."""
        name = (shot.get("crop") or {}).get("frame") or shot.get("frame")
        grab = shot["grab"]
        if not name and grab.get("target") in ("pan-visible", "pan-composite"):
            want = int(grab.get("selector", 0))
            for root in tree.get("roots", []):
                for node, _ in iter_nodes(root):
                    if short_class(node) != "PanadapterApplet" or not node.get("visible"):
                        continue
                    if any(n.get("panIndex") == want for n, _ in iter_nodes(node)):
                        return node
            name = "PanadapterApplet"
        frame = find_node(tree, name or grab["target"])
        if frame is None:
            raise BridgeError("frame %r not found in dumpTree" % (name or grab["target"]))
        return frame

    def crop_box(self, shot, raw_w, raw_h, tree, dpr):
        """Crop rectangle in device pixels, or None. Boxes and widget unions
        are given in logical pixels and scaled by the grab's device pixel
        ratio."""
        crop = shot.get("crop")
        if not crop:
            return None
        if "box" in crop:
            x, y, w, h = crop["box"]
            lw, lh = raw_w / dpr, raw_h / dpr
            w = lw - x if w in (None, 0, -1) else w
            h = lh - y if h in (None, 0, -1) else h
            x0, y0 = max(0, round(x * dpr)), max(0, round(y * dpr))
            return (x0, y0, min(raw_w - x0, round(w * dpr)), min(raw_h - y0, round(h * dpr)))
        frame = find_node(tree, crop["frame"]) if crop.get("frame") else self.grab_frame(tree, shot)
        if frame is None:
            raise BridgeError("crop frame %r not found" % crop["frame"])
        fg = frame["geometry"]
        xs, ys, xe, ye = [], [], [], []
        for name in crop["union"]:
            n = resolve_spec(tree, name, scope=crop.get("scope"))
            if n is None or not n.get("visible"):
                raise BridgeError("crop widget %r not found/visible" % name)
            g = n["geometry"]
            xs.append(g["x"] - fg["x"]); ys.append(g["y"] - fg["y"])
            xe.append(g["x"] - fg["x"] + g["w"]); ye.append(g["y"] - fg["y"] + g["h"])
        pad = crop.get("pad", 0)
        lw, lh = raw_w / dpr, raw_h / dpr
        x0 = max(0, min(xs) - crop.get("pad_left", pad))
        y0 = max(0, min(ys) - crop.get("pad_top", pad))
        x1 = min(lw, max(xe) + crop.get("pad_right", pad))
        y1 = min(lh, max(ye) + crop.get("pad_bottom", pad))
        if crop.get("min_w"):
            x1 = min(lw, max(x1, x0 + crop["min_w"]))
        if crop.get("min_h"):
            y1 = min(lh, max(y1, y0 + crop["min_h"]))
        if crop.get("max_w"):
            x1 = min(x1, x0 + crop["max_w"])
        if crop.get("max_h"):
            y1 = min(y1, y0 + crop["max_h"])
        x0, y0 = round(x0 * dpr), round(y0 * dpr)
        return x0, y0, round(x1 * dpr) - x0, round(y1 * dpr) - y0

    # -- redaction ---------------------------------------------------------

    def is_sensitive(self, text):
        if not text or not isinstance(text, str):
            return False
        if (has_private_ipv4(text) or MAC_RE.search(text) or looks_like_ipv6(text)
                or LATLON_RE.search(text) or GRID6_RE.search(text)
                or TAILNET_RE.search(text)):
            return True
        return any(sec in text for sec in self.secrets)

    def redact_rects(self, tree, shot, frame, box, dpr):
        """Rectangles (device pixels, relative to the cropped image) to fill
        black: every visible widget in the grabbed window whose text names an
        IP or MAC address, the radio's serial, the bridge token, the scratch
        paths or the host name; the widgets named in "redact_widgets"; and the
        manual "redact" boxes (logical pixels, relative to the cropped image)
        for text that is painted rather than held by a widget."""
        fg = frame["geometry"]
        ox, oy = (box[0], box[1]) if box else (0, 0)
        rects, found = [], []
        named = shot.get("redact_widgets", [])

        def add(g, why):
            x = round((g["x"] - fg["x"]) * dpr) - ox
            y = round((g["y"] - fg["y"]) * dpr) - oy
            rects.append((x, y, round(g["w"] * dpr), round(g["h"] * dpr)))
            found.append(why)

        # "widget_redact": false skips the whole-widget pass for a shot whose
        # sensitive text shares a widget with text worth keeping (a list row
        # with a name above its address); its manual boxes and the OCR pass
        # still apply, and the OCR check still verifies the result.
        for root in ([frame] if shot.get("widget_redact", True) else []):
            for node, anc in iter_nodes(root):
                if not node.get("visible") or "geometry" not in node:
                    continue
                if any(not a.get("visible") for a in anc):
                    continue
                texts = [node.get(k) for k in ("text", "value", "accessibleName", "title")]
                if any(self.is_sensitive(t) for t in texts):
                    # A child is painted only inside its ancestors within the
                    # grab (a widget scrolled out of a scroll area keeps its
                    # geometry); the grab itself renders its target whole.
                    g = dict(node["geometry"])
                    for a in anc:
                        ag = a.get("geometry")
                        if not ag:
                            continue
                        x0, y0 = max(g["x"], ag["x"]), max(g["y"], ag["y"])
                        x1 = min(g["x"] + g["w"], ag["x"] + ag["w"])
                        y1 = min(g["y"] + g["h"], ag["y"] + ag["h"])
                        g = {"x": x0, "y": y0, "w": max(0, x1 - x0), "h": max(0, y1 - y0)}
                    if g["w"] and g["h"]:
                        # Log the class only: an objectName can embed the
                        # very serial or address being hidden.
                        add(g, short_class(node))
        for spec in named:
            n = resolve_spec(tree, spec)
            if n is None:
                raise BridgeError("redact widget %r not found" % spec)
            add(n["geometry"], "named %s" % short_class(n))
        for x, y, w, h in shot.get("redact", []):
            rects.append((round(x * dpr), round(y * dpr), round(w * dpr), round(h * dpr)))
            found.append("manual box")
        return rects, found

    def apply_redaction(self, path, rects):
        if not rects:
            return
        tool = magick_bin()
        if not tool:
            raise BridgeError("ImageMagick is required to redact %s" % path)
        args = [tool, path, "-fill", "black"]
        for x, y, w, h in rects:
            args += ["-draw", "rectangle %d,%d %d,%d" % (x, y, x + w - 1, y + h - 1)]
        subprocess.run(args + [path], check=True)

    def ocr_sensitive(self, text):
        # OCR garbles text (light on a highlight, small print), so a word that
        # merely starts like a private, link-local or CGNAT (tailnet) address
        # counts too.
        return self.is_sensitive(text) or bool(OCR_PRIVATE_PREFIX.search(text))

    def ocr_rects(self, path):
        """Text that no widget holds -- list and table items, painted labels
        -- is found by OCR: black out each word (or, for a match that spans
        words, such as coordinates, the whole line) that reads as PII. OCR
        runs on the image as is and on an enlarged, inverted greyscale copy,
        which reads light-on-dark and highlighted text far better."""
        tool = magick_bin()
        if not shutil.which("tesseract") or not tool:
            return []
        variants = [(path, 1.0)]
        enhanced = path + ".ocr.png"
        subprocess.run([tool, path, "-colorspace", "Gray", "-negate", "-resize", "200%",
                        enhanced], check=True)
        variants.append((enhanced, 2.0))
        rects = []
        for src, scale in variants:
            r = subprocess.run(["tesseract", src, "-", "--psm", "11", "tsv"],
                               capture_output=True, text=True)
            lines = {}
            for row in r.stdout.splitlines()[1:]:
                f = row.split("\t")
                if len(f) < 12 or not f[11].strip():
                    continue
                key = (f[2], f[3], f[4])
                lines.setdefault(key, []).append(
                    (int(f[6]), int(f[7]), int(f[8]), int(f[9]), f[11]))
            for words in lines.values():
                if not self.ocr_sensitive(" ".join(w[4] for w in words)):
                    continue
                idx = [i for i, w in enumerate(words) if self.ocr_sensitive(w[4])]
                if not idx:
                    idx = list(range(len(words)))
                # OCR may split an address ("192.0.2." + "40"): take the
                # number-like words that follow a hit too.
                more = set(idx)
                for i in idx:
                    j = i + 1
                    while j < len(words) and re.match(r"^[\w.:/-]*\d[\w.:/-]*$", words[j][4]):
                        more.add(j)
                        j += 1
                hits = [words[i] for i in sorted(more)]
                for x, y, w, h, _ in hits:
                    rects.append((round(x / scale) - 4, round(y / scale) - 4,
                                  round(w / scale) + 8, round(h / scale) + 8))
        os.remove(enhanced)
        return rects

    def ocr_check(self, path):
        """Second line of defence: OCR the finished image and report anything
        that still reads like an address, the serial or the token."""
        if not shutil.which("tesseract"):
            return []
        r = subprocess.run(["tesseract", path, "-", "--psm", "11"],
                           capture_output=True, text=True)
        hits = []
        for line in r.stdout.splitlines():
            if self.ocr_sensitive(line):
                hits.append(line.strip())
        return hits

    def shoot(self, shot, out_dir):
        sid = shot["id"]
        for key in ("page", "grab", "caption", "alt"):
            if not shot.get(key):
                raise BridgeError("manifest entry %r is missing %r" % (sid, key))
        for step in self.manifest.get(self.key("before_each"), []):
            self.run_step(step)
        self.assert_radio()
        for step in shot.get("steps", []):
            self.run_step(step)
        time.sleep(shot.get("settle", self.manifest.get("settle", 0.8)))
        raw = os.path.join(self.workdir, "raw-%s.png" % sid)
        req = {"cmd": "grab", "path": raw}
        req.update(shot["grab"])
        resp = self.bridge.request(req)
        w, h = resp.get("width", 0), resp.get("height", 0)
        if w < 32 or h < 16:
            raise BridgeError("grab is implausibly small (%dx%d)" % (w, h))
        dst = os.path.join(out_dir, sid + ".png")
        tree = self.tree()
        frame = self.grab_frame(tree, shot)
        dpr = w / max(1, frame["geometry"]["w"])
        box = self.crop_box(shot, w, h, tree, dpr)
        if box:
            crop_png(raw, dst, *box)
            w, h = box[2], box[3]
        else:
            shutil.copyfile(raw, dst)
        rects, why = self.redact_rects(tree, shot, frame, box, dpr)
        rects = [r for r in rects if r[0] < w and r[1] < h and r[0] + r[2] > 0 and r[1] + r[3] > 0]
        self.apply_redaction(dst, rects)
        if not shot.get("no_ocr_redact"):
            extra = self.ocr_rects(dst)
            self.apply_redaction(dst, extra)
            rects += extra
            why += ["OCR text"] * len(extra)
        if rects:
            print("    redacted %d region(s): %s" % (len(rects), ", ".join(sorted(set(why)))))
        hits = self.ocr_check(dst)
        if hits:
            self.pii_hits.append((sid, hits))
            print("    ! OCR still reads possible PII in %s: %d line(s)" % (sid, len(hits)))
        if not self.args.no_optimize:
            optimise_png(dst, quantize=shot.get("quantize", self.args.quantize))
        for step in shot.get("cleanup", []):
            try:
                self.run_step(step)
            except BridgeError as e:
                print("    ! cleanup: %s" % e)
        return dst, w, h

    def run(self):
        out_dir = os.path.abspath(self.args.out)
        os.makedirs(out_dir, exist_ok=True)
        only = set(self.args.only.split(",")) if self.args.only else None
        everything = self.manifest["shots"]
        if only:
            missing = only - {s["id"] for s in everything}
            if missing:
                sys.exit("error: unknown shot id(s): %s" % ", ".join(sorted(missing)))
        # Popup menus need a focused window on Wayland, so shots tagged
        # "offscreen" run in a separate --platform offscreen pass.
        offscreen = self.args.platform == "offscreen"
        shots = [s for s in everything if (not only or s["id"] in only)
                 and (s.get("radio") == "demo") == self.demo
                 and bool(s.get("offscreen")) == offscreen]
        if not shots:
            sys.exit("error: no %s %s shots selected (shots tagged \"radio\": \"demo\" run "
                     "without --radio-serial, all others with it; shots tagged "
                     "\"offscreen\" run only with --platform offscreen)"
                     % ("offscreen" if offscreen else "on-screen",
                        "demo" if self.demo else "real-radio"))
        if self.args.socket:
            self.attach()
        else:
            self.launch()
        self.secrets = [x for x in (None if self.demo else self.serial, self.token,
                                    self.workdir, os.path.expanduser("~"),
                                    socket.gethostname()) if x and len(x) >= 4]
        results, failures = [], []
        try:
            ping = self.bridge.request({"cmd": "ping"})
            print("bridge: %s %s (%s)" % (ping.get("app"), ping.get("version"),
                                          (ping.get("build") or {}).get("describe")))
            self.connect_radio()
            if not self.args.skip_setup:
                for step in self.manifest.get(self.key("setup"), []):
                    self.run_step(step)
            self.assert_radio()
            for shot in shots:
                print("- %s" % shot["id"])
                try:
                    path, w, h = self.shoot(shot, out_dir)
                    size = os.path.getsize(path)
                    results.append((shot["id"], w, h, size))
                    print("    %s %dx%d %.0f KiB" % (os.path.relpath(path, REPO), w, h, size / 1024))
                except (BridgeError, subprocess.CalledProcessError) as e:
                    failures.append((shot["id"], str(e)))
                    print("    FAILED: %s" % e)
                    try:
                        self.close_windows()
                    except Exception:
                        pass
            self.assert_radio()
        finally:
            self.shutdown()
        total = sum(r[3] for r in results)
        print("\n%d captured, %d failed, %.2f MiB total" % (len(results), len(failures),
                                                           total / (1024 * 1024)))
        for sid, err in failures:
            print("  FAILED %s: %s" % (sid, err))
        for sid, hits in self.pii_hits:
            # The lines themselves may hold the very text being hidden.
            print("  CHECK %s: OCR found %d line(s) that look like an address, serial or "
                  "token -- inspect the image and add a redact box" % (sid, len(hits)))
        return 1 if (failures or self.pii_hits) and self.args.strict else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build-docs"),
                    help="CMake build dir holding the AetherSDR binary (default: build-docs)")
    ap.add_argument("--binary", help="explicit AetherSDR binary (skips the build-dir checks)")
    ap.add_argument("--socket", help="attach to an already-running bridge instead of launching")
    ap.add_argument("--token", help="bridge token: for --socket (default: $AETHER_MCP_TOKEN), "
                    "or for the launched instance (default: random)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--only", help="comma-separated shot ids")
    ap.add_argument("--platform", default="offscreen", help="QT_QPA_PLATFORM (default offscreen)")
    ap.add_argument("--allow-gpu-build", action="store_true")
    ap.add_argument("--dpr", type=float, default=1.0,
                    help="device pixel ratio of the offscreen screen (match the on-screen "
                         "pass, e.g. 1.6667, so popups are as sharp as the other shots)")
    ap.add_argument("--wayland-display", help="WAYLAND_DISPLAY for --platform wayland")
    ap.add_argument("--hypr-workspace", type=int,
                    help="Hyprland: open the app's windows silently on this workspace")
    ap.add_argument("--radio-serial",
                    help="run the real-radio shots against the radio with this serial "
                         "(needs --no-netns); without it, the demo shots run on DEMO-0001")
    ap.add_argument("--skip-setup", action="store_true",
                    help="with --socket: do not replay the manifest setup steps")
    ap.add_argument("--no-netns", action="store_true",
                    help="do not isolate the app in a private network namespace (Linux)")
    ap.add_argument("--no-optimize", action="store_true")
    ap.add_argument("--quantize", action="store_true",
                    help="lossy 256-colour quantization for every shot (smaller files)")
    ap.add_argument("--keep", action="store_true", help="leave the launched app running")
    ap.add_argument("--keep-workdir", action="store_true")
    ap.add_argument("--strict", action="store_true", help="exit non-zero if any shot fails")
    args = ap.parse_args()
    with open(args.manifest) as f:
        manifest = json.load(f)
    sys.exit(Capture(args, manifest).run())


if __name__ == "__main__":
    main()
