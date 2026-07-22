#!/usr/bin/env python3
"""Focused request-mapping checks for tools/automation_probe.py."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import automation_probe  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def test_drag_at_mapping():
    request = automation_probe.MAPPERS["dragAt"](
        ["SpectrumWidget", "1550", "250", "0", "400", "control"])
    check(request == {
        "target": "SpectrumWidget",
        "value": "1550 250 0 400 control",
    }, f"unexpected dragAt mapping: {request}")


def test_memory_mapping():
    request = automation_probe.MAPPERS["memory"](["activate", "7", "0x40000000"])
    check(request == {
        "action": "activate",
        "value": "7 0x40000000",
    }, f"unexpected memory mapping: {request}")


def test_token_attachment():
    captured = {}
    bridge = automation_probe.Bridge.__new__(automation_probe.Bridge)

    def fake_request_line(text, timeout_seconds=None):
        captured.update(json.loads(text))
        captured["timeout"] = timeout_seconds
        return {"ok": True}

    bridge.request_line = fake_request_line
    old_token = os.environ.get("AETHER_MCP_TOKEN")
    os.environ["AETHER_MCP_TOKEN"] = "test-token"
    try:
        response = bridge.request({"cmd": "verbs"}, timeout_seconds=3)
    finally:
        if old_token is None:
            os.environ.pop("AETHER_MCP_TOKEN", None)
        else:
            os.environ["AETHER_MCP_TOKEN"] = old_token

    check(response == {"ok": True}, f"unexpected bridge response: {response}")
    check(captured.get("token") == "test-token", f"token not attached: {captured}")
    check(captured.get("timeout") == 3, f"timeout not forwarded: {captured}")


if __name__ == "__main__":
    test_drag_at_mapping()
    test_memory_mapping()
    test_token_attachment()
    print("automation probe request-mapping checks passed")
