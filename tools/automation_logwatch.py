#!/usr/bin/env python3
"""
AetherSDR automation-bridge log/observability helper (issue #3646).

Companion to automation_probe.py. Drives the observability suite the bridge
exposes when launched with AETHER_AUTOMATION=1:

  log categories                 list logging categories + enabled state
  log get <cat>                  whether a category is enabled
  log set <cat> <on|off>         toggle a category at runtime (no restart)
  log set all <on|off>           toggle every category
  log reset                      restore the operator's persisted prefs
  log tail [n] [since=<seq>]     pull recent ring events (default newest 100)
  log subscribe / unsubscribe    push: stream events as they occur
  mark <text>                    annotate the timeline; returns {seq, mono_us}

Two correlation patterns:

  * Pull (robust, single connection):
        seq = client.mark("START")["seq"]
        ... drive controls via the same or another connection ...
        events = client.tail(since=seq)        # everything logged in between

  * Push (live stream, dedicated connection):
        with LogStream(cats=["aether.protocol"]) as stream:
            ... drive controls on a separate Bridge ...
        events = stream.events                 # collected in a reader thread

Each event: {seq, mono_us, t:"HH:mm:ss.zzz", lvl:"D|I|W|C", cat, msg}.
mono_us is a process-monotonic microsecond stamp (jitter-grade, NTP-immune).
"""

import argparse
import json
import sys
import threading

from automation_probe import Bridge, discover_socket


class LogClient:
    """Convenience wrappers over a Bridge for the log/mark verbs."""

    def __init__(self, bridge):
        self.b = bridge

    def categories(self):
        return self.b.request({"cmd": "log", "action": "categories"}).get("categories", [])

    def get(self, cat):
        return self.b.request({"cmd": "log", "action": "get", "value": cat}).get("enabled")

    def set(self, cat, on=True):
        return self.b.request({"cmd": "log", "action": "set",
                               "value": f"{cat} {'on' if on else 'off'}"})

    def set_all(self, on=True):
        return self.b.request({"cmd": "log", "action": "set",
                               "value": f"all {'on' if on else 'off'}"})

    def reset(self):
        return self.b.request({"cmd": "log", "action": "reset"})

    def mark(self, text):
        return self.b.request({"cmd": "mark", "value": text})

    def tail(self, n=100, since=None):
        val = str(n) + (f" since={since}" if since is not None else "")
        return self.b.request({"cmd": "log", "action": "tail", "value": val}).get("events", [])


class LogStream:
    """Dedicated subscribe connection with a background reader thread.

    Streamed event lines are read on their own socket so they never collide
    with request/response traffic on a control connection.
    """

    def __init__(self, sock_path=None, cats=None, enable=True):
        self._path = sock_path or discover_socket()
        self.events = []
        self._stop = threading.Event()
        self._thread = None
        self._cats = cats or []
        self._enable = enable

    def __enter__(self):
        # Toggle requested categories via a short-lived control connection.
        if self._cats and self._enable:
            ctl = Bridge(self._path)
            for c in self._cats:
                ctl.request({"cmd": "log", "action": "set", "value": f"{c} on"})
            ctl.close()
        self._b = Bridge(self._path)
        ack = self._b.request({"cmd": "log", "action": "subscribe"})
        self.start_seq = ack.get("seq")
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        return self

    def _reader(self):
        while not self._stop.is_set():
            try:
                line = self._b._read_line_sock()
            except Exception:
                break
            if isinstance(line, dict) and line.get("type") == "log":
                self.events.append(line)

    def __exit__(self, *exc):
        self._stop.set()
        try:
            self._b.close()  # closing the socket unblocks the reader's recv
        except Exception:
            pass
        if self._thread:
            self._thread.join(timeout=1.0)


def main():
    ap = argparse.ArgumentParser(description="AetherSDR bridge log/observability helper")
    ap.add_argument("action", choices=["categories", "get", "set", "reset",
                                        "tail", "mark", "watch"])
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--socket")
    ap.add_argument("--secs", type=float, default=3.0, help="watch duration")
    args = ap.parse_args()

    path = args.socket or discover_socket()
    if not path:
        sys.exit("error: no bridge socket; launch with AETHER_AUTOMATION=1")

    if args.action == "watch":
        import time
        with LogStream(path, cats=args.rest or None) as s:
            time.sleep(args.secs)
        for e in s.events:
            print(f"{e['mono_us']:>10} {e['lvl']} {e['cat']}: {e['msg']}")
        print(f"-- {len(s.events)} events in {args.secs}s", file=sys.stderr)
        return

    c = LogClient(Bridge(path))
    if args.action == "categories":
        for cat in c.categories():
            print(f"{'x' if cat['enabled'] else ' '} {cat['id']:<28} {cat['label']}")
    elif args.action == "get":
        print(c.get(args.rest[0]))
    elif args.action == "set":
        print(json.dumps(c.set(args.rest[0], args.rest[1].lower() in ("on", "true", "1"))))
    elif args.action == "reset":
        print(json.dumps(c.reset()))
    elif args.action == "mark":
        print(json.dumps(c.mark(" ".join(args.rest))))
    elif args.action == "tail":
        n = int(args.rest[0]) if args.rest else 100
        for e in c.tail(n):
            print(f"{e['seq']:>5} {e['mono_us']:>10} {e['lvl']} {e['cat']}: {e['msg']}")


if __name__ == "__main__":
    main()
