#!/usr/bin/env python3
"""Deterministic process-level tests for tools/radio_lock.py."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import radio_lock  # noqa: E402


def _append_event(path: Path, event: dict) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(event, sort_keys=True) + "\n")
        handle.flush()
        os.fsync(handle.fileno())


def _worker(argv: list[str]) -> int:
    action, root, label, events_path, ready_path, hold_seconds = argv
    coordinator = radio_lock.RadioCoordinator(
        Path(root),
        ticket_ttl_seconds=2.0,
        heartbeat_seconds=0.1,
        poll_seconds=0.02,
    )
    lease = coordinator.acquire(label, timeout_seconds=8.0)
    lease.start_heartbeat()
    _append_event(Path(events_path), {"event": "start", "label": label, "at": time.time()})
    Path(ready_path).touch()
    if action == "crash-with-child":
        command = [sys.executable, "-c", f"import time; time.sleep({float(hold_seconds)})"]
        subprocess.Popen(command, **lease.child_popen_kwargs())
        os._exit(0)
    time.sleep(float(hold_seconds))
    _append_event(Path(events_path), {"event": "end", "label": label, "at": time.time()})
    lease.release()
    return 0


class RadioLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aether-radio-lock-test-")
        self.root = Path(self.temporary.name) / "coordination"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def coordinator(self) -> radio_lock.RadioCoordinator:
        return radio_lock.RadioCoordinator(
            self.root,
            ticket_ttl_seconds=2.0,
            heartbeat_seconds=0.1,
            poll_seconds=0.02,
        )

    def spawn_worker(
        self,
        action: str,
        label: str,
        events: Path,
        ready: Path,
        hold_seconds: float,
    ) -> subprocess.Popen:
        return subprocess.Popen(
            [
                sys.executable,
                __file__,
                "--worker",
                action,
                str(self.root),
                label,
                str(events),
                str(ready),
                str(hold_seconds),
            ]
        )

    @staticmethod
    def wait_for(path: Path, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        while not path.exists():
            if time.monotonic() >= deadline:
                raise AssertionError(f"timed out waiting for {path}")
            time.sleep(0.01)

    def test_foreign_pid_visibility_is_never_consulted(self) -> None:
        coordinator = self.coordinator()
        with mock.patch("os.kill", side_effect=PermissionError("EPERM")) as kill_mock:
            lease = coordinator.acquire("eperm-proof", timeout_seconds=1.0)
            self.assertTrue(coordinator.status()["held"])
            lease.release()
        kill_mock.assert_not_called()

    def test_windows_child_kwargs_keep_default_handle_closure(self) -> None:
        lease = self.coordinator().acquire("windows-popen", timeout_seconds=1.0)
        was_inheritable = os.get_inheritable(lease.fileno)
        with mock.patch.object(radio_lock.os, "name", "nt"):
            self.assertEqual(lease.child_popen_kwargs(), {})
        self.assertEqual(os.get_inheritable(lease.fileno), was_inheritable)
        lease.release()

    def test_legacy_pidfile_and_queue_are_fail_closed_barriers(self) -> None:
        legacy_lock = Path(self.temporary.name) / "legacy.lock"
        legacy_queue = Path(self.temporary.name) / "legacy.queue"
        legacy_queue.mkdir()
        legacy_lock.write_text("48607 123 old-holder\n", encoding="utf-8")
        (legacy_queue / "25755").write_text("123 25755\n", encoding="utf-8")
        coordinator = radio_lock.RadioCoordinator(
            self.root,
            owner_lock_path=legacy_lock,
            legacy_queue_dir=legacy_queue,
            ticket_ttl_seconds=2.0,
            heartbeat_seconds=0.1,
            poll_seconds=0.02,
        )
        with mock.patch("os.kill", side_effect=PermissionError("EPERM")) as kill_mock:
            with self.assertRaises(TimeoutError):
                coordinator.acquire("v2-waiter", timeout_seconds=0.1)
        kill_mock.assert_not_called()
        self.assertEqual(legacy_lock.read_text(), "48607 123 old-holder\n")
        self.assertTrue((legacy_queue / "25755").exists())
        legacy_lock.unlink()
        (legacy_queue / "25755").unlink()
        lease = coordinator.acquire("v2-owner", timeout_seconds=1.0)
        compatibility = legacy_lock.read_text(encoding="utf-8")
        self.assertTrue(compatibility.startswith("0 "))
        self.assertIn(f"aethersdr-radio-v2:{lease.owner_id}:", compatibility)
        lease.release()
        self.assertFalse(legacy_lock.exists())

    def test_empty_legacy_claim_is_not_overwritten_during_creation(self) -> None:
        legacy_lock = Path(self.temporary.name) / "legacy-racing.lock"
        legacy_queue = Path(self.temporary.name) / "legacy-racing.queue"
        legacy_queue.mkdir()
        legacy_lock.touch()
        coordinator = radio_lock.RadioCoordinator(
            self.root,
            owner_lock_path=legacy_lock,
            legacy_queue_dir=legacy_queue,
            ticket_ttl_seconds=2.0,
            heartbeat_seconds=0.1,
            poll_seconds=0.02,
        )
        with self.assertRaises(TimeoutError):
            coordinator.acquire("v2-lost-create-race", timeout_seconds=0.1)
        self.assertEqual(legacy_lock.read_bytes(), b"")

    def test_non_owner_cannot_acquire_or_release_live_owner(self) -> None:
        owner = self.coordinator().acquire("owner", timeout_seconds=1.0)
        other = self.coordinator()
        with self.assertRaises(TimeoutError):
            other.acquire("intruder", timeout_seconds=0.1)
        self.assertTrue(other.status()["held"])
        self.assertEqual(other.status()["owner"]["ownerId"], owner.owner_id)
        owner.release()
        self.assertFalse(other.status()["held"])

    def test_healthy_waiters_keep_fifo_order_without_overlap(self) -> None:
        events = Path(self.temporary.name) / "events.jsonl"
        first_ready = Path(self.temporary.name) / "first.ready"
        second_ready = Path(self.temporary.name) / "second.ready"
        third_ready = Path(self.temporary.name) / "third.ready"
        first = self.spawn_worker("hold", "first", events, first_ready, 0.5)
        self.wait_for(first_ready)
        second = self.spawn_worker("hold", "second", events, second_ready, 0.15)
        time.sleep(0.08)
        third = self.spawn_worker("hold", "third", events, third_ready, 0.05)
        for process in (first, second, third):
            self.assertEqual(process.wait(timeout=10), 0)
        records = [json.loads(line) for line in events.read_text().splitlines()]
        starts = [record["label"] for record in records if record["event"] == "start"]
        self.assertEqual(starts, ["first", "second", "third"])
        active = None
        for record in records:
            if record["event"] == "start":
                self.assertIsNone(active, f"{record['label']} overlapped {active}")
                active = record["label"]
            else:
                self.assertEqual(active, record["label"])
                active = None
        self.assertIsNone(active)

    def test_abandoned_waiter_expires_by_heartbeat_not_pid(self) -> None:
        coordinator = self.coordinator()
        queue = coordinator.queue_dir
        stale_path = queue / "00000000000000000001-stale.json"
        fresh_path = queue / "00000000000000000002-fresh.json"
        now = time.time()
        radio_lock._write_json_atomic(
            stale_path,
            {"createdNs": 1, "heartbeatAt": now - 10, "label": "stale"},
        )
        radio_lock._write_json_atomic(
            fresh_path,
            {"createdNs": 2, "heartbeatAt": now, "label": "fresh"},
        )
        status = coordinator.status()
        self.assertFalse(stale_path.exists())
        self.assertTrue(fresh_path.exists())
        self.assertEqual([ticket["label"] for ticket in status["queue"]], ["fresh"])

    @unittest.skipIf(os.name != "posix", "descriptor inheritance contract is POSIX-specific")
    def test_child_inherits_lock_if_coordinator_is_killed(self) -> None:
        events = Path(self.temporary.name) / "orphan-events.jsonl"
        ready = Path(self.temporary.name) / "orphan.ready"
        parent = self.spawn_worker("crash-with-child", "orphan", events, ready, 0.7)
        self.wait_for(ready)
        self.assertEqual(parent.wait(timeout=5), 0)
        with self.assertRaises(TimeoutError):
            self.coordinator().acquire("too-early", timeout_seconds=0.2)
        time.sleep(0.7)
        lease = self.coordinator().acquire("after-child", timeout_seconds=2.0)
        lease.release()

    def test_status_reports_holder_and_queue_lease_age(self) -> None:
        owner = self.coordinator().acquire("status-owner", timeout_seconds=1.0)
        status = self.coordinator().status()
        self.assertTrue(status["held"])
        self.assertEqual(status["owner"]["label"], "status-owner")
        self.assertEqual(status["owner"]["authority"], "kernel-file-lock")
        self.assertEqual(status["protocolVersion"], 2)
        self.assertGreaterEqual(status["owner"]["heartbeatAgeSeconds"], 0)
        owner.release()


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        return _worker(sys.argv[2:])
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RadioLockTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
