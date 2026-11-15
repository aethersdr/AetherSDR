#!/usr/bin/env python3
"""
AetherSDR theme-seed freshness checker.

The compiled seed table is now GENERATED from resources/themes/default-dark.json
(tools/gen_theme_seed.py -> src/core/ThemeSeedGenerated.cpp), so the seed and the
resource can no longer disagree by drifting apart. What CAN still happen is
someone editing the bundled theme and not regenerating, leaving a stale table in
the tree — which would quietly reintroduce exactly the class of bug the generator
removed.

This checks that, and nothing else.

WHAT CHANGED: before the generator landed, this file tracked a KNOWN_SEED_DRIFT
baseline of the nine tokens that had already diverged by hand (`color.accent.dim`
and all eight `color.slice.a..h`). That list is gone because the class of bug is
gone: generation makes the resource authoritative by construction, and those nine
are now whatever the JSON says. The gate survives in a stronger form — it used to
freeze existing drift, it now prevents any. (#3184)

Usage:
    python tools/check_theme_seed.py            # report, exit 0
    python tools/check_theme_seed.py --strict   # exit 1 when stale
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GENERATOR = REPO / "tools" / "gen_theme_seed.py"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when the generated theme seed is stale")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when the generated file needs regenerating")
    args = parser.parse_args()

    if not GENERATOR.exists():
        print(f"FAIL: {GENERATOR.relative_to(REPO)} is missing — the seed cannot "
              f"be verified.")
        return 1

    # Delegate rather than reimplement. The generator already knows how to
    # produce the canonical output; a second implementation here would be a new
    # place for the two to disagree, which is the very failure being designed out.
    result = subprocess.run([sys.executable, str(GENERATOR), "--check"],
                            capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)

    if result.returncode == 0:
        return 0

    print("\nThe bundled theme changed without regenerating the compiled seed.\n"
          "Run:  python tools/gen_theme_seed.py   and commit the result.")
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
