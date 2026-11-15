#!/usr/bin/env python3
"""Argument-parsing checks for tools/automation_logwatch.py (#4912).

The bug: `main()` declared `rest` as `nargs="*"` and then indexed it blind —
`args.rest[0]`, `args.rest[1]` — so

    automation_logwatch.py set "lcConnection=true;lcHl2=true"

raised `IndexError: list index out of range`, because that batch form is ONE
shell token. Its traceback then interleaved on stderr with a concurrent `tail`
on stdout, which is how it was found. The quieter half was worse: the
`a=true;b=true` form was never a syntax the tool accepted, so the command did
nothing even when it did not crash.

These tests cover the parsers only. They take no socket and no bridge, which is
the point — arity is validated before anything connects.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import automation_logwatch as lw  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_usage_error(fn, *args):
    try:
        fn(*args)
    except lw.UsageError:
        return
    raise AssertionError("expected a UsageError from %s%r" % (fn.__name__, args))


def test_set_documented_form():
    check(lw.parse_set_targets(["lcAutomation", "on"]) == [("lcAutomation", True)],
          "single category on")
    check(lw.parse_set_targets(["lcAutomation", "off"]) == [("lcAutomation", False)],
          "single category off")
    check(lw.parse_set_targets(["all", "true"]) == [("all", True)],
          "'all' is just a category name here")
    check(lw.parse_set_targets(["lcConnection", "lcHl2", "on"])
          == [("lcConnection", True), ("lcHl2", True)],
          "several categories share one state")


def test_set_batch_form():
    # The exact invocation that produced the IndexError.
    check(lw.parse_set_targets(["lcConnection=true;lcHl2=true;lcAutomation=true"])
          == [("lcConnection", True), ("lcHl2", True), ("lcAutomation", True)],
          "the one-token batch form is accepted")
    check(lw.parse_set_targets(["lcHl2=off"]) == [("lcHl2", False)],
          "a single pair is a batch of one")
    check(lw.parse_set_targets(["a=true;", " b = false "])
          == [("a", True), ("b", False)],
          "stray separators and spacing are tolerated")


def test_set_rejections_are_usage_not_tracebacks():
    expect_usage_error(lw.parse_set_targets, [])
    expect_usage_error(lw.parse_set_targets, ["lcAutomation"])      # the old IndexError
    expect_usage_error(lw.parse_set_targets, ["lcAutomation", "maybe"])
    expect_usage_error(lw.parse_set_targets, ["=true"])
    expect_usage_error(lw.parse_set_targets, ["lcHl2=perhaps"])


def test_tail_arguments():
    check(lw.parse_tail_args([]) == (100, None), "tail defaults to the newest 100")
    check(lw.parse_tail_args(["50"]) == (50, None), "a bare count is the count")
    # Documented in the module docstring since day one, and a ValueError
    # traceback before this change.
    check(lw.parse_tail_args(["since=42"]) == (100, 42), "since= alone is accepted")
    check(lw.parse_tail_args(["25", "since=42"]) == (25, 42), "count then since")
    check(lw.parse_tail_args(["since=42", "25"]) == (25, 42), "since then count")
    expect_usage_error(lw.parse_tail_args, ["everything"])
    expect_usage_error(lw.parse_tail_args, ["since=soon"])
    expect_usage_error(lw.parse_tail_args, ["10", "20"])


def test_onoff_words():
    for word in ("on", "ON", "true", "1", "yes", "enabled"):
        check(lw.parse_onoff(word) is True, "%r should read as on" % word)
    for word in ("off", "OFF", "false", "0", "no", "disabled"):
        check(lw.parse_onoff(word) is False, "%r should read as off" % word)
    expect_usage_error(lw.parse_onoff, "sometimes")


if __name__ == "__main__":
    test_set_documented_form()
    test_set_batch_form()
    test_set_rejections_are_usage_not_tracebacks()
    test_tail_arguments()
    test_onoff_words()
    print("automation logwatch argument checks passed")
