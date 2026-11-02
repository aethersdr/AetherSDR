#!/usr/bin/env python3
"""Tests for the AetherHealth agent, run against a host build:

    python3 -I build.py --output OUT --host && python3 -I test_agent.py OUT/aether-health-host

Uses fixture /proc files in a temporary directory and loopback only.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]
failed = 0


def check(ok, label):
    global failed
    print(('[ OK ] ' if ok else '[FAIL] ') + label)
    failed += not ok


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def query(port):
    for _ in range(50):
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=2) as s:
                data = b''
                while chunk := s.recv(4096):
                    data += chunk
                return data.decode()
        except OSError:
            time.sleep(0.1)
    raise SystemExit('agent never answered')


def run(proc_dir, paths, stat_lines):
    port = free_port()
    with open(os.path.join(proc_dir, 'stat'), 'w') as f:
        f.write(stat_lines)
    p = subprocess.Popen([BIN, '-p', str(port), *paths],
                         env={'AETHER_HEALTH_PROC': proc_dir})
    try:
        return query(port)
    finally:
        p.kill()
        p.wait()


with tempfile.TemporaryDirectory() as d:
    stat = 'cpu  100 0 100 800 0 0 0 0\ncpu0 100 0 100 800 0 0 0 0\nintr 1\n'
    # A FLEX-6000 kernel (2.6.37) has no MemAvailable; SwapCached must not
    # be read as Cached.
    with open(os.path.join(d, 'meminfo'), 'w') as f:
        f.write('MemTotal:  480512 kB\nMemFree:  100000 kB\nBuffers:  20000 kB\n'
                'SwapCached:  999999 kB\nCached:  150000 kB\n')
    line = run(d, ['/nonexistent-path', d, 'bad"path'], stat)
    check(line.endswith('\n') and line.count('\n') == 1, 'one line per connection')
    r = json.loads(line)
    check(r['agent'] == '0.1.0', 'agent version')
    check(r['mem_total_kb'] == 480512, 'MemTotal')
    check(r['mem_available_kb'] == 270000 and r['mem_estimated'] is True,
          'no MemAvailable: estimated from MemFree + Buffers + Cached, not SwapCached')
    check(r['cpu_cores'] == 1, 'cores counted from cpuN lines')
    check(r['cpu_idle_pct'] == -1, 'idle is unknown until two samples')
    check([s['path'] for s in r['storage']] == [d],
          'unreadable and unquotable paths are skipped')
    check(r['storage'][0]['free_bytes'] > 0 and
          r['storage'][0]['total_bytes'] >= r['storage'][0]['free_bytes'], 'free space read')

    with open(os.path.join(d, 'meminfo'), 'w') as f:
        f.write('MemTotal:  2000000 kB\nMemFree:  1 kB\nMemAvailable:  1500000 kB\n')
    r = json.loads(run(d, [], stat))
    check(r['mem_available_kb'] == 1500000 and r['mem_estimated'] is False,
          'MemAvailable used when present')
    check(r['storage'] == [], 'no paths: empty storage list')

sys.exit(1 if failed else 0)
