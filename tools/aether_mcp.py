#!/usr/bin/env python3
"""
AetherSDR automation-bridge MCP server (issue #3646 follow-on).

Wraps the in-app automation bridge (src/core/AutomationServer.*) in the
Model Context Protocol so ANY MCP-capable AI coding assistant — Claude
Code, Cursor, Copilot Workspace, Codex CLI, Gemini CLI, … — can drive a
locally-running AetherSDR to validate a change end-to-end: dump the
widget tree, click controls, assert model state, capture screenshots.

Zero third-party deps, stdio transport, newline-delimited JSON-RPC.
Talks to the bridge over its AF_UNIX socket (macOS/Linux) or named pipe
(Windows), discovered from <temp>/aethersdr-automation.json exactly like
tools/automation_probe.py.

Setup (contributor):
    AETHER_AUTOMATION=1 ./AetherSDR &     # launch the app, bridge on
    # then register this server with your assistant, e.g. Claude Code:
    #   the repo's .mcp.json does it automatically, or:
    #   claude mcp add aethersdr -- python3 tools/aether_mcp.py

Safety: the bridge refuses transmit-keying controls (MOX/PTT/TUNE/ATU/
CWX send) unless the operator launches the app with
AETHER_AUTOMATION_ALLOW_TX — that gate lives in the app, not here, so
no MCP client can key a live radio by accident.

Auth: if the operator set an access token in Radio Setup → Network, the
bridge rejects every verb (except ping) without a matching token. Put
that token in AETHER_MCP_TOKEN and this server attaches it to every
request. Without the right token the app cannot be driven — that's what
stops a random local agent from touching the radio.

Env:
    AETHER_MCP_SOCKET   override the bridge socket path (else discovery)
    AETHER_MCP_TOKEN    access token from Radio Setup → Network (if set)
"""

import atexit
import base64
import difflib
import glob
import json
import os
import re
import secrets
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "aethersdr-automation", "version": "1.0.0"}
REQUEST_TIMEOUT_S = 60
MAX_INLINE_IMAGE_BYTES = 800_000  # larger grabs are returned as a path only

_gesture_bridge = None
_gesture_socket_path = None
_owned_app = None


# --------------------------------------------------------------------------
# Bridge client (mirrors tools/automation_probe.py)
# --------------------------------------------------------------------------

def discover_sockets():
    """All live bridge endpoints, newest first.

    The app writes <temp>/aethersdr-automation.json (latest instance) and
    <temp>/aethersdr-automation/<pid>.json (one per instance).
    """
    tmp = tempfile.gettempdir()
    entries = []
    for path in ([os.path.join(tmp, "aethersdr-automation.json")]
                 + glob.glob(os.path.join(tmp, "aethersdr-automation", "*.json"))):
        try:
            with open(path) as f:
                d = json.load(f)
            if d.get("socket"):
                entries.append(d)
        except (OSError, ValueError):
            continue
    seen, unique = set(), []
    for d in sorted(entries, key=lambda d: d.get("startedAt", 0), reverse=True):
        if d["socket"] not in seen:
            seen.add(d["socket"])
            unique.append(d)
    return unique


def bridge_socket_path():
    if _owned_app is not None and _owned_app["process"].poll() is None:
        return _owned_app["socket"]
    override = os.environ.get("AETHER_MCP_SOCKET")
    if override:
        return override
    entries = discover_sockets()
    return entries[0]["socket"] if entries else None


class Bridge:
    """One connection to the bridge; newline-delimited JSON both ways."""

    def __init__(self, sock_path, timeout=REQUEST_TIMEOUT_S):
        self.sock_path = sock_path
        self._buf = b""
        if sys.platform == "win32":
            self._pipe = open(rf"\\.\pipe\{os.path.basename(sock_path)}",
                              "r+b", buffering=0)
            self._sock = None
        else:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(timeout)
            self._sock.connect(sock_path)
            self._pipe = None

    def request(self, obj):
        line = (json.dumps(obj) + "\n").encode()
        if self._sock:
            self._sock.sendall(line)
            while b"\n" not in self._buf:
                chunk = self._sock.recv(65536)
                if not chunk:
                    raise ConnectionError("bridge closed the connection")
                self._buf += chunk
            line, _, self._buf = self._buf.partition(b"\n")
            return json.loads(line.decode())
        self._pipe.write(line)
        self._pipe.flush()
        return json.loads(self._pipe.readline().decode())

    def close(self):
        try:
            if self._sock:
                self._sock.close()
            if self._pipe:
                self._pipe.close()
        except OSError:
            pass  # best-effort teardown — a close failure is not actionable


def bridge_request(obj, timeout=REQUEST_TIMEOUT_S):
    """Connect, send one request (with the token, if set), return the response."""
    sock_path = bridge_socket_path()
    if not sock_path:
        raise RuntimeError(
            "No running AetherSDR automation bridge found. Launch the app "
            "with the AETHER_AUTOMATION=1 environment variable set (the "
            "bridge is off by default), then retry. Discovery file checked: "
            + os.path.join(tempfile.gettempdir(), "aethersdr-automation.json"))
    # Attach the access token to every request so the bridge's auth gate
    # accepts it. Harmless when the bridge has no token configured (the
    # field is simply ignored). `ping` doesn't need it but sending it is fine.
    obj = _authenticated_request(obj)
    b = Bridge(sock_path, timeout)
    try:
        return b.request(obj)
    finally:
        b.close()


def _authenticated_request(obj):
    """Copy a bridge request and attach the runtime token without logging it."""
    token = os.environ.get("AETHER_MCP_TOKEN")
    if token and "token" not in obj:
        return dict(obj, token=token)
    return obj


def _close_gesture_bridge():
    """Close the held gesture transport; app-side disconnect releases input."""
    global _gesture_bridge, _gesture_socket_path
    bridge = _gesture_bridge
    _gesture_bridge = None
    _gesture_socket_path = None
    if bridge is not None:
        bridge.close()


atexit.register(_close_gesture_bridge)
# --------------------------------------------------------------------------
# Secure fresh-build process handoff
# --------------------------------------------------------------------------

def _resolve_app_artifact(worktree):
    """Resolve only the canonical AetherSDR artifact under an absolute worktree."""
    if not worktree or not os.path.isabs(worktree):
        raise ValueError("`worktree` must be an absolute path")
    try:
        root = Path(worktree).resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"worktree could not be resolved: {exc}") from exc
    if not root.is_dir():
        raise ValueError("worktree is not a directory")
    if not (root / ".git").exists() or not (root / "AGENTS.md").is_file():
        raise ValueError("worktree is not an AetherSDR Git worktree")
    cmake_file = root / "CMakeLists.txt"
    try:
        cmake_head = cmake_file.read_text(encoding="utf-8")[:4096]
    except OSError as exc:
        raise ValueError(f"could not read worktree CMakeLists.txt: {exc}") from exc
    if "project(AetherSDR " not in cmake_head:
        raise ValueError("worktree CMakeLists.txt does not declare AetherSDR")

    if sys.platform == "darwin":
        executable = root / "build" / "AetherSDR.app" / "Contents" / "MacOS" / "AetherSDR"
    elif sys.platform == "win32":
        executable = root / "build" / "AetherSDR.exe"
    else:
        executable = root / "build" / "AetherSDR"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"fresh build artifact is missing or not executable: {executable}")
    return root, executable


def _instance_label(value):
    if value is None or not str(value).strip():
        return f"mcp-proof-{os.getpid()}-{secrets.token_hex(3)}"
    label = str(value).strip()
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,48}", label):
        raise ValueError("`label` must be 1-48 ASCII letters, digits, dot, underscore, or dash")
    return label


def _instance_socket():
    suffix = f"{os.getpid()}-{secrets.token_hex(4)}"
    if sys.platform == "win32":
        return f"aethersdr-mcp-{suffix}"
    # Keep the AF_UNIX path short enough for macOS even when TMPDIR is deeply
    # nested. /tmp is present on every supported Unix platform.
    return f"/tmp/aether-mcp-{suffix}.sock"


def _request_at(socket_path, obj, timeout=REQUEST_TIMEOUT_S, authenticated=True):
    bridge = Bridge(socket_path, timeout)
    try:
        request = _authenticated_request(obj) if authenticated else obj
        return bridge.request(request)
    finally:
        bridge.close()


def _wait_for_owned_app(instance, timeout=25):
    """Wait for exact authenticated identity; never accept discovery fallback."""
    deadline = time.monotonic() + timeout
    last_error = "bridge did not become ready"
    while time.monotonic() < deadline:
        return_code = instance["process"].poll()
        if return_code is not None:
            raise RuntimeError(f"AetherSDR exited before bridge startup (code {return_code})")
        try:
            ping = _request_at(instance["socket"], {"cmd": "ping"}, timeout=2,
                               authenticated=False)
            # A tokenless privileged call must be REFUSED, not merely advertised
            # as required — this demonstrates enforcement, not just the flag.
            tokenless = _request_at(instance["socket"], {"cmd": "whoami"},
                                    timeout=2, authenticated=False)
            whoami = _request_at(instance["socket"], {"cmd": "whoami"}, timeout=2)
        except (ConnectionError, FileNotFoundError, OSError, ValueError) as exc:
            last_error = str(exc)
            time.sleep(0.1)
            continue
        if not isinstance(ping, dict) or not ping.get("authRequired"):
            raise RuntimeError("fresh-build bridge did not require authentication")
        if isinstance(tokenless, dict) and tokenless.get("ok"):
            raise RuntimeError(
                "fresh-build bridge accepted a tokenless privileged command")
        if not isinstance(whoami, dict) or not whoami.get("ok"):
            raise RuntimeError("authenticated whoami failed")
        if int(whoami.get("pid", -1)) != instance["process"].pid:
            raise RuntimeError("authenticated bridge PID did not match the launched app")
        identity = whoami.get("automationIdentity") or whoami.get("socket")
        if identity != instance["socket"]:
            raise RuntimeError("authenticated bridge identity did not match the explicit socket")
        if whoami.get("label") != instance["label"]:
            raise RuntimeError("authenticated bridge label did not match the launched instance")
        if whoami.get("txAllowed") is not False:
            raise RuntimeError("fresh-build bridge did not keep TX automation disabled")
        return ping, whoami
    raise RuntimeError(last_error)


def _remove_owned_socket(instance):
    if sys.platform == "win32" or not instance:
        return
    socket_path = instance.get("socket", "")
    expected = rf"/tmp/aether-mcp-{os.getpid()}-[0-9a-f]{{8}}\.sock"
    if not re.fullmatch(expected, socket_path):
        return
    try:
        socket_stat = os.lstat(socket_path)
    except FileNotFoundError:
        return
    except OSError:
        return
    if not stat.S_ISSOCK(socket_stat.st_mode):
        return
    try:
        os.unlink(socket_path)
    except OSError:
        pass  # app-owned socket cleanup is best-effort after process exit


def _signal_owned_process(process, sig):
    """Signal the child's whole session, not just its PID.

    The child is launched with start_new_session=True, so it is a session
    leader (pgid == pid) and any helper subprocesses it spawns share that
    group; signalling the group reaps them too. Fall back to the single
    process if the group is already gone or on Windows (no POSIX groups)."""
    if sys.platform != "win32":
        try:
            os.killpg(os.getpgid(process.pid), sig)
            return
        except (ProcessLookupError, PermissionError, OSError):
            pass  # group already reaped, or racing exit — fall back below
    try:
        (process.kill if sig == signal.SIGKILL else process.terminate)()
    except OSError:
        pass


def _stop_owned_app():
    global _owned_app
    instance = _owned_app
    if instance is None:
        return {"ok": True, "running": False}
    process = instance["process"]
    if process.poll() is None:
        _signal_owned_process(process, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _signal_owned_process(process, signal.SIGKILL)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
    if process.poll() is None:
        return {
            "ok": False,
            "running": True,
            "pid": process.pid,
            "socket": instance["socket"],
            "label": instance["label"],
            "error": "AetherSDR did not exit after terminate and kill",
        }
    _owned_app = None
    _remove_owned_socket(instance)
    return {
        "ok": True,
        "running": False,
        "pid": process.pid,
        "socket": instance["socket"],
        "label": instance["label"],
        "exitCode": process.poll(),
    }


atexit.register(_stop_owned_app)


def _handle_shutdown_signal(signum, _frame):
    # atexit does NOT run when the process is killed by a signal, and an MCP
    # host shuts its server down with SIGTERM. Run the same cleanups atexit
    # would (reap the owned child so the app, its socket, and the shared
    # app/radio lock don't leak; release the held gesture transport), then die
    # with the default disposition so the exit status still reflects the signal.
    _stop_owned_app()
    _close_gesture_bridge()
    try:
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)
    except (OSError, ValueError):
        os._exit(128 + signum)


for _shutdown_signal in (signal.SIGTERM, signal.SIGINT):
    try:
        signal.signal(_shutdown_signal, _handle_shutdown_signal)
    except (OSError, ValueError, AttributeError):
        pass  # not the main thread, or the platform lacks this signal


# --------------------------------------------------------------------------
# Tree filtering (dumpTree responses are large; let callers prune)
# --------------------------------------------------------------------------

def prune_tree(node, needle):
    """Keep subtrees whose objectName/class/accessibleName/text matches."""
    if not isinstance(node, dict):
        return None
    hay = " ".join(str(node.get(k, "")) for k in
                   ("objectName", "class", "accessibleName", "text",
                    "title", "windowTitle", "value", "role")).lower()
    kids = node.get("children") or []
    kept = [c for c in (prune_tree(k, needle) for k in kids) if c]
    if needle in hay or kept:
        out = {k: v for k, v in node.items() if k != "children"}
        if kept:
            out["children"] = kept
        return out
    return None


# --------------------------------------------------------------------------
# MCP tools
# --------------------------------------------------------------------------

TOOLS = [
    {
        "name": "app_instance",
        "description": (
            "Securely launch, inspect, or stop one fresh local AetherSDR "
            "proof build owned by this MCP server. Launch accepts an absolute "
            "AetherSDR worktree and runs only its canonical build artifact, "
            "handing the existing AETHER_MCP_TOKEN to the child in memory. It "
            "uses a unique explicit socket/label, disables radio autoconnect, "
            "pins TX automation off, and succeeds only after authenticated "
            "whoami matches the child PID and identity. No token is returned, "
            "logged, passed on the command line, or written to settings."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string", "enum": ["launch", "status", "stop"]},
            "worktree": {"type": "string",
                         "description": "absolute AetherSDR worktree path for launch"},
            "label": {"type": "string",
                      "description": "optional safe instance label for launch"},
        }, "required": ["action"]},
    },
    {
        "name": "bridge_status",
        "description": (
            "Check the AetherSDR automation bridge: lists running "
            "instances (pid, socket, version, label) and pings the "
            "active one. Call this first — it tells you whether the app "
            "is running with AETHER_AUTOMATION=1 and which instance "
            "you're driving."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "dump_tree",
        "description": (
            "ARIA-style JSON snapshot of the live widget tree: "
            "objectName, class, accessibleName, role/value, enabled, "
            "visible, global geometry, windowState. THE way to find a "
            "control's target name before invoke/grab. Pass `filter` "
            "(case-insensitive substring of objectName/class/"
            "accessibleName/text) to prune the tree — full dumps are "
            "large; filter whenever you know roughly what you're "
            "looking for."),
        "inputSchema": {"type": "object", "properties": {
            "filter": {"type": "string",
                       "description": "substring to prune the tree to matching subtrees"},
        }},
    },
    {
        "name": "grab_widget",
        "description": (
            "Screenshot a single widget as PNG, resolved by objectName, "
            "class name, or accessibleName (use dump_tree to find it). "
            "GPU panadapter content is captured correctly via "
            "framebuffer readback. Special targets: 'pan', "
            "'pan-visible', 'pan-composite' with `selector` = pan "
            "index. Returns the image inline (and the saved file path)."),
        "inputSchema": {"type": "object", "properties": {
            "target": {"type": "string"},
            "selector": {"type": "string",
                         "description": "pan index, only for pan/pan-visible/pan-composite"},
            "path": {"type": "string",
                     "description": ("output PNG path; defaults to a temp file. The "
                                     "returned JSON's `path` is authoritative — the app "
                                     "may normalize it")},
        }, "required": ["target"]},
    },
    {
        "name": "invoke",
        "description": (
            "Drive a control deterministically: actions click, toggle, "
            "setChecked, setValue, setText, setCurrentText, "
            "setCurrentIndex, selectRow, submit (QLineEdit: setText + "
            "returnPressed), trigger (QAction/menu item, works while "
            "the menu is closed). Target = objectName / class / "
            "accessibleName from dump_tree. The bridge REFUSES "
            "transmit-keying controls (MOX/PTT/TUNE/ATU/CWX) unless the "
            "app was launched with AETHER_AUTOMATION_ALLOW_TX."),
        "inputSchema": {"type": "object", "properties": {
            "target": {"type": "string"},
            "action": {"type": "string"},
            "value": {"type": "string", "description": "for set* / submit actions"},
        }, "required": ["target", "action"]},
    },
    {
        "name": "get_state",
        "description": (
            "Live JSON snapshot of app/radio state — assert on state "
            "without screenshots. model = audio | dsp | radio | "
            "transmit | slice <selector: id|active|tx> | slices | pan "
            "<selector: id|active> | pans | flags | kiwi | sync | clock. "
            "Optional `property` returns just that field."),
        "inputSchema": {"type": "object", "properties": {
            "model": {"type": "string"},
            "selector": {"type": "string"},
            "property": {"type": "string"},
        }, "required": ["model"]},
    },
    {
        "name": "shortcut",
        "description": ("Invoke a registered ShortcutManager action by id, "
                        "without needing a physical key binding."),
        "inputSchema": {"type": "object", "properties": {
            "id": {"type": "string"},
        }, "required": ["id"]},
    },
    {
        "name": "get_log",
        "description": (
            "Tail recent app log events from the bridge's in-memory ring — "
            "the fastest way to see WHY an action didn't take (Qt warnings, "
            "category messages, your own `mark` annotations). Returns the "
            "newest `count` events; pass `since` (an event seq from a prior "
            "call) to fetch only newer ones for incremental polling."),
        "inputSchema": {"type": "object", "properties": {
            "count": {"type": "integer",
                      "description": "newest N events (default 100)"},
            "since": {"type": "integer",
                      "description": "only events with seq greater than this"},
        }},
    },
    {
        "name": "connect",
        "description": (
            "Drive the radio-connection lifecycle. action = list (discovered "
            "radios) | show / hide (the Connect dialog) | local (connect to a "
            "local radio — value 'first' or 'serial <serial>') | ip (connect "
            "by host/IP — value = host) | wait (block until connected — value "
            "= timeout in ms). Confirm with get_state model=radio."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["list", "show", "hide", "local", "ip", "wait"]},
            "value": {"type": "string",
                      "description": "'first' / 'serial N', a host/IP, or a timeout in ms"},
        }, "required": ["action"]},
    },
    {
        "name": "disconnect",
        "description": "Disconnect from the radio (the normal user disconnect path).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "capture_audio",
        "description": (
            "Capture RX audio from the engine's tap points for analysis. "
            "action = start (value = '<durationMs> <taps>', e.g. "
            "'3000 raw,post,final') | status | stop | read (write the captured "
            "buffer to `path` as JSON). Pair with get_state model=dsp to "
            "correlate with the noise-reduction chain."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["start", "stop", "status", "read"]},
            "value": {"type": "string",
                      "description": "for start: '<durationMs> <comma,taps>' (default 5000ms)"},
            "path": {"type": "string", "description": "for read: output file path"},
        }, "required": ["action"]},
    },
    {
        "name": "floors",
        "description": (
            "Per-pan measured noise floor and display floor in dBm — the "
            "numeric RX-noise readout, no screenshot needed."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "tune",
        "description": ("Set a slice frequency, in MHz (e.g. 14.074). "
                        "Tunes the active slice unless sliceId targets a "
                        "specific slice. Confirm with get_state model=slice."),
        "inputSchema": {"type": "object", "properties": {
            "mhz": {"type": "string", "description": "frequency in MHz, e.g. '14.074'"},
            "sliceId": {"type": "string",
                        "description": "optional slice id; omit for the active slice"},
        }, "required": ["mhz"]},
    },
    {
        "name": "slice",
        "description": (
            "Slice lifecycle / config. action = add | remove | select | tx | "
            "diversity | centerlock | txant | rxant | rxsource | fixture | "
            "clearfixture. `value` carries the action's args (e.g. add '14.2', "
            "select a slice id). See get_state model=slices to inspect."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string"},
            "value": {"type": "string", "description": "action arguments"},
        }, "required": ["action"]},
    },
    {
        "name": "pan",
        "description": (
            "Panadapter lifecycle. action = create | add | remove | close | "
            "center; `value` is the action arg (pan id, or a frequency for "
            "center). See get_state model=pans."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["create", "add", "remove", "close", "center"]},
            "value": {"type": "string", "description": "pan id / frequency"},
        }, "required": ["action"]},
    },
    {
        "name": "record",
        "description": (
            "QSO audio recording. action = start | stop | status | path (get "
            "the current file) | dir (value = a directory to record into)."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["start", "stop", "status", "path", "dir"]},
            "value": {"type": "string", "description": "e.g. a directory for 'dir'"},
        }, "required": ["action"]},
    },
    {
        "name": "mark",
        "description": (
            "Write a timestamped annotation into the app log ring — bracket "
            "your actions so a later get_log shows exactly what you did and "
            "when."),
        "inputSchema": {"type": "object", "properties": {
            "text": {"type": "string"},
        }, "required": ["text"]},
    },
    {
        "name": "window",
        "description": ("Drive a top-level window's state. Useful for headless "
                        "render tests (a real size gives the panadapter real "
                        "x_pixels)."),
        "inputSchema": {"type": "object", "properties": {
            "state": {"type": "string",
                      "enum": ["maximize", "restore", "minimize", "fullscreen"]},
            "target": {"type": "string", "description": "optional window target"},
        }, "required": ["state"]},
    },
    {
        "name": "menu",
        "description": ("Menu-bar menus. action = list (enumerate) | open "
                        "(pop `name`), so a follow-up dump_tree/grab_widget can "
                        "see the opened menu."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string", "enum": ["list", "open"]},
            "name": {"type": "string", "description": "menu name, for open"},
        }, "required": ["action"]},
    },
    {
        "name": "streams",
        "description": ("Stream diagnostics. scope empty = UDP-orphan layer; "
                        "'radio' = radio-authoritative display objects; "
                        "'reset' clears counters."),
        "inputSchema": {"type": "object", "properties": {
            "scope": {"type": "string", "enum": ["radio", "reset"]},
        }},
    },
    {
        "name": "memory_profile",
        "description": (
            "Cross-platform process and subsystem memory telemetry. snapshot "
            "reads current OS/allocator totals and tracked panadapter, audio, "
            "radio-model, GUI, and automation state. start samples on a bounded "
            "timer; report/stop return deltas, bytes-per-hour slopes, fit "
            "confidence, growth suspects, and QObject class-count growth. "
            "samples includes the retained raw time series."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["snapshot", "start", "sample", "status",
                                "report", "samples", "stop", "reset"]},
            "interval_ms": {"type": "integer",
                            "description": "start only; 250..3600000, default 5000"},
            "max_samples": {"type": "integer",
                            "description": "start only; 2..10000, default 10000"},
        }, "required": ["action"]},
    },
    {
        "name": "wait_for",
        "description": (
            "Poll a model property until it equals `expected` or `timeout_s` "
            "elapses — for awaiting async state (radio connected, a slice "
            "active) instead of guessing a sleep. Same model/selector/property "
            "as get_state. Returns matched:true/false with the last value."),
        "inputSchema": {"type": "object", "properties": {
            "model": {"type": "string"},
            "property": {"type": "string"},
            "expected": {"type": "string", "description": "value to wait for"},
            "selector": {"type": "string"},
            "timeout_s": {"type": "number", "description": "default 10, max 120"},
        }, "required": ["model", "property", "expected"]},
    },
    {
        "name": "assert_state",
        "description": (
            "One-shot assertion: read a model property and compare to "
            "`expected`, returning pass:true/false plus the actual value — so "
            "an agent's validation reads as a clean check, not a manual diff."),
        "inputSchema": {"type": "object", "properties": {
            "model": {"type": "string"},
            "property": {"type": "string"},
            "expected": {"type": "string"},
            "selector": {"type": "string"},
        }, "required": ["model", "property", "expected"]},
    },
    {
        "name": "gesture",
        "description": (
            "Hold a real pointer gesture open across MCP calls so independent "
            "bridge requests and delayed app/model events can interleave while "
            "a slider is genuinely down. Use begin(target, optional start x/y), "
            "then move(dx/dy), status, and end(dx/dy) or cancel. The wrapper "
            "keeps one private bridge connection for the gesture; any error, "
            "disconnect, or server lease timeout releases it. TX and read-only "
            "guards are enforced in the app."),
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string",
                       "enum": ["begin", "move", "end", "cancel", "status"]},
            "target": {"type": "string",
                       "description": "widget target, required for begin"},
            "value": {"type": "string",
                      "description": ("begin: optional local 'x y'; move/end: "
                                      "fixed-base 'dx dy' offsets")},
        }, "required": ["action"]},
    },
    {
        "name": "bridge_command",
        "description": (
            "Raw escape hatch for the verbs without a dedicated tool — "
            "send any JSON request object ({\"cmd\": ...}) straight to "
            "the bridge and get the raw response. Reaches: the low-level "
            "widget verbs (hover, tooltip, hitTest, clickAt, rightClick, "
            "contextMenu, close, scrollTo, drag, showMenu), the "
            "transmit-keying verbs (key, txtest, atu, cwx, testtone, "
            "txwaterfall — gated by AETHER_AUTOMATION_ALLOW_TX), and the "
            "niche ones (dss deterministic spectrum injection, resize, "
            "scale, layout, panmessage, tci, station, qrz). Full verb "
            "reference: src/core/AutomationServer.h header comment."),
        "inputSchema": {"type": "object", "properties": {
            "request": {"type": "object",
                        "description": "the raw bridge request, e.g. {\"cmd\":\"whoami\"}"},
            "timeout_s": {"type": "number",
                          "description": "override the 60s default (e.g. connect wait)"},
        }, "required": ["request"]},
    },
]


def text_result(obj):
    text = obj if isinstance(obj, str) else json.dumps(obj, indent=2)
    return {"content": [{"type": "text", "text": text}]}


def error_result(msg):
    return {"content": [{"type": "text", "text": f"Error: {msg}"}], "isError": True}


# ── Robustness helpers (#4188 area 4) ────────────────────────────────────────

def _values_equal(actual, expected):
    """Loose compare: JSON values arrive typed, `expected` arrives as text."""
    if actual is None:
        return False
    if isinstance(actual, bool):
        return str(actual).lower() == str(expected).strip().lower()
    if isinstance(actual, (int, float)):
        try:
            return float(actual) == float(expected)
        except (ValueError, TypeError):
            return str(actual) == str(expected)
    return str(actual).strip() == str(expected).strip()


def _get_property(model, selector, prop, timeout=REQUEST_TIMEOUT_S):
    """`get <model> [selector] <property>` → the raw {ok, property, value} dict."""
    req = {"cmd": "get", "model": model, "property": prop}
    if selector:
        req["selector"] = str(selector)
    return bridge_request(req, timeout=timeout)


_RESOLVE_ERR_HINTS = ("no widget", "not found", "could not", "no match",
                      "unresolved", "resolve", "unknown target", "no such")


def _collect_widget_labels(node, out):
    """Gather objectName / accessibleName / class strings from a dumpTree node."""
    if not isinstance(node, dict):
        return
    for k in ("objectName", "accessibleName", "class", "text"):
        v = node.get(k)
        if isinstance(v, str) and v.strip():
            out.add(v.strip())
    for child in (node.get("children") or []):
        _collect_widget_labels(child, out)


def _suggest_targets(target, limit=5):
    """Fuzzy-match a failed target against the live widget tree → candidates."""
    try:
        tree = bridge_request({"cmd": "dumpTree"}, timeout=15)
    except Exception:  # noqa: BLE001 — suggestions are best-effort
        return []
    labels = set()
    for root in (tree.get("roots") or []) if isinstance(tree, dict) else []:
        _collect_widget_labels(root, labels)
    if not labels:
        return []
    lc = {s.lower(): s for s in labels}
    hits = difflib.get_close_matches(str(target).lower(), list(lc), n=limit, cutoff=0.4)
    # De-dup while preserving the original-cased label.
    seen, out = set(), []
    for h in hits:
        s = lc[h]
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def _maybe_suggest(resp, target):
    """If `resp` is a target-resolution failure, append did_you_mean candidates."""
    if not isinstance(resp, dict) or resp.get("ok"):
        return resp
    err = str(resp.get("error", "")).lower()
    if not any(h in err for h in _RESOLVE_ERR_HINTS):
        return resp
    sugg = _suggest_targets(target)
    return dict(resp, did_you_mean=sugg) if sugg else resp


def handle_tool(name, args):
    if name == "app_instance":
        global _owned_app
        action = str(args.get("action") or "").strip().lower()
        if action not in ("launch", "status", "stop"):
            return error_result("app_instance action must be launch, status, or stop")
        if action == "stop":
            return text_result(_stop_owned_app())
        if action == "status":
            if _owned_app is None:
                return text_result({"ok": True, "running": False})
            instance = _owned_app
            process = instance["process"]
            return_code = process.poll()
            if return_code is not None:
                _owned_app = None
                _remove_owned_socket(instance)
                return text_result({
                    "ok": True,
                    "running": False,
                    "pid": process.pid,
                    "socket": instance["socket"],
                    "label": instance["label"],
                    "exitCode": return_code,
                })
            try:
                whoami = _request_at(instance["socket"], {"cmd": "whoami"}, timeout=5)
            except Exception as exc:  # noqa: BLE001 — status must remain inspectable
                return text_result({
                    "ok": True,
                    "running": True,
                    "authenticated": False,
                    "pid": process.pid,
                    "socket": instance["socket"],
                    "label": instance["label"],
                    "error": str(exc),
                })
            identity = whoami.get("automationIdentity") or whoami.get("socket")
            try:
                whoami_pid = int(whoami.get("pid", -1))
            except (TypeError, ValueError):
                whoami_pid = -1
            identity_matches = (
                bool(whoami.get("ok"))
                and whoami_pid == process.pid
                and identity == instance["socket"]
                and whoami.get("label") == instance["label"]
                and whoami.get("txAllowed") is False)
            return text_result({
                "ok": True,
                "running": True,
                "authenticated": identity_matches,
                "identityMatches": identity_matches,
                "pid": process.pid,
                "socket": instance["socket"],
                "label": instance["label"],
                "readOnly": whoami.get("readOnly", False),
                "txAllowed": whoami.get("txAllowed"),
                "version": whoami.get("version"),
            })

        if _owned_app is not None:
            if _owned_app["process"].poll() is None:
                return error_result(
                    "this MCP server already owns a running AetherSDR instance; stop it first")
            _remove_owned_socket(_owned_app)
            _owned_app = None
        if not os.environ.get("AETHER_MCP_TOKEN"):
            return error_result(
                "secure launch requires AETHER_MCP_TOKEN in this MCP server environment")
        try:
            root, executable = _resolve_app_artifact(args.get("worktree"))
            label = _instance_label(args.get("label"))
        except ValueError as exc:
            return error_result(str(exc))

        socket_path = _instance_socket()
        child_env = os.environ.copy()
        child_env["AETHER_AUTOMATION"] = "1"
        child_env["AETHER_AUTOMATION_NO_AUTOCONNECT"] = "1"
        child_env["AETHER_AUTOMATION_NO_TX"] = "1"
        child_env["AETHER_AUTOMATION_SOCKET"] = socket_path
        child_env["AETHER_AUTOMATION_LABEL"] = label
        child_env["AETHER_AUTOMATION_IDENTITY"] = socket_path
        child_env["AETHER_AUTOMATION_AGENT_NAME"] = label
        child_env.pop("AETHER_AUTOMATION_ALLOW_TX", None)
        child_env.pop("AETHER_MCP_SOCKET", None)

        popen_kwargs = {
            "cwd": str(root),
            "env": child_env,
            "stdin": subprocess.DEVNULL,
            "stdout": subprocess.DEVNULL,
            "stderr": subprocess.DEVNULL,
            "close_fds": True,
        }
        if sys.platform == "win32":
            popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            popen_kwargs["start_new_session"] = True
        try:
            process = subprocess.Popen([str(executable)], **popen_kwargs)
        except OSError as exc:
            return error_result(f"could not launch fresh AetherSDR build: {exc}")

        _owned_app = {
            "process": process,
            "socket": socket_path,
            "label": label,
            "worktree": str(root),
            "executable": str(executable),
        }
        try:
            ping, whoami = _wait_for_owned_app(_owned_app)
        except Exception as exc:  # noqa: BLE001 — fail closed and reap child
            _stop_owned_app()
            return error_result(f"fresh-build authentication failed: {exc}")
        return text_result({
            "ok": True,
            "running": True,
            "authenticated": True,
            "authRequired": ping.get("authRequired"),
            "pid": process.pid,
            "socket": socket_path,
            "label": label,
            "worktree": str(root),
            "executable": str(executable),
            "readOnly": whoami.get("readOnly", False),
            "txAllowed": whoami.get("txAllowed"),
            "version": whoami.get("version"),
        })

    if name == "bridge_status":
        entries = discover_sockets()
        status = {"instances": entries, "active": None,
                  "token_configured_here": bool(os.environ.get("AETHER_MCP_TOKEN"))}
        # A running owned app uses an explicit socket that never writes a
        # discovery file, yet bridge_request routes to it; surface it so status
        # reflects where commands actually go, not just discovered instances.
        if _owned_app is not None and _owned_app["process"].poll() is None:
            status["owned_instance"] = {
                "socket": _owned_app["socket"],
                "label": _owned_app["label"],
                "pid": _owned_app["process"].pid,
                "note": "MCP-owned fresh build; bridge commands route here.",
            }
        if entries or os.environ.get("AETHER_MCP_SOCKET"):
            # ping needs no token and reveals whether the bridge requires one.
            try:
                pong = bridge_request({"cmd": "ping"}, timeout=10)
                status["bridge_auth_required"] = pong.get("authRequired")
                # Observe-only gate (#4188 area 6). The bridge is authoritative;
                # this just reflects it so a client knows up front that mutating
                # verbs will be refused. Flip it in Radio Setup → Network.
                status["bridge_read_only"] = pong.get("readOnly", False)
            except Exception as e:  # noqa: BLE001
                status["ping_error"] = str(e)
            # whoami is auth-gated — its success confirms our token is accepted.
            try:
                status["active"] = bridge_request({"cmd": "whoami"}, timeout=10)
            except Exception as e:  # noqa: BLE001 — report, don't die
                status["active_error"] = str(e)
            if (status.get("bridge_auth_required")
                    and not status["token_configured_here"]):
                status["hint"] = ("This bridge requires a token, but "
                                  "AETHER_MCP_TOKEN is not set for this server. "
                                  "Copy the token from Radio Setup → Network → "
                                  "Access Token and set it in this MCP server's "
                                  "env config.")
            if status.get("bridge_read_only"):
                status["read_only_note"] = (
                    "This bridge is observe-only. Read verbs work; every "
                    "mutating verb (set/invoke/connect/tune/capture…) is "
                    "refused by the app. Uncheck \"Observe only\" in Radio "
                    "Setup → Network to allow driving.")
        if not entries and not os.environ.get("AETHER_MCP_SOCKET"):
            status["hint"] = ("No bridge running. Launch AetherSDR with "
                              "AETHER_AUTOMATION=1 or enable it in Radio Setup "
                              "→ Network.")
        return text_result(status)

    if name == "dump_tree":
        resp = bridge_request({"cmd": "dumpTree"})
        needle = (args.get("filter") or "").strip().lower()
        if needle and isinstance(resp, dict):
            # The bridge returns top-level widgets under "roots".
            roots = resp.get("roots")
            if isinstance(roots, list):
                pruned = [t for t in (prune_tree(r, needle) for r in roots) if t]
                resp = dict(resp, roots=pruned, filtered=needle,
                            matches=len(pruned))
            else:
                pruned = prune_tree(resp, needle)
                resp = pruned if pruned else {"filtered": needle, "matches": 0}
        return text_result(resp)

    if name == "grab_widget":
        target = args["target"]
        out = (args.get("path")
               or os.path.join(tempfile.gettempdir(),
                               f"aether-mcp-grab-{int(time.time())}.png"))
        req = {"cmd": "grab", "target": target, "path": out}
        if args.get("selector"):
            req["selector"] = str(args["selector"])
        resp = bridge_request(req)
        path = resp.get("path", out) if isinstance(resp, dict) else out
        content = [{"type": "text", "text": json.dumps(resp)}]
        try:
            size = os.path.getsize(path)
            if size <= MAX_INLINE_IMAGE_BYTES:
                with open(path, "rb") as f:
                    content.append({"type": "image",
                                    "data": base64.b64encode(f.read()).decode(),
                                    "mimeType": "image/png"})
            else:
                content[0]["text"] += (f"\n(image {size} bytes — too large to "
                                       f"inline, read it from {path})")
        except OSError as e:
            content[0]["text"] += f"\n(could not read capture: {e})"
        return {"content": content}

    if name == "invoke":
        req = {"cmd": "invoke", "target": args["target"], "action": args["action"]}
        if args.get("value") is not None:
            req["value"] = str(args["value"])
        # On a target-resolution failure, append fuzzy candidates from the tree
        # so the agent doesn't have to dump_tree and guess again.
        return text_result(_maybe_suggest(bridge_request(req), args["target"]))

    if name == "get_state":
        req = {"cmd": "get", "model": args["model"]}
        if args.get("selector"):
            req["selector"] = str(args["selector"])
        if args.get("property"):
            req["property"] = str(args["property"])
        return text_result(bridge_request(req))

    if name == "shortcut":
        # The registry's shortcut verb reads `id` (or `target`), not `value` —
        # a JSON request bypasses the bare-line positional parser.
        return text_result(bridge_request({"cmd": "shortcut", "id": args["id"]}))

    if name == "get_log":
        # log verb: action="tail", value="<n> [since=<seq>]".
        parts = [str(int(args.get("count", 100)))]
        if args.get("since") is not None:
            parts.append(f"since={int(args['since'])}")
        return text_result(bridge_request(
            {"cmd": "log", "action": "tail", "value": " ".join(parts)}))

    if name == "connect":
        req = {"cmd": "connect", "action": args["action"]}
        if args.get("value"):
            req["value"] = str(args["value"])
        # `connect wait <ms>` can legitimately run past the default request
        # timeout — stretch it to cover the requested wait plus slack.
        timeout = REQUEST_TIMEOUT_S
        if args["action"] == "wait" and args.get("value"):
            try:
                timeout = max(timeout, int(args["value"]) / 1000 + 10)
            except (ValueError, TypeError):
                pass  # non-numeric wait value → keep the default timeout
        return text_result(bridge_request(req, timeout=timeout))

    if name == "disconnect":
        return text_result(bridge_request({"cmd": "disconnect"}))

    if name == "capture_audio":
        req = {"cmd": "audioCapture", "action": args["action"]}
        if args.get("value"):
            req["value"] = str(args["value"])
        if args.get("path"):
            req["path"] = str(args["path"])
        return text_result(bridge_request(req))

    if name == "floors":
        return text_result(bridge_request({"cmd": "floors"}))

    if name == "tune":
        req = {"cmd": "tune", "value": str(args["mhz"])}
        if args.get("sliceId") not in (None, ""):
            req["id"] = str(args["sliceId"])
        return text_result(bridge_request(req))

    if name in ("slice", "record", "pan"):
        req = {"cmd": name, "action": args["action"]}
        if args.get("value"):
            req["value"] = str(args["value"])
        return text_result(bridge_request(req))

    if name == "mark":
        return text_result(bridge_request(
            {"cmd": "mark", "value": str(args["text"])}))

    if name == "window":
        # window reads `target`, not `value`.
        req = {"cmd": "window", "action": args["state"]}
        if args.get("target"):
            req["target"] = str(args["target"])
        return text_result(bridge_request(req))

    if name == "menu":
        req = {"cmd": "menu", "action": args["action"]}
        if args.get("name"):
            req["value"] = str(args["name"])
        return text_result(bridge_request(req))

    if name == "streams":
        req = {"cmd": "streams"}
        if args.get("scope"):
            req["action"] = str(args["scope"])
        return text_result(bridge_request(req))

    if name == "memory_profile":
        req = {"cmd": "memprofile", "action": args["action"]}
        if args["action"] == "start":
            values = []
            if args.get("interval_ms") is not None:
                values.append(str(int(args["interval_ms"])))
            elif args.get("max_samples") is not None:
                values.append("5000")
            if args.get("max_samples") is not None:
                values.append(str(int(args["max_samples"])))
            if values:
                req["value"] = " ".join(values)
        return text_result(bridge_request(req))

    if name == "assert_state":
        resp = _get_property(args["model"], args.get("selector"), args["property"])
        if isinstance(resp, dict) and "error" in resp:
            return text_result({"pass": False, "error": resp["error"]})
        actual = resp.get("value") if isinstance(resp, dict) else None
        return text_result({"pass": _values_equal(actual, args["expected"]),
                            "actual": actual, "expected": args["expected"]})

    if name == "wait_for":
        expected = args["expected"]
        timeout = max(0.0, min(float(args.get("timeout_s", 10)), 120.0))
        deadline = time.monotonic() + timeout
        interval, actual, last_err = 0.5, None, None
        while True:
            try:
                resp = _get_property(args["model"], args.get("selector"),
                                     args["property"])
            except Exception as e:  # noqa: BLE001 — transient bridge/socket
                resp = {"error": str(e)}        # error → keep polling, don't
                                                # abort the wait mid-reconnect
            if isinstance(resp, dict) and "error" in resp:
                last_err = resp["error"]        # keep polling — property may
            else:                               # not exist until state arrives
                actual = resp.get("value") if isinstance(resp, dict) else None
                if _values_equal(actual, expected):
                    return text_result({"matched": True, "value": actual,
                                        "expected": expected})
            if time.monotonic() >= deadline:
                out = {"matched": False, "value": actual, "expected": expected,
                       "timeout_s": timeout}
                if last_err and actual is None:
                    out["last_error"] = last_err
                return text_result(out)
            time.sleep(interval)

    if name == "gesture":
        global _gesture_bridge, _gesture_socket_path
        action = str(args.get("action") or "").strip().lower()
        if action not in ("begin", "move", "end", "cancel", "status"):
            return error_result(
                "gesture action must be begin, move, end, cancel, or status")

        if action == "begin":
            target = str(args.get("target") or "").strip()
            if not target:
                return error_result("gesture begin requires `target`")
            if _gesture_bridge is not None:
                return error_result(
                    "a gesture is already active; end or cancel it first")
            sock_path = bridge_socket_path()
            if not sock_path:
                return error_result(
                    "no running AetherSDR automation bridge found")
            try:
                _gesture_bridge = Bridge(sock_path, REQUEST_TIMEOUT_S)
                _gesture_socket_path = sock_path
                req = {"cmd": "gesture", "action": "begin", "target": target}
                if args.get("value") is not None:
                    req["value"] = str(args["value"])
                response = _gesture_bridge.request(_authenticated_request(req))
            except Exception as exc:  # noqa: BLE001 — cleanup is the contract
                _close_gesture_bridge()
                return error_result(str(exc))
            if not isinstance(response, dict) or not response.get("ok"):
                _close_gesture_bridge()
            return text_result(response)

        if action == "status" and _gesture_bridge is None:
            return text_result(bridge_request(
                {"cmd": "gesture", "action": "status"}))
        if _gesture_bridge is None:
            return error_result("no active gesture; begin one first")

        req = {"cmd": "gesture", "action": action}
        if args.get("value") is not None:
            req["value"] = str(args["value"])
        try:
            response = _gesture_bridge.request(_authenticated_request(req))
        except Exception as exc:  # noqa: BLE001 — closing releases app input
            _close_gesture_bridge()
            return error_result(str(exc))

        terminal = (action in ("end", "cancel")
                    or (isinstance(response, dict)
                        and response.get("active") is False))
        if terminal or not isinstance(response, dict) or not response.get("ok"):
            _close_gesture_bridge()
        return text_result(response)

    if name == "bridge_command":
        req = args.get("request")
        if not isinstance(req, dict) or "cmd" not in req:
            return error_result("`request` must be an object with a `cmd` key")
        timeout = float(args.get("timeout_s") or REQUEST_TIMEOUT_S)
        return text_result(bridge_request(req, timeout=timeout))

    return error_result(f"Unknown tool: {name}")


# ── MCP prompts (#4188 area 3) ───────────────────────────────────────────────
# Server-provided, reusable workflows so every assistant gets the validation
# loop out of the box instead of reinventing it.

PROMPTS = [
    {
        "name": "validate_ui_change",
        "description": ("Guided workflow to validate a UI change against the "
                        "running app using this server's tools."),
        "arguments": [
            {"name": "widget", "description": "the control/widget you changed "
             "(objectName or accessibleName)", "required": False},
            {"name": "change", "description": "one line on what should now be "
             "different", "required": False},
        ],
    },
]


def prompt_get(name, arguments):
    if name != "validate_ui_change":
        return None
    widget = (arguments or {}).get("widget") or "<your widget>"
    change = (arguments or {}).get("change") or "<what should be different>"
    text = (
        f"Validate this AetherSDR change against the running app: {change}\n\n"
        "Drive it with the MCP tools, don't guess:\n"
        f"1. `bridge_status` — confirm the app is up and which instance.\n"
        f"2. `dump_tree` with filter=\"{widget}\" — find the control's exact "
        "target name (objectName / accessibleName).\n"
        f"3. `invoke` that target to exercise the change (or `tune`/`slice`/"
        "`pan` for a radio-level intent). If the target isn't found, use the "
        "`did_you_mean` candidates it returns.\n"
        "4. `assert_state` (or `wait_for` if it's async) on the model property "
        "that should have changed — e.g. get_state model=slice property=... — "
        "so the check reads pass/fail, not a manual diff.\n"
        f"5. `grab_widget` target=\"{widget}\" for a visual confirmation.\n\n"
        "Report the assert_state result and attach the grab. Keep TX out of it "
        "unless the change is specifically a transmit path."
    )
    return {"description": "Validate a UI change against the running app",
            "messages": [{"role": "user",
                          "content": {"type": "text", "text": text}}]}


# ── MCP resources (#4188 area 3) ─────────────────────────────────────────────
# Read-only live state the model can pull as context without a tool round-trip.
# Each read fetches fresh from the bridge.

RESOURCES = [
    {"uri": "aethersdr://widget-tree", "name": "Widget tree",
     "description": "ARIA-style snapshot of the whole widget hierarchy",
     "mimeType": "application/json"},
    {"uri": "aethersdr://state/radio", "name": "Radio state",
     "description": "get radio — connection, model, callsign, …",
     "mimeType": "application/json"},
    {"uri": "aethersdr://state/slices", "name": "Slices",
     "description": "get slices — all receiver slices", "mimeType": "application/json"},
    {"uri": "aethersdr://state/pans", "name": "Panadapters",
     "description": "get pans — all panadapters", "mimeType": "application/json"},
    {"uri": "aethersdr://verbs", "name": "Bridge verb catalog",
     "description": "every bridge verb with aliases + help", "mimeType": "application/json"},
]

_RESOURCE_REQUESTS = {
    "aethersdr://widget-tree": {"cmd": "dumpTree"},
    "aethersdr://state/radio": {"cmd": "get", "model": "radio"},
    "aethersdr://state/slices": {"cmd": "get", "model": "slices"},
    "aethersdr://state/pans": {"cmd": "get", "model": "pans"},
    "aethersdr://verbs": {"cmd": "verbs"},
}


def resource_read(uri):
    req = _RESOURCE_REQUESTS.get(uri)
    if req is None:
        return None
    data = bridge_request(req)
    return {"contents": [{"uri": uri, "mimeType": "application/json",
                          "text": json.dumps(data, indent=2)}]}


# --------------------------------------------------------------------------
# MCP stdio loop (newline-delimited JSON-RPC)
# --------------------------------------------------------------------------

def main():
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except ValueError:
            continue
        # A JSON-RPC message must be an object. A batch array or a bare scalar
        # would make msg.get(...) raise AttributeError outside every try and
        # kill the loop — reject them per JSON-RPC (-32600) and keep serving.
        if not isinstance(msg, dict):
            sys.stdout.write(json.dumps({
                "jsonrpc": "2.0", "id": None,
                "error": {"code": -32600,
                          "message": "Invalid Request: expected a JSON object"
                                     " (batch requests are not supported)"}}) + "\n")
            sys.stdout.flush()
            continue
        method, mid = msg.get("method"), msg.get("id")
        if method == "initialize":
            resp = {"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}, "prompts": {}, "resources": {}},
                "serverInfo": SERVER_INFO}}
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            resp = {"jsonrpc": "2.0", "id": mid,
                    "result": {"tools": TOOLS}}
        elif method == "tools/call":
            params = msg.get("params") or {}
            try:
                result = handle_tool(params.get("name"),
                                     params.get("arguments") or {})
            except Exception as e:  # noqa: BLE001 — surface to the model
                result = error_result(str(e))
            resp = {"jsonrpc": "2.0", "id": mid, "result": result}
        elif method == "prompts/list":
            resp = {"jsonrpc": "2.0", "id": mid, "result": {"prompts": PROMPTS}}
        elif method == "prompts/get":
            params = msg.get("params") or {}
            got = prompt_get(params.get("name"), params.get("arguments"))
            if got is None:
                resp = {"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32602,
                                  "message": f"unknown prompt: {params.get('name')}"}}
            else:
                resp = {"jsonrpc": "2.0", "id": mid, "result": got}
        elif method == "resources/list":
            resp = {"jsonrpc": "2.0", "id": mid, "result": {"resources": RESOURCES}}
        elif method == "resources/read":
            params = msg.get("params") or {}
            try:
                got = resource_read(params.get("uri"))
            except Exception as e:  # noqa: BLE001 — bridge may be down
                got = None
                read_err = str(e)
            else:
                read_err = None
            if got is None:
                resp = {"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32602,
                                  "message": read_err or
                                  f"unknown resource: {params.get('uri')}"}}
            else:
                resp = {"jsonrpc": "2.0", "id": mid, "result": got}
        elif mid is not None:
            resp = {"jsonrpc": "2.0", "id": mid,
                    "error": {"code": -32601, "message": f"Unknown: {method}"}}
        else:
            continue
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
