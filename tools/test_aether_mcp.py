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
import tempfile

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
    check("grab_widget default path lands in the temp dir",
          r.get("path", "").startswith(tempfile.gettempdir()), str(r))

    reqs = run_tool("grab_widget", {"target": "SpectrumWidget",
                                    "path": "/tmp/shot.png"})
    r = reqs[0]
    check("grab_widget honors caller-supplied path (#4249)",
          r.get("path") == "/tmp/shot.png", str(r))

    reqs = run_tool("dump_tree", {})
    check("dump_tree sends cmd=dumpTree", reqs[-1].get("cmd") == "dumpTree", str(reqs))

    # Promoted first-class verbs (#4188 area 1) — the registry reads
    # action/value/path; these must map exactly or the tool is dead.
    r = run_tool("get_log", {"count": 50, "since": 7})[-1]
    check("get_log → log tail with count+since",
          r.get("cmd") == "log" and r.get("action") == "tail"
          and r.get("value") == "50 since=7", str(r))

    r = run_tool("connect", {"action": "ip", "value": "192.168.50.100"})[-1]
    check("connect → cmd=connect action+value",
          r.get("cmd") == "connect" and r.get("action") == "ip"
          and r.get("value") == "192.168.50.100", str(r))

    r = run_tool("disconnect", {})[-1]
    check("disconnect → cmd=disconnect", r.get("cmd") == "disconnect", str(r))

    r = run_tool("capture_audio",
                 {"action": "start", "value": "3000 raw,post"})[-1]
    check("capture_audio → cmd=audioCapture action+value",
          r.get("cmd") == "audioCapture" and r.get("action") == "start"
          and r.get("value") == "3000 raw,post", str(r))

    r = run_tool("floors", {})[-1]
    check("floors → cmd=floors", r.get("cmd") == "floors", str(r))

    r = run_tool("tune", {"mhz": "14.074"})[-1]
    check("tune → cmd=tune value(mhz)",
          r.get("cmd") == "tune" and r.get("value") == "14.074"
          and "id" not in r, str(r))

    r = run_tool("tune", {"mhz": "14.074", "sliceId": "1"})[-1]
    check("tune → cmd=tune value(mhz)+id(sliceId)",
          r.get("cmd") == "tune" and r.get("value") == "14.074"
          and r.get("id") == "1", str(r))

    r = run_tool("slice", {"action": "select", "value": "3"})[-1]
    check("slice → cmd=slice action+value",
          r.get("cmd") == "slice" and r.get("action") == "select"
          and r.get("value") == "3", str(r))

    r = run_tool("pan", {"action": "center", "value": "7.1"})[-1]
    check("pan → cmd=pan action+value",
          r.get("cmd") == "pan" and r.get("action") == "center"
          and r.get("value") == "7.1", str(r))

    r = run_tool("record", {"action": "start"})[-1]
    check("record → cmd=record action", r.get("cmd") == "record"
          and r.get("action") == "start", str(r))

    r = run_tool("mark", {"text": "agent bracket"})[-1]
    check("mark → cmd=mark value(text)",
          r.get("cmd") == "mark" and r.get("value") == "agent bracket", str(r))

    r = run_tool("window", {"state": "maximize", "target": "MainWindow"})[-1]
    check("window → action=state, target (NOT value)",
          r.get("cmd") == "window" and r.get("action") == "maximize"
          and r.get("target") == "MainWindow" and "value" not in r, str(r))

    r = run_tool("menu", {"action": "open", "name": "File"})[-1]
    check("menu → action + value(name)",
          r.get("cmd") == "menu" and r.get("action") == "open"
          and r.get("value") == "File", str(r))

    r = run_tool("streams", {"scope": "radio"})[-1]
    check("streams → cmd=streams action(scope)",
          r.get("cmd") == "streams" and r.get("action") == "radio", str(r))

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


def test_gesture_session():
    """A phaseful gesture must reuse one bridge connection until terminal."""
    aether_mcp._close_gesture_bridge()
    os.environ["AETHER_MCP_TOKEN"] = "gesture-secret"
    instances = []

    class _GestureBridge:
        def __init__(self, sock_path, timeout):
            self.sock_path = sock_path
            self.timeout = timeout
            self.requests = []
            self.closed = False
            instances.append(self)

        def request(self, obj):
            self.requests.append(obj)
            action = obj.get("action")
            return {"ok": True, "active": action not in ("end", "cancel"),
                    "sliderDown": action not in ("end", "cancel")}

        def close(self):
            self.closed = True

    original_bridge = aether_mcp.Bridge
    original_sockpath = aether_mcp.bridge_socket_path
    aether_mcp.Bridge = _GestureBridge
    aether_mcp.bridge_socket_path = lambda: "/tmp/gesture.sock"
    try:
        begin = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "begin", "target": "RF power"})
            ["content"][0]["text"])
        move = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "move", "value": "12 0"})
            ["content"][0]["text"])
        status = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "status"})["content"][0]["text"])

        check("gesture begin/move/status reuse one connection",
              len(instances) == 1 and len(instances[0].requests) == 3,
              str(instances))
        check("gesture keeps slider down before end",
              begin.get("sliderDown") and move.get("sliderDown")
              and status.get("sliderDown"), str((begin, move, status)))
        check("gesture requests carry runtime token",
              all(r.get("token") == "gesture-secret"
                  for r in instances[0].requests), str(instances[0].requests))

        ended = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "end"})["content"][0]["text"])
        check("gesture end closes held connection",
              ended.get("active") is False and instances[0].closed
              and aether_mcp._gesture_bridge is None, str(ended))

        class _ExpiredGestureBridge(_GestureBridge):
            def request(self, obj):
                self.requests.append(obj)
                if obj.get("action") == "status":
                    return {"ok": True, "active": False}
                return {"ok": True, "active": True}

        aether_mcp.Bridge = _ExpiredGestureBridge
        aether_mcp.handle_tool(
            "gesture", {"action": "begin", "target": "RF power"})
        expired = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "status"})["content"][0]["text"])
        check("gesture status clears an expired held connection",
              expired.get("active") is False and instances[-1].closed
              and aether_mcp._gesture_bridge is None, str(expired))

        class _FailingGestureBridge(_GestureBridge):
            def request(self, obj):
                self.requests.append(obj)
                if obj.get("action") == "move":
                    return {"ok": False, "error": "bad move"}
                return {"ok": True, "active": True}

        aether_mcp.Bridge = _FailingGestureBridge
        aether_mcp.handle_tool(
            "gesture", {"action": "begin", "target": "RF power"})
        failed = json.loads(aether_mcp.handle_tool(
            "gesture", {"action": "move", "value": "bad"})
            ["content"][0]["text"])
        check("gesture error closes transport for app-side release",
              failed.get("ok") is False and instances[-1].closed
              and aether_mcp._gesture_bridge is None, str(failed))
    finally:
        aether_mcp._close_gesture_bridge()
        aether_mcp.Bridge = original_bridge
        aether_mcp.bridge_socket_path = original_sockpath
        os.environ.pop("AETHER_MCP_TOKEN", None)


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


def test_robustness_tools():
    # assert_state / wait_for read a `get <model> property` and compare `value`.
    captured = []

    def fake(obj, timeout=None):
        captured.append(obj)
        if obj.get("cmd") == "get":
            return {"ok": True, "property": obj.get("property"), "value": "42"}
        return {"ok": True}

    orig = aether_mcp.bridge_request
    aether_mcp.bridge_request = fake
    try:
        captured.clear()
        r = json.loads(aether_mcp.handle_tool(
            "assert_state", {"model": "slice", "selector": "active",
                             "property": "gain", "expected": "42"}
        )["content"][0]["text"])
        req = captured[-1]
        check("assert_state → get model/selector/property",
              req.get("cmd") == "get" and req.get("model") == "slice"
              and req.get("selector") == "active" and req.get("property") == "gain",
              str(req))
        check("assert_state pass on equal value (42==42)", r.get("pass") is True, str(r))

        r = json.loads(aether_mcp.handle_tool(
            "assert_state", {"model": "slice", "property": "gain", "expected": "9"}
        )["content"][0]["text"])
        check("assert_state fail on mismatch (42!=9)",
              r.get("pass") is False and r.get("actual") == "42", str(r))

        r = json.loads(aether_mcp.handle_tool(
            "wait_for", {"model": "slice", "property": "gain",
                         "expected": "42", "timeout_s": 2}
        )["content"][0]["text"])
        check("wait_for matches immediately when value already equal",
              r.get("matched") is True and r.get("value") == "42", str(r))
    finally:
        aether_mcp.bridge_request = orig

    # A transient bridge exception mid-poll must NOT abort the wait — it keeps
    # polling to the deadline and returns matched:false + last_error.
    def boom(obj, timeout=None):
        raise ConnectionError("bridge reset")

    orig = aether_mcp.bridge_request
    aether_mcp.bridge_request = boom
    try:
        r = json.loads(aether_mcp.handle_tool(
            "wait_for", {"model": "radio", "property": "connected",
                         "expected": "true", "timeout_s": 0}
        )["content"][0]["text"])
        check("wait_for survives a transient bridge exception",
              r.get("matched") is False and "bridge reset" in str(r.get("last_error", "")),
              str(r))
    finally:
        aether_mcp.bridge_request = orig


def test_fuzzy_suggest():
    # A resolution-failure response gets did_you_mean candidates from the tree.
    def fake(obj, timeout=None):
        if obj.get("cmd") == "invoke":
            return {"ok": False, "error": "no widget matches 'Mastr volume'"}
        if obj.get("cmd") == "dumpTree":
            return {"ok": True, "roots": [
                {"objectName": "root", "children": [
                    {"accessibleName": "Master volume"},
                    {"accessibleName": "Headphone volume"}]}]}
        return {"ok": True}

    orig = aether_mcp.bridge_request
    aether_mcp.bridge_request = fake
    try:
        r = json.loads(aether_mcp.handle_tool(
            "invoke", {"target": "Mastr volume", "action": "click"}
        )["content"][0]["text"])
        check("invoke failure appends did_you_mean",
              "Master volume" in (r.get("did_you_mean") or []), str(r))
    finally:
        aether_mcp.bridge_request = orig


def test_prompts_and_resources():
    caps = drive_main([json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "initialize"})])[0]["result"]["capabilities"]
    check("initialize advertises prompts+resources",
          "prompts" in caps and "resources" in caps, str(caps))

    pl = drive_main([json.dumps(
        {"jsonrpc": "2.0", "id": 2, "method": "prompts/list"})])[0]["result"]["prompts"]
    check("prompts/list has validate_ui_change",
          any(p["name"] == "validate_ui_change" for p in pl), str(pl))

    pg = drive_main([json.dumps(
        {"jsonrpc": "2.0", "id": 3, "method": "prompts/get",
         "params": {"name": "validate_ui_change", "arguments": {"widget": "W"}}})])[0]
    check("prompts/get returns messages",
          "messages" in pg.get("result", {}), str(pg))

    rl = drive_main([json.dumps(
        {"jsonrpc": "2.0", "id": 4, "method": "resources/list"})])[0]["result"]["resources"]
    check("resources/list includes widget-tree",
          any(x["uri"] == "aethersdr://widget-tree" for x in rl), str(rl))

    bad = drive_main([json.dumps(
        {"jsonrpc": "2.0", "id": 5, "method": "prompts/get",
         "params": {"name": "nope"}})])[0]
    check("unknown prompt → error", "error" in bad, str(bad))


def test_read_only_reflected():
    # bridge_status must reflect the bridge's observe-only state (#4188 area 6)
    # from the token-free ping — the bridge is authoritative; the server only
    # mirrors it. readOnly:true → bridge_read_only + a read_only_note hint.
    orig = aether_mcp.bridge_request
    os.environ["AETHER_MCP_SOCKET"] = "/tmp/does-not-need-to-exist"

    def ro(obj, timeout=None):
        if obj.get("cmd") == "ping":
            return {"ok": True, "authRequired": False, "readOnly": True}
        if obj.get("cmd") == "whoami":
            return {"ok": True, "pid": 1, "readOnly": True}
        return {"ok": True}

    aether_mcp.bridge_request = ro
    try:
        r = json.loads(aether_mcp.handle_tool("bridge_status", {})["content"][0]["text"])
        check("bridge_status reflects readOnly", r.get("bridge_read_only") is True, str(r))
        check("bridge_status adds read_only_note", "read_only_note" in r, str(r))

        # readOnly absent/false → no note, mirroring an unlocked bridge.
        def rw(obj, timeout=None):
            if obj.get("cmd") == "ping":
                return {"ok": True, "authRequired": False, "readOnly": False}
            return {"ok": True}
        aether_mcp.bridge_request = rw
        r = json.loads(aether_mcp.handle_tool("bridge_status", {})["content"][0]["text"])
        check("bridge_status read-write has no note",
              r.get("bridge_read_only") is False and "read_only_note" not in r, str(r))
    finally:
        aether_mcp.bridge_request = orig
        os.environ.pop("AETHER_MCP_SOCKET", None)


if __name__ == "__main__":
    test_field_mapping()
    test_token_attached()
    test_gesture_session()
    test_jsonrpc_robustness()
    test_robustness_tools()
    test_fuzzy_suggest()
    test_prompts_and_resources()
    test_read_only_reflected()
    if _failures:
        print(f"\n{len(_failures)} FAILED: {_failures}")
        sys.exit(1)
    print("\nall MCP field-mapping/protocol checks passed")
