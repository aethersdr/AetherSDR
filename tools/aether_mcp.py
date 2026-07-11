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

import base64
import glob
import json
import os
import socket
import sys
import tempfile
import time

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "aethersdr-automation", "version": "1.0.0"}
REQUEST_TIMEOUT_S = 60
MAX_INLINE_IMAGE_BYTES = 800_000  # larger grabs are returned as a path only


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
    token = os.environ.get("AETHER_MCP_TOKEN")
    if token and "token" not in obj:
        obj = dict(obj, token=token)
    b = Bridge(sock_path, timeout)
    try:
        return b.request(obj)
    finally:
        b.close()


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
            "<selector: id|active> | pans | flags | kiwi | sync. "
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
        "name": "bridge_command",
        "description": (
            "Raw escape hatch for every other bridge verb — send any "
            "JSON request object ({\"cmd\": ...}) straight to the "
            "bridge and get the raw response. Useful verbs: hover, "
            "tooltip, hitTest, clickAt, rightClick, contextMenu, "
            "resize {value:'W H'}, window, menu, actions, connect "
            "(list/show/local/ip/wait), disconnect, slice, tune, pan, "
            "layout, scale, dss (deterministic spectrum injection), "
            "panmessage, audioCapture, record, testtone, whoami. Full "
            "verb reference: src/core/AutomationServer.h header "
            "comment."),
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


def handle_tool(name, args):
    if name == "bridge_status":
        entries = discover_sockets()
        status = {"instances": entries, "active": None,
                  "token_configured_here": bool(os.environ.get("AETHER_MCP_TOKEN"))}
        if entries or os.environ.get("AETHER_MCP_SOCKET"):
            # ping needs no token and reveals whether the bridge requires one.
            try:
                pong = bridge_request({"cmd": "ping"}, timeout=10)
                status["bridge_auth_required"] = pong.get("authRequired")
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
        out = os.path.join(tempfile.gettempdir(),
                           f"aether-mcp-grab-{int(time.time())}.png")
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
        return text_result(bridge_request(req))

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

    if name == "bridge_command":
        req = args.get("request")
        if not isinstance(req, dict) or "cmd" not in req:
            return error_result("`request` must be an object with a `cmd` key")
        timeout = float(args.get("timeout_s") or REQUEST_TIMEOUT_S)
        return text_result(bridge_request(req, timeout=timeout))

    return error_result(f"Unknown tool: {name}")


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
                "capabilities": {"tools": {}},
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
        elif mid is not None:
            resp = {"jsonrpc": "2.0", "id": mid,
                    "error": {"code": -32601, "message": f"Unknown: {method}"}}
        else:
            continue
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
