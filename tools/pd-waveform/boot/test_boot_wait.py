#!/usr/bin/env python3
"""Real transaction admission with simulated time/API availability; no sockets."""
import argparse
from pathlib import Path
import shutil
import subprocess
import unittest
import test_management
CHECKER=None

class BootWaitTest(unittest.TestCase):
    def setUp(self):
        self.fixture=test_management.ManagementTest(methodName='runTest');self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups);self.store=self.fixture.store;self.fixture.phase(3)
    def invoke(self,scenario,cold=1,owner=1,expected=0,probes=None):
        r=subprocess.run([str(CHECKER),str(self.store),scenario,str(cold),str(owner)],capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stderr);fields=r.stdout.split();self.assertEqual(int(fields[0]),expected,r.stdout)
        if probes is not None:self.assertEqual(int(fields[1]),probes,r.stdout)
        if expected:self.assertEqual(fields[3],'fixture-8600')
        else:self.assertEqual(len(fields),3,'failed wait exposed unvalidated identity')
        return int(fields[2]),int(fields[1])
    def test_immediate_and_delayed_identity_with_live_worker(self):
        self.invoke('immediate',expected=1,probes=1);self.invoke('delayed',expected=1,probes=4)
    def test_ordinary_committed_boot_waits(self):
        shutil.rmtree(self.fixture.root/'aetherpd-upgrade');(self.store/'upgrade-transaction').unlink()
        self.invoke('delayed',owner=0,expected=1,probes=4)
    def test_missing_worker_refuses_before_probe(self):self.invoke('immediate',owner=0,probes=0)
    def test_worker_loss_during_failed_or_successful_probe_refuses(self):
        self.invoke('worker-loss',probes=1);self.invoke('worker-loss-ready',probes=1)
    def test_phase_change_during_probe_refuses(self):self.invoke('phase-change',probes=1)
    def test_socket_owner_does_not_retry(self):
        self.invoke('delayed',cold=0,probes=1);self.invoke('immediate',cold=0,expected=1,probes=1)
    def test_fast_failures_and_full_probe_timeouts_are_bounded(self):
        for scenario in ['unavailable','slow-timeout']:
            elapsed,probes=self.invoke(scenario);self.assertLessEqual(elapsed,60000);self.assertGreater(probes,1)
    def test_clock_failure_backwards_and_probe_overrun_refuse(self):
        self.invoke('clock-error',probes=0);self.invoke('clock-backwards',probes=1);self.invoke('overrun',probes=1)
    def test_interrupted_sleep_rechecks_admission(self):self.invoke('interrupted',expected=1,probes=4)
    def test_invalid_mode_and_failed_sleep_refuse(self):
        self.invoke('immediate',cold=2,probes=0);self.invoke('sleep-error',probes=1)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    a,rest=p.parse_known_args();CHECKER=a.checker.resolve();unittest.main(argv=[__file__,*rest])
