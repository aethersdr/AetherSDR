#!/usr/bin/env python3
"""Measure how often a FlexRadio and a PGXL each refresh the PGXL's telemetry.

Read-only. Runs two captures side by side for the same length of time:

  radio  TCP API session on port 4992: registers a UDP port with
         `client udpport`, subscribes to `amplifier all` and `meter all`, and
         logs every meter sample for the PGXL's handle plus each amplifier
         `state` change.
  pgxl   Port 9008 session: authenticates with `auth code=`, then sends
         `status` every 50 ms with one request in flight, and logs the fields
         the amplifier panel shows.

It sends nothing else: no control or transmit commands. The authorization code
is read with a non-echoing prompt (or the PGXL_CODE environment variable) and
is never printed. Key a transmit yourself while it runs, then run with
--analyze on the two logs to get the per-reading refresh rates.

  python3 tools/probe_pgxl_telemetry_sources.py \\
      --radio-host <RADIO_HOST> --pgxl-host <PGXL_HOST> --seconds 240
  python3 tools/probe_pgxl_telemetry_sources.py --analyze
"""
import argparse, getpass, os, re, select, socket, struct, threading, time

FIELDS = ("state", "fwd", "swr", "id", "temp", "hltemp", "vdd", "vac", "meffa")


def radio_capture(host, seconds, out, t0):
    def ts():
        return f"{time.monotonic() - t0:9.3f}"
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("", 0))
    tcp = socket.create_connection((host, 4992), timeout=5)
    tcp.setblocking(False)
    for i, cmd in enumerate((f"client udpport {udp.getsockname()[1]}",
                             "sub amplifier all", "sub meter all"), 1):
        tcp.sendall(f"C{i}|{cmd}\n".encode())
    pgxl, names, buf = None, {}, b""
    end = time.monotonic() + seconds
    with open(out, "w") as log:
        while time.monotonic() < end:
            ready, _, _ = select.select([tcp, udp], [], [], 0.5)
            if tcp in ready:
                data = tcp.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.decode("latin1")
                    m = re.search(r"amplifier (0x[0-9A-F]+) .*model=PowerGeniusXL", line)
                    if m:
                        pgxl = m.group(1)
                    if pgxl and "|meter " in line and f"num={pgxl}" in line:
                        for mm in re.finditer(r"(\d+)\.nam=([A-Z]+)", line):
                            names[int(mm.group(1))] = mm.group(2)
                    if pgxl and f"amplifier {pgxl}" in line and "state=" in line:
                        state = re.search(r"state=(\S+)", line).group(1)
                        log.write(f"{ts()} STATE {state}\n")
            if udp in ready:
                pkt, _ = udp.recvfrom(65536)
                # Flex meter packets: VITA-49, packet class 0x8002, then
                # (uint16 meter id, int16 raw value) pairs after a 28-byte header.
                if len(pkt) < 28 or struct.unpack(">I", pkt[12:16])[0] & 0xFFFF != 0x8002:
                    continue
                body = pkt[28:]
                for i in range(0, len(body) - 3, 4):
                    mid, raw = struct.unpack(">Hh", body[i:i + 4])
                    if mid in names:
                        log.write(f"{ts()} M {names[mid]} {raw}\n")
    tcp.close()


def pgxl_capture(host, code, seconds, out, t0):
    def ts():
        return f"{time.monotonic() - t0:9.3f}"
    s = socket.create_connection((host, 9008), timeout=5)
    s.setblocking(False)
    buf, authed, seq, in_flight, last = b"", False, 1, None, 0.0
    end = time.monotonic() + seconds
    with open(out, "w") as log:
        while time.monotonic() < end:
            now = time.monotonic()
            if authed and in_flight is None and now - last >= 0.050:
                seq += 1
                s.sendall(f"C{seq}|status\n".encode())
                in_flight, last = seq, now
            elif in_flight is not None and now - last > 1.0:
                in_flight = None
            ready, _, _ = select.select([s], [], [], 0.01)
            if not ready:
                continue
            data = s.recv(65536)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode("latin1").strip()
                if line.startswith("V"):
                    log.write(f"{ts()} VERSION {line}\n")
                    if line.endswith(" AUTH"):
                        s.sendall(b"C1|auth code=" + code.encode() + b"\n")
                    else:
                        authed = True
                elif line.startswith("R1|"):
                    authed = line == "R1|0|Authorized"
                    log.write(f"{ts()} AUTH {'accepted' if authed else 'rejected'}\n")
                    if not authed:
                        return
                elif line.startswith("R"):
                    head, _, body = line.partition("|")
                    kvs = dict(p.split("=", 1) for p in body.partition("|")[2].split() if "=" in p)
                    if int(head[1:]) == in_flight:
                        in_flight = None
                    log.write(f"{ts()} S " + " ".join(f"{k}={kvs.get(k, '-')}" for k in FIELDS) + "\n")
    s.close()


def changes(samples):
    """Times at which a (time, value) series changes value."""
    return [samples[i][0] for i in range(1, len(samples)) if samples[i][1] != samples[i - 1][1]]


def analyze(radio_log, pgxl_log):
    ev = [l.split() for l in open(radio_log)]
    states = [(float(e[0]), e[2]) for e in ev if len(e) > 2 and e[1] == "STATE"]
    meters = [(float(e[0]), e[2], int(e[3])) for e in ev if len(e) > 3 and e[1] == "M"]
    on = next(t for t, s in states if s.startswith("TRANSMIT"))
    off = next(t for t, s in states if t > on and not s.startswith("TRANSMIT"))
    print(f"Radio: transmit {on:.2f}-{off:.2f} s ({off - on:.1f} s)")
    for name in ("FWD", "RL", "DRV", "ID", "TEMP"):
        series = [(t, v) for t, n, v in meters if n == name and on <= t <= off]
        print(f"  {name:5} {len(series) / (off - on):5.1f} samples/s, "
              f"{len(changes(series)) / (off - on):5.1f} changes/s")
    rows = []
    for line in open(pgxl_log):
        p = line.split()
        if len(p) > 2 and p[1] == "S":
            rows.append((float(p[0]), dict(x.split("=", 1) for x in p[2:])))
    tx = [t for t, k in rows if k["state"].startswith("TRANSMIT")]
    a, b = tx[0], tx[-1]
    print(f"PGXL: transmit {a:.2f}-{b:.2f} s ({b - a:.1f} s)")
    for f in ("fwd", "swr", "id", "temp", "hltemp", "vdd"):
        series = [(t, k[f]) for t, k in rows if a <= t <= b]
        print(f"  {f:6} {len(series) / (b - a):5.1f} replies/s, "
              f"{len(changes(series)) / (b - a):5.1f} changes/s")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--radio-host")
    ap.add_argument("--pgxl-host")
    ap.add_argument("--seconds", type=float, default=240.0)
    ap.add_argument("--radio-log", default="radio_meters.log")
    ap.add_argument("--pgxl-log", default="pgxl_status.log")
    ap.add_argument("--analyze", action="store_true")
    args = ap.parse_args()
    if args.analyze:
        analyze(args.radio_log, args.pgxl_log)
        return
    if not args.radio_host or not args.pgxl_host:
        ap.error("--radio-host and --pgxl-host are required to capture")
    code = os.environ.get("PGXL_CODE") or getpass.getpass("PGXL authorization code: ")
    t0 = time.monotonic()
    threads = [
        threading.Thread(target=radio_capture, args=(args.radio_host, args.seconds, args.radio_log, t0)),
        threading.Thread(target=pgxl_capture, args=(args.pgxl_host, code, args.seconds, args.pgxl_log, t0)),
    ]
    for t in threads:
        t.start()
    print(f"Capturing for {args.seconds:.0f} s. Key a transmit, then wait for it to finish.")
    for t in threads:
        t.join()
    print(f"Wrote {args.radio_log} and {args.pgxl_log}. Run with --analyze to summarise.")


if __name__ == "__main__":
    main()
