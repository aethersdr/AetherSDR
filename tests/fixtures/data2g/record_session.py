#!/usr/bin/env python3
"""Record tests/fixtures/data2g/session-<version>.wav.

Runs two upstream data2g-host processes, N0CALL and W1AW, cross-connected
through named pipes (--audio-io), and records what a third station would hear:
a KISS broadcast, a connect, data both ways, a disconnect and both ID frames.
No radio and no sound card are involved, and nothing is transmitted.

    record_session.py /path/to/data2g-host OUTDIR

Writes OUTDIR/session.wav (8 kHz, 16-bit, mono). Linux or macOS; needs numpy.
Each take has its own session nonce and key, so a new recording needs the
expectations in tests/data2g_replay_test.cpp rewritten from data2g-monitor's
reading of it (see the README).
"""
import os, socket, subprocess, sys, threading, time, wave
import numpy as np
host, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
fifos = {n: os.path.join(out, n) for n in ("a_in", "a_out", "b_in", "b_out")}
for p in fifos.values():
    if os.path.exists(p): os.remove(p)
    os.mkfifo(p)
fd = {n: os.open(p, os.O_RDWR) for n, p in fifos.items()}
stop = False
def relay(src, dst, rec):
    with open(os.path.join(out, rec), "wb") as f:
        while not stop:
            d = os.read(fd[src], 65536)
            if not d: break
            f.write(d); f.flush(); os.write(fd[dst], d)
for a in (("a_out", "b_in", "a_tx.f32"), ("b_out", "a_in", "b_tx.f32")):
    threading.Thread(target=relay, args=a, daemon=True).start()
def start(tag, call, cmd, kiss):
    # The child keeps its own copy of the log descriptor; ours closes here.
    with open(os.path.join(out, f"{tag}.log"), "w") as log:
        return subprocess.Popen([host, "--mycall", call, "--command-port", str(cmd), "--kiss-port", str(kiss),
            "--audio-io", f"pipe:{fifos[tag+'_in']},{fifos[tag+'_out']}", "--no-realtime", "--log-level", "INFO"],
            stdout=log, stderr=subprocess.STDOUT)
A = start("a", "N0CALL", 18300, 18100); B = start("b", "W1AW", 18310, 18110)
t0 = time.time()
def conn(port):
    for _ in range(100):
        try: return socket.create_connection(("127.0.0.1", port))
        except OSError: time.sleep(0.1)
    raise SystemExit(f"port {port} never opened")
ac, ad, bc, bd, ak = conn(18300), conn(18301), conn(18310), conn(18311), conn(18100)
seen = {"a": [], "b": []}
def reader(tag, s):
    buf = b""
    while True:
        d = s.recv(4096)
        if not d: return
        buf += d
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            line = line.decode(errors="replace").strip()
            if line and not line.startswith(("BUFFER", "PTT", "BUSY", "IAMALIVE", "SN ", "BITRATE")):
                print(f"{time.time()-t0:6.1f} {tag}: {line}", flush=True)
            seen[tag].append(line)
for tag, s in (("a", ac), ("b", bc)): threading.Thread(target=reader, args=(tag, s), daemon=True).start()
def cmd(s, line): s.sendall(line.encode() + b"\r")
def wait(tag, prefix, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(l.startswith(prefix) for l in seen[tag]): return True
        time.sleep(0.1)
    print(f"TIMEOUT waiting for {tag} {prefix}", flush=True); return False
cmd(bc, "MYCALL W1AW"); cmd(bc, "LISTEN ON"); cmd(ac, "MYCALL N0CALL")
time.sleep(1.0)
# 1. a KISS broadcast: AX.25 UI N0CALL>APRS "!beacon"
ui = b""
for call, last in (("APRS", False), ("N0CALL", True)):
    ui += bytes((ord(c) << 1) for c in call.ljust(6)) + bytes([0x60 | last])
ui += b"\x03\xf0!beacon"
ak.sendall(b"\xc0\x00" + ui + b"\xc0")
time.sleep(4.0)
# 2. a connected session with data both ways
cmd(ac, "CONNECT N0CALL W1AW")
ok = wait("a", "CONNECTED", 60)
if ok:
    ad.sendall(b"Hello W1AW, this is the AetherSDR replay fixture.\r")
    time.sleep(1)
    bd.sendall(b"Hello N0CALL, copied.\r")
    got = b""; bd.settimeout(40)
    try:
        while b"fixture" not in got: got += bd.recv(4096)
    except OSError: pass  # timed out: print whatever arrived
    print(f"{time.time()-t0:6.1f} b data: {got!r}", flush=True)
    got = b""; ad.settimeout(40)
    try:
        while b"copied" not in got: got += ad.recv(4096)
    except OSError: pass  # timed out: print whatever arrived
    print(f"{time.time()-t0:6.1f} a data: {got!r}", flush=True)
    seen["a"].clear()
    cmd(ac, "DISCONNECT"); wait("a", "DISCONNECTED", 60)
time.sleep(6.0)
print(f"{time.time()-t0:6.1f} done", flush=True)
time.sleep(0.5)
for p in (A, B):
    p.terminate()
for p in (A, B):
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
time.sleep(0.5)
a = np.fromfile(os.path.join(out, "a_tx.f32"), dtype="<f4")
b = np.fromfile(os.path.join(out, "b_tx.f32"), dtype="<f4")
n = min(len(a), len(b))
mix = a[:n] + b[:n]
keyed = np.nonzero(np.abs(mix) > 1e-5)[0]
mix = mix[max(0, keyed[0] - 4000):min(n, keyed[-1] + 8000)]  # 0.5 s before the first burst, 1 s after the last
pcm = np.clip(np.round(mix * 32767), -32768, 32767).astype("<i2")
with wave.open(os.path.join(out, "session.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000); w.writeframes(pcm.tobytes())
print(f"{len(pcm) / 8000:.1f} s written to {os.path.join(out, 'session.wav')}", flush=True)
sys.stdout.flush(); os._exit(0)
