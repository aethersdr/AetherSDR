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
from pathlib import Path

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

    r = run_tool("memory_profile",
                 {"action": "start", "interval_ms": 2000,
                  "max_samples": 500})[-1]
    check("memory_profile → cmd=memory action+bounded sampler args",
          r.get("cmd") == "memprofile" and r.get("action") == "start"
          and r.get("value") == "2000 500", str(r))

    r = run_tool("memory_profile", {"action": "snapshot"})[-1]
    check("memory_profile snapshot has no synthetic value",
          r.get("cmd") == "memprofile" and r.get("action") == "snapshot"
          and "value" not in r, str(r))

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
def test_secure_app_instance():
    """Fresh builds inherit the token only at runtime and are identity-checked."""
    aether_mcp._stop_owned_app()
    original_popen = aether_mcp.subprocess.Popen
    original_bridge = aether_mcp.Bridge
    saved_env = {key: os.environ.get(key) for key in (
        "AETHER_MCP_TOKEN", "AETHER_MCP_SOCKET", "AETHER_AUTOMATION_ALLOW_TX")}
    popen_calls = []
    bridge_requests = []
    tx_allowed = [False]
    ignore_stop = [False]
    accept_tokenless = [False]  # simulate a bridge that advertises but doesn't enforce auth

    class _Process:
        next_pid = 4200

        def __init__(self):
            self.pid = _Process.next_pid
            _Process.next_pid += 1
            self.returncode = None
            self.terminated = False

        def poll(self):
            return self.returncode

        def terminate(self):
            self.terminated = True
            if not ignore_stop[0]:
                self.returncode = -15

        def wait(self, timeout=None):
            if self.returncode is None:
                raise aether_mcp.subprocess.TimeoutExpired("AetherSDR", timeout)
            return self.returncode

        def kill(self):
            if not ignore_stop[0]:
                self.returncode = -9

    def fake_popen(argv, **kwargs):
        process = _Process()
        popen_calls.append((argv, kwargs, process))
        return process

    class _Bridge:
        def __init__(self, socket_path, timeout=None):
            self.socket_path = socket_path

        def request(self, obj):
            bridge_requests.append(obj)
            if obj.get("cmd") == "ping":
                return {"ok": True, "authRequired": True}
            # Model the real bridge's shared-secret gate: a privileged command
            # without the matching token is refused (never ok). This lets the
            # launch proof assert an actual tokenless rejection, not just the
            # advertised authRequired flag.
            if (obj.get("token") != os.environ.get("AETHER_MCP_TOKEN")
                    and not accept_tokenless[0]):
                return {"error": "unauthorized: this bridge requires a token"}
            instance = aether_mcp._owned_app
            return {
                "ok": True,
                "pid": instance["process"].pid,
                "automationIdentity": instance["socket"],
                "socket": instance["socket"],
                "label": instance["label"],
                "txAllowed": tx_allowed[0],
                "readOnly": False,
                "version": "test",
            }

        def close(self):
            pass

    aether_mcp.subprocess.Popen = fake_popen
    aether_mcp.Bridge = _Bridge
    try:
        with tempfile.TemporaryDirectory() as temp_root:
            worktree = Path(temp_root) / "AetherSDR"
            (worktree / ".git").mkdir(parents=True)
            (worktree / "AGENTS.md").write_text("test\n", encoding="utf-8")
            (worktree / "CMakeLists.txt").write_text(
                "project(AetherSDR VERSION 1.0 LANGUAGES CXX)\n", encoding="utf-8")
            if sys.platform == "darwin":
                executable = (worktree / "build" / "AetherSDR.app" /
                              "Contents" / "MacOS" / "AetherSDR")
            elif sys.platform == "win32":
                executable = worktree / "build" / "AetherSDR.exe"
            else:
                executable = worktree / "build" / "AetherSDR"
            executable.parent.mkdir(parents=True)
            executable.write_text("test artifact\n", encoding="utf-8")
            executable.chmod(0o700)

            os.environ["AETHER_MCP_TOKEN"] = "runtime-only-secret"
            os.environ["AETHER_MCP_SOCKET"] = "/tmp/unrelated.sock"
            os.environ["AETHER_AUTOMATION_ALLOW_TX"] = "1"
            launched = json.loads(aether_mcp.handle_tool(
                "app_instance", {"action": "launch", "worktree": str(worktree),
                                 "label": "secure-proof"})["content"][0]["text"])

            argv, kwargs, process = popen_calls[-1]
            child_env = kwargs["env"]
            check("app_instance launches exact canonical artifact without shell",
                  argv == [str(executable.resolve())] and "shell" not in kwargs
                  and kwargs.get("close_fds") is True, str(argv))
            check("app_instance hands token only through child environment",
                  child_env.get("AETHER_MCP_TOKEN") == "runtime-only-secret"
                  and "runtime-only-secret" not in json.dumps(launched)
                  and "runtime-only-secret" not in " ".join(argv), str(launched))
            check("app_instance pins no-autoconnect and no-TX",
                  child_env.get("AETHER_AUTOMATION_NO_AUTOCONNECT") == "1"
                  and child_env.get("AETHER_AUTOMATION_NO_TX") == "1"
                  and child_env.get("AETHER_AUTOMATION_IDENTITY") == launched.get("socket")
                  and child_env.get("AETHER_AUTOMATION_AGENT_NAME") == "secure-proof"
                  and "AETHER_AUTOMATION_ALLOW_TX" not in child_env, str(child_env.keys()))
            whoami_reqs = [req for req in bridge_requests
                           if req.get("cmd") == "whoami"]
            check("app_instance verifies exact authenticated identity",
                  launched.get("authenticated") is True
                  and launched.get("authRequired") is True
                  and launched.get("pid") == process.pid
                  and launched.get("txAllowed") is False
                  and all("token" not in req for req in bridge_requests
                          if req.get("cmd") == "ping")
                  # The authenticated identity check carries the runtime token.
                  and any(req.get("token") == "runtime-only-secret"
                          for req in whoami_reqs)
                  # The launch proof also sends a TOKENLESS whoami and requires
                  # the bridge to refuse it — enforcement, not just the flag.
                  and any("token" not in req for req in whoami_reqs),
                  str(launched))
            check("owned instance becomes the wrapper bridge target",
                  aether_mcp.bridge_socket_path() == launched.get("socket"), str(launched))

            status = json.loads(aether_mcp.handle_tool(
                "app_instance", {"action": "status"})["content"][0]["text"])
            check("app_instance status is authenticated and scoped to owned PID",
                  status.get("running") is True and status.get("authenticated") is True
                  and status.get("pid") == process.pid, str(status))
            stopped = json.loads(aether_mcp.handle_tool(
                "app_instance", {"action": "stop"})["content"][0]["text"])
            check("app_instance stop reaps only the owned process",
                  stopped.get("running") is False and process.terminated
                  and aether_mcp._owned_app is None, str(stopped))

            os.environ.pop("AETHER_MCP_TOKEN", None)
            before = len(popen_calls)
            refused = aether_mcp.handle_tool(
                "app_instance", {"action": "launch", "worktree": str(worktree)})
            check("app_instance refuses tokenless launch before spawning",
                  refused.get("isError") is True and len(popen_calls) == before,
                  str(refused))

            os.environ["AETHER_MCP_TOKEN"] = "runtime-only-secret"
            tx_allowed[0] = True
            rejected = aether_mcp.handle_tool(
                "app_instance", {"action": "launch", "worktree": str(worktree),
                                 "label": "unsafe-proof"})
            unsafe_process = popen_calls[-1][2]
            check("app_instance kills child when authenticated TX is not pinned off",
                  rejected.get("isError") is True and unsafe_process.terminated
                  and aether_mcp._owned_app is None, str(rejected))

            tx_allowed[0] = False
            ignore_stop[0] = True
            stubborn = json.loads(aether_mcp.handle_tool(
                "app_instance", {"action": "launch", "worktree": str(worktree),
                                 "label": "stubborn-proof"})["content"][0]["text"])
            refused_stop = json.loads(aether_mcp.handle_tool(
                "app_instance", {"action": "stop"})["content"][0]["text"])
            check("app_instance does not claim an unkillable child was stopped",
                  stubborn.get("running") is True and refused_stop.get("ok") is False
                  and refused_stop.get("running") is True
                  and aether_mcp._owned_app is not None, str(refused_stop))
            ignore_stop[0] = False
            # The stubborn instance is reapable again now — clear it before the
            # next launch (which refuses while an instance is still owned).
            aether_mcp.handle_tool("app_instance", {"action": "stop"})

            # #3: a bridge that ADVERTISES authRequired but ACCEPTS a tokenless
            # privileged command must be refused (fail closed), not launched.
            accept_tokenless[0] = True
            insecure = aether_mcp.handle_tool(
                "app_instance", {"action": "launch", "worktree": str(worktree),
                                 "label": "insecure-proof"})
            check("app_instance refuses a bridge that accepts tokenless commands",
                  insecure.get("isError") is True
                  and aether_mcp._owned_app is None, str(insecure))
            accept_tokenless[0] = False

            if sys.platform != "win32":
                non_socket_path = aether_mcp._instance_socket()
                Path(non_socket_path).write_text("do not unlink\n", encoding="utf-8")
                try:
                    aether_mcp._remove_owned_socket({"socket": non_socket_path})
                    check("socket cleanup refuses an owned-name non-socket",
                          Path(non_socket_path).is_file(), non_socket_path)
                finally:
                    Path(non_socket_path).unlink(missing_ok=True)
    finally:
        aether_mcp._stop_owned_app()
        aether_mcp.subprocess.Popen = original_popen
        aether_mcp.Bridge = original_bridge
        for key, value in saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


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
    test_secure_app_instance()
    test_jsonrpc_robustness()
    test_robustness_tools()
    test_fuzzy_suggest()
    test_prompts_and_resources()
    test_read_only_reflected()
    if _failures:
        print(f"\n{len(_failures)} FAILED: {_failures}")
        sys.exit(1)
    print("\nall MCP field-mapping/protocol checks passed")
