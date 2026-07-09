#!/usr/bin/env python3
"""
AetherSDR automation-bridge probe (issue #3646, Phase 0).

Drives the in-app automation bridge that AetherSDR exposes when launched with
AETHER_AUTOMATION=1. The bridge is a QLocalServer speaking newline-delimited
JSON. This probe needs no Qt or third-party deps -- it talks to the AF_UNIX
socket (macOS/Linux) or the named pipe (Windows) directly.

It exercises the two Phase-0 verbs and saves the canonical deliverables:
  * an applet semantic snapshot  -> <out>/tree.json
  * a panadapter PNG capture     -> <out>/panadapter.png

Discovery: the running app writes the resolved socket path to
<temp>/aethersdr-automation.json, so you don't have to know the platform
endpoint. Pass --socket to override.

Usage:
    AETHER_AUTOMATION=1 ./AetherSDR &        # launch the app with the bridge on
    python tools/automation_probe.py         # snapshot + panadapter grab
    python tools/automation_probe.py ping
    python tools/automation_probe.py connect list
    python tools/automation_probe.py connect show
    python tools/automation_probe.py connect local first
    python tools/automation_probe.py connect wait 30000
    python tools/automation_probe.py get sync
    python tools/automation_probe.py grab SpectrumWidget /tmp/pan.png
    python tools/automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png
    python tools/automation_probe.py panmessage add 0 kiwi 0 "Waiting|Queued"
    python tools/automation_probe.py audioCapture start 3000 raw,post,final
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

    def request(self, obj):
        line = (json.dumps(obj) + "\n").encode()
        if self._sock:
            self._sock.sendall(line)
            return self._read_line_sock()
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


def main():
    ap = argparse.ArgumentParser(
        description="Drive the AetherSDR automation bridge",
        epilog="examples:\n"
               "  automation_probe.py demo\n"
               "  automation_probe.py connect list\n"
               "  automation_probe.py connect show\n"
               "  automation_probe.py connect local first\n"
               "  automation_probe.py connect wait 30000\n"
               "  automation_probe.py get radio\n"
               "  automation_probe.py get sync\n"
               "  automation_probe.py get slice active frequency\n"
               "  automation_probe.py slice rxsource 7 K4JK\n"
               "  automation_probe.py invoke 'Master volume' setValue 35\n"
               "  automation_probe.py hitTest SpectrumWidget 80 80\n"
               "  automation_probe.py clickAt 1420 210          # global point (dumpTree geometry)\n"
               "  automation_probe.py clickAt AppletPanel 12 34  # point local to a widget\n"
               "  automation_probe.py resize 1600 900\n"
               "  automation_probe.py audioCapture start 3000 raw,post,final\n"
               "  automation_probe.py audioCapture read /tmp/aether-audio.json\n"
               "  automation_probe.py grab SpectrumWidget /tmp/pan.png\n"
               "  automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png\n"
               "  automation_probe.py dss inject 0 3 100 100 native\n"
               "  automation_probe.py dss inject 0 3 100 0 kiwi 14.000 14.010\n"
               "  automation_probe.py panmessage add 0 kiwi 0 'Waiting|Queued'\n"
               "  automation_probe.py panmessage add 0 tx 10000 tone=warning 'Transmit disabled|TX blocked'",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="demo",
                    choices=["demo", "ping", "dumpTree", "grab", "invoke", "get",
                             "connect", "disconnect", "slice", "audioCapture",
                             "record", "testtone", "tci", "panmessage",
                             "hitTest", "clickAt", "resize", "dss",
                             "pan", "layout", "scale"],
                    help="verb to run (default: demo = dumpTree + panadapter grab)")
    ap.add_argument("rest", nargs="*",
                    help="verb args: grab <target> [path] | grab pan-visible <index> [path] | "
                         "invoke <target> <action> [value] | "
                         "get <model> [selector] [property] | "
                         "hitTest <target> [x y] | "
                         "clickAt <x> <y> | clickAt <target> <x> <y> | "
                         "resize <w> <h> [target] | "
                         "connect <list|show|hide|local|ip|wait> [args] | "
                         "slice <add|remove|select|tx|txant|rxant|rxsource> [args] | "
                         "dss <snapshot|reset|live> [pan] [args] | "
                         "dss inject [pan] <count> <firstPeakBin> <stepBin> "
                         "[native|kiwi [rowLowMhz rowHighMhz]] | "
                         "dss scrollback [pan] <offsetRows> | "
                         "pan <create|add|center|close|remove> [value] | "
                         "layout <rearrange <id>|get> | scale [pct] | "
                         "panmessage <add|remove|clear|list> <target> [id timeout [tone=info|warning] title|detail] | "
                         "audioCapture <start|stop|status|read> [args]")
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
        if args.command == "ping":
            print(json.dumps(bridge.request({"cmd": "ping"}), indent=2))

        elif args.command == "dumpTree":
            print(json.dumps(bridge.request({"cmd": "dumpTree"}), indent=2))

        elif args.command == "grab":
            if not args.rest:
                sys.exit("error: grab requires a target widget name")
            req = {"cmd": "grab", "target": args.rest[0]}
            if args.rest[0] in ("pan", "pan-visible", "pan-composite"):
                if len(args.rest) < 2:
                    sys.exit(f"error: grab {args.rest[0]} requires a pan index")
                req["selector"] = args.rest[1]
                if len(args.rest) > 2:
                    req["path"] = args.rest[2]
            elif len(args.rest) > 1:
                req["path"] = args.rest[1]
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "invoke":
            if len(args.rest) < 2:
                sys.exit("error: invoke needs <target> <action> [value]")
            req = {"cmd": "invoke", "target": args.rest[0], "action": args.rest[1]}
            if len(args.rest) > 2:
                req["value"] = " ".join(args.rest[2:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "get":
            if not args.rest:
                sys.exit("error: get needs <model> [selector] [property] "
                         "(model = radio|slice|slices|pan|pans)")
            req = {"cmd": "get", "model": args.rest[0]}
            if len(args.rest) > 1:
                req["selector"] = args.rest[1]
            if len(args.rest) > 2:
                req["property"] = args.rest[2]
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "hitTest":
            if not args.rest:
                sys.exit("error: hitTest needs <target> [x y]")
            req = {"cmd": "hitTest", "target": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "clickAt":
            # clickAt <x> <y>            -> global screen coords (dumpTree geometry)
            # clickAt <target> <x> <y>  -> coords local to <target>
            # Disambiguate like the server: first token numeric => global form.
            # int() (not isdigit) so a float like 1420.5 errors loudly instead
            # of being reclassified as a widget NAME ("widget not found: 1420.5").
            def _as_int(tok):
                try:
                    return int(tok)
                except ValueError:
                    return None

            if len(args.rest) >= 2 and _as_int(args.rest[0]) is not None:
                if _as_int(args.rest[1]) is None:
                    sys.exit(f"error: clickAt y must be an integer, got {args.rest[1]!r}")
                req = {"cmd": "clickAt", "value": f"{args.rest[0]} {args.rest[1]}"}
            elif len(args.rest) >= 3 and _as_int(args.rest[0]) is None:
                if _as_int(args.rest[1]) is None or _as_int(args.rest[2]) is None:
                    sys.exit("error: clickAt <target> <x> <y> — x and y must be integers")
                req = {"cmd": "clickAt", "target": args.rest[0],
                       "value": f"{args.rest[1]} {args.rest[2]}"}
            else:
                sys.exit("error: clickAt needs <x> <y> or <target> <x> <y>")

        elif args.command == "pan":
            if not args.rest:
                sys.exit("error: pan needs <create|add|center|close|remove> [value]")
            req = {"cmd": "pan", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "layout":
            if not args.rest:
                sys.exit("error: layout needs <rearrange <id> | get>")
            req = {"cmd": "layout", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "scale":
            req = {"cmd": "scale"}
            if args.rest:
                req["value"] = args.rest[0]
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "resize":
            if len(args.rest) < 2:
                sys.exit("error: resize needs <w> <h> [target]")
            req = {"cmd": "resize", "value": f"{args.rest[0]} {args.rest[1]}"}
            if len(args.rest) > 2:
                req["target"] = " ".join(args.rest[2:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "connect":
            if not args.rest:
                sys.exit("error: connect needs <list|local|ip|wait> [args]")
            req = {"cmd": "connect", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "disconnect":
            print(json.dumps(bridge.request({"cmd": "disconnect"}), indent=2))

        elif args.command == "slice":
            if not args.rest:
                sys.exit("error: slice needs an action")
            req = {"cmd": "slice", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "dss":
            if not args.rest:
                sys.exit("error: dss needs <snapshot|reset|inject|scrollback|live> [pan] [args]")
            action = args.rest[0]
            req = {"cmd": "dss", "action": action}
            dss_args = args.rest[1:]
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
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "audioCapture":
            action = args.rest[0] if args.rest else "status"
            req = {"cmd": "audioCapture", "action": action}
            if len(args.rest) > 1:
                if action == "read" and len(args.rest) == 2:
                    req["path"] = args.rest[1]
                else:
                    req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command in ("record", "testtone", "tci"):
            # record <start|stop|status|path|dir [path]> | testtone <on [hz] [db]|off>
            # tci <start [port]|status|stop [abrupt]>
            req = {"cmd": args.command}
            if args.rest:
                req["action"] = args.rest[0]
                if len(args.rest) > 1:
                    req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "panmessage":
            if not args.rest:
                sys.exit("error: panmessage needs <add|remove|clear|list> <target> ...")
            req = {"cmd": "panmessage", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["target"] = args.rest[1]
            if args.rest[0] in ("add", "upsert"):
                if len(args.rest) < 5:
                    sys.exit("error: panmessage add needs <target> <id> <timeoutMs> <title|detail>")
                req["id"] = args.rest[2]
                try:
                    req["timeoutMs"] = int(args.rest[3])
                except ValueError:
                    sys.exit("error: panmessage add timeoutMs must be an integer")
                text_start = 4
                if len(args.rest) > text_start and args.rest[text_start].lower().startswith("tone="):
                    req["tone"] = args.rest[text_start].split("=", 1)[1].strip()
                    text_start += 1
                text = " ".join(args.rest[text_start:])
                title, sep, detail = text.partition("|")
                req["title"] = title.strip()
                req["detail"] = detail.strip() if sep else ""
            elif args.rest[0] in ("remove", "dismiss"):
                if len(args.rest) < 3:
                    sys.exit("error: panmessage remove needs <target> <id>")
                req["id"] = args.rest[2]
            print(json.dumps(bridge.request(req), indent=2))

        else:  # demo: produce the Phase-0 deliverables
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
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
