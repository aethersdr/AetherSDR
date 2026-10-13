#!/usr/bin/env python3
"""
Self-test for tools/check_wdsp_test_links.py.

Each case builds a small synthetic tree (tests/tests.cmake, tests/, src/) in a
scratch directory, runs the checker's check() on it, and requires either no
finding or a finding containing a given substring. The include-cycle case is
the one a cycle guard that memoises "no WDSP" for an unfinished file gets
wrong: the second test reaches WDSP only through a header whose first scan was
cut short by the cycle.

Usage:
    python tools/test_check_wdsp_test_links.py
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_wdsp_test_links as checker  # noqa: E402

BASE_CMAKE = """\
add_executable(plain_test tests/plain_test.cpp)
target_link_libraries(plain_test PRIVATE aethercore Qt6::Core)
"""
BASE_FILES = {
    "tests/plain_test.cpp": '#include "core/dsp/WdspChannel.h"\nint main() { return 0; }\n',
    "src/core/dsp/WdspChannel.h": "#pragma once\nclass WdspChannel {};\n",
}

# (name, extra cmake, extra files, allowlist, substrings: [] = must be clean)
CASES = [
    ("clean tree", "", {}, {}, []),
    ("direct WDSP include",
     "add_executable(direct_test tests/direct_test.cpp)\n"
     "target_link_libraries(direct_test PRIVATE aethercore)\n",
     {"tests/direct_test.cpp": "#include <aether_wdsp.h>\n"},
     {}, ["direct_test: tests/direct_test.cpp includes aether_wdsp.h"]),
    ("a WDSP include inside a block comment is not one",
     "add_executable(comment_test tests/comment_test.cpp)\n",
     {"tests/comment_test.cpp": "/*\n#include <aether_wdsp.h>\n*/\n"},
     {}, []),
    ("transitive through a quoted repo header",
     "add_executable(deep_test tests/deep_test.cpp)\n"
     "target_link_libraries(deep_test PRIVATE aethercore)\n",
     {"tests/deep_test.cpp": '#include "core/Mid.h"\n',
      "src/core/Mid.h": '#include "core/Leaf.h"\n',
      "src/core/Leaf.h": '#include "../third_party/wdsp/port/include/wdsp_port.h"\n'},
     {}, ["deep_test: tests/deep_test.cpp includes core/Mid.h -> core/Leaf.h -> "]),
    ("include cycle does not hide the header for a later includer",
     "add_executable(cycle_a_test tests/cycle_a_test.cpp)\n"
     "add_executable(cycle_b_test tests/cycle_b_test.cpp)\n",
     {"tests/cycle_a_test.cpp": '#include "cyc/A.h"\n',
      "tests/cycle_b_test.cpp": '#include "cyc/B.h"\n',
      "src/cyc/A.h": '#include "cyc/B.h"\n#include <aether_wdsp.h>\n',
      "src/cyc/B.h": '#include "cyc/A.h"\n'},
     {}, ["cycle_a_test: tests/cycle_a_test.cpp includes cyc/A.h",
          "cycle_b_test: tests/cycle_b_test.cpp includes cyc/B.h -> cyc/A.h -> aether_wdsp.h"]),
    ("linking aether_wdsp with no WDSP include",
     "add_executable(linked_test tests/linked_test.cpp)\n"
     "target_link_libraries(linked_test PRIVATE aethercore aether_wdsp)\n",
     {"tests/linked_test.cpp": "int main() { return 0; }\n"},
     {}, ["linked_test links aether_wdsp", "Do not link aether_wdsp"]),
    ("a commented-out link is not one",
     "add_executable(quiet_test tests/quiet_test.cpp)\n"
     "# target_link_libraries(quiet_test PRIVATE aether_wdsp)\n",
     {"tests/quiet_test.cpp": "int main() { return 0; }\n"},
     {}, []),
    ("source behind a variable is still scanned",
     "set(HIDDEN tests/hidden_test.cpp)\nadd_executable(hidden_test ${HIDDEN})\n",
     {"tests/hidden_test.cpp": "#include <aether_wdsp.h>\n"},
     {}, ["tests/hidden_test.cpp includes aether_wdsp.h"]),
    ("a header under tests/ that reaches WDSP",
     "",
     {"tests/support/Helper.h": '#include "core/Leaf.h"\n',
      "src/core/Leaf.h": "#include <aether_wdsp.h>\n"},
     {}, ["tests/support/Helper.h includes core/Leaf.h -> aether_wdsp.h"]),
    ("allowlisted pure WDSP unit test",
     "add_executable(pure_test tests/pure_test.cpp)\n"
     "target_link_libraries(pure_test PRIVATE aether_wdsp)\n",
     {"tests/pure_test.cpp": "#include <aether_wdsp.h>\n"},
     {"pure_test": "pure WDSP unit test"}, []),
    ("allowlisted test that also links aethercore holds two copies",
     "add_executable(pure_test tests/pure_test.cpp)\n"
     "target_link_libraries(pure_test PRIVATE aethercore aether_wdsp)\n",
     {"tests/pure_test.cpp": "#include <aether_wdsp.h>\n"},
     {"pure_test": "pure WDSP unit test"},
     ["ALLOWLIST: pure_test links aethercore"]),
    ("stale allowlist entry: no WDSP include any more",
     "add_executable(pure_test tests/pure_test.cpp)\n",
     {"tests/pure_test.cpp": "int main() { return 0; }\n"},
     {"pure_test": "pure WDSP unit test"},
     ["ALLOWLIST: pure_test no longer includes a WDSP header"]),
    ("stale allowlist entry: no such target",
     "", {}, {"gone_test": "pure WDSP unit test"},
     ["ALLOWLIST: gone_test is not a target"]),
]


def run_case(extra_cmake: str, extra_files: dict[str, str],
             allowlist: dict[str, str]) -> list[str]:
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch).resolve()
        files = {**BASE_FILES, **extra_files,
                 "tests/tests.cmake": BASE_CMAKE + extra_cmake}
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        errors, _ = checker.check(root, allowlist)
        return errors


def main() -> int:
    failures = 0
    for name, extra_cmake, extra_files, allowlist, expected in CASES:
        errors = run_case(extra_cmake, extra_files, allowlist)
        if not expected:
            ok = not errors
        else:
            ok = (len(errors) >= 1
                  and all(any(want in error for error in errors) for want in expected))
        print(f"{'PASS' if ok else 'FAIL'}: {name}")
        if not ok:
            failures += 1
            print(f"  expected: {expected or 'no finding'}")
            for error in errors or ["(no finding)"]:
                print(f"  got: {error}")

    # The real allowlist is live against the real tree.
    errors, targets = checker.check()
    ok = not errors and targets >= 100
    print(f"{'PASS' if ok else 'FAIL'}: real tree ({targets} targets)")
    failures += 0 if ok else 1
    for error in errors:
        print(f"  got: {error}")

    print(f"{len(CASES) + 1 - failures}/{len(CASES) + 1} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
