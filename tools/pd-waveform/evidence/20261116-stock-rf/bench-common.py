"""Fixed-target bench controls; positive keying is only exposed by GUI Bridge."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parents[1]/'aetherpd-source/tools'))
from automation_probe import Bridge

def bridge(q):
    b = Bridge('/tmp/aetherpd-partial-release')
    try:
        result = b.request(q, 3)
        assert result.get('ok'), result
        return result
    finally:
        b.close()

def invoke(target, action='click', value=None):
    q = {'cmd':'invoke', 'target':target, 'action':action}
    if value is not None:
        q['value'] = value
    return bridge(q)

def api(command, identifier=None, transient=False):
    cmd = [str(ROOT/'bench-api'), command]
    if identifier:
        cmd.append(identifier)
    run = subprocess.run(cmd, capture_output=True, text=True, timeout=16)
    result = json.loads(run.stdout)
    if transient and command == 'status' and result['http_status'] == 503:
        return result
    assert run.returncode == 0 and result['complete'], result
    return result

def wait(f, seconds=15):
    end = time.monotonic()+seconds
    while True:
        value = f()
        if value:
            return value
        assert time.monotonic()<end, 'Timed out waiting for observed prerequisite'
        time.sleep(.1)

class Observer:
    """Read-only API subscription, with no transmit/configuration verbs."""
    def __init__(self):
        self.sock = socket.create_connection(('192.168.50.100',4992),2)
        self.sock.settimeout(.01)
        self.buffer = b''
        self.rows = []
        self.handle = None
        self.replies = {}
        self.apd = {}
        self.sampler = {}
        self.interlock = {}
        self.pump(.15)
        for i, command in enumerate(('client bind client_id=5D14B8D4-A745-5620-BAF8-B3614CA1C814','sub tx all','sub apd all'),1):
            self.sock.sendall(f'C{i}|{command}\n'.encode())
            end = time.monotonic()+2
            while i not in self.replies:
                assert time.monotonic()<end
                self.pump(.02)
            assert self.replies[i] == 0
        self.pump(.2)

    def pump(self, seconds=.02):
        end = time.monotonic()+seconds
        while time.monotonic()<end:
            try:
                data = self.sock.recv(32768)
                assert data, 'Observer disconnected'
                self.buffer += data
            except socket.timeout:
                continue
            assert len(self.buffer) <= 262144
            while b'\n' in self.buffer:
                line,self.buffer = self.buffer.split(b'\n',1)
                assert len(line)<=16384 and b'\0' not in line
                if line.startswith(b'H'):
                    self.handle = int(line[1:],16)
                elif line.startswith(b'R'):
                    parts = line.split(b'|',2)
                    if len(parts)==3:
                        self.replies[int(parts[0][1:])] = int(parts[1],16)
                elif line.startswith(b'S') and b'|' in line:
                    header,body = line.split(b'|',1)
                    if int(header[1:],16) not in (0,self.handle):
                        continue
                    text = body.decode('ascii')
                    if not text.startswith(('apd ','interlock ')) or text.startswith('interlock band '):
                        continue
                    fields = {}
                    for word in text.split():
                        if '=' in word:
                            key,value = word.split('=',1)
                            assert key not in fields
                            fields[key] = value
                    self.rows.append({'monotonic_ns':time.monotonic_ns(),'body':text})
                    assert len(self.rows)<=10000
                    if text.startswith('apd sampler ') and fields.get('tx_ant')=='ANT1':
                        self.sampler.update(fields)
                    elif text.startswith('apd ') and not text.startswith('apd sampler '):
                        self.apd.update(fields)
                    elif 'state' in fields:
                        self.interlock = fields

    def snapshot(self):
        self.pump(.01)
        return {'apd':dict(self.apd),'sampler':dict(self.sampler),'interlock':dict(self.interlock)}

    def close(self):
        self.sock.close()
