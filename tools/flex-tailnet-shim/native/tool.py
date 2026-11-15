#!/usr/bin/env python3
"""Install and drive the native AetherTailnet shim on a FLEX-6000 from a LAN
computer, with nothing but Python 3.

    tool.py --host RADIO install AetherTailnet.ssdr_waveform
    tool.py --host RADIO status
    tool.py --host RADIO provision [--hostname NAME]        (sign in by link)
    tool.py --host RADIO provision --key-file KEY [--hostname NAME]
    tool.py --host RADIO login                              (the sign-in link again)
    tool.py --host RADIO [--tailnet-ip 100.x.y.z] watch --seconds 600 --out idle.jsonl
    tool.py --tailnet-ip 100.x.y.z session          (from a device on the tailnet)
    tool.py --host RADIO signout
    tool.py --host RADIO uninstall

The install is the legacy waveform upload FlexLib uses (Radio.cs: `file
filename`, `file upload <n> new_waveform`, then the bytes on the port the
radio names, 42607 if none). The provisioning API (TCP 48992) answers LAN
callers only.

Without --key-file, provision signs the radio in by link (shim 0.5.0): it
prints the sign-in link to open on a phone or in a browser, then waits until
the radio is on the tailnet. Whoever opens that link first adds the radio to
their tailnet, so don't paste it anywhere else. An auth key is read from a
file and never printed. The admin token the shim returns is saved to
admin-token.txt (mode 600) and sent as a Bearer token on later changes.
"""
import argparse
import json
import os
import pathlib
import socket
import sys
import time
import urllib.error
import urllib.request

NAME = 'AetherTailnet'
API_PORT, TELEMETRY_PORT = 48992, 48993
TOKEN_FILE = pathlib.Path('admin-token.txt')


class RadioApi:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 4992), 5)
        self.sock.settimeout(15)
        self.buf, self.seq = b'', 0
        while not self.line().startswith('H'):
            pass

    def line(self):
        while b'\n' not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('radio closed the API connection')
            self.buf += chunk
        l, _, self.buf = self.buf.partition(b'\n')
        return l.decode(errors='replace').strip()

    def command(self, cmd):
        self.seq += 1
        self.sock.sendall(f'C{self.seq}|{cmd}\n'.encode())
        while True:
            l = self.line()
            if l.startswith(f'R{self.seq}|'):
                _, status, *rest = l.split('|', 2)
                if int(status, 16) != 0:
                    raise RuntimeError(f'{cmd!r} failed: {l}')
                return rest[0] if rest else ''


def http(method, url, body=None, token=None, timeout=150):
    req = urllib.request.Request(url, method=method,
                                 data=None if body is None else json.dumps(body).encode())
    req.add_header('Content-Type', 'application/json')
    if token:
        req.add_header('Authorization', 'Bearer ' + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b'{}')
        except ValueError:
            return e.code, {}


def token():
    return TOKEN_FILE.read_text().strip() if TOKEN_FILE.exists() else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', help="the radio's LAN address")
    ap.add_argument('--tailnet-ip', help="the shim's tailnet address (for session)")
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('install'); p.add_argument('package', type=pathlib.Path)
    sub.add_parser('uninstall'); sub.add_parser('restart'); sub.add_parser('status')
    p = sub.add_parser('provision')
    p.add_argument('--key-file', type=pathlib.Path, help='join with this auth key instead of a sign-in link')
    p.add_argument('--hostname', default='flex-6000')
    p.add_argument('--no-share-discovered', action='store_true', help="don't share 4O3A devices the radio finds")
    p.add_argument('--wait', type=int, default=600, help='seconds to wait for the sign-in (link only)')
    sub.add_parser('login')
    p = sub.add_parser('watch')
    p.add_argument('--seconds', type=int, default=600); p.add_argument('--every', type=int, default=10)
    p.add_argument('--out', type=pathlib.Path, required=True)
    sub.add_parser('session'); sub.add_parser('signout')
    a = ap.parse_args()

    if a.cmd == 'session':
        if not a.tailnet_ip:
            sys.exit('session needs --tailnet-ip (run it from a device on the tailnet)')
        code, body = http('GET', f'http://{a.tailnet_ip}:{TELEMETRY_PORT}/v1/session', timeout=10)
        print(code, json.dumps(body, indent=2))
        return
    if not a.host:
        sys.exit('--host is required')
    base = f'http://{a.host}:{API_PORT}'

    if a.cmd == 'install':
        if a.package.name != f'{NAME}.ssdr_waveform':
            sys.exit(f'expected {NAME}.ssdr_waveform')
        data = a.package.read_bytes()
        api = RadioApi(a.host)
        api.command(f'file filename {a.package.name}')
        reply = api.command(f'file upload {len(data)} new_waveform')
        port = int(reply) if reply.strip().isdigit() else 42607
        time.sleep(0.2)
        with socket.create_connection((a.host, port), 5) as up:
            up.settimeout(120)
            up.sendall(data)
        print(f'sent {len(data)} bytes on port {port}; check `status` in a minute')
    elif a.cmd in ('uninstall', 'restart'):
        RadioApi(a.host).command(f'waveform {a.cmd} {NAME}')
        print(f'{a.cmd}: ok')
    elif a.cmd == 'status':
        code, body = http('GET', base + '/v1/status', timeout=10)
        print(code, json.dumps(body, indent=2))
    elif a.cmd == 'provision':
        req = {'hostname': a.hostname, 'allow': [], 'routes': [],
               'share_discovered': not a.no_share_discovered}
        if a.key_file:
            req['auth_key'] = a.key_file.read_text().strip()
            print('joining the tailnet with the key (can take a minute on this radio)…')
        else:
            print('asking the radio for a sign-in link…')
        code, body = http('POST', base + '/v1/provision', req, token())
        if code == 200 and body.get('admin_token'):
            fd = os.open(TOKEN_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as f:
                f.write(body['admin_token'] + '\n')
            body['admin_token'] = f'(saved to {TOKEN_FILE})'
        url = body.pop('auth_url', '')
        print(code, json.dumps(body, indent=2))
        if code != 200 or a.key_file:
            return
        if not url:
            code, lb = http('GET', base + '/v1/login', None, token(), timeout=10)
            url = lb.get('auth_url', '') if code == 200 else ''
        if url:
            print('\nOpen this link on your phone or in a browser, sign in to Tailscale and approve')
            print('the radio. Don\'t share it: whoever opens it first adds the radio to their tailnet.\n')
            print('    ' + url + '\n')
        else:
            print('no sign-in link yet; run `login` in a moment')
        end, last = time.time() + a.wait, ''
        while time.time() < end:
            code, st = http('GET', base + '/v1/status', timeout=10)
            state = st.get('state', '?')
            if state != last:
                print(time.strftime('%H:%M:%S'), state, st.get('tailnet_ip') or '', st.get('last_error') or '')
                last = state
            if state == 'running':
                print('on the tailnet; sign-in expires', st.get('key_expiry') or 'never')
                return
            time.sleep(3)
        print('still not signed in; run `login` for the link and `status` to check')
    elif a.cmd == 'login':
        code, body = http('GET', base + '/v1/login', None, token(), timeout=10)
        if code == 401:
            sys.exit('the radio refused admin-token.txt (or it is missing)')
        print(code, body.get('state'), 'expires', body.get('key_expiry') or 'never')
        if body.get('auth_url'):
            print('\nsign-in link (don\'t share it):\n    ' + body['auth_url'])
    elif a.cmd == 'signout':
        code, body = http('POST', base + '/v1/signout', {}, token())
        print(code, json.dumps(body, indent=2))
    elif a.cmd == 'watch':
        end = time.time() + a.seconds
        with a.out.open('a') as f:
            while time.time() < end:
                code, body = http('GET', base + '/v1/status', timeout=10)
                rec = {'t': time.strftime('%Y-%m-%dT%H:%M:%S'), 'code': code,
                       'state': body.get('state'), 'sessions': body.get('sessions'),
                       'last_error': body.get('last_error'), 'session_detail': body.get('session_detail')}
                # The shim's CPU and link are only reported over the tailnet,
                # to a caller its allowlist admits.
                if a.tailnet_ip:
                    scode, sess = http('GET', f'http://{a.tailnet_ip}:{TELEMETRY_PORT}/v1/session', timeout=10)
                    rec['session_code'] = scode
                    for k in ('shim_cpu_pct', 'shim_rss_kb', 'path', 'rtt_ms', 'to_client_kbps', 'from_client_kbps', 'sessions'):
                        rec[k if k != 'sessions' else 'session_streams'] = sess.get(k)
                f.write(json.dumps(rec) + '\n'); f.flush()
                print(rec['t'], code, rec['state'], rec['sessions'], rec.get('shim_cpu_pct'), rec.get('path'), rec['last_error'] or '')
                time.sleep(a.every)


if __name__ == '__main__':
    main()
