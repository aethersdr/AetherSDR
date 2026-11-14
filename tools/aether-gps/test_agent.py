#!/usr/bin/env python3
"""Tests for the AetherGPS relay, run against a host build:

    python3 -I build.py --output OUT --host && python3 -I test_agent.py OUT/aether-gps-host

A stand-in gpsd on loopback records what the relay sends it and plays back
reports. Loopback only; the relay is pinned to 127.0.0.1 and never publishes
to a tailnet shim.
"""
import json
import socket
import subprocess
import sys
import threading
import time

BIN = sys.argv[1]
failed = 0

VERSION = b'{"class":"VERSION","release":"3.22","rev":"3.22","proto_major":3,"proto_minor":14}\r\n'
SKY = (b'{"class":"SKY","device":"/dev/ttyGPS","nSat":2,"uSat":1,"satellites":['
       b'{"PRN":5,"gnssid":0,"svid":5,"el":41.0,"az":120.0,"ss":38.0,"used":true},'
       b'{"PRN":70,"gnssid":6,"svid":6,"el":12.0,"az":300.0,"ss":0.0,"used":false}]}\r\n')
TPV = b'{"class":"TPV","device":"/dev/ttyGPS","mode":3}\r\n'
NMEA = [b'$GPGSV,1,1,01,05,41,120,38*4A\r\n', b'$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*66\r\n']


def check(ok, label):
    global failed
    print(('[ OK ] ' if ok else '[FAIL] ') + label)
    failed += not ok


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class FakeGpsd:
    """Answers each connection with VERSION; after a ?WATCH, streams the
    reports (JSON or NMEA, as asked). Records every command it receives."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.commands = []
        self.lock = threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.session, args=(c,), daemon=True).start()

    def session(self, c):
        c.sendall(VERSION)
        buf = b''
        try:
            while chunk := c.recv(4096):
                buf += chunk
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    with self.lock:
                        self.commands.append(line)
                    if line.startswith(b'?WATCH'):
                        if b'"nmea":true' in line:
                            for _ in range(20):
                                for s in NMEA:
                                    c.sendall(s)
                                time.sleep(0.1)
                        else:
                            c.sendall(SKY + TPV)
                    elif line.startswith(b'?POLL'):
                        c.sendall(b'{"class":"POLL","active":1}\r\n')
        except OSError:
            # The relay under test drops sessions on purpose (refusals, idle
            # eviction, the ninth client); the stand-in just ends this one.
            pass
        c.close()

    def seen(self):
        with self.lock:
            return list(self.commands)


def connect(port):
    for _ in range(50):
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=3)
        except OSError:
            time.sleep(0.1)
    raise SystemExit('relay never listened')


def read_lines(s, want, timeout=3):
    s.settimeout(timeout)
    data = b''
    end = time.time() + timeout
    while data.count(b'\n') < want and time.time() < end:
        try:
            chunk = s.recv(65536)
        except (socket.timeout, ConnectionResetError):
            break
        if not chunk:
            break
        data += chunk
    return [json.loads(l) for l in data.split(b'\n') if l.strip()]


gpsd = FakeGpsd()
listen, udp = free_port(), free_port(socket.SOCK_DGRAM)
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(('127.0.0.1', udp))
rx.settimeout(3)
relay = subprocess.Popen([BIN, '-a', '127.0.0.1', '-b', '127.0.0.1', '-l', str(listen),
                          '-g', str(gpsd.port), '-u', str(udp), '-s', '0'])
try:
    s = connect(listen)
    first = read_lines(s, 1)
    check(first and first[0]['class'] == 'VERSION' and first[0]['release'] == '3.22',
          'a client is greeted by gpsd itself, unchanged')
    s.sendall(b'?WATCH={"enable":true,"json":true};')
    got = read_lines(s, 2)
    check([r['class'] for r in got] == ['SKY', 'TPV'], 'a watch streams gpsd reports through')
    check(len(got[0]['satellites']) == 2 and got[0]['satellites'][0]['used'] is True,
          'SKY arrives intact, satellites and all')

    s.sendall(b'?DEVICE={"path":"/dev/ttyGPS","bps":4800};\n')
    got = read_lines(s, 1)
    check(got and got[0]['class'] == 'ERROR' and 'read-only' in got[0]['message'],
          '?DEVICE is answered with an ERROR')
    s.sendall(b'  ?POLL ;\n?BOGUS;?DEVICES;')
    got = read_lines(s, 2)
    # The refusal is the relay's own and immediate; the POLL reply comes from
    # gpsd, so their order is not fixed.
    check(sorted(r['class'] for r in got) == ['ERROR', 'POLL'],
          'allowed and refused commands in one burst')
    time.sleep(0.3)
    cmds = gpsd.seen()
    check(not any(c.startswith(b'?DEVICE=') or c.startswith(b'?BOGUS') for c in cmds),
          'refused commands never reach gpsd')
    check(b'?POLL;' in cmds and b'?DEVICES;' in cmds and
          b'?WATCH={"enable":true,"json":true};' in cmds,
          'allowed commands reach gpsd, trimmed and terminated')

    # gpsd runs a command that follows a WATCH's JSON on the same line, so a
    # write must not ride inside an allowed command.
    for smuggled in (b'?WATCH={"enable":true}?DEVICE={"path":"/dev/ttyGPS","bps":4800};',
                     b'?WATCH={"enable":true} ?DEVICE={"bps":4800};',
                     b'?WATCH={"device":"?DEVICE"};',
                     b'?POLL={"x":1};', b'?DEVICES={"bps":4800};', b'?WATCH=junk;',
                     b'?WATCH={"enable":true}junk;'):
        s.sendall(smuggled + b'\n')
        got = read_lines(s, 1)
        check(got and got[0]['class'] == 'ERROR', 'refused: ' + smuggled.decode())
    time.sleep(0.3)
    check(not any(b'DEVICE=' in c or b'?DEVICE' in c.replace(b'?DEVICES', b'') or b'junk' in c
                  or b'"x"' in c for c in gpsd.seen()),
          'nothing smuggled reaches gpsd')
    s.sendall(b'?WATCH={"enable":true,"json":true,"device":"/dev/ttyGPS"};')
    got = read_lines(s, 2)
    check([r['class'] for r in got] == ['SKY', 'TPV'], 'a WATCH naming a device is still allowed')

    others = [connect(listen) for _ in range(7)]
    extra = connect(listen)
    check(all(read_lines(o, 1)[0]['class'] == 'VERSION' for o in others),
          'eight clients are served at once, each with its own gpsd session')
    check(read_lines(extra, 1, timeout=1) == [], 'a ninth is closed')
    for o in others + [extra]:
        o.close()

    s.sendall(b'?' + b'X' * 2000)
    time.sleep(0.3)
    check(read_lines(s, 1, timeout=1) == [], 'an overlong command drops the client')
    s.close()
    check(relay.poll() is None, 'the relay survives all of it')

    seen = set()
    end = time.time() + 5
    while time.time() < end and len(seen) < 2:
        try:
            seen.add(rx.recv(512))
        except socket.timeout:
            break
    check(seen == set(NMEA), 'NMEA sentences are broadcast on UDP, one per datagram')
    check(any(b'"nmea":true' in c for c in gpsd.seen()), 'the broadcast comes from an NMEA watch')
finally:
    relay.kill()
    relay.wait()

# A client that never sends a command gives its slot back.
relay = subprocess.Popen([BIN, '-a', '127.0.0.1', '-l', str(listen), '-g', str(gpsd.port),
                          '-u', '0', '-s', '0', '-i', '1500'])
try:
    idle = connect(listen)
    busy = connect(listen)
    busy.sendall(b'?WATCH={"enable":true,"json":true};')
    read_lines(idle, 1)
    read_lines(busy, 3)
    time.sleep(3)
    idle.settimeout(1)
    try:
        closed = idle.recv(1) == b''
    except (socket.timeout, ConnectionResetError):
        closed = False
    check(closed, 'a client that sends no command is closed after the idle limit')
    busy.sendall(b'?POLL;')
    check(any(r['class'] == 'POLL' for r in read_lines(busy, 1)),
          'a client that sent a command is kept')
    idle.close()
    busy.close()
finally:
    relay.kill()
    relay.wait()

# Without gpsd, a client is told so and closed.
dead = free_port()
relay = subprocess.Popen([BIN, '-a', '127.0.0.1', '-l', str(listen), '-g', str(dead),
                          '-u', '0', '-s', '0'])
try:
    s = connect(listen)
    got = read_lines(s, 1)
    check(got and got[0]['class'] == 'ERROR' and 'not running' in got[0]['message'],
          'no gpsd: the client gets an ERROR saying so')
finally:
    relay.kill()
    relay.wait()

sys.exit(1 if failed else 0)
