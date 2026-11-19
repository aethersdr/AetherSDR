#!/usr/bin/env python3
"""Explicit opt-in: exercise production persistence using disposable OS-keychain slots."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


class Child:
    def __init__(self, binary, mode, identifier, value, directory, env, extra_args=()):
        self.mailbox = directory / (uuid.uuid4().hex + ".command")
        self.output = []
        self.errors = []
        self.lines = queue.Queue()
        self.process = subprocess.Popen(
            [str(binary), mode, identifier, value, *extra_args, str(self.mailbox)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", env=env,
        )
        self.readers = [
            threading.Thread(target=self._read, args=(self.process.stdout, self.output, self.lines)),
            threading.Thread(target=self._read, args=(self.process.stderr, self.errors, None)),
        ]
        for reader in self.readers:
            reader.start()

    @staticmethod
    def _read(stream, output, lines):
        try:
            for line in stream:
                output.append(line.rstrip("\r\n"))
                if lines is not None:
                    lines.put(output[-1])
        finally:
            stream.close()
            if lines is not None:
                lines.put(None)

    def expect(self, expected):
        actual = self.lines.get(timeout=8)
        require(actual == expected, f"expected {expected!r}, got {actual!r}")

    def send(self, command):
        require(not self.mailbox.exists(), "mailbox contains an unconsumed command")
        temporary = self.mailbox.with_suffix(".tmp")
        temporary.write_bytes(command.encode("ascii"))
        temporary.replace(self.mailbox)

    def finish(self, expected=0, killed=False):
        try:
            status = self.process.wait(timeout=18)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise RuntimeError("probe exceeded parent deadline") from None
        finally:
            for reader in self.readers:
                reader.join(timeout=5)
        require(all(not reader.is_alive() for reader in self.readers), "pipe reader did not terminate")
        record = {"exit": status, "output": self.output, "error": self.errors}
        require(status != 0 if killed else status == expected, f"unexpected probe result: {record}")
        return record

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        for reader in self.readers:
            reader.join(timeout=5)


CASES = (
    "stale_changed_reload", "stale_same_reload", "stale_changed_no_reload", "stale_same_no_reload",
    "matching_retirement", "lease_blocks_writer", "lease_crash_recovery", "closed_caller_queued",
    "waiting_writer_killed", "busy_admission", "crash_before_primary", "crash_after_primary",
    "legacy_read_upgrade", "pending_write", "inflight_read_save",
    "owner_promotion", "owner_promotion_closed_context", "owner_stale_identical_save",
    "owner_stale_foreign_save", "owner_active_deleted",
    "owner_transaction", "owner_transaction_lost", "owner_transaction_missing",
)


def qualify(binary, receipt):
    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "system": platform.system(), "machine": platform.machine(), "platform": platform.platform(),
        "scope": "Real local OS-keychain and production token/persistence sources, independent processes. No radio, network peer, settings database, UI lifecycle or RF qualification.",
        "fixture_policy": "Unique aetherpd-native-credential-UUID primary, derived owner candidate and version keys only. No enumeration or access to real radio credentials; insecure fallback disabled.",
        "cases": [], "success": False,
    }
    atomic_json(receipt, result)
    with tempfile.TemporaryDirectory(prefix="aetherpd-native-credentials-") as temporary:
        directory = Path(temporary)
        if platform.system() == "Darwin":
            # Read-only negative control: the original core-only loop times out;
            # the following cases must complete with the native GUI event loop.
            control = Child(binary, "core-loop-absent", "aetherpd-native-credential-" + uuid.uuid4().hex,
                            "", directory, None)
            try:
                result["macos_core_loop_control"] = control.finish(expected=3)
                require(control.output == ["deadline"], "core-only control did not reproduce callback starvation")
                require(any("QEventDispatcherUNIX" in line for line in control.errors), "unexpected core-only dispatcher")
                atomic_json(receipt, result)
            except Exception as error:
                result["macos_core_loop_control_failure"] = str(error)
                atomic_json(receipt, result)
                raise
            finally:
                control.close()
        for kind in CASES:
            case = {"kind": kind, "identifier": "aetherpd-native-credential-" + uuid.uuid4().hex,
                    "operations": [], "cleanup": []}
            result["cases"].append(case)
            children = []

            def save():
                atomic_json(receipt, result)

            def start(mode, value="", env=None):
                child = Child(binary, mode, case["identifier"], value, directory, env)
                children.append(child)
                case.setdefault("children", []).append({"mode": mode, "pid": child.process.pid})
                save()
                return child

            def finish(child, expected=0, killed=False):
                record = child.finish(expected, killed)
                case.setdefault("child_results", []).append(record)
                save()

            def command(mode, value="", cleanup=False, env=None):
                record = {"mode": mode}
                case["cleanup" if cleanup else "operations"].append(record)
                save()
                child = start(mode, value, env)
                record.update(child.finish())
                save()

            admitted = False
            try:
                # Never replace a pre-existing entry, even for this random identifier.
                command("absent")
                command("absent-metadata")
                if kind.startswith("owner_transaction"):
                    command("owner-transaction-absent")
                    command("owner-transaction-absent-meta")
                elif kind.startswith("owner_"):
                    command("owner-candidate-absent")
                    command("owner-candidate-absent-meta")
                admitted = True
                if not kind.startswith("owner_transaction"):
                    command("legacy-write" if kind == "legacy_read_upgrade" else "write", "fixture-old")
                    command("raw-read", "fixture-old")
                if kind.startswith("owner_transaction"):
                    command(kind.replace("_", "-"))
                    # The controller fixture uses one fixed, non-production key.
                    candidate = "b" * 64
                    command("raw-read", candidate)
                    command("read", candidate)
                    command("owner-transaction-absent")
                    command("owner-transaction-absent-meta")
                elif kind in ("owner_promotion", "owner_promotion_closed_context"):
                    command("owner-promote" if kind == "owner_promotion" else "owner-promote-close", "fixture-old")
                    candidate = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
                    command("raw-read", candidate)
                    command("read", candidate)
                elif kind.startswith("owner_"):
                    actor = start("owner-actor", "fixture-old")
                    actor.expect("actor_loaded")
                    if kind == "owner_active_deleted":
                        command("cleanup")
                    else:
                        newer = "fixture-old" if kind == "owner_stale_identical_save" else "fixture-new"
                        command("write", newer)
                    actor.send("promote")
                    actor.expect("promotion_rejected")
                    finish(actor)
                    if kind == "owner_active_deleted":
                        command("absent")
                    else:
                        command("raw-read", newer)
                elif kind.startswith("stale_") or kind == "matching_retirement":
                    actor = start("actor", "fixture-old")
                    actor.expect("actor_loaded")
                    if kind.startswith("stale_"):
                        newer = "fixture-old" if "_same_" in kind else "fixture-new"
                        command("write", newer)
                        if kind.endswith("_reload") and not kind.endswith("no_reload"):
                            actor.send("reload")
                            actor.expect("reload_same_token" if newer == "fixture-old" else "reload_changed_token")
                    actor.send("retire")
                    actor.expect("retirement_completed" if kind == "matching_retirement" else "retirement_refused")
                    finish(actor, 0 if kind == "matching_retirement" else 4)
                    if kind == "matching_retirement":
                        command("absent")
                        command("absent-metadata")
                    else:
                        command("raw-read", newer)
                        command("raw-metadata")
                elif kind == "legacy_read_upgrade":
                    command("read", "fixture-old")
                    command("absent-metadata")
                    actor = start("actor", "fixture-old")
                    actor.expect("actor_loaded")
                    actor.send("retire")
                    finish(actor, 4)
                    command("raw-read", "fixture-old")
                    command("write", "fixture-old")
                    actor = start("actor", "fixture-old")
                    actor.expect("actor_loaded")
                    actor.send("retire")
                    finish(actor)
                elif kind in ("pending_write", "inflight_read_save"):
                    command("pending-write" if kind == "pending_write" else "inflight-read-save", "fixture-new")
                    command("read", "fixture-new")
                elif kind.startswith("crash_"):
                    before = kind == "crash_before_primary"
                    writer = start("phase-before" if before else "phase-after", "fixture-new")
                    writer.expect("phase_paused_before_primary" if before else "phase_paused_after_primary")
                    writer.process.kill()
                    finish(writer, killed=True)
                    expected = "fixture-old" if before else "fixture-new"
                    command("read", expected)
                    command("raw-read", expected)
                    actor = start("actor", expected)
                    actor.expect("actor_loaded")
                    actor.send("retire")
                    finish(actor, 4)
                    command("write", "fixture-recovered")
                    command("read", "fixture-recovered")
                else:
                    # The production authority must span different app settings/temp roots.
                    environments = []
                    for profile in ("profile-A", "profile-B"):
                        root = directory / profile
                        root.mkdir(exist_ok=True)
                        environments.append(dict(os.environ, TMPDIR=str(root), TMP=str(root), TEMP=str(root),
                                                 AETHER_SETTINGS_DIR=str(root)))
                    holder = start("lease-holder", env=environments[0])
                    holder.expect("lease_held")
                    if kind == "busy_admission":
                        command("busy-write", "fixture-new", env=environments[1])
                        command("raw-read", "fixture-old")
                    else:
                        writer = start("write-close" if kind == "closed_caller_queued" else "write", "fixture-new", environments[1])
                        time.sleep(0.25)
                        require(writer.process.poll() is None, "writer did not wait for live authority")
                        command("raw-read", "fixture-old")
                        case["writer_blocked_while_lease_live"] = True
                        if kind == "waiting_writer_killed":
                            writer.process.kill()
                            finish(writer, killed=True)
                    if kind == "lease_crash_recovery":
                        holder.process.kill()
                        finish(holder, killed=True)
                    else:
                        holder.send("release")
                        finish(holder)
                    if kind not in ("waiting_writer_killed", "busy_admission"):
                        finish(writer)
                        command("read", "fixture-new")
                    else:
                        command("read", "fixture-old")
                case["passed"] = True
            except Exception as error:
                case["failure"] = str(error)
            finally:
                for child in children:
                    try:
                        child.close()
                    except Exception as error:
                        case.setdefault("cleanup_errors", []).append(str(error))
                if admitted:
                    if kind.startswith("owner_transaction"):
                        modes = ("owner-transaction-cleanup", "absent", "absent-metadata", "owner-transaction-absent", "owner-transaction-absent-meta")
                    else:
                        modes = ("owner-cleanup", "absent", "absent-metadata", "owner-candidate-absent", "owner-candidate-absent-meta") \
                            if kind.startswith("owner_") else ("cleanup", "cleanup", "absent", "absent-metadata")
                    for mode in modes:
                        try:
                            command(mode, cleanup=True)
                        except Exception as error:
                            case.setdefault("cleanup_errors", []).append(str(error))
                else:
                    case["cleanup_skipped"] = "No mutation admitted: initial absence could not be proved."
                save()
            print(json.dumps({"case": kind, "passed": case.get("passed", False),
                              "cleaned": admitted and not case.get("cleanup_errors")}), flush=True)
            # An unavailable backend is a failed qualification, not a silently passing skip.
            if not admitted:
                break
    result["success"] = len(result["cases"]) == len(CASES) and all(
        case.get("passed") and not case.get("cleanup_errors") for case in result["cases"])
    atomic_json(receipt, result)
    print(json.dumps({"success": result["success"], "cases": len(result["cases"]), "receipt": str(receipt)}))
    return 0 if result["success"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--receipt", type=Path)
    output.add_argument("--receipt-directory", type=Path)
    parser.add_argument("--allow-disposable-keychain-writes", action="store_true", required=True)
    args = parser.parse_args()
    receipt = args.receipt if args.receipt else args.receipt_directory / ("native-credential-" + uuid.uuid4().hex + ".json")
    require(not receipt.exists(), "receipt already exists; choose a new path")
    require(receipt.parent.is_dir(), "receipt parent directory does not exist")
    return qualify(args.binary.resolve(strict=True), receipt.resolve())


if __name__ == "__main__":
    sys.exit(main())
