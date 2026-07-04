#!/usr/bin/env python3
"""Zombie-session simulator for #3977 eviction proof.

Connects to the FLEX TCP API, registers as program=AetherSDR with the
victim's station name, and replays `display pan set <pan> min_dbm=...`
every 0.5s against the victim's pan — the #3951 zombie signature.
RX-only: sends nothing but display pan set commands.

Exits when the radio drops the connection (i.e. the victim evicted us)
or after --max-writes.
"""
import argparse, socket, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--radio", default="192.168.1.203")
ap.add_argument("--station", required=True)
ap.add_argument("--pan", required=True, help="victim pan id, e.g. 0x40000000")
ap.add_argument("--gui-client-id", default="",
                help="register as a GUI client with this client_id (the "
                     "victim's own UUID = faithful relaunched-zombie repro)")
ap.add_argument("--bind-client-id", default="",
                help="register as a NON-GUI client bound to this GUI "
                     "client_id (avoids the radio's duplicate_client_id "
                     "self-heal so the client-side eviction can be proven)")
ap.add_argument("--max-writes", type=int, default=20)
args = ap.parse_args()

s = socket.create_connection((args.radio, 4992), timeout=10)
s.settimeout(2)
buf = b""
seq = 0
handle = None

class Evicted(Exception):
    pass

def send(cmd):
    global seq
    seq += 1
    line = f"C{seq}|{cmd}\n".encode()
    try:
        s.sendall(line)
    except (BrokenPipeError, ConnectionResetError, OSError):
        # A forced `client disconnect` usually surfaces as RST, not a clean
        # FIN — that's the eviction succeeding, not a test failure.
        print("CONNECTION RESET BY RADIO (during send)", flush=True)
        raise Evicted
    print(f"TX {line.decode().strip()}", flush=True)

def pump(duration):
    global buf, handle
    end = time.time() + duration
    while time.time() < end:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        except (ConnectionResetError, ConnectionAbortedError, OSError):
            print("CONNECTION RESET BY RADIO", flush=True)
            return False
        if not chunk:
            print("CONNECTION CLOSED BY RADIO", flush=True)
            return False
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode(errors="replace").strip()
            if t.startswith("H"):
                handle = t[1:]
                print(f"RX handle {handle}", flush=True)
            elif "disconnected" in t or t.startswith("M"):
                print(f"RX {t[:120]}", flush=True)
    return True

pump(2)
send("client program AetherSDR")
if args.gui_client_id:
    send(f"client gui {args.gui_client_id}")
if args.bind_client_id:
    send(f"client bind client_id={args.bind_client_id}")
send(f"client station {args.station}")
pump(1)

writes = 0
base = -117.0
try:
    while writes < args.max_writes:
        mn = base + (writes % 5) * 0.1
        send(f"display pan set {args.pan} min_dbm={mn:.2f} max_dbm={mn+90:.2f}")
        writes += 1
        if not pump(0.5):
            print(f"EVICTED after {writes} writes", flush=True)
            sys.exit(0)
except Evicted:
    print(f"EVICTED after {writes} writes", flush=True)
    sys.exit(0)

print(f"NOT evicted after {writes} writes", flush=True)
sys.exit(1)
