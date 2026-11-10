#!/usr/bin/env python3
"""Checks for tools/flex-tailnet-shim/discovery_helper.py: the relay loop, run
over a real socket pair, and a real broadcast reaching a listener on 4992's
stand-in port on loopback."""

import logging
import os
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discovery_helper  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def frame(payload):
    return struct.pack("!H", len(payload)) + payload


def run_relay(data, send):
    """relay() over a socket pair fed with data, then closed; the exception
    it ended with."""
    shim, helper = socket.socketpair()
    with shim, helper:
        shim.sendall(data)
        shim.shutdown(socket.SHUT_WR)
        try:
            discovery_helper.relay(helper, send)
        except Exception as e:  # noqa: BLE001 - the ending is what is checked
            return e
    raise AssertionError("relay returned instead of ending with an error")


class Captured(logging.Handler):
    def __init__(self):
        super().__init__()
        self.lines = []

    def emit(self, record):
        self.lines.append(record.getMessage())


def test_relay():
    """Each announcement is sent as it arrives, anything else is skipped, a
    failed send is logged once and the relay carries on, and a frame longer
    than an announcement can be ends it."""
    data = (frame(b"model=FLEX-6500 serial=1 ip=100.64.0.1")
            + frame(b"not an announcement")
            + frame(b"serial=2 ip=100.64.0.1")   # its send fails
            + frame(b"serial=2b ip=100.64.0.1")  # and so does this one's
            + frame(b"serial=3 ip=100.64.0.1")
            + b"\x08\x01")                       # 2049 bytes: too long
    sent = []

    def send(packet):
        sent.append(packet)
        if packet.startswith(b"serial=2"):
            raise OSError("no route")

    captured = Captured()
    discovery_helper.log.addHandler(captured)
    try:
        err = run_relay(data, send)
    finally:
        discovery_helper.log.removeHandler(captured)
    check(isinstance(err, discovery_helper.FrameError) and "2049" in str(err),
          f"ended with {err!r}, want a bad length")
    check(sent == [b"model=FLEX-6500 serial=1 ip=100.64.0.1", b"serial=2 ip=100.64.0.1",
                   b"serial=2b ip=100.64.0.1", b"serial=3 ip=100.64.0.1"],
          f"sent {sent}")
    failures = [line for line in captured.lines if line.startswith("broadcasting:")]
    check(len(failures) == 1, f"send failures logged {len(failures)} times: {captured.lines}")


def test_relay_endings():
    """A zero length is malformed; a connection that drops mid-frame ends it."""
    err = run_relay(b"\x00\x00" + frame(b"serial=1"), lambda p: None)
    check(isinstance(err, discovery_helper.FrameError) and "length 0" in str(err),
          f"zero length: {err!r}")
    err = run_relay(frame(b"serial=1")[:5], lambda p: None)
    check(isinstance(err, EOFError), f"truncated frame: {err!r}")


def answer_hello(reply, close=True):
    """hello() against a fake shim that sends reply (then closes, if close);
    what the helper sent, and how hello() ended (None for OK)."""
    shim, helper = socket.socketpair()
    with shim, helper:
        helper.settimeout(0.3)
        shim.sendall(reply)
        if close:
            shim.shutdown(socket.SHUT_WR)
        try:
            discovery_helper.hello(helper)
            ended = None
        except Exception as e:  # noqa: BLE001 - the ending is what is checked
            ended = e
        shim.settimeout(1)
        return shim.recv(64), ended


def test_hello():
    """The helper opens with the hello and reads the shim's one-line answer:
    OK carries on, REFUSED says the shim's reason, and a shim that closes
    unanswered, answers with something else (a shim older than the hello
    sends frames at once) or says nothing each give a reason."""
    sent, ended = answer_hello(b"AETHER-DISCOVERY 1 OK\n" + frame(b"serial=1"), close=False)
    check(sent == b"AETHER-DISCOVERY 1\n", f"hello sent {sent!r}")
    check(ended is None, f"OK: {ended!r}")
    _, ended = answer_hello(b"AETHER-DISCOVERY 1 REFUSED 16 discovery helpers are already connected\n")
    check(isinstance(ended, discovery_helper.Refused)
          and str(ended) == "16 discovery helpers are already connected", f"REFUSED: {ended!r}")
    for reply, want in ((b"", "without answering"),
                        (b"AETHER-DISC", "partway"),
                        (frame(b"model=FLEX-6500 serial=1 ip=100.64.0.1"), "older than this helper"),
                        (b"AETHER-DISCOVERY 1 MAYBE\n", "unexpected answer"),
                        (b"AETHER-DISCOVERY 1 REFUSED " + b"A" * 300, "too long")):
        _, ended = answer_hello(reply)
        check(isinstance(ended, discovery_helper.NoAnswer) and want in str(ended),
              f"reply {reply[:20]!r}: {ended!r}, want NoAnswer about {want!r}")
    _, ended = answer_hello(b"", close=False)
    check(isinstance(ended, discovery_helper.NoAnswer) and "didn't answer the hello within" in str(ended),
          f"silent shim: {ended!r}")


def test_matches_the_shim():
    """The port and the hello are the ones announce.go serves."""
    import re
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "announce.go"), encoding="utf-8") as f:
        go = f.read()
    port = int(re.search(r"AnnounceFeedPort\s*=\s*(\d+)", go).group(1))
    prefix = re.search(r'helloPrefix\s*=\s*"([^"]*)"', go).group(1)
    version = re.search(r"helloVersion\s*=\s*(\d+)", go).group(1)
    check(discovery_helper.FEED_PORT == port, f"FEED_PORT {discovery_helper.FEED_PORT}, shim {port}")
    check(port not in (48994, 48995), f"feed port {port} is an on-radio agent's")
    check(discovery_helper.ANSWER_PREFIX == prefix, f"answer prefix {discovery_helper.ANSWER_PREFIX!r}, shim {prefix!r}")
    check(discovery_helper.HELLO == f"{prefix}{version}\n".encode(), f"hello {discovery_helper.HELLO!r}")


def test_broadcast_reaches_a_listener():
    """The socket main() sends with may broadcast, and a packet it sends
    arrives at a listener as is (loopback stands in for the PC)."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("127.0.0.1", 0))
    listener.settimeout(2)
    out = discovery_helper.broadcast_socket()
    with listener, out:
        check(out.getsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST) != 0,
              "SO_BROADCAST is not set")
        packet = b"serial=1 ip=100.64.0.1"
        out.sendto(packet, listener.getsockname())
        got, _ = listener.recvfrom(4096)
        check(got == packet, f"listener got {got!r}")


def test_default_broadcast():
    """On Windows the announcement stays on the PC (the loopback network);
    elsewhere, where a loopback broadcast reaches no one, it is the limited
    broadcast. --broadcast overrides it, and --from is gone."""
    check(discovery_helper.default_broadcast("win32") == "127.255.255.255", "Windows default")
    for platform in ("darwin", "linux"):
        check(discovery_helper.default_broadcast(platform) == "255.255.255.255",
              f"{platform} default")
    args = discovery_helper.parse_args(["--shim", "flex-radio"])
    check(args.broadcast == discovery_helper.default_broadcast(), f"parsed default {args.broadcast}")
    args = discovery_helper.parse_args(["--shim", "flex-radio", "--broadcast", "192.168.1.255"])
    check(args.broadcast == "192.168.1.255", f"override {args.broadcast}")
    try:
        import contextlib, io
        with contextlib.redirect_stderr(io.StringIO()):
            discovery_helper.parse_args(["--shim", "flex-radio", "--from", "100.64.0.2"])
    except SystemExit:
        pass
    else:
        raise AssertionError("--from was accepted")


def test_windows_loopback_broadcast_reaches_every_listener():
    """What the Windows default rests on: a broadcast to the loopback network
    reaches every program sharing the port, as SmartSDR, DAX and CAT do.
    Windows only; macOS delivers it to no one."""
    if sys.platform != "win32":
        return
    first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    first.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    first.bind(("0.0.0.0", 0))
    port = first.getsockname()[1]
    second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    second.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    second.bind(("0.0.0.0", port))
    out = discovery_helper.broadcast_socket()
    with first, second, out:
        first.settimeout(2)
        second.settimeout(2)
        packet = b"serial=1 ip=100.64.0.1"
        out.sendto(packet, (discovery_helper.default_broadcast(), port))
        for name, s in (("first", first), ("second", second)):
            got, _ = s.recvfrom(4096)
            check(got == packet, f"{name} listener got {got!r}")


def test_keepalive_is_on():
    """A connection that dies silently is noticed rather than waited on."""
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    accepted = []
    t = threading.Thread(target=lambda: accepted.append(server.accept()[0]))
    t.start()
    conn = socket.create_connection(server.getsockname())
    t.join()
    with server, conn, accepted[0]:
        discovery_helper.enable_keepalive(conn)
        check(conn.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE) != 0,
              "SO_KEEPALIVE is not set")


CTRL_C_WHILE_WAITING = r"""
import _thread, socket, sys, threading, time
sys.path.insert(0, sys.argv[1])
import discovery_helper

shim = socket.socket()
shim.bind(("127.0.0.1", 0))
shim.listen(1)
discovery_helper.FEED_PORT = shim.getsockname()[1]
held = []

def quiet_shim():
    conn = shim.accept()[0]
    held.append(conn)
    conn.recv(64)                       # the hello
    conn.sendall(b"AETHER-DISCOVERY 1 OK\n")  # and then never a frame
    time.sleep(0.5)                     # the helper is now waiting to read
    # Ctrl+C as Windows delivers it: Python is told, but a blocked socket
    # call isn't woken. (A POSIX SIGINT would wake it, hiding the problem.)
    _thread.interrupt_main()

threading.Thread(target=quiet_shim, daemon=True).start()
try:
    discovery_helper.main(["--shim", "127.0.0.1", "--broadcast", "127.0.0.1"])
except KeyboardInterrupt:
    print("stopped", flush=True)
"""


def test_ctrl_c_stops_a_waiting_helper():
    """Ctrl+C stops the helper while it waits for an announcement that never
    comes; on Windows it once had to be killed from Task Manager."""
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        done = subprocess.run([sys.executable, "-c", CTRL_C_WHILE_WAITING, here],
                              capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        raise AssertionError("Ctrl+C did not stop the helper within 10 s") from None
    check("stopped" in done.stdout, f"helper output: {done.stdout!r} {done.stderr!r}")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
