#!/usr/bin/env python3
"""Native storage transactions: interruption, rollback and boundary behavior."""
import argparse
import fcntl
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import bundle

CHECKER = None
POINTS = ('payload-written', 'payload-synced', 'payload-renamed', 'payload-committed',
          'selector-written', 'selector-synced', 'selector-renamed',
          'selector-committed', 'retired-collected')


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='aetherpd-store-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root / 'store'
        self.store.mkdir(mode=0o700)
        (self.store / 'slots').mkdir(mode=0o700)
        self.seed = hashlib.sha256(b'host store tests only; never a release key').digest()
        self.key = self.root / 'public-key'
        self.key.write_bytes(Ed25519PrivateKey.from_private_bytes(self.seed).public_key().public_bytes_raw())
        self.files = {}
        for name in bundle.NAMES:
            raw = bytearray(4096)
            raw[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH', raw, 16, 2, 183)
            self.files[name] = bytes(raw) if name != 'LICENSES' else b'fixture license'
        self.packages = []
        for i in range(3):
            raw, _ = bundle.assemble(self.files, 64, bytes([i]) * 32, self.seed)
            path = self.root / f'input{i}.bundle'
            path.write_bytes(raw)
            self.packages.append(path)

    def call(self, operation='update', index=0, expected=0, fault='', space=None):
        args = [str(CHECKER), str(self.store), str(self.key), '64', operation,
                str(self.packages[index]), fault]
        if space is not None:
            args.append(str(space))
        result = subprocess.run(args, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))
        return result.stdout

    def selection(self):
        path = self.store / 'active'
        if not path.exists():
            return None, None
        raw = path.read_bytes()
        self.assertEqual(len(raw), 88)
        self.assertEqual(raw[:8], b'PDSEL002')
        values = []
        for pos in [8, 48]:
            if not raw[pos]:
                self.assertEqual(raw[pos:pos+40], bytes(40))
                values.append(None)
                continue
            data = (self.store / 'slots' / (chr(raw[pos]) + '.bundle')).read_bytes()
            self.assertEqual(bundle.blake(data), raw[pos+8:pos+40])
            self.assertEqual(raw[pos+1:pos+8], bytes(7))
            self.assertIn(data, [p.read_bytes() for p in self.packages])
            values.append(next(i for i,p in enumerate(self.packages) if p.read_bytes() == data))
        return tuple(values)

    def test_install_update_rollback_idempotency(self):
        self.call(); self.assertEqual(self.selection(), (0, None))
        old = (self.store / 'active').stat().st_ino
        self.call(); self.assertEqual((self.store / 'active').stat().st_ino, old)
        self.call(index=1); self.assertEqual(self.selection(), (1, 0))
        self.call(index=2); self.assertEqual(self.selection(), (2, 1))
        self.assertEqual(len(list((self.store / 'slots').glob('*.bundle'))), 2)
        self.call('rollback'); self.assertEqual(self.selection(), (1, 2))
        self.call('rollback'); self.assertEqual(self.selection(), (2, 1))

    def test_retirement_record_blocks_original_update_and_rollback(self):
        self.call();self.call(index=1)
        for name in ['slots-retired','slots-retired.new']:
            path=self.store/name;path.write_bytes(b'protected retirement marker')
            before={str(p.relative_to(self.store)):p.read_bytes() for p in self.store.rglob('*') if p.is_file()}
            self.call(index=2,expected=1);self.call('rollback',expected=1)
            self.assertEqual(before,{str(p.relative_to(self.store)):p.read_bytes() for p in self.store.rglob('*') if p.is_file()})
            path.unlink()

    def test_interrupted_update_preserves_current_and_rollback(self):
        for kind in ['crash:', 'fail:']:
            for point in POINTS:
                with self.subTest(kind=kind, point=point):
                    with tempfile.TemporaryDirectory(dir=self.root) as location:
                        saved = self.store; self.store = Path(location)
                        (self.store / 'slots').mkdir(mode=0o700)
                        self.call(); self.call(index=1)
                        self.call(index=2, fault=kind+point, expected=87 if kind=='crash:' else 1)
                        current, previous = self.selection()
                        self.assertIn((current, previous), [(1, 0), (2, 1)])
                        self.call(index=2)
                        self.assertEqual(self.selection(), (2, 1))
                        self.assertEqual(len(list((self.store/'slots').glob('*.bundle'))), 2)
                        self.store = saved

    def test_interrupted_first_install_recovers(self):
        for point in POINTS:
            with self.subTest(point=point), tempfile.TemporaryDirectory(dir=self.root) as location:
                saved = self.store; self.store = Path(location)
                (self.store/'slots').mkdir(mode=0o700)
                self.call(fault='crash:'+point, expected=87)
                self.assertIn(self.selection(), [(None, None), (0, None)])
                self.call(); self.assertEqual(self.selection(), (0, None)); self.store=saved

    def test_retry_sync_failure_preserves_unreferenced_authenticated_slot(self):
        self.call(); self.call(index=1)
        self.call(index=2, fault='crash:selector-renamed', expected=87)
        self.assertEqual(self.selection(), (2, 1))
        before = {p.name: p.read_bytes() for p in (self.store/'slots').iterdir()}
        self.assertEqual(len(before), 3)
        # Closing the actual store descriptor forces fsync to fail after all
        # temporary-file checks, before any unreferenced slot can be collected.
        self.call(index=2, fault='close:retry-selector-observed', expected=1)
        self.assertEqual({p.name: p.read_bytes() for p in (self.store/'slots').iterdir()}, before)
        self.assertEqual(self.selection(), (2, 1))
        self.call(index=2)
        self.assertEqual(self.selection(), (2, 1))
        self.assertEqual(len(list((self.store/'slots').iterdir())), 2)

    def test_bad_signature_and_space_preserve_state(self):
        self.call(); self.call(index=1)
        state = (self.store/'active').read_bytes()
        self.call(index=2, expected=1, space=8*1024*1024)
        self.assertEqual((self.store/'active').read_bytes(), state)
        raw = bytearray(self.packages[2].read_bytes());raw[840]^=1;self.packages[2].write_bytes(raw)
        self.call(index=2, expected=1)
        self.assertEqual((self.store/'active').read_bytes(), state)
        self.assertEqual(len(list((self.store/'slots').glob('*.bundle'))), 2)

    def test_signed_invalid_expansion_cannot_activate(self):
        self.call()
        raw = bytearray(self.packages[1].read_bytes());raw[88]^=1
        raw[840:904] = Ed25519PrivateKey.from_private_bytes(self.seed).sign(raw[:840])
        self.packages[1].write_bytes(raw)
        self.call(index=1, expected=1);self.assertEqual(self.selection(), (0, None))

    def test_links_and_foreign_files_refused(self):
        victim=self.root/'victim';victim.write_bytes(b'preserve')
        for name in ['incoming.new', 'A.bundle']:
            with self.subTest(name=name):
                path=self.store/'slots'/name;path.symlink_to(victim)
                self.call(expected=1);self.assertEqual(victim.read_bytes(), b'preserve');path.unlink()
        (self.store/'slots/A.bundle').write_bytes(b'foreign')
        self.call(expected=1);self.assertEqual((self.store/'slots/A.bundle').read_bytes(), b'foreign')
        self.assertFalse((self.store/'active').exists())

    def test_unsafe_directory_and_lock_contention(self):
        (self.store/'slots').chmod(0o777);self.call(expected=1)
        (self.store/'slots').chmod(0o700)
        with (self.store/'store.lock').open('wb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.call(expected=1)
        self.call()

    def test_selector_corruption_and_legacy_migration(self):
        self.call()
        p=self.store/'active';raw=p.read_bytes();p.write_bytes(b'PDSEL001'+raw[8:48])
        self.assertEqual(self.call('read').strip(), b'A -')
        self.call(index=1);self.assertEqual(self.selection(), (1, 0))
        for offset in [0, 8, 9, 48, 49]:
            raw=p.read_bytes();bad=bytearray(raw);bad[offset]=0xff;p.write_bytes(bad)
            self.call(index=2, expected=1);self.assertEqual(p.read_bytes(), bad);p.write_bytes(raw)

    def test_corrupt_current_can_roll_back_to_verified_previous(self):
        self.call();self.call(index=1)
        raw=(self.store/'active').read_bytes();path=self.store/'slots'/(chr(raw[8])+'.bundle')
        path.write_bytes(b'corrupt')
        self.call('rollback');self.assertEqual(self.selection(), (0, None))
        self.assertEqual(path.read_bytes(), b'corrupt')
        # Explicit outer repair is required before replacing a corrupt orphan.
        self.call(index=2, expected=1)

    def test_rollback_does_not_follow_replaced_current_link(self):
        self.call();self.call(index=1)
        raw=(self.store/'active').read_bytes();path=self.store/'slots'/(chr(raw[8])+'.bundle')
        path.unlink();victim=self.root/'foreign';victim.write_bytes(b'preserve');path.symlink_to(victim)
        self.call('rollback')
        self.assertEqual(self.selection(), (0, None))
        self.assertEqual(victim.read_bytes(), b'preserve');self.assertTrue(path.is_symlink())

    def test_missing_or_corrupt_rollback_refused(self):
        self.call();self.call('rollback', expected=1);self.call(index=1)
        raw=(self.store/'active').read_bytes();path=self.store/'slots'/(chr(raw[48])+'.bundle')
        path.write_bytes(b'corrupt');self.call('rollback', expected=1)
        self.assertEqual((self.store/'active').read_bytes(), raw)


if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker', type=Path, required=True)
    args, rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_store.py', *rest])
