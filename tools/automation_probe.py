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
    python tools/automation_probe.py grab SpectrumWidget /tmp/pan.png
    python tools/automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png
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
               "  automation_probe.py get slice active frequency\n"
               "  automation_probe.py invoke 'Master volume' setValue 35\n"
               "  automation_probe.py grab SpectrumWidget /tmp/pan.png\n"
               "  automation_probe.py grab pan-visible 1 /tmp/pan1-visible.png",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="demo",
                    choices=["demo", "ping", "dumpTree", "grab", "invoke", "get",
                             "connect", "disconnect"],
                    help="verb to run (default: demo = dumpTree + panadapter grab)")
    ap.add_argument("rest", nargs="*",
                    help="verb args: grab <target> [path] | grab pan-visible <index> [path] | "
                         "invoke <target> <action> [value] | "
                         "get <model> [selector] [property] | "
                         "connect <list|show|hide|local|ip|wait> [args]")
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

        elif args.command == "connect":
            if not args.rest:
                sys.exit("error: connect needs <list|local|ip|wait> [args]")
            req = {"cmd": "connect", "action": args.rest[0]}
            if len(args.rest) > 1:
                req["value"] = " ".join(args.rest[1:])
            print(json.dumps(bridge.request(req), indent=2))

        elif args.command == "disconnect":
            print(json.dumps(bridge.request({"cmd": "disconnect"}), indent=2))

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
