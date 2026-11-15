#!/usr/bin/env python3
"""Upgrade preflight and immutable pair staging; no service or radio operations."""
import argparse
import fcntl
import hashlib
import json
import os
import selectors
import shutil
import signal
import stat
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import approval
import bundle
import migration
import release
import units
import upgrade

CHECKER = None
FIRMWARE = '4.3.16.42034'
SERIAL = 'fixture-8600'


class UpgradeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-upgrade-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root/'store'
        for name in ['store', 'store/slots', 'store/units', 'units']:
            (self.root/name).mkdir(mode=0o700)
        self.seeds = [hashlib.sha256(('upgrade fixture only '+str(i)).encode()).digest() for i in range(2)]
        self.keys = [Ed25519PrivateKey.from_private_bytes(s).public_key().public_bytes_raw() for s in self.seeds]
        self.artifacts = []
        self.files = []
        for i in range(2):
            raw = bytearray(4096)
            raw[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH', raw, 16, 2, 183)
            raw[64] = i
            payload, _ = bundle.assemble({n: bytes(raw) if n != 'LICENSES' else b'fixture'
                                          for n in bundle.NAMES}, 64, bytes([i])*32, self.seeds[i])
            files = {'pd-boot': bytes(raw), 'pd-update': bytes(raw), 'payload.bundle': payload,
                     'BOOT-LICENSES.txt': b'fixture licenses',
                     **{n: text.encode() for n, text in units.render('arm64', bool(i)).items()}}
            artifact, _ = release.assemble(files, 'flex8600', FIRMWARE, bytes([i])*32, self.seeds[i], paired_boot=bool(i), managed_startup=bool(i))
            self.files.append(files)
            self.artifacts.append(artifact)
        self.paths = [self.root/'previous.release', self.root/'candidate.release']
        for p, data in zip(self.paths, self.artifacts):
            p.write_bytes(data)
        self.key = self.root/'bootstrap-key'
        self.key.write_bytes(self.keys[1])
        self.approved = self.root/'approved'
        self.good = upgrade.encode(1, FIRMWARE, SERIAL, *map(bundle.blake, self.artifacts), *self.keys)
        self.write(self.approved, self.good)
        self.write(self.store/'installed', b'PDINST01'+struct.pack('>II', 64, 1)+self.keys[0])
        for n in ['pd-boot', 'BOOT-LICENSES.txt']:
            self.write(self.store/n, self.files[0][n], 0o755 if n == 'pd-boot' else 0o644)
        for n in units.render('arm64'):
            for d in ['store/units', 'units']:
                self.write(self.root/d/n, self.files[0][n], 0o644)
        payload = self.files[0]['payload.bundle']
        self.write(self.store/'slots/A.bundle', payload)
        self.write(self.store/'active', b'PDSEL002A'+bytes(7)+bundle.blake(payload)+bytes(40))
        old_approval = approval.encode(1, FIRMWARE, SERIAL, bundle.blake(self.artifacts[0]))
        plan = migration.encode(old_approval, 'a'*64, 'b'*64, bytes(range(32)))
        self.write(self.store/'migration-approved', plan)
        self.write(self.store/'transaction', b'PDTXN001'+plan+struct.pack('>I', 5))
        self.write(self.store/'transaction.lock', b'')

    def write(self, path, data, mode=0o600):
        path.write_bytes(data)
        path.chmod(mode)

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.lstat().st_mode, p.lstat().st_ino,
                p.read_bytes() if p.is_file() else None)
                for base in [self.store, self.root/'units'] for p in base.rglob('*')}

    def call(self, expected=0, stage=False, identity=None, swap=None, reopen=False, resume=False, fault='',
             budget=False, space=None, inodes=None, pair_mode=None, origin=False):
        out = self.root/'stage'
        if stage or resume or budget:
            out.mkdir(mode=0o700, exist_ok=True)
        if pair_mode:
            (self.root/'pair').mkdir(mode=0o700, exist_ok=True)
        before = self.snapshot()
        args = [str(CHECKER), str(self.root), *map(str, self.paths), str(self.approved), str(self.key),
                identity or f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial={SERIAL}',
                pair_mode or ('origin' if origin else 'reopen' if reopen else 'budget' if budget else 'resume' if resume else 'stage' if stage else 'inspect'), str(out)]
        if swap is not None:
            replacement = self.root/'swap'
            replacement.write_bytes(swap)
            args.append(str(replacement))
        result = subprocess.run(args, capture_output=True, timeout=10,
                                env=dict(os.environ, PD_UPGRADE_TEST_FAULT=fault,
                                         PD_UPGRADE_TEST_SPACE='' if space is None else str(space),
                                         PD_UPGRADE_TEST_INODES='' if inodes is None else str(inodes)))
        self.assertEqual(self.snapshot(), before, 'preflight/staging changed the installation')
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))
        return json.loads(result.stdout) if budget or ((origin or pair_mode=='pair-read') and expected==0) else None

    def resign_candidate(self, files):
        raw, _ = release.assemble(files, 'flex8600', FIRMWARE, bytes([1])*32, self.seeds[1], managed_startup=True)
        self.paths[1].write_bytes(raw)
        self.write(self.approved, upgrade.encode(1, FIRMWARE, SERIAL,
                   bundle.blake(self.artifacts[0]), bundle.blake(raw), *self.keys))

    def test_key_transition_and_exact_pair_staged(self):
        self.call(stage=True)
        stage = self.root/'stage'
        self.assertEqual((stage/'approved').read_bytes(), self.good)
        for directory, files, artifact in zip(['previous', 'candidate'], self.files, self.artifacts):
            header = stage/directory/'release.header'
            self.assertEqual(header.read_bytes(), artifact[:release.PREFIX])
            self.assertEqual(header.stat().st_mode & 0o7777, 0o600)
            for name, data in files.items():
                self.assertEqual((stage/directory/name).read_bytes(), data)
            reconstructed = header.read_bytes()+b''.join((stage/directory/n).read_bytes() for n in release.NAMES)
            self.assertEqual(reconstructed, artifact)
        self.call(expected=1, stage=True)

    def terminal_predecessor(self, committed):
        self.call(stage=True)
        self.call(pair_mode='select-previous')
        if committed:
            self.call(pair_mode='select-candidate')
        (self.root/'stage').rename(self.store/'upgrade-pair')
        (self.root/'pair').rename(self.store/'upgrade-control')
        self.write(self.store/'upgrade-transaction', b'PDUTXN01'+self.good+struct.pack('>I', 4 if committed else 7))
        for name in ['pd-boot', 'BOOT-LICENSES.txt']:
            self.write(self.store/name, self.files[1][name], 0o755 if name=='pd-boot' else 0o644)
        for name in units.render('arm64'):
            for directory in [self.store/'units', self.root/'units']:
                self.write(directory/name, self.files[1][name], 0o644)
        self.next_inputs(committed)

    def next_inputs(self, committed):
        seed=hashlib.sha256(b'next upgrade fixture only').digest()
        key=Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw()
        raw=bytearray(self.files[1]['pd-boot']);raw[64]=2
        files=dict(self.files[1]);files['pd-boot']=bytes(raw)
        files['payload.bundle'],_=bundle.assemble(
            {n:bytes(raw) if n!='LICENSES' else b'fixture' for n in bundle.NAMES},64,bytes([2])*32,seed)
        artifact,_=release.assemble(files,'flex8600',FIRMWARE,bytes([2])*32,seed,paired_boot=True,managed_startup=True)
        selected=int(committed)
        self.paths[0].write_bytes(self.artifacts[selected]);self.paths[1].write_bytes(artifact)
        self.key.write_bytes(key)
        self.write(self.approved,upgrade.encode(1,FIRMWARE,SERIAL,
            bundle.blake(self.artifacts[selected]),bundle.blake(artifact),self.keys[selected],key))

    def test_next_generation_after_commit(self):
        self.terminal_predecessor(True)
        self.call(stage=True)
        self.assertEqual((self.root/'stage/previous/payload.bundle').read_bytes(),
                         self.files[1]['payload.bundle'])

    def test_next_generation_after_rollback(self):
        self.terminal_predecessor(False)
        self.call(stage=True)
        self.assertEqual((self.root/'stage/previous/payload.bundle').read_bytes(),
                         self.files[0]['payload.bundle'])

    def check_origin(self, predecessor, header):
        record=bytes.fromhex(self.call(origin=True)['origin'])
        self.assertEqual(record,b'PDORIG01'+bundle.blake(self.approved.read_bytes())+predecessor+header)

    def test_first_upgrade_origin_records_original_management(self):
        self.check_origin(bytes(364),self.artifacts[0][:release.PREFIX])

    def test_committed_origin_records_selected_candidate_management(self):
        self.terminal_predecessor(True)
        self.check_origin((self.store/'upgrade-transaction').read_bytes(),self.artifacts[1][:release.PREFIX])

    def test_rollback_origin_records_management_independent_of_payload(self):
        self.terminal_predecessor(False)
        self.check_origin((self.store/'upgrade-transaction').read_bytes(),self.artifacts[1][:release.PREFIX])

    def test_next_generation_refuses_wrong_selected_side(self):
        self.terminal_predecessor(True)
        p=self.store/'upgrade-control/active'
        raw=bytearray(p.read_bytes());raw[8]=0;raw[48:80]=bundle.blake(self.artifacts[0])
        self.write(p,raw)
        self.call(expected=1)

    def test_next_generation_cannot_revert_lineage_to_original_installation(self):
        self.terminal_predecessor(True)
        self.paths[0].write_bytes(self.artifacts[0])
        self.write(self.approved,upgrade.encode(1,FIRMWARE,SERIAL,
            bundle.blake(self.artifacts[0]),bundle.blake(self.paths[1].read_bytes()),
            self.keys[0],self.key.read_bytes()))
        self.call(expected=1)

    def test_next_generation_requires_journal_and_stage_agreement(self):
        self.terminal_predecessor(False)
        raw=bytearray(self.good);raw[148+76]^=1
        self.write(self.store/'upgrade-transaction',b'PDUTXN01'+raw+struct.pack('>I',7))
        self.call(expected=1)

    def test_next_generation_requires_actual_management_and_payload(self):
        self.terminal_predecessor(False)
        for p in [self.store/'pd-boot',self.store/'units/aetherpd-shim.service',
                  self.root/'units/aetherpd-boot.service',self.store/'upgrade-pair/previous/payload.bundle',
                  self.store/'upgrade-pair/candidate/release.header']:
            original=p.read_bytes();p.write_bytes(original[:-1]+bytes([original[-1]^1]))
            self.call(expected=1);p.write_bytes(original)
        self.call()

    def test_next_generation_requires_quiet_complete_terminal_state(self):
        self.terminal_predecessor(True)
        journal=self.store/'upgrade-transaction';original=journal.read_bytes()
        for phase in [1,2,3,5,6]:
            self.write(journal,original[:-1]+bytes([phase]));self.call(expected=1)
        self.write(journal,original)
        for p in [self.store/'upgrade-transaction.new',self.store/'upgrade-control/active.new']:
            self.write(p,b'');self.call(expected=1);p.unlink()
        capsule=self.root/'aetherpd-upgrade';capsule.mkdir(mode=0o700)
        self.call(expected=1);capsule.rmdir()
        self.call()

    def test_same_key_update(self):
        files = dict(self.files[1])
        files['payload.bundle'], _ = bundle.assemble(
            {n: self.files[1]['pd-boot'] if n != 'LICENSES' else b'fixture' for n in bundle.NAMES},
            64, bytes([1])*32, self.seeds[0])
        raw, _ = release.assemble(files, 'flex8600', FIRMWARE, bytes([1])*32, self.seeds[0], managed_startup=True)
        self.paths[1].write_bytes(raw)
        self.key.write_bytes(self.keys[0])
        self.write(self.approved, upgrade.encode(1, FIRMWARE, SERIAL,
                   bundle.blake(self.artifacts[0]), bundle.blake(raw), self.keys[0], self.keys[0]))
        self.call(stage=True)

    def test_unapproved_new_key_and_wrong_old_key_refused(self):
        self.key.write_bytes(self.keys[0]); self.call(expected=1)
        self.key.write_bytes(self.keys[1])
        bad = bytearray(self.good); bad[288] ^= 1
        self.write(self.approved, bad); self.call(expected=1)

    def test_wrong_target_and_mixed_pair_targets_refused(self):
        self.call(expected=1, identity=f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial=other')
        for offset in [8+11, 8+48, 8+76, 148+11, 148+48, 148+76]:
            bad = bytearray(self.good); bad[offset] ^= 1
            self.write(self.approved, bad); self.call(expected=1)

    def test_record_length_magic_and_same_release_refused(self):
        for data in [self.good[:-1], self.good+b'\0', b'x'+self.good[1:],
                     self.good[:148]+self.good[8:148]+self.good[288:]]:
            self.write(self.approved, data); self.call(expected=1)
        for i in [8+12, 148+12]:
            data = bytearray(self.good); data[i] = 1
            self.write(self.approved, data); self.call(expected=1)

    def test_changed_either_release_refused(self):
        for p, original in zip(self.paths, self.artifacts):
            for offset in [0, release.MANIFEST, len(original)-1]:
                bad = bytearray(original); bad[offset] ^= 1
                p.write_bytes(bad); self.call(expected=1); p.write_bytes(original)

    def test_actual_boot_payload_key_and_units_must_match_old_release(self):
        for relative in ['pd-boot', 'slots/A.bundle', 'installed', 'units/aetherpd-shim.service']:
            p = self.store/relative; original = p.read_bytes()
            p.write_bytes(original[:-1]+bytes([original[-1]^1])); self.call(expected=1); p.write_bytes(original)
        p = self.root/'units/aetherpd-boot.service'
        p.write_bytes(p.read_bytes()+b'\n'); self.call(expected=1)

    def test_selector_digest_and_foreign_slot_refused(self):
        p = self.store/'active'; original = p.read_bytes()
        bad = bytearray(original); bad[16] ^= 1
        p.write_bytes(bad); self.call(expected=1); p.write_bytes(original)
        slot = self.store/'slots/A.bundle'; data = slot.read_bytes(); slot.unlink()
        victim = self.root/'victim'; victim.write_bytes(data); slot.symlink_to(victim)
        self.call(expected=1); self.assertEqual(victim.read_bytes(), data)

    def test_only_committed_quiet_transaction_admitted(self):
        p = self.store/'transaction'; original = p.read_bytes()
        for phase in [1, 2, 3, 4, 6, 7]:
            p.write_bytes(original[:-1]+bytes([phase])); self.call(expected=1)
        p.write_bytes(original)
        with (self.store/'transaction.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB); self.call(expected=1)
        (self.root/'aetherpd-installation').mkdir(); self.call(expected=1)

    def test_metadata_links_and_nonregular_approval_refused(self):
        for mode in [0o644, 0o400, 0o4600]:
            self.approved.chmod(mode); self.call(expected=1)
        self.approved.chmod(0o600)
        alias = self.root/'alias'; os.link(self.approved, alias); self.call(expected=1); alias.unlink()
        self.approved.unlink(); os.mkfifo(self.approved, 0o600); self.call(expected=1)

    def test_unprotected_installed_directory_or_file_refused(self):
        for p, mode in [(self.store, 0o777), (self.root/'units', 0o777),
                        (self.store/'pd-boot', 0o4755), (self.store/'slots', 0o777)]:
            old = p.stat().st_mode; p.chmod(mode); self.call(expected=1); p.chmod(old)
        p = self.store/'pd-boot'; os.link(p, self.root/'alias'); self.call(expected=1)

    def test_changed_input_after_inspection_cannot_stage(self):
        self.call(expected=1, stage=True, swap=self.artifacts[0])
        self.assertFalse((self.root/'stage/candidate/aetherpd-shim.service').exists())

    def test_outer_signature_does_not_replace_inner_validation(self):
        files = dict(self.files[1]); raw = bytearray(files['payload.bundle']); raw[-1] ^= 1
        files['payload.bundle'] = bytes(raw); self.resign_candidate(files)
        self.call(); self.call(expected=1, stage=True)
        self.assertFalse((self.root/'stage/candidate/aetherpd-shim.service').exists())

    def test_nonempty_or_writable_stage_refused_without_deleting_foreign_state(self):
        stage = self.root/'stage'; stage.mkdir(mode=0o700)
        victim = stage/'foreign'; victim.write_bytes(b'preserve')
        self.call(expected=1, stage=True); self.assertEqual(victim.read_bytes(), b'preserve')
        victim.unlink(); stage.chmod(0o777); self.call(expected=1, stage=True)
        self.assertEqual(list(stage.iterdir()), [])

    def test_encoder_rejects_bad_keys_and_noop(self):
        for old, new in [(bytes(31), self.keys[1]), (self.keys[0], bytes(33))]:
            with self.assertRaises(ValueError):
                upgrade.encode(1, FIRMWARE, SERIAL, bytes(32), bytes([1])*32, old, new)
        with self.assertRaises(ValueError):
            upgrade.encode(1, FIRMWARE, SERIAL, bytes(32), bytes(32), *self.keys)

    def stage_snapshot(self):
        result = {}
        for p in (self.root/'stage').rglob('*'):
            metadata = p.lstat()
            data = p.read_bytes() if stat.S_ISREG(metadata.st_mode) else os.readlink(p) if p.is_symlink() else None
            result[str(p.relative_to(self.root/'stage'))] = (metadata.st_ino, metadata.st_mode, data)
        return result

    def reopen_refused(self, **kwargs):
        before = self.stage_snapshot()
        self.call(expected=1, reopen=True, **kwargs)
        self.assertEqual(self.stage_snapshot(), before)

    def test_reopen_without_original_inputs_or_installed_store(self):
        self.call(stage=True)
        for p in [*self.paths, self.approved]:
            p.unlink()
        self.store.rename(self.root/'old-store')
        self.call(reopen=True)

    def test_reopen_requires_actual_target_and_pinned_key(self):
        self.call(stage=True)
        self.reopen_refused(identity=f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial=other')
        self.key.write_bytes(self.keys[0]); self.reopen_refused()

    def test_reopen_checks_each_signed_header_and_member(self):
        self.call(stage=True)
        for directory in ['previous', 'candidate']:
            for name in ['release.header', *release.NAMES]:
                p = self.root/'stage'/directory/name
                original = p.read_bytes()
                data = bytearray(original); data[0] ^= 1
                p.write_bytes(data); self.reopen_refused(); p.write_bytes(original)
            p = self.root/'stage'/directory/'release.header'
            original = p.read_bytes()
            data = bytearray(original); data[release.MANIFEST] ^= 1
            p.write_bytes(data); self.reopen_refused(); p.write_bytes(original)

    def test_reopen_checks_whole_approval_not_only_member_signatures(self):
        self.call(stage=True)
        p = self.root/'stage/approved'; original = p.read_bytes()
        for offset in [24, 164]:
            data = bytearray(original); data[offset] ^= 1
            p.write_bytes(data); self.reopen_refused(); p.write_bytes(original)

    def test_reopen_revalidates_signed_inner_payload(self):
        self.call(stage=True)
        files = dict(self.files[1]); raw = bytearray(files['payload.bundle']); raw[-1] ^= 1
        files['payload.bundle'] = bytes(raw); self.resign_candidate(files)
        self.write(self.root/'stage/approved', self.approved.read_bytes())
        self.write(self.root/'stage/candidate/release.header', self.paths[1].read_bytes()[:release.PREFIX])
        self.write(self.root/'stage/candidate/payload.bundle', files['payload.bundle'], 0o644)
        self.reopen_refused()

    def test_reopen_preserves_unknown_missing_and_partial_files(self):
        self.call(stage=True)
        for name in ['foreign', 'previous/foreign', 'candidate/foreign']:
            p = self.root/'stage'/name; p.write_bytes(b'preserve')
            self.reopen_refused(); p.unlink()
        for name in ['approved', 'previous/release.header', 'candidate/pd-boot', 'previous/payload.bundle']:
            p = self.root/'stage'/name; original = p.read_bytes(); mode = p.stat().st_mode
            for length in [0, 1, len(original)-1]:
                p.write_bytes(original[:length]); self.reopen_refused()
            p.unlink(); self.reopen_refused(); self.write(p, original, mode)

    def test_reopen_refuses_links_modes_and_special_objects(self):
        self.call(stage=True)
        for name in ['approved', 'candidate/release.header', 'previous/pd-boot']:
            p = self.root/'stage'/name; data = p.read_bytes(); mode = p.stat().st_mode
            p.chmod(0o666); self.reopen_refused(); p.chmod(mode)
            alias = self.root/'alias'; os.link(p, alias); self.reopen_refused(); alias.unlink()
            p.unlink(); alias.write_bytes(data); p.symlink_to(alias); self.reopen_refused(); p.unlink(); alias.unlink()
            os.mkfifo(p, 0o600); self.reopen_refused(); p.unlink(); self.write(p, data, mode)
        p = self.root/'stage/candidate'; moved = self.root/'elsewhere'
        p.rename(moved); p.symlink_to(moved); self.reopen_refused()

    def test_fresh_process_after_every_staging_boundary(self):
        points = [('approval-written', 0), ('approval-synced', 0)]
        points += [(point, i) for i in range(2) for point in ['directory-created', 'header-written', 'release-written']]
        points += [('stage-synced', 0)]
        for point, index in points:
            with self.subTest(point=point, index=index):
                self.call(stage=True, expected=-signal.SIGKILL, fault=f'crash:{point}:{index}')
                if (point, index) in [('release-written', 1), ('stage-synced', 0)]:
                    self.call(reopen=True)
                else:
                    self.reopen_refused()
                shutil.rmtree(self.root/'stage')

    def test_reopen_sync_failure_cannot_report_durable_pair(self):
        self.call(stage=True)
        self.reopen_refused(fault='close:reopen-verified')
        self.call(reopen=True)

    def assert_pair(self):
        for directory, source in zip(['previous', 'candidate'], self.paths):
            base = self.root/'stage'/directory
            actual = (base/'release.header').read_bytes()+b''.join((base/n).read_bytes() for n in release.NAMES)
            self.assertEqual(actual, source.read_bytes())
        self.call(reopen=True)

    def resume_refused(self, **kwargs):
        before = self.stage_snapshot()
        self.call(expected=1, resume=True, **kwargs)
        self.assertEqual(self.stage_snapshot(), before, 'refused resume modified existing staged state')

    def test_resume_after_every_original_staging_boundary(self):
        points = [('approval-written', 0), ('approval-synced', 0), ('stage-synced', 0)]
        points += [(point, i) for i in range(2) for point in ['directory-created', 'header-written', 'release-written']]
        for point, index in points:
            with self.subTest(point=point, index=index):
                self.call(stage=True, expected=-signal.SIGKILL, fault=f'crash:{point}:{index}')
                self.call(resume=True); self.assert_pair()
                shutil.rmtree(self.root/'stage')

    def test_resume_every_approval_and_header_prefix(self):
        stage = self.root/'stage'
        for header in [False, True]:
            data = self.artifacts[0][:release.PREFIX] if header else self.good
            for length in range(len(data)+1):
                with self.subTest(header=header, length=length):
                    stage.mkdir(mode=0o700)
                    if header:
                        self.write(stage/'approved', self.good)
                        (stage/'previous').mkdir(mode=0o700)
                    self.write(stage/('previous/release.header' if header else 'approved'), data[:length])
                    self.call(resume=True); self.assert_pair()
                    shutil.rmtree(stage)

    def test_resume_member_prefixes_and_idempotence(self):
        self.call(stage=True)
        for directory, files in zip(['previous', 'candidate'], self.files):
            for name, data in files.items():
                p = self.root/'stage'/directory/name
                for length in [0, 1, len(data)//2, len(data)-1]:
                    self.write(p, data[:length])
                    inode = p.stat().st_ino
                    self.call(resume=True); self.assert_pair()
                    self.assertEqual(p.stat().st_ino, inode)
        before = self.stage_snapshot()
        self.call(resume=True)
        self.assertEqual(self.stage_snapshot(), before)

    def test_resume_missing_members_and_empty_directories(self):
        self.call(resume=True); self.assert_pair()
        for directory in ['previous', 'candidate']:
            for name in release.NAMES:
                (self.root/'stage'/directory/name).unlink()
                self.call(resume=True); self.assert_pair()
            shutil.rmtree(self.root/'stage'/directory)
            (self.root/'stage'/directory).mkdir(mode=0o700)
            self.call(resume=True); self.assert_pair()

    def test_resume_refuses_late_mismatch_before_early_repair(self):
        self.call(stage=True)
        early = self.root/'stage/previous/pd-boot'
        self.write(early, self.files[0]['pd-boot'][:1])
        for directory in ['previous', 'candidate']:
            for name in release.NAMES:
                p = self.root/'stage'/directory/name
                data = p.read_bytes(); mode = p.stat().st_mode
                self.write(p, data[:-1]+bytes([data[-1]^1]), mode)
                self.resume_refused(); self.write(p, data, mode)
        self.call(resume=True); self.assert_pair()

    def test_resume_refuses_inconsistent_order_and_final_partial_mode(self):
        self.call(stage=True)
        for name in ['approved', 'previous/release.header', 'candidate/release.header', 'candidate/pd-boot']:
            p = self.root/'stage'/name; data = p.read_bytes(); mode = p.stat().st_mode
            p.write_bytes(data[:1]); self.resume_refused()
            p.unlink();
            if name != 'candidate/pd-boot':
                self.resume_refused()
            self.write(p, data, mode)

    def test_resume_preserves_foreign_inventory_links_and_modes(self):
        self.call(stage=True)
        self.write(self.root/'stage/previous/pd-boot', b'')
        for name in ['foreign', 'previous/foreign', 'candidate/foreign']:
            p = self.root/'stage'/name; p.write_bytes(b'preserve')
            self.resume_refused(); p.unlink()
        for name in ['approved', 'candidate/release.header', 'candidate/pd-update']:
            p = self.root/'stage'/name; data = p.read_bytes(); mode = p.stat().st_mode
            p.chmod(0o666); self.resume_refused(); p.chmod(mode)
            alias = self.root/'alias'; os.link(p, alias); self.resume_refused(); alias.unlink()
            p.unlink(); alias.write_bytes(data); p.symlink_to(alias); self.resume_refused(); p.unlink(); alias.unlink()
            os.mkfifo(p, 0o600); self.resume_refused(); p.unlink(); self.write(p, data, mode)
        p = self.root/'stage/candidate'; moved = self.root/'elsewhere'
        p.rename(moved); p.symlink_to(moved); self.resume_refused()

    def test_resume_authenticates_private_input_snapshots(self):
        self.call(stage=True)
        self.write(self.root/'stage/previous/pd-boot', b'')
        self.resume_refused(swap=self.artifacts[0])
        self.paths[1].write_bytes(self.artifacts[1])
        self.call(resume=True, fault='replace-input-after-snapshot')
        self.assertEqual(self.paths[1].read_bytes(), b'changed')
        self.paths[1].write_bytes(self.artifacts[1]); self.assert_pair()

    def test_resume_rechecks_inner_payload(self):
        files = dict(self.files[1]); raw = bytearray(files['payload.bundle']); raw[-1] ^= 1
        files['payload.bundle'] = bytes(raw); self.resign_candidate(files)
        self.call(expected=1, resume=True)
        self.reopen_refused()

    def test_resume_fresh_process_after_each_file_and_mid_copy(self):
        files = dict(self.files[1]); files['pd-boot'] += bytes(60000)
        self.resign_candidate(files)
        points = [('resume-file-synced', i) for i in range(19)] + [('resume-copy', 11)]
        for point, index in points:
            with self.subTest(point=point, index=index):
                self.call(resume=True, expected=-signal.SIGKILL, fault=f'crash:{point}:{index}')
                if point == 'resume-copy':
                    p = self.root/'stage/candidate/pd-boot'
                    self.assertEqual(p.read_bytes(), files['pd-boot'][:16384])
                    self.assertEqual(p.stat().st_mode & 0o7777, 0o600)
                self.call(resume=True); self.assert_pair()
                shutil.rmtree(self.root/'stage')

    def test_resume_sync_failure_cannot_report_durable_pair(self):
        self.call(resume=True, expected=1, fault='close:reopen-verified')
        self.call(resume=True); self.assert_pair()

    def test_space_budget_counts_both_releases_and_reserve(self):
        budget = self.call(budget=True)
        block = os.statvfs(self.root/'stage').f_frsize
        sizes = [len(self.good), release.PREFIX, release.PREFIX]
        sizes += [len(data) for files in self.files for data in files.values()]
        expected = 8*1024*1024 + 3*block + sum(((size+block-1)//block)*block for size in sizes)
        self.assertEqual(budget['required_bytes'], expected)
        self.assertEqual(budget['required_inodes'], 21)
        for mode in ['stage', 'resume']:
            self.call(expected=1, space=expected-1, **{mode: True})
            self.assertEqual(list((self.root/'stage').iterdir()), [])
            self.call(space=expected, inodes=21, **{mode: True}); self.assert_pair()
            shutil.rmtree(self.root/'stage')

    def test_space_refusal_preserves_partial_stage_and_resume_credits_prefixes(self):
        original = self.call(budget=True)
        self.call(resume=True, expected=-signal.SIGKILL, fault='crash:resume-file-synced:10')
        partial = self.call(budget=True)
        self.assertLess(partial['required_bytes'], original['required_bytes'])
        self.assertEqual(partial['required_inodes'], 8)
        self.resume_refused(space=partial['required_bytes']-1)
        self.resume_refused(inodes=7)
        self.call(resume=True, space=partial['required_bytes'], inodes=8); self.assert_pair()
        finished = self.call(budget=True)
        block = os.statvfs(self.root/'stage').f_frsize
        self.assertEqual(finished['required_bytes'], 8*1024*1024+3*block)
        self.assertEqual(finished['required_inodes'], 0)
        before = self.stage_snapshot()
        self.call(resume=True, space=finished['required_bytes'], inodes=0)
        self.assertEqual(self.stage_snapshot(), before)

    def test_space_boundary_block_and_inode_exhaustion(self):
        self.call(stage=True)
        p = self.root/'stage/candidate/pd-boot'; data = p.read_bytes()
        self.write(p, data[:1])
        budget = self.call(budget=True)
        block = os.statvfs(self.root/'stage').f_frsize
        self.assertEqual(budget['required_bytes'], 8*1024*1024+3*block+((len(data)+block-1)//block)*block)
        self.resume_refused(space=budget['required_bytes']-1)
        self.call(resume=True, space=budget['required_bytes'], inodes=0); self.assert_pair()
        shutil.rmtree(self.root/'stage')
        self.call(expected=1, stage=True, inodes=20)
        self.assertEqual(list((self.root/'stage').iterdir()), [])

    def pair_prepare(self):
        self.call(stage=True)
        self.call(pair_mode='select-previous')

    def pair_assert(self, choice):
        actual = self.call(pair_mode='pair-read')
        self.assertEqual(actual, {'choice': choice, 'handoff': int(self.artifacts[choice][:8] in (b'AEPDR002',b'AEPDR003')),
                                 'boot': bundle.blake(self.files[choice]['pd-boot']).hex(),
                                 'payload': bundle.blake(self.files[choice]['payload.bundle']).hex(),
                                 'payload_key': self.keys[choice].hex()})

    def pair_snapshot(self):
        result = {}
        for p in (self.root/'pair').iterdir():
            metadata = p.lstat()
            result[p.name] = (metadata.st_ino, metadata.st_mode,
                p.read_bytes() if stat.S_ISREG(metadata.st_mode) else os.readlink(p) if p.is_symlink() else None)
        return result

    def pair_refused(self, mode='select-candidate', **kwargs):
        before = self.pair_snapshot()
        self.call(expected=1, pair_mode=mode, **kwargs)
        self.assertEqual(self.pair_snapshot(), before)

    def test_pair_initial_selection_transition_and_rollback(self):
        self.call(stage=True)
        self.call(expected=1, pair_mode='select-candidate')
        self.assertFalse((self.root/'pair/active').exists())
        self.call(pair_mode='select-previous'); self.pair_assert(0)
        self.call(pair_mode='select-candidate'); self.pair_assert(1)
        self.call(pair_mode='select-previous'); self.pair_assert(0)
        before = self.pair_snapshot(); self.call(pair_mode='select-previous')
        self.assertEqual(self.pair_snapshot(), before)

    def test_pair_survives_every_publication_boundary(self):
        self.pair_prepare()
        for point in ['pair-written', 'pair-synced', 'pair-renamed', 'pair-committed']:
            self.call(pair_mode='select-previous')
            self.call(pair_mode='select-candidate', expected=-signal.SIGKILL, fault=f'crash:{point}:1')
            self.pair_assert(0 if point in ['pair-written', 'pair-synced'] else 1)
            self.call(pair_mode='select-candidate'); self.pair_assert(1)
            self.assertFalse((self.root/'pair/active.new').exists())

    def test_pair_sync_failure_and_retry(self):
        self.pair_prepare()
        self.call(expected=1, pair_mode='select-candidate', fault='close:pair-sync')
        self.pair_assert(1)
        self.call(expected=1, pair_mode='select-candidate', fault='close:pair-sync')
        self.call(pair_mode='select-candidate'); self.pair_assert(1)

    def test_pair_initial_publication_crash_recovers_without_guessing(self):
        self.call(stage=True)
        for point in ['pair-written', 'pair-synced', 'pair-renamed', 'pair-committed']:
            self.call(pair_mode='select-previous', expected=-signal.SIGKILL, fault=f'crash:{point}:0')
            if point in ['pair-written', 'pair-synced']:
                self.call(expected=1, pair_mode='pair-read')
                self.call(expected=1, pair_mode='select-candidate')
            else:
                self.pair_assert(0)
            self.call(pair_mode='select-previous'); self.pair_assert(0)
            shutil.rmtree(self.root/'pair')

    def test_pair_matching_partial_intents_and_foreign_preservation(self):
        self.pair_prepare(); previous = (self.root/'pair/active').read_bytes()
        self.call(pair_mode='select-candidate'); candidate = (self.root/'pair/active').read_bytes()
        pending = self.root/'pair/active.new'
        for record in [previous, candidate]:
            for length in range(len(record)+1):
                self.write(pending, record[:length])
                self.call(pair_mode='select-previous'); self.pair_assert(0)
                self.assertFalse(pending.exists())
        for data in [b'foreign', candidate+b'\0', bytes([candidate[0]^1])+candidate[1:]]:
            self.write(pending, data); self.pair_refused()

    def test_pair_binds_record_to_pair_target_and_bootstrap_key(self):
        self.pair_prepare(); active = self.root/'pair/active'; original = active.read_bytes()
        for offset in [0, 8, 9, 16, 48, 79]:
            data = bytearray(original); data[offset] ^= 2
            active.write_bytes(data); self.pair_refused(mode='pair-read'); self.pair_refused()
            active.write_bytes(original)
        self.pair_refused(identity=f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial=other')
        self.key.write_bytes(self.keys[0]); self.pair_refused(); self.pair_refused(mode='pair-read')

    def test_pair_selected_bytes_revalidated_and_bad_candidate_rollback(self):
        self.pair_prepare(); self.call(pair_mode='select-candidate')
        candidate = self.root/'stage/candidate/pd-boot'; data = candidate.read_bytes()
        candidate.write_bytes(data[:-1]+bytes([data[-1]^1]))
        self.pair_refused(mode='pair-read')
        self.call(pair_mode='select-previous'); self.pair_assert(0)
        self.pair_refused()
        shutil.rmtree(self.root/'stage/candidate')
        self.call(pair_mode='select-previous'); self.pair_assert(0)
        previous = self.root/'stage/previous/payload.bundle'
        previous.write_bytes(previous.read_bytes()[:-1]); self.pair_refused(mode='pair-read')

    def test_pair_unselected_damage_does_not_break_selected_candidate(self):
        self.pair_prepare(); self.call(pair_mode='select-candidate')
        shutil.rmtree(self.root/'stage/previous')
        self.pair_assert(1)
        self.pair_refused(mode='select-previous')

    def test_pair_recovery_works_without_original_inputs_or_legacy_store(self):
        self.pair_prepare(); self.call(pair_mode='select-candidate')
        for p in [*self.paths, self.approved]:
            p.unlink()
        self.store.rename(self.root/'legacy-store')
        self.pair_assert(1); self.call(pair_mode='select-previous'); self.pair_assert(0)

    def test_pair_preserves_unsafe_control_files(self):
        self.pair_prepare()
        for name in ['active', 'lock', 'active.new']:
            p = self.root/'pair'/name
            if name == 'active.new': self.write(p, b'')
            data = p.read_bytes()
            p.chmod(0o644); self.pair_refused(); p.chmod(0o600)
            alias = self.root/'alias'; os.link(p, alias); self.pair_refused(); alias.unlink()
            p.unlink(); alias.write_bytes(data); p.symlink_to(alias); self.pair_refused(); p.unlink(); alias.unlink()
            os.mkfifo(p, 0o600); self.pair_refused(); p.unlink(); self.write(p, data)
        foreign = self.root/'pair/foreign'; foreign.write_bytes(b'preserve')
        self.pair_refused(); self.pair_refused(mode='pair-read')

    def test_pair_shared_reader_blocks_switch_until_release_or_death(self):
        self.pair_prepare()
        for killed in [False, True]:
            self.call(pair_mode='select-previous')
            args = [str(CHECKER), str(self.root), *map(str, self.paths), str(self.approved), str(self.key),
                    f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial={SERIAL}',
                    'pair-read', str(self.root/'stage')]
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=dict(os.environ, PD_UPGRADE_TEST_FAULT='hold:pair-reader'))
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(10), 'reader did not acquire lease')
                self.assertEqual(process.stdout.readline(), b'READY\n')
                self.pair_refused(); self.pair_assert(0)
                if killed:
                    process.kill(); process.communicate(timeout=10)
                    self.assertEqual(process.returncode, -signal.SIGKILL)
                else:
                    stdout, stderr = process.communicate(input=b'\n', timeout=10)
                    self.assertEqual(process.returncode, 0, stderr.decode())
                    self.assertEqual(json.loads(stdout)['choice'], 0)
                self.call(pair_mode='select-candidate'); self.pair_assert(1)
            finally:
                if process.poll() is None: process.kill()
                process.communicate(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--checker', type=Path, required=True)
    args, rest = parser.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
