#!/usr/bin/env python3
"""Bounded public-identity parsing and durable migration binding; no sockets."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
import approval
import migration

CHECKER = None


class ReadinessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-readiness-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root / 'store'; self.store.mkdir(mode=0o700)
        self.plan = self.root / 'plan'
        self.raw = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bytes(range(32))),
                                    'a' * 64, 'b' * 64, bytes(range(32)))
        self.plan.write_bytes(self.raw); self.plan.chmod(0o600)
        self.input = self.root / 'input'
        self.status = dict(hostname='flx8600', tailnet_ip='100.122.118.62', dns_name='flx8600.example.ts.net',
                           provisioned=True, state='running', sessions=0, claimable=False)
        self.expected = b'flx8600\n100.122.118.62\nflx8600.example.ts.net\n'
        self.record = self.store / 'expected-identity'

    def invoke(self, command='json', data=None, expected=0, fault=''):
        if data is None:
            data = json.dumps(self.status).encode()
        self.input.write_bytes(data)
        args = [str(CHECKER), command, str(self.input)]
        if command in ('prepare', 'read'):
            args += [str(self.plan), str(self.store), fault]
        p = subprocess.run(args, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, expected, p.stderr.decode(errors='replace'))
        if not expected:
            self.assertEqual(p.stdout, self.expected)
        else:
            self.assertEqual(p.stdout, b'')

    def test_ready_status_and_nested_extensions(self):
        self.invoke()
        self.status['extension'] = dict(a=[True, False, None, 1.25e-4, {'unicode': 'é\n'}])
        self.invoke()

    def test_each_required_field_is_mandatory(self):
        for key in self.status:
            value = dict(self.status); del value[key]
            self.invoke(data=json.dumps(value).encode(), expected=1)

    def test_not_ready_states_refuse(self):
        for key, bad in [('state', 'starting'), ('state', 'error'), ('sessions', 1), ('sessions', -1),
                         ('sessions', 0.0), ('sessions', False), ('provisioned', False), ('claimable', True)]:
            value = dict(self.status); value[key] = bad
            self.invoke(data=json.dumps(value).encode(), expected=1)

    def test_duplicates_and_escaped_keys_refuse(self):
        body = json.dumps(self.status).encode()
        for key, value in self.status.items():
            self.invoke(data=body[:-1] + b',' + json.dumps(key).encode() + b':' + json.dumps(value).encode() + b'}', expected=1)
        self.invoke(data=body.replace(b'hostname', b'host\\u006eame'), expected=1)

    def test_malformed_and_trailing_json_refuse(self):
        body = json.dumps(self.status).encode()
        for data in [b'', b'{}', b'[]', body + b'{}', body[:-1] + b',}', body[:-1], body + b'\0',
                     body.replace(b'"sessions": 0', b'"sessions": 00'), body.replace(b'true', b'truex')]:
            self.invoke(data=data, expected=1)

    def test_identity_bounds_and_canonical_forms(self):
        for key, bad in [('hostname', ''), ('hostname', 'x' * 64), ('hostname', '-bad'), ('hostname', 'bad-'),
                         ('hostname', 'HasCapital'), ('hostname', 'a.b'), ('hostname', 'a\n'),
                         ('tailnet_ip', '192.168.1.2'), ('tailnet_ip', '100.63.255.255'), ('tailnet_ip', '100.128.0.1'),
                         ('tailnet_ip', '0100.64.1.2'), ('tailnet_ip', 'fd00::1'),
                         ('dns_name', ''), ('dns_name', 'a..b'), ('dns_name', 'a.-b'), ('dns_name', 'a-.b'),
                         ('dns_name', 'x' * 64 + '.net'), ('dns_name', 'x.' * 127), ('dns_name', '../other')]:
            value = dict(self.status); value[key] = bad
            self.invoke(data=json.dumps(value).encode(), expected=1)

    def test_extension_depth_and_size_are_bounded(self):
        body = json.dumps(self.status).encode()[:-1]
        self.invoke(data=body + b',"extra":' + b'[' * 18 + b'0' + b']' * 18 + b'}', expected=1)
        self.invoke(data=body + b',"extra":[' + b'0,' * 9000 + b'0]}', expected=1)
        self.invoke(data=b' ' * 65537, expected=1)
        for extra in [b'NaN', b'01', b'1.', b'1e', b'[1,]', b'{"a":}', b'"\\x"', b'"\\\0"']:
            self.invoke(data=body + b',"extra":' + extra + b'}', expected=1)

    def response(self, headers=b'', version=b'1.0', body=None):
        body = json.dumps(self.status).encode() if body is None else body
        return b'HTTP/' + version + b' 200 OK\r\nContent-Type: application/json\r\n' + headers + b'\r\n' + body

    def test_http_close_and_content_length(self):
        self.invoke('http', self.response())
        size = len(json.dumps(self.status).encode())
        self.invoke('http', self.response(b'Content-Length: ' + str(size).encode() + b'\r\n', version=b'1.1'))

    def test_http_wrong_status_encoding_and_duplicate_headers_refuse(self):
        for header in [b'Transfer-Encoding: chunked\r\n', b'transfer-encoding: identity\r\n',
                       b'Content-Length: 1\r\n', b'Content-Length: 999999999999\r\n',
                       b'Content-Length: 1\r\nContent-Length: 1\r\n', b'Content-Length: -1\r\n',
                       b'Content-Type: application/json\r\n', b' Content-Type: application/json\r\n']:
            self.invoke('http', self.response(header), expected=1)
        self.invoke('http', self.response().replace(b'200 OK', b'302 OK'), expected=1)
        self.invoke('http', self.response().replace(b'application/json', b'text/html'), expected=1)
        self.invoke('http', self.response(b'X-Large: ' + b'x' * 4096 + b'\r\n'), expected=1)

    def test_http_truncation_and_appended_data_refuse(self):
        body = self.response()
        for size in [0, 16, 17, 18, 40, len(body) - 1]:
            self.invoke('http', body[:size], expected=1)
        self.invoke('http', body + b'other', expected=1)
        self.invoke('http', body.replace(b'\r\n', b'\n'), expected=1)

    def test_persist_exact_binding_and_idempotency(self):
        self.invoke('prepare'); self.invoke('read')
        expected = b'PDREADY1' + self.raw + b'flx8600'.ljust(64, b'\0') + b'100.122.118.62'.ljust(16, b'\0') + b'flx8600.example.ts.net'.ljust(256, b'\0')
        self.assertEqual(self.record.read_bytes(), expected)
        self.assertEqual(self.record.stat().st_mode & 0o7777, 0o600)
        inode = self.record.stat().st_ino; self.invoke('prepare')
        self.assertEqual(inode, self.record.stat().st_ino)

    def test_changed_identity_and_migration_preserve_original(self):
        self.invoke('prepare'); original = self.record.read_bytes()
        self.status['hostname'] = 'changed'; self.invoke('prepare', expected=1)
        raw = bytearray(self.raw); raw[148] = ord('c'); self.plan.write_bytes(raw)
        self.invoke('read', expected=1); self.invoke('prepare', expected=1)
        self.assertEqual(self.record.read_bytes(), original)

    def test_record_metadata_and_padding_refuse(self):
        self.invoke('prepare'); self.record.chmod(0o644)
        self.invoke('read', expected=1); self.invoke('prepare', expected=1)
        self.record.chmod(0o600); os.link(self.record, self.root / 'alias')
        self.invoke('read', expected=1); (self.root / 'alias').unlink()
        raw = bytearray(self.record.read_bytes()); raw[379] = 1; self.record.write_bytes(raw)
        self.invoke('read', expected=1)

    def test_unknown_and_symlinked_records_are_preserved(self):
        self.record.write_bytes(b'foreign'); self.record.chmod(0o600)
        self.invoke('prepare', expected=1); self.assertEqual(self.record.read_bytes(), b'foreign')
        self.record.unlink(); self.record.symlink_to(self.plan)
        self.invoke('prepare', expected=1); self.invoke('read', expected=1)
        self.assertTrue(self.record.is_symlink())

    def test_process_death_at_publication_boundaries(self):
        for point in ['identity-file', 'identity-rename']:
            self.invoke('prepare', fault='kill:' + point, expected=-signal.SIGKILL)
            self.invoke('prepare'); self.invoke('read'); self.record.unlink()

    def test_partial_prefix_resume_and_foreign_preservation(self):
        self.invoke('prepare'); raw = self.record.read_bytes(); self.record.unlink()
        partial = self.store / 'expected-identity.new'
        for size in [0, 1, 8, 316, 380, 651, 652]:
            partial.write_bytes(raw[:size]); partial.chmod(0o600)
            self.invoke('prepare'); self.invoke('read'); self.record.unlink()
        partial.write_bytes(b'foreign'); partial.chmod(0o600)
        self.invoke('prepare', expected=1); self.assertEqual(partial.read_bytes(), b'foreign')

    def test_pending_record_blocks_read_or_existing_prepare(self):
        self.invoke('prepare')
        p = self.store / 'expected-identity.new'; p.symlink_to(self.root / 'missing')
        self.invoke('read', expected=1); self.invoke('prepare', expected=1)
        self.assertTrue(p.is_symlink())

    def test_unprotected_store_refuses(self):
        self.store.chmod(0o777); self.invoke('prepare', expected=1)
        self.assertEqual(list(self.store.iterdir()), [])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--checker', required=True, type=Path)
    args, remaining = p.parse_known_args(); CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *remaining])
