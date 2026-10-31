"""Fail-closed preparation for simulator-only proof processes (no GUI launch)."""

import json
import os
import subprocess
import sys


def demo_environment(settings_dir, inherited=None):
    """Use an owned settings directory and process pins, never inherited profiles."""
    env = dict(os.environ if inherited is None else inherited)
    env.update({"AETHER_SETTINGS_DIR": str(settings_dir),
                "AETHER_AUTOMATION_DEMO_ONLY": "1",
                "AETHER_AUTOMATION_NO_TX": "1"})
    env.pop("AETHER_AUTOMATION_ALLOW_TX", None)
    return env


def prepare_demo_launch(executable, env):
    """Verify the executable's policy, then seed and read back idle startup.

    Only the existing headless --config entry point is invoked. Old binaries
    reject its unknown subcommand without entering QApplication/main-window
    startup. A setting alone cannot enforce simulator-only connections.
    """
    if ("AETHER_AUTOMATION_DEMO_ONLY" not in env
            or env.get("AETHER_AUTOMATION_NO_TX") != "1"
            or "AETHER_AUTOMATION_ALLOW_TX" in env
            or not env.get("AETHER_SETTINGS_DIR")):
        raise RuntimeError("demo launch requires an isolated profile and demo-only/TX-off pins")

    def config(*args):
        try:
            result = subprocess.run(
                [str(executable), "--config", *args], env=env,
                cwd=env["AETHER_SETTINGS_DIR"], stdin=subprocess.DEVNULL,
                capture_output=True, text=True, timeout=10, check=False)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("demo preflight timed out: " + args[0]) from exc
        except (OSError, UnicodeError) as exc:
            raise RuntimeError("demo preflight could not run: " + args[0]) from exc
        if result.returncode != 0:
            raise RuntimeError("demo preflight failed: " + args[0])
        return result.stdout.strip()

    try:
        policy = json.loads(config("isolation-policy"))
    except (ValueError, TypeError) as exc:
        raise RuntimeError("demo isolation-policy returned malformed JSON") from exc
    if (not isinstance(policy, dict)
            or policy.get("demoOnlySupported") is not True
            or policy.get("demoOnly") is not True
            or type(policy.get("version")) is not int
            or policy["version"] != 1):
        raise RuntimeError("binary does not confirm supported demo-only isolation policy v1")
    config("set", "AutoConnectToLastRadio", "False")
    if config("get", "AutoConnectToLastRadio") != "False":
        raise RuntimeError("demo preflight could not verify autoconnect disabled")


def require_demo_identity(whoami):
    """Reject old or unisolated processes before sending any setup commands."""
    if (not isinstance(whoami, dict) or whoami.get("ok") is not True
            or whoami.get("demoOnly") is not True):
        raise RuntimeError("authenticated whoami did not confirm demo-only isolation")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: automation_isolation.py /absolute/path/to/AetherSDR")
    try:
        prepare_demo_launch(sys.argv[1], dict(os.environ))
    except RuntimeError as exc:
        sys.exit(str(exc))
