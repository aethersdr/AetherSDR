#!/usr/bin/env python3
"""Capture the user-docs screenshots from a scripted AetherSDR demo session.

Every shot is declared in ``docs/user/screens.json``: the page it belongs to,
the automation-bridge steps that put the app into the right state, the widget
to grab, an optional crop, a caption and (mandatory) alt text. This script
launches a private AetherSDR instance against the built-in DEMO simulator,
drives it over the automation bridge's raw unix-socket JSON protocol
(docs/automation-bridge.md -- no MCP client needed), grabs each widget to PNG
and writes the results to ``docs/user/static/img/screens/<id>.png``.

Re-run it whenever the UI changes (Linux; the build only needs the app target):

    cmake -S . -B build-docs -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DAETHER_GPU_SPECTRUM=OFF
    cmake --build build-docs --target AetherSDR
    python3 tools/docs/capture_screenshots.py --build-dir build-docs

``--only id1,id2`` re-shoots a subset; ``--socket``/``--token`` attach to an
instance you launched yourself (it must already be on, or able to reach,
DEMO-0001).

Rendering. Under ``QT_QPA_PLATFORM=offscreen`` there is no GL context, so the
default QRhi (GPU) panadapter cannot render and shows "Spectrum renderer
unavailable" -- ``AETHER_NO_GPU=1`` does not help offscreen. Build the app with
``-DAETHER_GPU_SPECTRUM=OFF`` (the QPainter spectrum/waterfall) for docs
captures; the script refuses a GPU build unless ``--allow-gpu-build`` is given
(e.g. when a real X server is available via ``--platform xcb``).

Isolation and safety. The app is launched with a fresh settings directory,
``AETHER_AUTOMATION_NO_TX=1``, a random bridge token and an explicit socket.
On Linux it also runs in a private network + UTS namespace
(``unshare -n -u --map-current-user``): no real radio on the LAN can be
discovered or reached, discovered stations' callsigns cannot leak into the
title bar or the Connect dialog, and the hostname shown as the station name is
fixed. The script connects only to serial DEMO-0001 and aborts (after
disconnecting) if the radio it lands on reports any other serial. It never
sends a transmit verb. On exit it kills only the PID it launched.

Requirements: Python 3.8+, an AetherSDR binary; ImageMagick (``magick``) for
crops and PNG recompression (without it, crops are skipped and PNGs are left
as Qt wrote them). ``oxipng``/``pngquant`` are used when present.
"""

import argparse
import json
import math
import os
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


def find_button_by_text(tree, text, scope=None):
    for root in tree.get("roots", []):
        for node, anc in iter_nodes(root):
            if not node.get("visible") or node.get("text") != text:
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
        self.proc = None
        self.pid = None
        self.bridge = None
        self.workdir = None

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
        self.token = secrets.token_hex(16)
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
        if self.args.platform != "offscreen" and os.environ.get("DISPLAY"):
            env["DISPLAY"] = os.environ["DISPLAY"]
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

    def attach(self):
        token = self.args.token or os.environ.get("AETHER_MCP_TOKEN")
        self.socket_path = self.args.socket
        self.workdir = tempfile.mkdtemp(prefix="ads-", dir=tempfile.gettempdir())
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
        if self.workdir and not self.args.keep_workdir and not self.args.keep:
            shutil.rmtree(self.workdir, ignore_errors=True)

    # -- radio -------------------------------------------------------------

    def assert_demo(self):
        radio = self.bridge.request({"cmd": "get", "model": "radio"})["radio"]
        if radio.get("connected") and radio.get("serial") != DEMO_SERIAL:
            self.bridge.request({"cmd": "disconnect"}, allow_fail=True)
            raise SystemExit("ABORT: connected radio serial is %r, not %s -- disconnected"
                             % (radio.get("serial"), DEMO_SERIAL))
        return radio

    def connect_demo(self):
        radio = self.assert_demo()
        if radio.get("connected"):
            return
        listing = self.bridge.request({"cmd": "connect", "action": "list"})
        serials = [r.get("serial") for r in listing.get("radios", [])]
        if DEMO_SERIAL not in serials:
            raise SystemExit("ABORT: %s is not in the discovery list (%s); is the demo "
                             "simulator hidden in this settings profile?" % (DEMO_SERIAL, serials))
        # Raw wire form: selector "serial" is honoured (never "first").
        self.bridge.request({"cmd": "connect", "action": "local",
                             "value": "serial " + DEMO_SERIAL})
        self.bridge.request({"cmd": "connect", "action": "wait", "value": "30000"})
        radio = self.assert_demo()
        if not radio.get("connected"):
            raise SystemExit("ABORT: demo connect did not complete")
        print("connected to %s (%s)" % (radio.get("serial"), radio.get("model")))

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
        raise BridgeError("unknown step %r" % step)

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

    def crop_box(self, shot, raw_w, raw_h):
        crop = shot.get("crop")
        if not crop:
            return None
        if "box" in crop:
            x, y, w, h = crop["box"]
            w = raw_w - x if w in (None, 0, -1) else w
            h = raw_h - y if h in (None, 0, -1) else h
            return x, y, w, h
        tree = self.tree()
        frame = find_node(tree, crop["frame"])
        if frame is None:
            raise BridgeError("crop frame %r not found" % crop["frame"])
        fg = frame["geometry"]
        xs, ys, xe, ye = [], [], [], []
        for name in crop["union"]:
            n = find_node(tree, name, scope=crop.get("scope"))
            if n is None or not n.get("visible"):
                raise BridgeError("crop widget %r not found/visible" % name)
            g = n["geometry"]
            xs.append(g["x"] - fg["x"]); ys.append(g["y"] - fg["y"])
            xe.append(g["x"] - fg["x"] + g["w"]); ye.append(g["y"] - fg["y"] + g["h"])
        pad = crop.get("pad", 0)
        x0, y0 = max(0, min(xs) - pad), max(0, min(ys) - pad)
        x1, y1 = min(raw_w, max(xe) + pad), min(raw_h, max(ye) + pad)
        if crop.get("min_w"):
            x1 = min(raw_w, max(x1, x0 + crop["min_w"]))
        if crop.get("min_h"):
            y1 = min(raw_h, max(y1, y0 + crop["min_h"]))
        return x0, y0, x1 - x0, y1 - y0

    def shoot(self, shot, out_dir):
        sid = shot["id"]
        for key in ("page", "grab", "caption", "alt"):
            if not shot.get(key):
                raise BridgeError("manifest entry %r is missing %r" % (sid, key))
        for step in self.manifest.get("before_each", []):
            self.run_step(step)
        self.assert_demo()
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
        box = self.crop_box(shot, w, h)
        if box:
            crop_png(raw, dst, *box)
            w, h = box[2], box[3]
        else:
            shutil.copyfile(raw, dst)
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
        shots = [s for s in self.manifest["shots"] if not only or s["id"] in only]
        if only and len(shots) != len(only):
            missing = only - {s["id"] for s in shots}
            sys.exit("error: unknown shot id(s): %s" % ", ".join(sorted(missing)))
        if self.args.socket:
            self.attach()
        else:
            self.launch()
        results, failures = [], []
        try:
            ping = self.bridge.request({"cmd": "ping"})
            print("bridge: %s %s (%s)" % (ping.get("app"), ping.get("version"),
                                          (ping.get("build") or {}).get("describe")))
            self.connect_demo()
            for step in self.manifest.get("setup", []):
                self.run_step(step)
            self.assert_demo()
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
            self.assert_demo()
        finally:
            self.shutdown()
        total = sum(r[3] for r in results)
        print("\n%d captured, %d failed, %.2f MiB total" % (len(results), len(failures),
                                                           total / (1024 * 1024)))
        for sid, err in failures:
            print("  FAILED %s: %s" % (sid, err))
        return 1 if failures and self.args.strict else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build-docs"),
                    help="CMake build dir holding the AetherSDR binary (default: build-docs)")
    ap.add_argument("--binary", help="explicit AetherSDR binary (skips the build-dir checks)")
    ap.add_argument("--socket", help="attach to an already-running bridge instead of launching")
    ap.add_argument("--token", help="bridge token for --socket (default: $AETHER_MCP_TOKEN)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--only", help="comma-separated shot ids")
    ap.add_argument("--platform", default="offscreen", help="QT_QPA_PLATFORM (default offscreen)")
    ap.add_argument("--allow-gpu-build", action="store_true")
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
