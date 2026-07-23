#!/usr/bin/env python3
"""
AetherSDR automation-bridge probe (issue #3646, Phase 0; generic since #4174).

Drives the in-app automation bridge that AetherSDR exposes when launched with
AETHER_AUTOMATION=1. The bridge is a QLocalServer speaking newline-delimited
JSON. This probe needs no Qt or third-party deps -- it talks to the AF_UNIX
socket (macOS/Linux) or the named pipe (Windows) directly.

Any bridge verb works: verbs with a bespoke CLI->JSON mapping (multi-word
arguments, client-side validation) are listed in MAPPERS below; every other
verb is passed through as a bare positional line and parsed by the server's
verb registry, so new simple verbs need no probe changes at all. Ask the
running app what it understands:

    python tools/automation_probe.py verbs

Discovery: the running app writes the resolved socket path to
<temp>/aethersdr-automation.json, so you don't have to know the platform
endpoint. Pass --socket to override.

Usage:
    AETHER_AUTOMATION=1 ./AetherSDR &        # launch the app with the bridge on
    python tools/automation_probe.py         # demo: snapshot + panadapter grab
    python tools/automation_probe.py ping
    python tools/automation_probe.py verbs
    python tools/automation_probe.py connect list
    python tools/automation_probe.py connect show
    python tools/automation_probe.py connect local first
    python tools/automation_probe.py connect wait 30000
    python tools/automation_probe.py get sync
    python tools/automation_probe.py whoami
    python tools/automation_probe.py mark before-test
    python tools/automation_probe.py key ptt on
    python tools/automation_probe.py key ptt off
    python tools/automation_probe.py slice mode DSTR
    python tools/automation_probe.py waveform start dstar
    python tools/automation_probe.py waveform unregister ExampleName
    python tools/automation_probe.py grab SpectrumWidget /tmp/pan.png
    python tools/automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png
    python tools/automation_probe.py panmessage add 0 kiwi 0 "Waiting|Queued"
    python tools/automation_probe.py audioCapture start 3000 raw,post,final
    python tools/automation_probe.py audioCapture probeDspStereo all [strict]  # may take up to 120s
    python tools/automation_probe.py audioCapture read /tmp/aether-audio.json
"""

import argparse
import json
import os
import socket
import sys
import tempfile


def discover_socket():
    """Read the discovery file the running app drops in the temp dir."""
    disc = os.path.join(tempfile.gettempdir(), "aethersdr-automation.json")
    if not os.path.exists(disc):
        return None
    try:
        with open(disc) as f:
            return json.load(f).get("socket")
    except (OSError, ValueError):
        return None


class Bridge:
    """One short-lived connection to the bridge; supports many requests."""

    def __init__(self, sock_path):
        self.sock_path = sock_path
        self._buf = b""
        if sys.platform == "win32":
            # Qt named pipe: \\.\pipe\<name>. Open like a file.
            self._pipe = open(rf"\\.\pipe\{os.path.basename(sock_path)}", "r+b", buffering=0)
            self._sock = None
        else:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.connect(sock_path)
            self._pipe = None

    def request(self, obj, timeout_seconds=None):
        request = dict(obj)
        token = os.environ.get("AETHER_MCP_TOKEN")
        if token and "token" not in request:
            request["token"] = token
        return self.request_line(json.dumps(request), timeout_seconds)

    def request_line(self, text, timeout_seconds=None):
        """Send one raw request line (JSON or bare positional) and return the
        decoded JSON response."""
        line = (text + "\n").encode()
        if self._sock:
            previous_timeout = self._sock.gettimeout()
            try:
                self._sock.settimeout(timeout_seconds)
                self._sock.sendall(line)
                return self._read_line_sock()
            finally:
                self._sock.settimeout(previous_timeout)
        self._pipe.write(line)
        self._pipe.flush()
        return json.loads(self._pipe.readline().decode())

    def _read_line_sock(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("bridge closed the connection")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return json.loads(line.decode())

    def close(self):
        if self._sock:
            self._sock.close()
        if self._pipe:
            self._pipe.close()


def _as_int(tok):
    """Strict int parse: None for floats/non-numerics (so '1420.5' errors
    loudly instead of being reclassified as a widget name)."""
    try:
        return int(tok)
    except ValueError:
        return None


def collect_actions(node, pattern=None):
    """Return QAction summaries from a dumpTree response, optionally filtered."""
    needle = pattern.lower() if pattern else None
    matches = []

    def visit(value):
        if isinstance(value, dict):
            if value.get("class") == "QAction" and not value.get("separator"):
                fields = [
                    str(value.get("text", "")),
                    str(value.get("accessibleName", "")),
                    str(value.get("toolTip", "")),
                    str(value.get("value", "")),
                ]
                haystack = " ".join(fields).lower()
                if needle is None or needle in haystack:
                    matches.append({
                        "text": value.get("text", ""),
                        "accessibleName": value.get("accessibleName", ""),
                        "toolTip": value.get("toolTip", ""),
                        "value": value.get("value", ""),
                        "visible": value.get("visible", False),
                        "enabled": value.get("enabled", False),
                        "checkable": value.get("checkable", False),
                        "checked": value.get("checked", False),
                    })
            for child in value.get("actions", []):
                visit(child)
            for child in value.get("children", []):
                visit(child)
        elif isinstance(value, list):
            for item in value:
                visit(item)

    visit(node)
    return matches


# ── Verb → JSON-request mappers ──────────────────────────────────────────────
# Only verbs whose CLI form needs a bespoke JSON mapping are listed: multi-word
# arguments that shell quoting must survive (a bare line re-splits on spaces),
# JSON-only fields (audioCapture's path, panmessage's id/timeoutMs), verbs
# whose bare form the server doesn't parse (tci), or client-side validation.
# Every mapper returns the request dict WITHOUT "cmd"; main() adds it and does
# the one send/print. Anything not listed goes over the wire as a bare
# positional line for the server's verb registry to parse.

def _need(rest, n, usage):
    if len(rest) < n:
        sys.exit(f"error: {usage}")


def _map_target_value(usage):
    """<target> [value…] — quoting-safe target, rest joined into value."""
    def mapper(rest):
        _need(rest, 1, usage)
        req = {"target": rest[0]}
        if len(rest) > 1:
            req["value"] = " ".join(rest[1:])
        return req
    return mapper


def _map_action_value(usage=None):
    """[action [value…]] — action + rest joined into value."""
    def mapper(rest):
        if usage:
            _need(rest, 1, usage)
        req = {}
        if rest:
            req["action"] = rest[0]
            if len(rest) > 1:
                req["value"] = " ".join(rest[1:])
        return req
    return mapper


def _map_grab(rest):
    _need(rest, 1, "grab requires a target widget name")
    req = {"target": rest[0]}
    if rest[0] in ("pan", "pan-visible", "pan-composite"):
        _need(rest, 2, f"grab {rest[0]} requires a pan index")
        req["selector"] = rest[1]
        if len(rest) > 2:
            req["path"] = rest[2]
    elif len(rest) > 1:
        req["path"] = rest[1]
    return req


def _map_invoke(rest):
    _need(rest, 2, "invoke needs <target> <action> [value]")
    req = {"target": rest[0], "action": rest[1]}
    if len(rest) > 2:
        req["value"] = " ".join(rest[2:])
    return req


def _map_get(rest):
    _need(rest, 1, "get needs <model> [selector] [property]")
    req = {"model": rest[0]}
    if len(rest) > 1:
        req["selector"] = rest[1]
    if len(rest) > 2:
        req["property"] = rest[2]
    return req


def _map_clickat(rest):
    # clickAt <x> <y>           -> global screen coords (dumpTree geometry)
    # clickAt <target> <x> <y>  -> coords local to <target>
    # Disambiguate like the server: first token numeric => global form
    # (_as_int: strict int, floats error loudly).
    if len(rest) >= 2 and _as_int(rest[0]) is not None:
        if _as_int(rest[1]) is None:
            sys.exit(f"error: clickAt y must be an integer, got {rest[1]!r}")
        return {"value": f"{rest[0]} {rest[1]}"}
    if len(rest) >= 3 and _as_int(rest[0]) is None:
        if _as_int(rest[1]) is None or _as_int(rest[2]) is None:
            sys.exit("error: clickAt <target> <x> <y> — x and y must be integers")
        return {"target": rest[0], "value": f"{rest[1]} {rest[2]}"}
    raise SystemExit("error: clickAt needs <x> <y> or <target> <x> <y>")


def _map_dragat(rest):
    _need(rest, 5, "dragAt needs <target> <x> <y> <dx> <dy> [modifiers]")
    for name, value in zip(("x", "y", "dx", "dy"), rest[1:5]):
        if _as_int(value) is None:
            sys.exit(f"error: dragAt {name} must be an integer, got {value!r}")
    return {"target": rest[0], "value": " ".join(rest[1:])}


def _map_rightclick(rest):
    if not rest:
        sys.exit("error: rightClick needs <target> [x y]")
    if len(rest) not in (1, 3):
        sys.exit("error: rightClick needs <target> or <target> <x> <y>")
    if len(rest) == 3:
        if _as_int(rest[1]) is None or _as_int(rest[2]) is None:
            sys.exit("error: rightClick x and y must be integers")
    req = {"target": rest[0]}
    if len(rest) > 1:
        req["value"] = " ".join(rest[1:])
    return req


def _map_hover(rest):
    _need(rest, 1, "hover needs <target> [leave]")
    req = {"target": rest[0]}
    if len(rest) > 1:
        req["action"] = rest[1]
    return req


def _map_tooltip(rest):
    _need(rest, 1, "tooltip needs <target> [hide|text...]")
    req = {"target": rest[0]}
    if len(rest) > 1:
        if rest[1] == "hide":
            if len(rest) != 2:
                sys.exit("error: tooltip hide takes no extra arguments")
            req["action"] = "hide"
        else:
            req["value"] = " ".join(rest[1:])
    return req


def _map_resize(rest):
    _need(rest, 2, "resize needs <w> <h> [target]")
    req = {"value": f"{rest[0]} {rest[1]}"}
    if len(rest) > 2:
        req["target"] = " ".join(rest[2:])
    return req


def _map_dss(rest):
    _need(rest, 1, "dss needs <snapshot|reset|inject|scrollback|live> [pan] [args]")
    action = rest[0]
    req = {"action": action}
    dss_args = rest[1:]
    if action == "inject":
        stream_names = {"native", "flex", "kiwi", "kiwisdr"}
        if len(dss_args) < 3:
            sys.exit("error: dss inject needs [pan] <count> <firstPeakBin> <stepBin> "
                     "[native|kiwi [rowLowMhz rowHighMhz]]")
        if (len(dss_args) == 3
                or (len(dss_args) == 4 and dss_args[3].lower() in stream_names)
                or (len(dss_args) == 6 and dss_args[3].lower() in stream_names)):
            req["value"] = " ".join(dss_args)
        else:
            req["target"] = dss_args[0]
            req["value"] = " ".join(dss_args[1:])
    elif action == "scrollback":
        if not dss_args:
            sys.exit("error: dss scrollback needs [pan] <offsetRows>")
        if len(dss_args) == 1:
            req["value"] = dss_args[0]
        else:
            req["target"] = dss_args[0]
            req["value"] = " ".join(dss_args[1:])
    else:
        if dss_args:
            req["target"] = dss_args[0]
        if len(dss_args) > 1:
            req["value"] = " ".join(dss_args[1:])
    return req


def _map_audiocapture(rest):
    action = rest[0] if rest else "status"
    req = {"action": action}
    if len(rest) > 1:
        if action == "read" and len(rest) == 2:
            req["path"] = rest[1]
        else:
            req["value"] = " ".join(rest[1:])
    return req


def _map_panmessage(rest):
    if not rest:
        sys.exit("error: panmessage needs <add|remove|clear|list> <target> ...")
    req = {"action": rest[0]}
    if len(rest) > 1:
        req["target"] = rest[1]
    if rest[0] in ("add", "upsert"):
        if len(rest) < 5:
            sys.exit("error: panmessage add needs <target> <id> <timeoutMs> <title|detail>")
        req["id"] = rest[2]
        try:
            req["timeoutMs"] = int(rest[3])
        except ValueError:
            sys.exit("error: panmessage add timeoutMs must be an integer")
        text_start = 4
        if len(rest) > text_start and rest[text_start].lower().startswith("tone="):
            req["tone"] = rest[text_start].split("=", 1)[1].strip()
            text_start += 1
        text = " ".join(rest[text_start:])
        title, sep, detail = text.partition("|")
        req["title"] = title.strip()
        req["detail"] = detail.strip() if sep else ""
    elif rest[0] in ("remove", "dismiss"):
        if len(rest) < 3:
            sys.exit("error: panmessage remove needs <target> <id>")
        req["id"] = rest[2]
    return req


MAPPERS = {
    "grab": _map_grab,
    "invoke": _map_invoke,
    "get": _map_get,
    "hitTest": _map_target_value("hitTest needs <target> [x y]"),
    "contextMenu": _map_target_value("contextMenu needs <target> [x y]"),
    "rightClick": _map_rightclick,
    "clickAt": _map_clickat,
    "dragAt": _map_dragat,
    "hover": _map_hover,
    "tooltip": _map_tooltip,
    "resize": _map_resize,
    "connect": _map_action_value("connect needs <list|show|hide|local|ip|wait> [args]"),
    "slice": _map_action_value("slice needs an action"),
    "memory": _map_action_value("memory needs <activate> <index> [panId]"),
    "waveform": _map_action_value(
        "waveform needs <start|stop|unregister|resync> [args]"),
    "pan": _map_action_value("pan needs <create|add|center|close|remove> [value]"),
    "layout": _map_action_value("layout needs <rearrange <id> | get>"),
    "record": _map_action_value(),
    "testtone": _map_action_value(),
    # tci has no bare-line parse arm server-side (see the verb registry) —
    # the JSON action/value form is the only supported request shape.
    "tci": _map_action_value(),
    "dss": _map_dss,
    "audioCapture": _map_audiocapture,
    "panmessage": _map_panmessage,
}


def main():
    ap = argparse.ArgumentParser(
        description="Drive the AetherSDR automation bridge",
        epilog="Any bridge verb is accepted; run `automation_probe.py verbs` to list\n"
               "what the running app understands. examples:\n"
               "  automation_probe.py demo\n"
               "  automation_probe.py verbs\n"
               "  automation_probe.py connect list\n"
               "  automation_probe.py connect local first\n"
               "  automation_probe.py connect wait 30000\n"
               "  automation_probe.py get radio\n"
               "  automation_probe.py get slice active frequency\n"
               "  automation_probe.py slice rxsource 7 K4JK\n"
               "  automation_probe.py slice fixture 4 B\n"
               "  automation_probe.py invoke 'Master volume' setValue 35\n"
               "  automation_probe.py hover E\n"
               "  automation_probe.py tooltip E\n"
               "  automation_probe.py menu open Tools\n"
               "  automation_probe.py close WaveformsDialog\n"
               "  automation_probe.py hitTest SpectrumWidget 80 80\n"
               "  automation_probe.py rightClick 'Panadapter spectrum display'\n"
               "  automation_probe.py clickAt 1420 210          # global point (dumpTree geometry)\n"
               "  automation_probe.py clickAt AppletPanel 12 34  # point local to a widget\n"
               "  automation_probe.py resize 1600 900\n"
               "  automation_probe.py audioCapture start 3000 raw,post,final\n"
               "  automation_probe.py audioCapture probeNr2Stereo\n"
               "  automation_probe.py audioCapture probeDspStereo all [strict]  # up to 120s\n"
               "  automation_probe.py audioCapture read /tmp/aether-audio.json\n"
               "  automation_probe.py grab SpectrumWidget /tmp/pan.png\n"
               "  automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png\n"
               "  automation_probe.py dss inject 0 3 100 100 native\n"
               "  automation_probe.py dss inject 0 3 100 0 kiwi 14.000 14.010\n"
               "  automation_probe.py panmessage add 0 kiwi 0 'Waiting|Queued'\n"
               "  automation_probe.py panmessage add 0 tx 10000 tone=warning 'Transmit disabled|TX blocked'",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="demo",
                    help="bridge verb, or: demo (dumpTree + panadapter grab), "
                         "actions [text-filter] (client-side QAction search)")
    ap.add_argument("rest", nargs="*",
                    help="verb arguments (positional, same shapes as the bare "
                         "socket protocol; see `verbs` and docs/automation-bridge.md)")
    ap.add_argument("--socket", help="override the bridge socket path")
    ap.add_argument("--out", default=".", help="output dir for demo artifacts")
    args = ap.parse_args()

    sock_path = args.socket or discover_socket()
    if not sock_path:
        sys.exit("error: no bridge socket found. Launch the app with "
                 "AETHER_AUTOMATION=1, or pass --socket.")

    try:
        bridge = Bridge(sock_path)
    except OSError as e:
        sys.exit(f"error: could not connect to {sock_path}: {e}")

    try:
        if args.command == "demo":
            # Produce the Phase-0 deliverables.
            os.makedirs(args.out, exist_ok=True)

            pong = bridge.request({"cmd": "ping"})
            print(f"connected: {pong.get('app')} {pong.get('version')}")

            tree = bridge.request({"cmd": "dumpTree"})
            tree_path = os.path.join(args.out, "tree.json")
            with open(tree_path, "w") as f:
                json.dump(tree, f, indent=2)
            n_roots = len(tree.get("roots", []))
            print(f"snapshot: {n_roots} top-level window(s) -> {tree_path}")

            png_path = os.path.abspath(os.path.join(args.out, "panadapter.png"))
            grab = bridge.request({"cmd": "grab", "target": "SpectrumWidget",
                                   "path": png_path})
            if grab.get("ok"):
                print(f"panadapter: {grab['width']}x{grab['height']}, "
                      f"{grab['bytes']} bytes -> {grab['path']}")
            else:
                print(f"panadapter grab failed: {grab.get('error')}", file=sys.stderr)
                sys.exit(1)

        elif args.command == "actions":
            tree = bridge.request({"cmd": "dumpTree"})
            pattern = " ".join(args.rest) if args.rest else None
            actions = collect_actions(tree, pattern)
            print(json.dumps({"ok": True, "count": len(actions), "actions": actions}, indent=2))

        else:
            mapper = MAPPERS.get(args.command)
            action = ""
            if mapper is not None:
                req = mapper(args.rest)
                req["cmd"] = args.command
                action = req.get("action", "")
                probe_timeout = (120 if args.command == "audioCapture"
                                 and action in ("probeNr2Stereo", "probeDspStereo")
                                 else None)
                try:
                    resp = bridge.request(req, timeout_seconds=probe_timeout)
                except TimeoutError:
                    sys.exit(f"error: {action} exceeded the {probe_timeout}s bridge timeout")
            else:
                # No bespoke mapping: pass the bare positional line through and
                # let the server's verb registry parse it. Note: the server
                # re-splits on spaces, so multi-word arguments need a MAPPERS
                # entry (or the JSON protocol) to survive quoting.
                if os.environ.get("AETHER_MCP_TOKEN"):
                    req = {"cmd": args.command}
                    if args.rest:
                        req["args"] = " ".join(args.rest)
                    resp = bridge.request(req)
                else:
                    resp = bridge.request_line(" ".join([args.command] + args.rest))
            print(json.dumps(resp, indent=2))
            if (args.command == "audioCapture"
                    and action in ("probeNr2Stereo", "probeDspStereo")
                    and resp.get("ok") is False):
                sys.exit(1)
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
