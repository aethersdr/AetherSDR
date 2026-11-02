#!/usr/bin/env python3
"""Receive the shim's diagnostic log lines (a build made with
build.py --log-url http://<this computer>:8765/) and append them to
shim-log.txt. The radio gives waveforms no other log channel.

    python3 -I logsink.py [--port 8765] [--bind ADDR] [--out shim-log.txt]
"""
import argparse
import http.server
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--bind', default='', help='address to listen on (default: all)')
    ap.add_argument('--out', default='shim-log.txt')
    a = ap.parse_args()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = max(0, min(int(self.headers.get('Content-Length') or 0), 1 << 20))
            body = self.rfile.read(n).decode(errors='replace')
            with open(a.out, 'a') as f:
                f.write(f'--- {time.strftime("%Y-%m-%dT%H:%M:%S")} from {self.client_address[0]}\n{body}')
            print(body, end='', flush=True)
            self.send_response(204)
            self.end_headers()

        def log_message(self, *args):
            pass

    print(f'listening on {a.bind or "*"}:{a.port}, appending to {a.out}')
    http.server.ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
