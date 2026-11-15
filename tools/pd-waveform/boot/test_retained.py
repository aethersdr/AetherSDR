#!/usr/bin/env python3
"""Terminal payload compaction and receipt-bound successor inspection."""
import argparse
import os
from pathlib import Path
import signal
import unittest
import bundle
import test_generation
import test_upgrade
import units


class RetainedTest(test_generation.GenerationTest):
    def terminal_pair(self,committed=True):
        self.generation=self.stage();self.begin();self.invoke('select')
        if committed:self.invoke('candidate')
        self.begin(4 if committed else 7)
        self.selected=self.generation/'pair'/('candidate' if committed else 'previous')
        self.unselected=self.generation/'pair'/('previous' if committed else 'candidate')
        self.receipts=self.store/'upgrade-retained'/bundle.blake(self.fixture.approved.read_bytes()).hex()

    def test_compaction_preserves_selected_bytes_and_boot_for_both_terminal_sides(self):
        for committed in [False,True]:
            with self.subTest(committed=committed):
                self.setUp();self.terminal_pair(committed);before=self.snapshot(self.selected)
                self.invoke('compact');self.invoke('retained-read')
                self.assertFalse((self.unselected/'payload.bundle').exists())
                self.assertEqual(before,self.snapshot(self.selected))
                self.assertEqual((self.receipts/'receipt').stat().st_size,1236)
                self.assertEqual(self.invoke('active')['choice'],int(committed))
                after=self.snapshot(self.store);self.invoke('compact');self.assertEqual(after,self.snapshot(self.store))

    def test_compacted_pair_requires_receipt_for_successor_but_boot_remains_complete(self):
        self.terminal_pair();self.invoke('compact');(self.receipts/'receipt').unlink()
        self.invoke('retained-read',expected=1);self.assertEqual(self.invoke('active')['choice'],1)
        self.invoke('compact',expected=1)

    def test_every_compaction_boundary_is_bootable_and_resumable(self):
        points=['receipt-bytes','receipt-file','receipt-rename','retained-durable:1',
                'retained-before-unlink:1','retained-unlinked:1','retained-complete:1']
        for point in points:
            with self.subTest(point=point):
                self.setUp();self.terminal_pair();before=self.snapshot(self.selected)
                self.invoke('compact',expected=-signal.SIGKILL,fault=point)
                self.assertEqual(self.invoke('active')['choice'],1)
                self.assertEqual(before,self.snapshot(self.selected));self.invoke('compact');self.invoke('retained-read')
                self.assertFalse((self.unselected/'payload.bundle').exists())

    def test_matching_partial_receipt_resumes_and_foreign_prefix_is_preserved(self):
        self.terminal_pair();self.invoke('compact',expected=-signal.SIGKILL,fault='retained-durable:1')
        receipt=(self.receipts/'receipt').read_bytes();(self.receipts/'receipt').unlink()
        for length in [0,8,16,363,372,803,804,1235,1236]:
            with self.subTest(length=length):
                self.fixture.write(self.receipts/'receipt.new',receipt[:length])
                self.invoke('compact',expected=-signal.SIGKILL,fault='retained-durable:1')
                self.assertEqual((self.receipts/'receipt').read_bytes(),receipt);(self.receipts/'receipt').unlink()
        self.fixture.write(self.receipts/'receipt.new',b'foreign');before=self.snapshot(self.store)
        self.invoke('compact',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_receipt_wrong_phase_approval_signature_and_metadata_refuse(self):
        self.terminal_pair();self.invoke('compact');path=self.receipts/'receipt';original=path.read_bytes()
        for offset in [0,16+32,371,803,1235]:
            changed=bytearray(original);changed[offset]^=1;self.fixture.write(path,changed)
            self.invoke('retained-read',expected=1);self.assertEqual(path.read_bytes(),changed)
        self.fixture.write(path,original);path.chmod(0o644);self.invoke('retained-read',expected=1);path.chmod(0o600)
        alias=self.root/'alias';os.link(path,alias);self.invoke('retained-read',expected=1);alias.unlink()
        path.unlink();self.fixture.write(alias,original);path.symlink_to(alias);self.invoke('retained-read',expected=1)

    def test_damaged_selected_or_retained_management_refuses(self):
        self.terminal_pair(False);self.invoke('compact')
        for path in [self.selected/'payload.bundle',self.unselected/'pd-boot',self.unselected/'release.header',self.unselected/'aetherpd-boot.service']:
            original=path.read_bytes();path.write_bytes(b'foreign');self.invoke('retained-read',expected=1)
            self.invoke('compact',expected=1);self.assertEqual(path.read_bytes(),b'foreign');path.write_bytes(original)
        foreign=self.unselected/'unknown';self.fixture.write(foreign,b'preserve');self.invoke('retained-read',expected=1)
        self.assertEqual(foreign.read_bytes(),b'preserve')

    def test_missing_nonpayload_retained_member_is_not_compaction(self):
        self.terminal_pair(False);self.invoke('compact');(self.unselected/'BOOT-LICENSES.txt').unlink()
        self.invoke('retained-read',expected=1);self.assertEqual(self.invoke('active')['choice'],0)

    def test_foreign_payload_before_compaction_is_preserved(self):
        self.terminal_pair();path=self.unselected/'payload.bundle';path.write_bytes(b'foreign')
        self.invoke('compact',expected=1);self.assertEqual(path.read_bytes(),b'foreign');self.assertFalse(self.receipts.exists())

    def test_foreign_receipt_inventory_prevents_removal(self):
        self.terminal_pair();self.receipts.mkdir(parents=True,mode=0o700)
        self.receipts.parent.chmod(0o700)
        self.fixture.write(self.receipts/'unknown',b'preserve');self.invoke('compact',expected=1)
        self.assertTrue((self.unselected/'payload.bundle').exists());self.assertEqual((self.receipts/'unknown').read_bytes(),b'preserve')

    def test_compaction_requires_terminal_selection_and_live_worker_lease(self):
        self.terminal_pair()
        for phase in [1,2,3,5,6,7]:
            self.begin(phase);self.invoke('compact',expected=1)
        self.begin(4);self.invoke('compact',expected=1,fault='no-lock')
        (self.root/'aetherpd-upgrade').rmdir();self.invoke('compact',expected=1)
        self.assertTrue((self.unselected/'payload.bundle').exists());self.assertFalse(self.receipts.exists())

    def test_pending_pair_selection_prevents_compaction(self):
        self.terminal_pair();self.fixture.write(self.generation/'control/active.new',b'')
        self.invoke('compact',expected=1);self.assertTrue((self.unselected/'payload.bundle').exists())
        self.assertFalse(self.receipts.exists())

    def test_next_upgrade_authenticates_compacted_commit_and_rollback(self):
        for committed in [False,True]:
            with self.subTest(committed=committed):
                self.setUp();self.terminal_pair(committed);self.invoke('compact')
                (self.root/'aetherpd-upgrade').rmdir()
                for name in ['pd-boot','BOOT-LICENSES.txt']:
                    self.fixture.write(self.store/name,self.fixture.files[1][name],0o755 if name=='pd-boot' else 0o644)
                for name in units.render('arm64'):
                    for directory in [self.store/'units',self.root/'units']:
                        self.fixture.write(directory/name,self.fixture.files[1][name],0o644)
                before=self.snapshot(self.generation);self.fixture.next_inputs(committed);self.stage()
                self.assertEqual(before,self.snapshot(self.generation))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',required=True,type=Path)
    p.add_argument('--upgrade-checker',required=True,type=Path)
    args,rest=p.parse_known_args();test_generation.CHECKER=args.checker.resolve()
    test_upgrade.CHECKER=args.upgrade_checker.resolve()
    unittest.main(argv=[__file__,*rest])
