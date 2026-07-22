#!/usr/bin/env python3
"""FIFO coordinator for AetherSDR proofs that share one physical radio.

The coordination directory is shared between agent sandboxes, but their PID
visibility is not.  Cross-sandbox liveness therefore comes from a kernel file
lock, never ``kill -0`` or process enumeration.  Queue tickets use renewable
heartbeats only so an abandoned waiter can be removed without inspecting its
process namespace.

The supported proof launch is::

    AETHER_AUTOMATION=1 python3 tools/radio_lock.py run \
        --label issue-4372 -- ./build/AetherSDR

``run`` holds the lock for the child lifetime.  On POSIX it also passes the
locked descriptor into the child, so an abruptly killed coordinator cannot
leave an unlocked AetherSDR process behind.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import secrets
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Sequence


PROTOCOL_VERSION = 2
DEFAULT_TIMEOUT_SECONDS = 570.0
DEFAULT_TICKET_TTL_SECONDS = 45.0
DEFAULT_HEARTBEAT_SECONDS = 2.0
DEFAULT_POLL_SECONDS = 0.25


def default_coordination_root() -> Path:
    """Return a path shared by this user's sandbox contexts.

    Codex-style macOS sandboxes may each receive a private ``TMPDIR`` while
    still sharing ``/tmp``.  Use the literal shared location on POSIX; Windows
    has no equivalent split and uses its normal temporary directory.
    """
    override = os.environ.get("AETHER_RADIO_LOCK_DIR")
    if override:
        return Path(override)
    if os.name == "posix":
        return Path("/tmp/aethersdr-radio-proof")
    return Path(tempfile.gettempdir()) / "aethersdr-radio-proof"


def _capability_hash(capability: str) -> str:
    return hashlib.sha256(capability.encode("ascii")).hexdigest()


def _safe_label(value: str) -> str:
    label = str(value).strip()
    if not label or len(label) > 80:
        raise ValueError("label must contain 1-80 characters")
    if any(ord(char) < 32 or ord(char) == 127 for char in label):
        raise ValueError("label must not contain control characters")
    return label


def _write_json_atomic(path: Path, value: dict) -> None:
    """Write one complete JSON object and atomically replace the old file."""
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(8)}.tmp")
    payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()


def _read_json(path: Path) -> Optional[dict]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


class _KernelFileLock:
    """A non-PID kernel lock held by one open file descriptor."""

    def __init__(self, path: Path):
        self.path = path
        self._handle = None
        self._held = False

    @property
    def fileno(self) -> int:
        if self._handle is None:
            raise RuntimeError("lock is not open")
        return self._handle.fileno()

    def acquire(
        self,
        blocking: bool,
        *,
        create: bool = True,
        exclusive_create: bool = False,
    ) -> bool:
        if self._held:
            return True
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        flags = os.O_RDWR
        if create:
            flags |= os.O_CREAT
        if exclusive_create:
            flags |= os.O_EXCL
        try:
            descriptor = os.open(self.path, flags, 0o600)
        except FileExistsError:
            return False
        except FileNotFoundError:
            return False
        self._handle = os.fdopen(descriptor, "r+b", buffering=0)
        if self.path.stat().st_size == 0:
            self._handle.write(b"\0")
            self._handle.flush()
        self._handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                mode = msvcrt.LK_LOCK if blocking else msvcrt.LK_NBLCK
                msvcrt.locking(self._handle.fileno(), mode, 1)
            else:
                import fcntl

                flags = fcntl.LOCK_EX
                if not blocking:
                    flags |= fcntl.LOCK_NB
                fcntl.flock(self._handle.fileno(), flags)
        except (BlockingIOError, OSError):
            self._handle.close()
            self._handle = None
            return False
        self._held = True
        return True

    def release(self) -> None:
        handle = self._handle
        self._handle = None
        if handle is None:
            return
        if self._held:
            with contextlib.suppress(OSError):
                handle.seek(0)
                if os.name == "nt":
                    import msvcrt

                    msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl

                    fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        self._held = False
        handle.close()

    def __enter__(self) -> "_KernelFileLock":
        if not self.acquire(blocking=True):
            raise RuntimeError(f"could not acquire coordination guard {self.path}")
        return self

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        self.release()


@dataclass(frozen=True)
class QueueSnapshot:
    position: int
    total: int
    holder: Optional[str]
    holder_heartbeat_age_seconds: Optional[float]


class RadioLease:
    """Opaque ownership capability returned only after kernel-lock acquisition."""

    def __init__(
        self,
        coordinator: "RadioCoordinator",
        kernel_lock: _KernelFileLock,
        capability: str,
        label: str,
        acquired_at: float,
    ):
        self._coordinator = coordinator
        self._kernel_lock = kernel_lock
        self._capability = capability
        self.label = label
        self.owner_id = _capability_hash(capability)[:16]
        self.acquired_at = acquired_at
        self._released = False
        self._heartbeat_stop = threading.Event()
        self._heartbeat_thread: Optional[threading.Thread] = None

    @property
    def fileno(self) -> int:
        return self._kernel_lock.fileno

    @property
    def released(self) -> bool:
        return self._released

    def heartbeat(self) -> None:
        if self._released:
            return
        self._coordinator._write_owner_metadata(self)

    def start_heartbeat(self) -> None:
        if self._heartbeat_thread is not None:
            return

        def beat() -> None:
            while not self._heartbeat_stop.wait(self._coordinator.heartbeat_seconds):
                with contextlib.suppress(OSError):
                    self.heartbeat()

        self.heartbeat()
        self._heartbeat_thread = threading.Thread(
            target=beat,
            name="aethersdr-radio-lease",
            daemon=True,
        )
        self._heartbeat_thread.start()

    def child_popen_kwargs(self) -> dict:
        """Keep the POSIX kernel lock alive if this wrapper is killed."""
        if os.name == "posix":
            os.set_inheritable(self.fileno, True)
            return {"pass_fds": (self.fileno,)}
        # The crash-survival inheritance contract is POSIX-specific.  Keep
        # Popen's close_fds default on Windows rather than leaking unrelated
        # inheritable handles into the proof process.
        return {}

    def release(self) -> None:
        if self._released:
            return
        self._heartbeat_stop.set()
        if self._heartbeat_thread is not None:
            self._heartbeat_thread.join(timeout=self._coordinator.heartbeat_seconds * 2)
        self._coordinator._release(self)
        self._released = True

    def __enter__(self) -> "RadioLease":
        return self

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        self.release()


class RadioCoordinator:
    """FIFO ticket queue plus process-namespace-independent exclusive lock."""

    def __init__(
        self,
        root: Optional[Path] = None,
        *,
        owner_lock_path: Optional[Path] = None,
        legacy_queue_dir: Optional[Path] = None,
        ticket_ttl_seconds: float = DEFAULT_TICKET_TTL_SECONDS,
        heartbeat_seconds: float = DEFAULT_HEARTBEAT_SECONDS,
        poll_seconds: float = DEFAULT_POLL_SECONDS,
        wall_clock: Callable[[], float] = time.time,
        monotonic_clock: Callable[[], float] = time.monotonic,
        sleeper: Callable[[float], None] = time.sleep,
    ):
        if ticket_ttl_seconds <= 0 or heartbeat_seconds <= 0 or poll_seconds <= 0:
            raise ValueError("TTL, heartbeat, and poll intervals must be positive")
        if heartbeat_seconds >= ticket_ttl_seconds:
            raise ValueError("heartbeat interval must be shorter than ticket TTL")
        self.root = Path(root) if root is not None else default_coordination_root()
        self.queue_dir = self.root / "queue"
        using_default_root = root is None
        self.owner_lock_path = (
            Path(owner_lock_path)
            if owner_lock_path is not None
            else Path("/tmp/aethersdr-fb.lock")
            if using_default_root and os.name == "posix"
            else self.root / "owner.lock"
        )
        self.legacy_queue_dir = (
            Path(legacy_queue_dir)
            if legacy_queue_dir is not None
            else Path("/tmp/aethersdr-fb.queue")
            if using_default_root and os.name == "posix"
            else None
        )
        self.owner_metadata_path = self.root / "owner.json"
        self.queue_guard_path = self.root / "queue.guard"
        self.ticket_ttl_seconds = float(ticket_ttl_seconds)
        self.heartbeat_seconds = float(heartbeat_seconds)
        self.poll_seconds = float(poll_seconds)
        self._wall_clock = wall_clock
        self._monotonic_clock = monotonic_clock
        self._sleep = sleeper
        self._ensure_root()

    def _ensure_root(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.root.is_symlink() or not self.root.is_dir():
            raise RuntimeError(f"coordination root is not a real directory: {self.root}")
        if os.name == "posix" and self.root.stat().st_uid != os.getuid():
            raise RuntimeError(f"coordination root is owned by another user: {self.root}")
        os.chmod(self.root, 0o700)
        self.queue_dir.mkdir(exist_ok=True, mode=0o700)
        os.chmod(self.queue_dir, 0o700)

    def _ticket_record(
        self,
        capability_hash: str,
        label: str,
        created_ns: int,
        created_at: float,
    ) -> dict:
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "ticketId": capability_hash[:16],
            "capabilityHash": capability_hash,
            "label": label,
            "createdNs": created_ns,
            "createdAt": created_at,
            "heartbeatAt": self._wall_clock(),
        }

    def _update_ticket(self, path: Path, record: dict) -> None:
        updated = dict(record)
        updated["heartbeatAt"] = self._wall_clock()
        _write_json_atomic(path, updated)

    def _ticket_entries_locked(self) -> list[tuple[Path, dict]]:
        now = self._wall_clock()
        entries: list[tuple[Path, dict]] = []
        for path in self.queue_dir.glob("*.json"):
            record = _read_json(path)
            if record is None:
                try:
                    age = max(0.0, now - path.stat().st_mtime)
                except OSError:
                    continue
                if age > self.ticket_ttl_seconds:
                    with contextlib.suppress(FileNotFoundError):
                        path.unlink()
                else:
                    entries.append((path, {"createdNs": 0, "label": "invalid-fresh-ticket"}))
                continue
            heartbeat = float(record.get("heartbeatAt", 0.0))
            if max(0.0, now - heartbeat) > self.ticket_ttl_seconds:
                with contextlib.suppress(FileNotFoundError):
                    path.unlink()
                continue
            entries.append((path, record))
        entries.sort(key=lambda item: (int(item[1].get("createdNs", 0)), item[0].name))
        return entries

    def _owner_status(self) -> tuple[bool, Optional[dict]]:
        legacy = self._legacy_owner_metadata()
        if legacy is not None:
            return True, legacy
        if not self.owner_lock_path.exists():
            return False, None
        probe = _KernelFileLock(self.owner_lock_path)
        free = probe.acquire(blocking=False)
        if free:
            probe.release()
        metadata = _read_json(self.owner_metadata_path)
        return not free, metadata

    def _legacy_owner_metadata(self) -> Optional[dict]:
        """Return status for a v1 PID file without inspecting its PID."""
        if self.legacy_queue_dir is None:
            return None
        try:
            modified_at = self.owner_lock_path.stat().st_mtime
        except FileNotFoundError:
            return None
        try:
            lines = self.owner_lock_path.read_text(encoding="utf-8").splitlines()
            line = lines[0] if lines else ""
        except (OSError, UnicodeError):
            line = ""
        fields = line.split()
        if not fields:
            return {
                "ownerId": None,
                "label": "legacy PID helper (creation in progress)",
                "heartbeatAt": modified_at,
                "authority": "legacy-pidfile-barrier",
            }
        if fields[0] == "0" and any(
            field.startswith("aethersdr-radio-v2:") for field in fields[2:]
        ):
            return None
        return {
            "ownerId": None,
            "label": " ".join(fields[2:]) if len(fields) > 2 else "legacy PID helper",
            "heartbeatAt": modified_at,
            "authority": "legacy-pidfile-barrier",
        }

    def _try_acquire_owner_lock(self) -> Optional[_KernelFileLock]:
        """Atomically arbitrate the shared path with old noclobber helpers.

        An old helper claims ownership by creating the pidfile with shell
        noclobber.  When the path is absent, O_EXCL makes exactly one protocol
        generation win that creation race.  When a stale v2 sentinel exists,
        open it without O_CREAT and let the kernel lock decide.  Rechecking the
        contents after locking fences an unlink/recreate race.
        """
        owner_lock = _KernelFileLock(self.owner_lock_path)
        if not self.owner_lock_path.exists():
            if owner_lock.acquire(blocking=False, exclusive_create=True):
                return owner_lock
            return None
        if self._legacy_owner_metadata() is not None:
            return None
        if not owner_lock.acquire(blocking=False, create=False):
            return None
        if self._legacy_owner_metadata() is not None:
            owner_lock.release()
            return None
        return owner_lock

    def _legacy_tickets_locked(self) -> list[dict]:
        """Treat every v1 ticket as ahead of v2; never prune it by foreign PID."""
        if self.legacy_queue_dir is None or not self.legacy_queue_dir.is_dir():
            return []
        tickets = []
        for path in self.legacy_queue_dir.iterdir():
            try:
                modified_at = path.stat().st_mtime
            except OSError:
                continue
            tickets.append(
                {
                    "ticketId": "legacy",
                    "label": "legacy PID-helper waiter",
                    "createdNs": int(modified_at * 1_000_000_000),
                    "heartbeatAt": modified_at,
                }
            )
        tickets.sort(key=lambda record: int(record["createdNs"]))
        return tickets

    def _snapshot_locked(
        self,
        ticket_path: Path,
        entries: list[tuple[Path, dict]],
        legacy_count: int,
    ) -> QueueSnapshot:
        position = 0
        for index, (path, _record) in enumerate(entries, start=1):
            if path == ticket_path:
                position = legacy_count + index
                break
        held, owner = self._owner_status()
        holder = None
        heartbeat_age = None
        if held:
            holder = str((owner or {}).get("label") or "unknown")
            if owner and "heartbeatAt" in owner:
                heartbeat_age = max(0.0, self._wall_clock() - float(owner["heartbeatAt"]))
        return QueueSnapshot(position, legacy_count + len(entries), holder, heartbeat_age)

    def acquire(
        self,
        label: str,
        timeout_seconds: Optional[float] = DEFAULT_TIMEOUT_SECONDS,
        status_callback: Optional[Callable[[QueueSnapshot], None]] = None,
    ) -> RadioLease:
        safe_label = _safe_label(label)
        if timeout_seconds is not None and timeout_seconds < 0:
            raise ValueError("timeout must be non-negative or None")
        capability = secrets.token_hex(32)
        capability_hash = _capability_hash(capability)
        created_at = self._wall_clock()
        with _KernelFileLock(self.queue_guard_path):
            existing = self._ticket_entries_locked()
            newest_ns = max(
                (int(record.get("createdNs", 0)) for _path, record in existing),
                default=0,
            )
            created_ns = max(time.time_ns(), newest_ns + 1)
            ticket_path = (
                self.queue_dir / f"{created_ns:020d}-{capability_hash[:16]}.json"
            )
            ticket = self._ticket_record(
                capability_hash, safe_label, created_ns, created_at
            )
            _write_json_atomic(ticket_path, ticket)
        deadline = (
            None
            if timeout_seconds is None
            else self._monotonic_clock() + float(timeout_seconds)
        )
        last_heartbeat = 0.0
        acquired = False
        try:
            while True:
                now_mono = self._monotonic_clock()
                if now_mono - last_heartbeat >= self.heartbeat_seconds:
                    self._update_ticket(ticket_path, ticket)
                    last_heartbeat = now_mono
                with _KernelFileLock(self.queue_guard_path):
                    entries = self._ticket_entries_locked()
                    legacy_tickets = self._legacy_tickets_locked()
                    snapshot = self._snapshot_locked(
                        ticket_path, entries, len(legacy_tickets)
                    )
                    if status_callback is not None:
                        status_callback(snapshot)
                    if (
                        not legacy_tickets
                        and self._legacy_owner_metadata() is None
                        and entries
                        and entries[0][0] == ticket_path
                    ):
                        owner_lock = self._try_acquire_owner_lock()
                        if owner_lock is not None:
                            lease = RadioLease(
                                self,
                                owner_lock,
                                capability,
                                safe_label,
                                self._wall_clock(),
                            )
                            self._write_compatibility_line(lease)
                            lease.heartbeat()
                            with contextlib.suppress(FileNotFoundError):
                                ticket_path.unlink()
                            acquired = True
                            return lease
                if deadline is not None and now_mono >= deadline:
                    raise TimeoutError(
                        f"timed out waiting for the radio proof lock after "
                        f"{timeout_seconds:g}s"
                    )
                self._sleep(self.poll_seconds)
        finally:
            if not acquired:
                self._remove_ticket_if_owned(ticket_path, capability_hash)

    def _remove_ticket_if_owned(self, path: Path, capability_hash: str) -> None:
        with _KernelFileLock(self.queue_guard_path):
            record = _read_json(path)
            if record and secrets.compare_digest(
                str(record.get("capabilityHash", "")), capability_hash
            ):
                with contextlib.suppress(FileNotFoundError):
                    path.unlink()

    def _write_owner_metadata(self, lease: RadioLease) -> None:
        if lease.released:
            return
        metadata = {
            "protocolVersion": PROTOCOL_VERSION,
            "ownerId": lease.owner_id,
            "capabilityHash": _capability_hash(lease._capability),
            "label": lease.label,
            "acquiredAt": lease.acquired_at,
            "heartbeatAt": self._wall_clock(),
            "authority": "kernel-file-lock",
        }
        _write_json_atomic(self.owner_metadata_path, metadata)
        self._touch_compatibility_lock(lease)

    def _write_compatibility_line(self, lease: RadioLease) -> None:
        """Make v1 helpers fail closed while the v2 kernel lock is held.

        PID 0 names the caller's own process group, so a v1 ``kill -0 0``
        succeeds without depending on visibility of this owner.  A v1 helper
        therefore never reaps the file; normal v2 release removes it.
        """
        payload = (
            f"0 {int(lease.acquired_at)} "
            f"aethersdr-radio-v2:{lease.owner_id}:{lease.label}\n"
        ).encode("utf-8")
        descriptor = lease.fileno
        os.lseek(descriptor, 0, os.SEEK_SET)
        os.ftruncate(descriptor, 0)
        os.write(descriptor, payload)
        os.fsync(descriptor)

    def _touch_compatibility_lock(self, lease: RadioLease) -> None:
        try:
            path_stat = self.owner_lock_path.stat()
            descriptor_stat = os.fstat(lease.fileno)
            if (path_stat.st_dev, path_stat.st_ino) == (
                descriptor_stat.st_dev,
                descriptor_stat.st_ino,
            ):
                os.utime(self.owner_lock_path, None)
        except OSError:
            pass

    def _release(self, lease: RadioLease) -> None:
        expected_hash = _capability_hash(lease._capability)
        metadata = _read_json(self.owner_metadata_path)
        if metadata and secrets.compare_digest(
            str(metadata.get("capabilityHash", "")), expected_hash
        ):
            tombstone = self.root / f".owner.{lease.owner_id}.released"
            with contextlib.suppress(FileNotFoundError):
                os.replace(self.owner_metadata_path, tombstone)
            with contextlib.suppress(FileNotFoundError):
                tombstone.unlink()
        try:
            path_stat = self.owner_lock_path.stat()
            descriptor_stat = os.fstat(lease.fileno)
            if (path_stat.st_dev, path_stat.st_ino) == (
                descriptor_stat.st_dev,
                descriptor_stat.st_ino,
            ):
                line = self.owner_lock_path.read_text(encoding="utf-8")
                marker = f"aethersdr-radio-v2:{lease.owner_id}:"
                if marker in line:
                    self.owner_lock_path.unlink()
        except OSError:
            pass
        lease._kernel_lock.release()

    def status(self) -> dict:
        with _KernelFileLock(self.queue_guard_path):
            entries = self._ticket_entries_locked()
            legacy_tickets = self._legacy_tickets_locked()
            held, owner = self._owner_status()
            now = self._wall_clock()
            queue = []
            for index, record in enumerate(legacy_tickets, start=1):
                queue.append(
                    {
                        "position": index,
                        "ticketId": record["ticketId"],
                        "label": record["label"],
                        "heartbeatAgeSeconds": round(
                            max(0.0, now - float(record["heartbeatAt"])), 3
                        ),
                    }
                )
            for index, (_path, record) in enumerate(
                entries, start=len(legacy_tickets) + 1
            ):
                queue.append(
                    {
                        "position": index,
                        "ticketId": record.get("ticketId"),
                        "label": record.get("label", "unknown"),
                        "heartbeatAgeSeconds": round(
                            max(0.0, now - float(record.get("heartbeatAt", now))), 3
                        ),
                    }
                )
            owner_status = None
            if held:
                owner_status = {
                    "ownerId": (owner or {}).get("ownerId"),
                    "label": (owner or {}).get("label", "unknown"),
                    "heartbeatAgeSeconds": None,
                    "authority": (owner or {}).get("authority", "kernel-file-lock"),
                }
                if owner and "heartbeatAt" in owner:
                    owner_status["heartbeatAgeSeconds"] = round(
                        max(0.0, now - float(owner["heartbeatAt"])), 3
                    )
            return {
                "protocolVersion": PROTOCOL_VERSION,
                "root": str(self.root),
                "held": held,
                "owner": owner_status,
                "queue": queue,
            }


def _print_snapshot(snapshot: QueueSnapshot) -> None:
    holder = snapshot.holder or "none"
    age = (
        "unknown"
        if snapshot.holder_heartbeat_age_seconds is None
        else f"{snapshot.holder_heartbeat_age_seconds:.1f}s"
    )
    print(
        f"queued {snapshot.position}/{snapshot.total} — "
        f"holder={holder}, holder_heartbeat_age={age}",
        flush=True,
    )


def _terminate_child(child: subprocess.Popen) -> None:
    if child.poll() is not None:
        return
    with contextlib.suppress(OSError):
        child.terminate()
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(OSError):
            child.kill()
        with contextlib.suppress(subprocess.TimeoutExpired):
            child.wait(timeout=5)


def _run_child(args: argparse.Namespace) -> int:
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise ValueError("run requires a command after --")
    coordinator = RadioCoordinator(root=Path(args.root) if args.root else None)
    last_report = {"value": None, "at": 0.0}

    def report(snapshot: QueueSnapshot) -> None:
        value = (snapshot.position, snapshot.total, snapshot.holder)
        now = time.monotonic()
        if value != last_report["value"] or now - last_report["at"] >= 15.0:
            _print_snapshot(snapshot)
            last_report["value"] = value
            last_report["at"] = now

    timeout = None if args.timeout < 0 else args.timeout
    with coordinator.acquire(args.label, timeout, report) as lease:
        lease.start_heartbeat()
        print(
            f"acquired radio proof lock — owner={lease.owner_id}, label={lease.label}",
            flush=True,
        )
        child = subprocess.Popen(command, **lease.child_popen_kwargs())
        interrupted = threading.Event()
        prior_handlers = {}

        def on_signal(_signum, _frame) -> None:
            interrupted.set()

        for signum in (signal.SIGINT, signal.SIGTERM):
            prior_handlers[signum] = signal.getsignal(signum)
            signal.signal(signum, on_signal)
        try:
            while child.poll() is None and not interrupted.wait(0.2):
                pass
            if interrupted.is_set():
                _terminate_child(child)
            return_code = child.wait()
            print(
                f"proof child exited with code {return_code}; releasing radio proof lock",
                flush=True,
            )
            return return_code
        finally:
            _terminate_child(child)
            for signum, handler in prior_handlers.items():
                signal.signal(signum, handler)


def _status_command(args: argparse.Namespace) -> int:
    coordinator = RadioCoordinator(root=Path(args.root) if args.root else None)
    status = coordinator.status()
    if args.json:
        print(json.dumps(status, indent=2, sort_keys=True))
        return 0
    if status["held"]:
        owner = status["owner"] or {}
        print(
            f"holder: {owner.get('label', 'unknown')} "
            f"(owner={owner.get('ownerId') or 'unknown'}, "
            f"heartbeat_age={owner.get('heartbeatAgeSeconds')})"
        )
    else:
        print("holder: none")
    print("queue (front first):")
    if not status["queue"]:
        print("  (empty)")
    for ticket in status["queue"]:
        print(
            f"  {ticket['position']}. {ticket['label']} "
            f"(ticket={ticket['ticketId']}, "
            f"heartbeat_age={ticket['heartbeatAgeSeconds']}s)"
        )
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    run = subparsers.add_parser("run", help="queue, lock, and run one proof app")
    run.add_argument("--label", required=True, help="human-readable issue/worktree label")
    run.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="queue timeout in seconds; negative waits indefinitely",
    )
    run.add_argument("--root", help="override coordination root (tests only)")
    run.add_argument("command", nargs=argparse.REMAINDER)
    run.set_defaults(handler=_run_child)

    status = subparsers.add_parser("status", help="show holder and FIFO queue")
    status.add_argument("--root", help="override coordination root (tests only)")
    status.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    status.set_defaults(handler=_status_command)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"radio-lock: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
