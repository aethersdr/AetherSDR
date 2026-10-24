#!/usr/bin/env python3
"""Check production VOX shortcut admission through an explicitly selected demo bridge.

Launch the app with fresh AETHER_SETTINGS_DIR, AETHER_AUTOMATION_NO_TX=1,
AETHER_AUTOMATION=1 and an explicit AETHER_AUTOMATION_SOCKET. Connect only to
DEMO-0001, then run this probe with --socket <that socket>. No keying verbs.
This is an opt-in app integration probe, not a default CTest socket fixture.
"""

import argparse
import json

from automation_probe import Bridge


def verify(bridge):
    def request(**command):
        reply = bridge.request(command, timeout_seconds=10)
        print(json.dumps({"request": command, "reply": reply}), flush=True)
        return reply

    identity = request(cmd="whoami")
    if not identity.get("ok") or identity.get("txAllowed") is not False:
        raise RuntimeError("probe requires bridge TX permission off")
    radio = request(cmd="get", model="radio")
    state = radio.get("radio", {})
    if not radio.get("ok") or state.get("serial") != "DEMO-0001" or not state.get("connected"):
        raise RuntimeError("probe requires connected DEMO-0001")
    before = request(cmd="get", model="transmit")
    if not before.get("ok") or before.get("transmit", {}).get("voxEnable") is not False:
        raise RuntimeError("probe requires VOX initially disarmed")
    blocked = request(cmd="shortcut", target="vox_toggle")
    expected = "blocked: 'vox_toggle' keys the transmitter (TX-safety guard)."
    if blocked.get("ok") is not False or not blocked.get("error", "").startswith(expected):
        raise RuntimeError("production vox_toggle did not return the specific TX refusal")
    after = request(cmd="get", model="transmit")
    if not after.get("ok") or after.get("transmit", {}).get("voxEnable") is not False:
        raise RuntimeError("VOX changed after the refused shortcut")
    print("PASS production vox_toggle registration and dispatch reject unpermitted arming")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="explicit socket of your isolated demo app")
    args = parser.parse_args()
    bridge = Bridge(args.socket)
    try:
        verify(bridge)
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
