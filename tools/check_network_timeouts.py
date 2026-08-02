#!/usr/bin/env python3
"""
AetherSDR network transfer-timeout ratchet (#4688 §6, PR #4698).

WHY THIS EXISTS

A QNetworkAccessManager with no transfer timeout turns a half-open TCP
connection into a reply that never emits finished() and never emits
errorOccurred(). The surface that issued it simply stops: no error, no retry,
no way for the user to tell "slow" from "gone". #4698 swept every existing
client, but the sweep is not the durable part — the convention is. Seven call
sites set a per-request timeout for years and eleven more never got the memo,
which is exactly what a convention with no enforcement looks like after enough
merges.

So this gate is the point of that PR, not an accessory to it: it fails a new
HTTP client that forgets, rather than waiting for the next audit to find it.

WHAT IT CHECKS

A file under src/ that CONSTRUCTS a QNetworkAccessManager (news one up, wraps
one in a smart pointer, or declares one by value) must bound its transfers.
Header and implementation are treated as one unit — the member is declared in
the .h and the timeout is set in the .cpp — so evidence from either satisfies
the other.

Either form counts as bounding:

  * QNetworkAccessManager::setTransferTimeout  — covers every request the
    manager will ever issue, including ones added years from now. Preferred,
    and the reason #4698 moved off the per-request form.
  * QNetworkRequest::setTransferTimeout        — per request. Accepted because
    several long-standing clients (QrzClient, KiwiPublicDirectory,
    LocationAddressResolver, CallsignLookupService, UpdateChecker,
    WhatsNewDialog, KiwiSdrClient) are bounded that way and are not wrong,
    only easier to forget at the NEXT call site.

Merely mentioning or including the type is not construction, so a stale
#include does not trip the gate.

This is a text scanner, not a compiler. It is deliberately blunt: it asks
whether a file that owns a manager talks about transfer timeouts at all. A
false pass is possible (a second manager added to a file that already bounds a
first one); a silent, permanently-hung request is not, which is the failure
being designed out.

Usage:
    python tools/check_network_timeouts.py           # report, exit 0
    python tools/check_network_timeouts.py --strict  # exit 1 on any violation
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"

# Ways a file takes ownership of a manager. Each is (regex, description).
CONSTRUCTION_PATTERNS = [
    (re.compile(r"\bnew\s+QNetworkAccessManager\b"),
     "new QNetworkAccessManager"),
    (re.compile(r"\b(?:make_unique|make_shared|unique_ptr|shared_ptr)\s*<\s*"
                r"QNetworkAccessManager\s*>"),
     "smart-pointer QNetworkAccessManager"),
    # Value member or local: "QNetworkAccessManager m_nam;" / "... nam{...};".
    # Excludes pointer and reference declarations (those are caught at the
    # site that actually constructs the object) and function parameters.
    (re.compile(r"^\s*(?:static\s+|mutable\s+|inline\s+)*"
                r"QNetworkAccessManager\s+\w+\s*[;{=]", re.MULTILINE),
     "QNetworkAccessManager by value"),
]

BOUNDING_PATTERN = re.compile(r"\bsetTransferTimeout\s*\(")

# Units exempt from the gate. Keep this list SHORT and each entry justified —
# an entry is a promise that the requests are bounded some other way, not that
# the timeout does not matter.
ALLOWLIST = {
    "src/asr/RemoteAsrBackend":
        "runs its own QTimer + QEventLoop on a user-configurable "
        "m_config.timeoutMs; a transfer timeout could abort a legitimate "
        "remote inference before the user's own limit (#4698)",
}


def unit_key(path: Path) -> str:
    """Header and implementation of the same class share one key."""
    return str(path.relative_to(REPO).with_suffix("")).replace("\\", "/")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a src/ file owns a QNetworkAccessManager "
                    "without bounding its transfers")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a violation is found")
    args = parser.parse_args()

    if not SRC.is_dir():
        print(f"FAIL: {SRC} not found — run from the repository.")
        return 1

    # unit -> {"constructs": [(path, line, why)], "bounded": bool}
    units: dict[str, dict] = {}

    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".cc", ".mm"):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:  # unreadable file is a real problem, not a skip
            print(f"FAIL: cannot read {path}: {exc}")
            return 1

        if "QNetworkAccessManager" not in text and "setTransferTimeout" not in text:
            continue

        key = unit_key(path)
        entry = units.setdefault(key, {"constructs": [], "bounded": False})

        if BOUNDING_PATTERN.search(text):
            entry["bounded"] = True

        for pattern, why in CONSTRUCTION_PATTERNS:
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                entry["constructs"].append((path, line, why))

    violations = []
    exempt = []
    checked = 0

    for key, entry in sorted(units.items()):
        if not entry["constructs"]:
            continue
        if key in ALLOWLIST:
            exempt.append(key)
            continue
        checked += 1
        if not entry["bounded"]:
            violations.append((key, entry["constructs"][0]))

    print(f"Network transfer-timeout ratchet: {checked} manager-owning unit(s) "
          f"checked, {len(exempt)} allow-listed, {len(violations)} violation(s).")

    for key in exempt:
        print(f"  allow-listed: {key} — {ALLOWLIST[key]}")

    for key, (path, line, why) in violations:
        rel = path.relative_to(REPO)
        print(f"::error file={rel},line={line}::"
              f"{why} here, but neither {key}.h nor {key}.cpp calls "
              f"setTransferTimeout(). An unbounded manager turns a half-open "
              f"connection into a reply that never finishes and never errors "
              f"(#4688 §6).")

    if violations:
        print(
            "\nSet the timeout on the MANAGER, not the request:\n"
            "\n"
            "    namespace {\n"
            "    // Stall timeout for <what this client fetches> (#4688 §6).\n"
            "    constexpr int kTransferTimeoutMs = 15000;  // 30000 for large downloads\n"
            "    } // namespace\n"
            "    ...\n"
            "    m_nam->setTransferTimeout(kTransferTimeoutMs);\n"
            "\n"
            "It is a STALL timeout: the clock resets on every byte received, so "
            "it bounds a dead\nconnection without capping a slow one — safe even "
            "on a multi-hundred-MB download.\nNote it also covers the wait for "
            "the first byte, so allow headroom where the server\nhas real "
            "think time before it starts writing.\n"
            "\n"
            "If the requests are genuinely bounded some other way, add the unit "
            "to ALLOWLIST in\nthis script with the reason.")
        return 1 if args.strict else 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
