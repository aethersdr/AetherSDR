#!/usr/bin/env python3
"""
AetherSDR automation-bridge log/observability helper (issue #3646).

Companion to automation_probe.py. Drives the observability suite the bridge
exposes when launched with AETHER_AUTOMATION=1:

  log categories                 list logging categories + enabled state
  log get <cat>                  whether a category is enabled
  log set <cat> [<cat>...] <on|off>
                                 toggle one or more categories at runtime
  log set <cat>=<on|off>[;...]   batch form, a single shell token
  log set all <on|off>           toggle every category
  log reset                      restore the operator's persisted prefs
  log tail [n] [since=<seq>]     pull recent ring events (default newest 100);
                                 response carries oldest=<seq> still resident —
                                 if since < oldest, earlier events were evicted
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
        return self.tail_window(n, since).get("events", [])

    def tail_window(self, n=100, since=None):
        """Full tail response: {events, seq, oldest}.

        `oldest` is the lowest seq still resident in the ring (#3756). When
        `since` is given and `since < oldest`, events between `since` and
        `oldest` were evicted before this call — the returned window is a
        truncated suffix, not a complete bracket. Check `gap_after(...)`.
        """
        val = str(n) + (f" since={since}" if since is not None else "")
        return self.b.request({"cmd": "log", "action": "tail", "value": val})

    @staticmethod
    def gap_after(resp, since):
        """True if events logged after `since` were evicted before the tail."""
        oldest = resp.get("oldest")
        return oldest is not None and since is not None and since < oldest


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


class UsageError(Exception):
    """A CLI argument problem. Reported as one usage line, never a traceback."""


_TRUE_WORDS = ("on", "true", "1", "yes", "enable", "enabled")
_FALSE_WORDS = ("off", "false", "0", "no", "disable", "disabled")


def parse_onoff(word):
    w = word.strip().lower()
    if w in _TRUE_WORDS:
        return True
    if w in _FALSE_WORDS:
        return False
    raise UsageError("expected on|off, got %r" % word)


def parse_set_targets(rest):
    """`set` arguments -> [(category, enabled), ...].

    Two accepted forms, because both are reached for in practice (#4912):

        set <cat> [<cat> ...] <on|off>   the documented form, now also taking
                                         several categories against one state
        set "a=true;b=false"             the batch form — ONE shell token, which
                                         is exactly why the old blind rest[1]
                                         raised IndexError on it, and why the
                                         command silently did nothing even when
                                         it did not crash

    A '=' anywhere switches the whole call to pair parsing, so the two forms
    cannot mix into an ambiguous half-state.
    """
    if not rest:
        raise UsageError("set requires <cat> <on|off>, or <cat>=<on|off>[;...]")

    if any("=" in tok for tok in rest):
        pairs = []
        for tok in rest:
            for item in tok.split(";"):
                item = item.strip()
                if not item:
                    continue
                if "=" not in item:
                    raise UsageError("expected <cat>=<on|off>, got %r" % item)
                cat, _, state = item.partition("=")
                cat = cat.strip()
                if not cat:
                    raise UsageError("missing category name in %r" % item)
                pairs.append((cat, parse_onoff(state)))
        if not pairs:
            raise UsageError("set requires at least one <cat>=<on|off>")
        return pairs

    if len(rest) < 2:
        raise UsageError("set requires <cat> <on|off> (got only %r)" % rest[0])
    on = parse_onoff(rest[-1])
    return [(cat, on) for cat in rest[:-1]]


def parse_tail_args(rest):
    """`tail` arguments -> (count, since). Accepts [n] [since=<seq>], any order.

    The docstring has always advertised `since=<seq>`, but the old code ran
    int(rest[0]) on whatever came first, so `tail since=42` died with a
    ValueError traceback.
    """
    n = 100
    since = None
    seen_count = False
    for tok in rest:
        if tok.startswith("since="):
            value = tok[len("since="):]
            try:
                since = int(value)
            except ValueError:
                raise UsageError("since= expects an integer sequence, got %r" % value)
            continue
        if seen_count:
            raise UsageError("unexpected argument %r" % tok)
        try:
            n = int(tok)
        except ValueError:
            raise UsageError("tail expects a count, got %r" % tok)
        seen_count = True
    return n, since


def main():
    ap = argparse.ArgumentParser(description="AetherSDR bridge log/observability helper")
    ap.add_argument("action", choices=["categories", "get", "set", "reset",
                                        "tail", "mark", "watch"])
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--socket")
    ap.add_argument("--secs", type=float, default=3.0, help="watch duration")
    args = ap.parse_args()

    # Validate arity BEFORE touching the bridge, so a typo costs a usage line
    # rather than a socket connection and a traceback interleaved with whatever
    # else is writing to the terminal (#4912).
    try:
        set_targets = parse_set_targets(args.rest) if args.action == "set" else []
        tail_count, tail_since = (parse_tail_args(args.rest)
                                  if args.action == "tail" else (100, None))
        if args.action == "get":
            if len(args.rest) != 1:
                raise UsageError("get requires exactly one category")
        if args.action == "mark" and not args.rest:
            raise UsageError("mark requires the text to record")
    except UsageError as exc:
        sys.exit("error: %s\nusage: %s %s"
                 % (exc, ap.prog,
                    "categories | get <cat> | set <cat> <on|off> | "
                    "set <cat>=<on|off>[;...] | reset | tail [n] [since=<seq>] | "
                    "mark <text> | watch [cat ...]"))

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
        for cat, on in set_targets:
            print(json.dumps(c.set(cat, on)))
    elif args.action == "reset":
        print(json.dumps(c.reset()))
    elif args.action == "mark":
        print(json.dumps(c.mark(" ".join(args.rest))))
    elif args.action == "tail":
        for e in c.tail(tail_count, tail_since):
            print(f"{e['seq']:>5} {e['mono_us']:>10} {e['lvl']} {e['cat']}: {e['msg']}")


if __name__ == "__main__":
    main()
