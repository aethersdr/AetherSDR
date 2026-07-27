#!/usr/bin/env python3
"""A killed AetherSDR must still release the Hermes-Lite 2.

An HL2 that is never told to stop keeps streaming at a host that is gone and
then stops answering discovery until it is physically power-cycled. Qt tears
nothing down on a signal, so Hl2Backend's destructor -- which does send the stop
-- never runs.

This stands in as the fake radio: bind a UDP socket, run a real MetisClient
against it, kill that process, and require a metis-stop datagram to arrive.

Deliberately an end-to-end process test rather than a unit test. Calling
fireEmergencyStop() directly would prove sendto() works; it would not prove the
handler is installed, that MetisClient armed it, or that the descriptor is still
usable at the moment the process dies -- which are the three things that can
actually be wrong.

Usage: hl2_signal_stop_test.py <path-to-hl2_signal_stop_child>
"""
import os
import signal
import socket
import subprocess
import sys
import time

# EF FE 04 <cmd>, 64 bytes. cmd bit 0 clear == stop; bit 7 is watchdog_disable,
# which MetisClient leaves clear (watchdog ENABLED) by default.
STOP_PREFIX = bytes([0xEF, 0xFE, 0x04])
START_CMD_BIT = 0x01

failures = 0


def check(ok, what):
    global failures
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        failures += 1


def drain(sock, seconds):
    """Collect every datagram that arrives within `seconds`."""
    out = []
    deadline = time.monotonic() + seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return out
        sock.settimeout(remaining)
        try:
            out.append(sock.recv(4096))
        except socket.timeout:
            return out


def run_case(child, sig, extra_args=()):
    """Kill the child with `sig`; return the datagrams seen afterwards."""
    radio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    radio.bind(("127.0.0.1", 0))
    port = radio.getsockname()[1]

    proc = subprocess.Popen([child, str(port), *extra_args],
                            stdout=subprocess.PIPE)
    try:
        # Wait for the client to be streaming before killing it -- killing it
        # mid-startup would test nothing.
        line = proc.stdout.readline().decode().strip()
        if line != "READY":
            check(False, f"{sig}: child failed to start (got {line!r})")
            return []
        pre = drain(radio, 1.0)
        check(any(d[:3] == STOP_PREFIX and (d[3] & START_CMD_BIT) for d in pre),
              f"{signal.Signals(sig).name}: radio saw metis-START before the kill")

        os.kill(proc.pid, sig)
        proc.wait(timeout=10)
        return drain(radio, 1.5)
    finally:
        if proc.poll() is None:
            proc.kill()
        radio.close()


def main():
    if len(sys.argv) < 2:
        print("usage: hl2_signal_stop_test.py <path-to-hl2_signal_stop_child>")
        return 2
    child = sys.argv[1]

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        name = signal.Signals(sig).name
        after = run_case(child, sig)
        stops = [d for d in after
                 if d[:3] == STOP_PREFIX and not (d[3] & START_CMD_BIT)]
        check(bool(stops),
              f"{name}: radio received metis-STOP after the process was killed")
        if stops:
            check(len(stops[0]) == 64, f"{name}: stop datagram is 64 bytes")
            # Watchdog must stay ENABLED (bit 7 clear). Disabling it here would
            # tell the gateware to keep streaming forever at a host that is
            # about to vanish -- the exact wedge this whole mechanism exists to
            # prevent.
            check(not (stops[0][3] & 0x80),
                  f"{name}: stop leaves the gateware watchdog enabled")

    # Negative control: the same kill with the handlers NOT installed must
    # leave the radio un-stopped. This is what makes the passes above mean
    # something -- it reproduces the original bug in the same harness.
    after = run_case(child, signal.SIGTERM, ("--no-handlers",))
    stops = [d for d in after
             if d[:3] == STOP_PREFIX and not (d[3] & START_CMD_BIT)]
    check(not stops,
          "control: WITHOUT the handlers a killed process sends no stop "
          "(i.e. this test can fail)")

    print()
    print("hl2_signal_stop_test: "
          + ("all checks passed" if not failures else f"{failures} FAILURES"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
