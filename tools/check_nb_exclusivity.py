#!/usr/bin/env python3
"""
Noise-blanker exclusivity ratchet: WdspChannel must never leave both impulse
blankers running.

WDSP has two, ANB (nob.c) and NOB (nobII.c). They watch the same detector and
blank the same window, so with both running the second reconstructs what the
first just zeroed. WdspChannel::setNoiseBlanker() keeps them exclusive by
writing BOTH run flags on every call, outside the per-kind branches, so the
stage that was not chosen is explicitly stopped rather than left as it was.

This is a text check because it cannot be anything else: WDSP's EXT API offers
no way to read a run flag back, so no runtime test can observe which stages are
running. What is checkable is the shape that makes the invariant hold -- each
Run setter called exactly once per call, and neither one inside a branch. Moving
either call inside a kind branch is the edit this exists to stop.

Usage:
    python tools/check_nb_exclusivity.py           # report, exit 0
    python tools/check_nb_exclusivity.py --strict  # exit 1 on any finding
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "src" / "core" / "dsp" / "WdspChannel.cpp"

SETTER = "::setNoiseBlanker("
BRANCHES = ("Impulse", "Advanced")
RUN_SETTERS = ("SetEXTANBRun", "SetEXTNOBRun")


def _branch_if(kind: str) -> re.Pattern[str]:
    """The `if (kind == NoiseBlanker::<kind>) {` that opens one stage's branch.

    Anchored on the CONDITION, not on the enum token: the token also appears in
    each run flag's own argument, and a check that matched there would hand the
    wrong block to the inside-a-branch test and would still call itself healthy
    after the branch it meant to read had been renamed away.
    """
    return re.compile(
        r"if\s*\(\s*kind\s*==\s*NoiseBlanker::" + kind + r"\s*\)\s*\{")

# Comments and literals are not code. A flush or a run flag named in a comment
# must not satisfy this check, because a comment claiming one is exactly the
# failure that made the blanker's own documentation wrong (#5236).
_STRIP = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S,
)
# Digit separators are not quotes: 1'000 would otherwise open a char literal
# and hide the rest of the line. Borrowed from tools/check_qsettings.py.
_NUMBER = re.compile(r"(?<!['\w])\d[\w.']*")


def strip_noncode(text: str) -> str:
    text = _NUMBER.sub(lambda m: m.group(0).replace("'", ""), text)
    return _STRIP.sub(" ", text)


def block_at(text: str, start: int) -> str:
    """The brace-delimited block that follows `start`, braces included."""
    open_at = text.find("{", start)
    if open_at < 0:
        return ""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at:i + 1]
    return ""


def calls(text: str, name: str) -> int:
    """Calls to `name`. C++ allows whitespace before the argument list."""
    return len(re.findall(r"\b" + re.escape(name) + r"\s*\(", text))


def findings_for(source: str) -> list[str]:
    code = strip_noncode(source)

    at = code.find(SETTER)
    if at < 0:
        return [f"no {SETTER} found -- this ratchet is no longer measuring anything"]
    setter = block_at(code, at)
    if not setter:
        return [f"{SETTER} has no parsable body -- this ratchet is measuring nothing"]

    branches = {}
    for kind in BRANCHES:
        hits = list(_branch_if(kind).finditer(setter))
        if len(hits) != 1:
            return [f"setNoiseBlanker() has {len(hits)} `if (kind == "
                    f"NoiseBlanker::{kind})` branches, expected exactly 1 -- "
                    "this ratchet is measuring nothing"]
        body = block_at(setter, hits[0].start())
        if not body:
            return [f"the `NoiseBlanker::{kind}` branch has no parsable block -- "
                    "this ratchet is measuring nothing"]
        branches[f"NoiseBlanker::{kind}"] = body

    out = []
    for run in RUN_SETTERS:
        n = calls(setter, run)
        if n != 1:
            out.append(f"{run} is called {n} time(s) in setNoiseBlanker(), expected "
                       "exactly 1: both run flags are written on every call so the "
                       "unchosen stage is always stopped")
        for kind, body in branches.items():
            inside = calls(body, run)
            if inside:
                out.append(f"{run} is called inside the `{kind}` branch; it belongs "
                           "outside both kind branches, or switching kind starts one "
                           "stage without stopping the other and both blank the same "
                           "window")
    return out


# The healthy shape, then the one mutation that matters, then a decoy. If the
# healthy miniature failed or the mutation passed, the check is not measuring.
_HEALTHY = """
bool WdspChannel::setNoiseBlanker(NoiseBlanker kind, int level, NoiseBlankerFill fill) noexcept
{
    if (m_nbOpen) {
        if (kind == NoiseBlanker::Impulse) {
            flush_anbEXT(m_channelId);
        } else if (kind == NoiseBlanker::Advanced) {
            flush_nobEXT(m_channelId);
        }
        SetEXTANBRun(m_channelId, kind == NoiseBlanker::Impulse ? 1 : 0);
        SetEXTNOBRun(m_channelId, kind == NoiseBlanker::Advanced ? 1 : 0);
    }
    return true;
}
"""

# Run flags moved inside the branches: switching to NB2 starts the NOB and never
# stops the ANB. What an "only touch what changed" edit produces.
_BOTH_RUN = """
bool WdspChannel::setNoiseBlanker(NoiseBlanker kind, int level, NoiseBlankerFill fill) noexcept
{
    if (m_nbOpen) {
        if (kind == NoiseBlanker::Impulse) {
            flush_anbEXT(m_channelId);
            SetEXTANBRun(m_channelId, 1);
        } else if (kind == NoiseBlanker::Advanced) {
            flush_nobEXT(m_channelId);
            SetEXTNOBRun(m_channelId, 1);
        }
    }
    return true;
}
"""

# A run flag that exists only in a comment satisfies nothing.
_COMMENT_ONLY = """
bool WdspChannel::setNoiseBlanker(NoiseBlanker kind, int level, NoiseBlankerFill fill) noexcept
{
    if (m_nbOpen) {
        if (kind == NoiseBlanker::Impulse) {
            flush_anbEXT(m_channelId);
        } else if (kind == NoiseBlanker::Advanced) {
            // SetEXTANBRun(m_channelId, 0);
            flush_nobEXT(m_channelId);
        }
        SetEXTANBRun(m_channelId, kind == NoiseBlanker::Impulse ? 1 : 0);
        SetEXTNOBRun(m_channelId, kind == NoiseBlanker::Advanced ? 1 : 0);
    }
    return true;
}
"""

# The branch this reads, renamed away. The run flags are untouched and still
# look healthy, so a check anchored on the enum token rather than on the
# condition reports OK here -- measuring nothing and saying so is the one
# failure a text check cannot be allowed to have.
_BRANCH_RENAMED = _HEALTHY.replace(
    "if (kind == NoiseBlanker::Impulse) {",
    "if (kindOfThing == NoiseBlanker::ImpulseRenamed) {")

_SELF_TEST = [
    ("healthy", _HEALTHY, True),
    ("run flags inside the kind branches", _BOTH_RUN, False),
    ("a run flag in a comment only", _COMMENT_ONLY, True),
    ("the kind branch renamed away", _BRANCH_RENAMED, False),
]


def self_test() -> list[str]:
    broken = []
    for name, source, want_clean in _SELF_TEST:
        clean = not findings_for(source)
        if clean != want_clean:
            broken.append(f"self-test: the {name!r} miniature should be "
                          f"{'accepted' if want_clean else 'rejected'}")
    return broken


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    args = parser.parse_args()

    broken = self_test()
    if broken:
        for line in broken:
            print(f"NB-EXCLUSIVITY: {line}")
        return 1

    if not TARGET.exists():
        print(f"NB-EXCLUSIVITY: {TARGET.relative_to(REPO).as_posix()} is missing")
        return 1

    findings = findings_for(TARGET.read_text(encoding="utf-8", errors="replace"))
    rel = TARGET.relative_to(REPO).as_posix()
    for line in findings:
        print(f"NB-EXCLUSIVITY: {rel}: {line}")
    if not findings:
        print("Noise-blanker exclusivity ratchet: OK "
              "(both run flags written once, outside the kind branches)")
    return 1 if findings and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
