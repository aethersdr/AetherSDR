#!/usr/bin/env python3
"""List a tailnet radio in SmartSDR for Windows: the tailnet shim's discovery helper.

SmartSDR for Windows finds radios only by their discovery broadcast, and has
no connect-by-address. A tailnet carries no broadcast, and the copy the shim
sends each device reaches only one of the programs listening on UDP 4992
there, while SmartSDR, DAX and CAT each need it. So this helper runs on the
PC: it connects to the radio's tailnet shim, receives each announcement
(already naming the shim's tailnet address), and broadcasts it on the PC,
where every listener gets it.

On Windows the broadcast goes to the loopback network, so it never leaves the
PC: the announcement carries the radio's serial number and callsign, and other
devices on the PC's network couldn't use it anyway, since they can't reach the
tailnet address it names.

    python discovery_helper.py --shim flex-radio

It keeps no copy: when the shim can't be reached, nothing is broadcast, and
the radio drops out of the list as it would on a LAN. Python 3.8 or newer,
standard library only.
"""

import argparse
import logging
import socket
import struct
import sys
import threading
import time

# The shim's discovery helper port (announce.go: AnnounceFeedPort).
FEED_PORT = 48996
# The hello naming the feed version this helper speaks, and the start of the
# shim's one-line answer: "AETHER-DISCOVERY 1 OK" or "... REFUSED <reason>".
HELLO = b"AETHER-DISCOVERY 1\n"
ANSWER_PREFIX = "AETHER-DISCOVERY "
MAX_ANSWER = 256
HELLO_TIMEOUT = 10
# A refusal (another feed version, too many helpers) won't clear in seconds.
REFUSED_RETRY_SECONDS = 30
# Where clients listen for the radio's announcement.
ANNOUNCE_PORT = 4992
# An announcement is under 1 KB; the shim never frames more than this.
MAX_FRAME = 2048
RETRY_SECONDS = 5
CONNECT_TIMEOUT = 10
# How often the main thread wakes to notice Ctrl+C. On Windows, Ctrl+C can't
# interrupt a blocked socket call, and the helper waits on one indefinitely
# while the radio is quiet, so the network side runs in its own thread.
INTERRUPT_POLL_SECONDS = 0.5


def default_broadcast(platform=sys.platform):
    """Where the announcement is broadcast unless --broadcast says otherwise.

    Windows delivers a loopback broadcast to every program listening on the
    port, and it never leaves the PC; 255.255.255.255 also goes out on the
    PC's network. macOS delivers a loopback broadcast to no one, so other
    systems keep the limited broadcast."""
    return "127.255.255.255" if platform == "win32" else "255.255.255.255"

log = logging.getLogger("discovery-helper")


class FrameError(Exception):
    """The shim sent a frame length that can't be an announcement."""


class Refused(Exception):
    """The shim answered the hello with REFUSED; the message is its reason."""


class NoAnswer(Exception):
    """The shim didn't answer the hello as a shim that speaks it would."""


def hello(sock):
    """Send the hello and read the shim's answer. Returns when it is OK;
    raises Refused with the shim's reason, or NoAnswer."""
    sock.sendall(HELLO)
    prefix = ANSWER_PREFIX.encode()
    line = bytearray()
    try:
        while not line.endswith(b"\n"):
            if len(line) >= MAX_ANSWER:
                raise NoAnswer("the shim's answer to the hello is too long")
            chunk = sock.recv(1)
            if not chunk:
                raise NoAnswer(closed_unanswered(line))
            line += chunk
            # A shim older than the hello sends frames at once; say so on the
            # first byte that can't be an answer, not after MAX_ANSWER.
            if not prefix.startswith(bytes(line[:len(prefix)])):
                raise NoAnswer(NOT_A_HELLO_SHIM)
    except socket.timeout:
        raise NoAnswer(f"the shim didn't answer the hello within {HELLO_TIMEOUT} s; "
                       "it may be older than this helper") from None
    except ConnectionResetError:
        raise NoAnswer(closed_unanswered(line)) from None
    text = line.decode("utf-8", "replace").rstrip("\r\n")
    _version, _, verdict = text[len(ANSWER_PREFIX):].partition(" ")
    if verdict == "OK":
        return
    if verdict.startswith("REFUSED"):
        raise Refused(verdict[len("REFUSED"):].strip() or "no reason given")
    raise NoAnswer(f"unexpected answer to the hello: {text!r}")


NOT_A_HELLO_SHIM = "the shim didn't answer the hello; it may be older than this helper"


def closed_unanswered(line):
    if line:
        return "the shim closed the connection partway through its answer"
    return ("the shim closed the connection without answering: this device may not "
            "be allowed to use the radio (check Remote Access in AetherSDR)")


def read_exactly(sock, n):
    """n bytes from sock, or EOFError when the connection ends first."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed by the shim")
        buf += chunk
    return bytes(buf)


def relay(sock, send):
    """Read announcements from the shim (a 2-byte big-endian length, then the
    packet) and send each, until the connection fails. A frame that isn't an
    announcement is skipped; a malformed length ends the connection. A failed
    send is logged once until one succeeds, and the relay carries on."""
    started = failing = False
    while True:
        (size,) = struct.unpack("!H", read_exactly(sock, 2))
        if size == 0 or size > MAX_FRAME:
            raise FrameError(f"bad frame length {size}")
        packet = read_exactly(sock, size)
        if b"serial=" not in packet:
            continue
        try:
            send(packet)
        except OSError as e:
            if not failing:
                log.warning("broadcasting: %s", e)
                failing = True
            continue
        failing = False
        if not started:
            log.info("broadcasting the radio's announcement")
            started = True


def enable_keepalive(sock):
    """Notice a connection that died silently (a sleeping laptop, a lost
    path): the shim sends nothing while the radio is quiet, so a plain read
    would wait forever. Probes after 15 s idle, then every 5 s."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if hasattr(socket, "SIO_KEEPALIVE_VALS"):  # Windows
        sock.ioctl(socket.SIO_KEEPALIVE_VALS, (1, 15000, 5000))
        return
    for name, value in (("TCP_KEEPIDLE", 15), ("TCP_KEEPALIVE", 15),
                        ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
        if hasattr(socket, name):
            sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, name), value)


def broadcast_socket():
    """A UDP socket allowed to broadcast."""
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return out


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Broadcast a tailnet radio's discovery announcement on this computer.")
    parser.add_argument("--shim", required=True,
                        help="the radio's tailnet name or address, e.g. flex-radio")
    parser.add_argument("--broadcast", default=default_broadcast(),
                        help="address to broadcast the announcement to (default: %(default)s)")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s",
                        datefmt="%Y/%m/%d %H:%M:%S")

    try:
        out = broadcast_socket()
    except OSError as e:
        log.error("broadcast socket: %s", e)
        return 1
    worker = threading.Thread(target=serve, args=(args.shim, out, (args.broadcast, ANNOUNCE_PORT)),
                              name="relay", daemon=True)
    worker.start()
    while worker.is_alive():
        worker.join(INTERRUPT_POLL_SECONDS)
    return 1


def serve(shim, out, dst):
    """Connect to the shim, say hello, and relay its announcements,
    reconnecting for as long as the helper runs."""
    feed = f"{shim}:{FEED_PORT}"
    while True:
        try:
            conn = socket.create_connection((shim, FEED_PORT), timeout=CONNECT_TIMEOUT)
        except OSError as e:
            log.warning("cannot reach %s: %s; retrying", feed, e)
            time.sleep(RETRY_SECONDS)
            continue
        with conn:
            conn.settimeout(HELLO_TIMEOUT)
            try:
                hello(conn)
            except Refused as e:
                log.error("%s refused this helper: %s; retrying in %d s",
                          feed, e, REFUSED_RETRY_SECONDS)
                time.sleep(REFUSED_RETRY_SECONDS)
                continue
            except (NoAnswer, OSError) as e:
                log.warning("%s: %s; retrying", feed, e)
                time.sleep(RETRY_SECONDS)
                continue
            conn.settimeout(None)
            enable_keepalive(conn)
            log.info("connected to %s; broadcasting its announcements to %s:%d", feed, *dst)
            try:
                relay(conn, lambda packet: out.sendto(packet, dst))
                why = "ended"
            except (OSError, EOFError, FrameError) as e:
                why = str(e)
        log.warning("disconnected from %s: %s; retrying", feed, why)
        time.sleep(RETRY_SECONDS)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
