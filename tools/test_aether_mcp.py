#!/usr/bin/env python3
"""
Field-mapping / protocol regression test for the MCP server (tools/aether_mcp.py).

Runs WITHOUT a live app: it stubs the bridge transport and asserts that each
MCP tool emits the exact request the AutomationServer verb registry reads
(e.g. shortcut → `id`, not `value`), and that the JSON-RPC loop rejects
non-object messages. This is the guard jensenpat asked for on #4177 so the
schema ↔ bridge field mapping cannot silently drift again.

    python3 tools/test_aether_mcp.py     # exits non-zero on any failure
"""

import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aether_mcp  # noqa: E402

_REAL_BRIDGE_REQUEST = aether_mcp.bridge_request  # save before any monkeypatch
_failures = []


def check(name, cond, detail=""):
    if cond:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}  {detail}")
        _failures.append(name)


# ── Tool → bridge request field mapping ──────────────────────────────────────
# Stub bridge_request to capture the request dicts and return a canned reply.

_captured = []


def _fake_bridge_request(obj, timeout=None):
    _captured.append(obj)
    cmd = obj.get("cmd")
    if cmd == "ping":
        return {"ok": True, "authRequired": False}
    if cmd == "whoami":
        return {"ok": True, "pid": 1, "socket": "/tmp/x"}
    if cmd == "grab":
        return {"ok": True, "path": "/nonexistent.png"}
    return {"ok": True, "cmd": cmd, "echo": obj}


def run_tool(name, args):
    _captured.clear()
    aether_mcp.handle_tool(name, args)
    return list(_captured)


def test_field_mapping():
    aether_mcp.bridge_request = _fake_bridge_request  # monkeypatch

    reqs = run_tool("shortcut", {"id": "band_zoom"})
    r = reqs[-1]
    check("shortcut sends cmd=shortcut", r.get("cmd") == "shortcut")
    check("shortcut sends id (not value)",
          r.get("id") == "band_zoom" and "value" not in r, detail=str(r))

    reqs = run_tool("invoke", {"target": "Master volume", "action": "setValue",
                               "value": "35"})
    r = reqs[-1]
    check("invoke target/action/value",
          r.get("cmd") == "invoke" and r.get("target") == "Master volume"
          and r.get("action") == "setValue" and r.get("value") == "35", str(r))

    reqs = run_tool("get_state", {"model": "slice", "selector": "active",
                                  "property": "frequency"})
    r = reqs[-1]
    check("get_state model/selector/property",
          r.get("cmd") == "get" and r.get("model") == "slice"
          and r.get("selector") == "active" and r.get("property") == "frequency", str(r))

    reqs = run_tool("grab_widget", {"target": "SpectrumWidget"})
    r = reqs[0]
    check("grab_widget sends cmd=grab + target",
          r.get("cmd") == "grab" and r.get("target") == "SpectrumWidget", str(r))

    reqs = run_tool("dump_tree", {})
    check("dump_tree sends cmd=dumpTree", reqs[-1].get("cmd") == "dumpTree", str(reqs))

    reqs = run_tool("bridge_command", {"request": {"cmd": "whoami"}})
    check("bridge_command passes raw request", reqs[-1].get("cmd") == "whoami", str(reqs))


def test_token_attached():
    # With AETHER_MCP_TOKEN set, real bridge_request must attach it to every req.
    os.environ["AETHER_MCP_TOKEN"] = "secret-xyz"
    seen = {}

    class _StubBridge:
        def __init__(self, *a, **k):
            pass

        def request(self, obj):
            seen.update(obj)
            return {"ok": True}

        def close(self):
            pass

    orig_bridge = aether_mcp.Bridge
    orig_sockpath = aether_mcp.bridge_socket_path
    orig_req = aether_mcp.bridge_request
    aether_mcp.Bridge = _StubBridge
    aether_mcp.bridge_socket_path = lambda: "/tmp/fake.sock"
    aether_mcp.bridge_request = _REAL_BRIDGE_REQUEST  # exercise the real token-attach path
    try:
        aether_mcp.bridge_request({"cmd": "get", "model": "radio"})
        check("token attached from AETHER_MCP_TOKEN", seen.get("token") == "secret-xyz",
              str(seen))
    finally:
        aether_mcp.Bridge = orig_bridge
        aether_mcp.bridge_socket_path = orig_sockpath
        aether_mcp.bridge_request = orig_req
        del os.environ["AETHER_MCP_TOKEN"]


# ── JSON-RPC loop robustness (non-dict must not crash the loop) ──────────────

def drive_main(lines):
    """Feed newline-delimited JSON-RPC into main(), capture stdout responses."""
    stdin, stdout = sys.stdin, sys.stdout
    sys.stdin = io.StringIO("\n".join(lines) + "\n")
    sys.stdout = io.StringIO()
    try:
        aether_mcp.main()
        out = sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = stdin, stdout
    return [json.loads(l) for l in out.splitlines() if l.strip()]


def test_jsonrpc_robustness():
    resps = drive_main([
        json.dumps([1, 2, 3]),            # batch array — must be rejected, not crash
        json.dumps("scalar"),             # bare scalar — rejected
        json.dumps({"jsonrpc": "2.0", "id": 7, "method": "tools/list"}),  # still served
    ])
    codes = [r.get("error", {}).get("code") for r in resps]
    check("batch array rejected -32600", -32600 in codes, str(resps))
    served = [r for r in resps if r.get("id") == 7 and "result" in r]
    check("loop keeps serving after bad input", len(served) == 1, str(resps))


if __name__ == "__main__":
    test_field_mapping()
    test_token_attached()
    test_jsonrpc_robustness()
    if _failures:
        print(f"\n{len(_failures)} FAILED: {_failures}")
        sys.exit(1)
    print("\nall MCP field-mapping/protocol checks passed")
