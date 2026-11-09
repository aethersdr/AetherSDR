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


def test_broadcast_reaches_a_listener():
    """The socket main() sends with may broadcast, and a packet it sends
    arrives at a listener as is (loopback stands in for the PC)."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("127.0.0.1", 0))
    listener.settimeout(2)
    out = discovery_helper.broadcast_socket("127.0.0.1")
    with listener, out:
        check(out.getsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST) != 0,
              "SO_BROADCAST is not set")
        packet = b"serial=1 ip=100.64.0.1"
        out.sendto(packet, listener.getsockname())
        got, _ = listener.recvfrom(4096)
        check(got == packet, f"listener got {got!r}")


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
    held.append(shim.accept()[0])   # connected, and never sends a frame
    time.sleep(0.5)                 # the helper is now waiting to read
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
