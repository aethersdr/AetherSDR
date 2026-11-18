#!/usr/bin/env python3
"""Opt-in setup-window lifetime checks with real keychain/locks and injected HTTP."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import tempfile
import uuid

from qualify_native_credentials import Child, atomic_json, require


def qualify(binary, credential_binary, receipt):
    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(), "system": platform.system(),
        "machine": platform.machine(), "platform": platform.platform(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "credential_binary_sha256": hashlib.sha256(credential_binary.read_bytes()).hexdigest(),
        "scope": "Production Remote Access window/client/token persistence with injected HTTP only; real OS keychain and same-user process locks. Private settings, production cache/lock location. No real radio, Tailscale request or RF.",
        "cases": [], "success": False,
    }
    atomic_json(receipt, result)
    env = dict(os.environ, QT_QPA_PLATFORM="cocoa" if platform.system() == "Darwin" else "offscreen")
    with tempfile.TemporaryDirectory(prefix="aetherpd-native-enrollment-") as temporary:
        directory = Path(temporary)
        unavailable = False
        for managed in (False, True):
            for kind in ("live_request", "closed_window", "pending_backend", "explicit_retry",
                         "failed_request", "different_radio", "caller_crash"):
                identifier = "aetherpd-native-credential-" + uuid.uuid4().hex
                initial = "fixture-initial"
                issued = initial if managed else "fixture-first"
                case = {"kind": kind, "managed": managed, "identifiers": [identifier],
                        "operations": [], "cleanup": []}
                result["cases"].append(case)
                children = []
                admitted = []
                prepared = False

                def save():
                    atomic_json(receipt, result)

                def start(mode, current=identifier, expected=initial, next_token=issued, storage=False):
                    suffix = "-managed" if managed and not storage else ""
                    child = Child(credential_binary if storage else binary, mode + suffix, current,
                                  expected, directory, env, extra_args=() if storage else (next_token,))
                    children.append(child)
                    case.setdefault("children", []).append({"pid": child.process.pid, "mode": mode,
                                                            "identifier": current, "storage_only": storage})
                    save()
                    return child

                def command(mode, current=identifier, expected="", next_token=issued, code=0, cleanup=False):
                    record = {"mode": mode, "identifier": current}
                    case["cleanup" if cleanup else "operations"].append(record)
                    save()
                    child = start(mode, current, expected, next_token)
                    record.update(child.finish(expected=code))
                    save()
                    return record

                def admit(current):
                    command("absent", current)
                    command("absent-metadata", current)
                    admitted.append(current)
                    command("write", current, initial)

                def finish(child, killed=False):
                    record = child.finish(killed=killed)
                    case.setdefault("child_results", []).append(record)
                    save()

                try:
                    admit(identifier)
                    prepared = True
                    actor = start("actor")
                    actor.expect("request_sent_with_fresh_credential")
                    if kind == "closed_window":
                        actor.send("close")
                        actor.expect("window_closed")
                    if kind == "pending_backend":
                        holder = start("lease-holder", storage=True)
                        holder.expect("lease_held")
                        actor.send("complete")
                        actor.expect("reply_delivered")
                        command("try", code=4)
                        actor.send("close")
                        actor.expect("window_closed")
                        command("try", code=4)
                        holder.send("release")
                        holder.expect("lease_released")
                        finish(holder)
                        actor.expect("setup_released")
                        finish(actor)
                    elif kind == "different_radio":
                        other = "aetherpd-native-credential-" + uuid.uuid4().hex
                        case["identifiers"].append(other)
                        admit(other)
                        other_token = initial if managed else "fixture-other"
                        record = command("try", other, initial, other_token)
                        require("request_sent_with_fresh_credential" in record["output"]
                                and "setup_released" in record["output"], "different radio did not complete")
                        command("read", other, other_token)
                        actor.send("complete")
                        actor.expect("reply_delivered")
                        actor.expect("setup_released")
                        finish(actor)
                    elif kind == "caller_crash":
                        actor.process.kill()
                        finish(actor, killed=True)
                        command("read", expected=initial)
                        after = initial if managed else "fixture-after-crash"
                        command("try", expected=initial, next_token=after)
                        command("read", expected=after)
                    else:
                        command("try", code=4)
                        actor.send("reject" if kind == "failed_request" else "complete")
                        actor.expect("reply_delivered")
                        actor.expect("setup_released")
                        finish(actor)
                    if kind != "caller_crash":
                        expected = initial if kind == "failed_request" else issued
                        command("read", expected=expected)
                        if kind in ("explicit_retry", "failed_request"):
                            after = expected if managed else "fixture-next"
                            command("try", expected=expected, next_token=after)
                            command("read", expected=after)
                    case["passed"] = True
                except Exception as error:
                    case["failure"] = str(error)
                    unavailable = not prepared
                finally:
                    for child in children:
                        try:
                            child.close()
                        except Exception as error:
                            case.setdefault("cleanup_errors", []).append(str(error))
                    for current in admitted:
                        for mode in ("cleanup", "cleanup", "absent", "absent-metadata"):
                            try:
                                command(mode, current, cleanup=True)
                            except Exception as error:
                                case.setdefault("cleanup_errors", []).append(str(error))
                    case["admitted_identifiers_cleaned"] = not case.get("cleanup_errors") and bool(admitted)
                    save()
                print(json.dumps({"case": kind, "managed": managed, "passed": case.get("passed", False),
                                  "cleaned": case["admitted_identifiers_cleaned"]}), flush=True)
                if unavailable:
                    break
            if unavailable:
                break
    result["success"] = len(result["cases"]) == 14 and all(
        case.get("passed") and case["admitted_identifiers_cleaned"] for case in result["cases"])
    atomic_json(receipt, result)
    print(json.dumps({"success": result["success"], "cases": len(result["cases"]), "receipt": str(receipt)}))
    return 0 if result["success"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--credential-binary", required=True, type=Path)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--receipt", type=Path)
    output.add_argument("--receipt-directory", type=Path)
    parser.add_argument("--allow-disposable-keychain-writes", action="store_true", required=True)
    args = parser.parse_args()
    receipt = args.receipt if args.receipt else args.receipt_directory / ("native-enrollment-" + uuid.uuid4().hex + ".json")
    require(not receipt.exists(), "receipt already exists; choose a new path")
    require(receipt.parent.is_dir(), "receipt parent directory does not exist")
    return qualify(args.binary.resolve(strict=True), args.credential_binary.resolve(strict=True), receipt.resolve())


if __name__ == "__main__":
    sys.exit(main())
